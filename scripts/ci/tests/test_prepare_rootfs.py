"""The rootfs cache: fast when it is right, never fast when it is stale.

Bootstrap is the slowest step of the image pipeline and has nothing to do with the
change being built, so its result is cached under a hash of the five inputs it depends
on.  The dangerous failure mode is not slowness, it is a cache that cannot tell an
input changed and silently builds every later image from a stale rootfs.  So these
tests use a stub `mmdebstrap` that counts calls: a cache hit must not call it at all,
and any input change must.

The stub also proves the failure path: a bootstrap that dies leaves no key behind, so
the next build tries again instead of restoring a half-written rootfs.

The vendor apt key is the other half of the contract.  prepare-rootfs.sh needs it as
`signed-by` while bootstrapping, and the Makefile copies the very same file into the
chroot; both sides have to name one path, because a warm cache restores a tarball and
skips this script entirely.  That is what the CI run of 2026-11-09 got wrong: the key was
fetched into the cache directory and copied from the build directory, so every board with
the vendor repo enabled failed at `cp: cannot stat .../public-key.asc` while the key sat
unused next to the cache.  The script therefore takes the path from the Makefile
(`--vendor-key`) and refuses to run without it, and a `curl` stub proves it never reaches
for the network itself.
"""

from __future__ import annotations

import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT = REPO_ROOT / "scripts/ci/prepare-rootfs.sh"
MAKEFILE = REPO_ROOT / "scripts/Makefile"
VENDOR_KEY_URL = "https://sophgo.my-ho.st/public-key.asc"

STUB = """#!/bin/bash
set -e
echo "call" >> "$STUB_LOG"
printf '%s\\n' "$@" >> "$STUB_ARGS"
if [ "${STUB_FAIL:-0}" = 1 ]; then
	echo "stub bootstrap failed" >&2
	exit 1
fi
target=
for argument in "$@"; do
	if [ -d "$argument" ] && [ -z "$(ls -A "$argument")" ]; then target=$argument; fi
done
[ -n "$target" ] || { echo "stub: no empty target directory in $*" >&2; exit 2; }
mkdir -p "$target/etc" "$target/usr/bin"
echo "$STUB_MARKER" > "$target/etc/hostname"
head -c 65536 /dev/zero > "$target/usr/bin/blob"
"""

# The key is produced by the Makefile, never by this script: a stub that records the call
# and writes nothing turns a stray fetch into a visible failure instead of a slow one.
CURL = """#!/bin/bash
set -e
echo "curl $*" >> "$STUB_CURL_LOG"
out=
while [ $# -gt 0 ]; do
	case "$1" in
		-o) out=$2; shift 2 ;;
		*) shift ;;
	esac
done
[ -n "$out" ] || { echo "stub curl: no -o in $*" >&2; exit 2; }
echo "stub-key" > "$out"
"""


@unittest.skipIf(shutil.which("sha256sum") is None, "sha256sum is not available")
class RootfsCacheTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        stub = self.bin / "mmdebstrap"
        stub.write_text(STUB, encoding="utf-8")
        stub.chmod(stub.stat().st_mode | stat.S_IEXEC)
        curl = self.bin / "curl"
        curl.write_text(CURL, encoding="utf-8")
        curl.chmod(curl.stat().st_mode | stat.S_IEXEC)
        self.rootfs = self.root / "rootfs"
        self.cache = self.root / "cache"
        self.log = self.root / "stub.log"
        self.log.write_text("")
        self.args = self.root / "stub-args.log"
        self.args.write_text("")
        self.curl_log = self.root / "curl.log"
        self.curl_log.write_text("")

    def tearDown(self) -> None:
        self.directory.cleanup()

    def run_script(self, *extra: str, packages: str = "a b", fail: bool = False):
        environment = dict(os.environ)
        environment["PATH"] = "%s:%s" % (self.bin, environment["PATH"])
        environment["STUB_LOG"] = str(self.log)
        environment["STUB_ARGS"] = str(self.args)
        environment["STUB_CURL_LOG"] = str(self.curl_log)
        environment["STUB_MARKER"] = packages
        environment["STUB_FAIL"] = "1" if fail else "0"
        return subprocess.run(
            [str(SCRIPT), "--board", "maixcam", "--storage", "sd",
             "--rootfs", str(self.rootfs), "--cache-dir", str(self.cache),
             "--packages", packages, *extra],
            capture_output=True, text=True, check=False, env=environment,
        )

    def calls(self) -> int:
        return self.log.read_text().count("call")

    def bootstraps(self) -> list[str]:
        return self.args.read_text().splitlines()

    def downloads(self) -> list[str]:
        return self.curl_log.read_text().splitlines()

    def stage_vendor_key(self) -> Path:
        """Stand in for the Makefile's key target: the file it promises to provide."""
        key = self.cache / "public-key.asc"
        key.parent.mkdir(parents=True, exist_ok=True)
        key.write_text("staged vendor key\n", encoding="utf-8")
        return key

    def test_the_first_build_bootstraps_and_caches(self) -> None:
        done = self.run_script()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(self.calls(), 1)
        self.assertTrue((self.cache / "rootfs-maixcam-sd.tar").is_file())
        self.assertTrue((self.cache / "rootfs-maixcam-sd.key").is_file())
        self.assertEqual((self.rootfs / "etc/hostname").read_text().strip(), "a b")

    def test_a_lost_stamp_restores_instead_of_bootstrapping(self) -> None:
        self.run_script()
        shutil.rmtree(self.rootfs)
        done = self.run_script()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("cache hit", done.stdout)
        self.assertEqual(self.calls(), 1, "the cache hit must not call mmdebstrap")
        self.assertEqual((self.rootfs / "etc/hostname").read_text().strip(), "a b")

    def test_a_changed_package_list_invalidates_the_cache(self) -> None:
        self.run_script()
        done = self.run_script(packages="a b c")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(self.calls(), 2)
        self.assertEqual((self.rootfs / "etc/hostname").read_text().strip(), "a b c")

    def test_a_changed_mirror_invalidates_the_cache(self) -> None:
        self.run_script()
        done = self.run_script("--mirror", "https://example.invalid/debian")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(self.calls(), 2)

    def test_no_cache_forces_a_real_bootstrap(self) -> None:
        self.run_script()
        done = self.run_script("--no-cache")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(self.calls(), 2)

    def test_a_failed_bootstrap_leaves_no_cache_to_restore(self) -> None:
        done = self.run_script(fail=True)
        self.assertNotEqual(done.returncode, 0)
        self.assertFalse((self.cache / "rootfs-maixcam-sd.key").exists())
        self.assertFalse((self.cache / "rootfs-maixcam-sd.tar").exists())

    def test_an_empty_bootstrap_is_an_error(self) -> None:
        # mmdebstrap is allowed to leave an empty rootfs behind; treating that as
        # success would cache an empty tarball and poison every later build.
        empty = self.bin / "mmdebstrap"
        empty.write_text("#!/bin/bash\nexit 0\n", encoding="utf-8")
        empty.chmod(empty.stat().st_mode | stat.S_IEXEC)
        done = self.run_script()
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("empty rootfs", done.stderr)

    def test_the_cache_key_covers_every_input_it_says_it_does(self) -> None:
        self.run_script()
        key = (self.cache / "rootfs-maixcam-sd.key").read_text()
        self.assertEqual(len(key.strip()), 64)
        self.run_script("--storage", "emmc", packages="a b")
        other = (self.cache / "rootfs-maixcam-emmc.key").read_text()
        self.assertNotEqual(key, other, "storage must be part of the key")

    def test_the_vendor_key_is_signed_by_what_the_makefile_staged(self) -> None:
        key = self.stage_vendor_key()
        done = self.run_script("--vendor-apt", "1", "--vendor-key", str(key))
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(self.calls(), 1)
        self.assertIn("signed-by=%s" % key, " ".join(self.bootstraps()))
        self.assertEqual(self.downloads(), [], "the Makefile owns the download")

    def test_the_vendor_key_is_not_fetched_by_the_script(self) -> None:
        # A cache hit is the case that broke CI: bootstrap is skipped, so anything only
        # bootstrap did never happens -- and the copy into the chroot still needs the key.
        self.run_script("--vendor-apt", "1", "--vendor-key", str(self.stage_vendor_key()))
        done = self.run_script("--vendor-apt", "1", "--vendor-key", str(self.stage_vendor_key()))
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("cache hit", done.stdout)
        self.assertEqual(self.calls(), 1, "the cache hit must not call mmdebstrap")
        self.assertEqual(self.downloads(), [])

    def test_a_cache_hit_does_not_hide_a_missing_vendor_key(self) -> None:
        self.run_script("--vendor-apt", "1", "--vendor-key", str(self.stage_vendor_key()))
        (self.cache / "public-key.asc").unlink()
        done = self.run_script("--vendor-apt", "1",
                               "--vendor-key", str(self.cache / "public-key.asc"))
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("vendor apt key is missing or empty", done.stderr)
        self.assertEqual(self.calls(), 1, "a missing key must not fall back to bootstrap")

    def test_a_missing_vendor_key_stops_the_build_before_it_starts(self) -> None:
        done = self.run_script("--vendor-apt", "1",
                               "--vendor-key", str(self.cache / "absent.asc"))
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("vendor apt key is missing or empty", done.stderr)
        self.assertEqual(self.calls(), 0)

    def test_vendor_apt_off_needs_no_key_at_all(self) -> None:
        done = self.run_script("--vendor-apt", "0")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(self.calls(), 1)
        self.assertNotIn("signed-by", self.bootstraps())
        self.assertEqual(self.downloads(), [])

    def dry_run(self, board: str, storage: str, build: Path, rootfs: Path,
                target: str, *extra: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            ["make", "-C", str(MAKEFILE.parent), "-n", *extra, target,
             "BOARD=%s" % board, "STORAGE_TYPE=%s" % storage,
             "BUILDDIR=%s" % build, "ROOTFS=%s" % rootfs,
             "CONFIG_ROOT=%s" % (REPO_ROOT / "configs"),
             "VERSION_FILE=%s" % (REPO_ROOT / "versions.env"),
             "PLAN_TOOL=%s" % (REPO_ROOT / "scripts/ci/plan.py")],
            capture_output=True, text=True, check=False,
        )

    def argument(self, output: str, name: str) -> str:
        match = re.search(r'%s "([^"]+)"' % re.escape(name), output)
        self.assertIsNotNone(match, "%s is missing from the dry run" % name)
        return match.group(1)

    def test_the_makefile_copies_the_key_it_hands_to_prepare_rootfs(self) -> None:
        # The CI failure, as a test: the key the Makefile passes to prepare-rootfs.sh must
        # be the file it copies into the chroot, and it must live in the cache directory
        # the script is told to use.  When the two disagreed the build died at
        # `cp: cannot stat .../public-key.asc` with the downloaded key in the other place.
        build = self.root / "build"
        rootfs = self.root / "rootfs-target"
        build.mkdir()
        rootfs.mkdir()
        prepare = self.dry_run("duo256", "sd", build, rootfs,
                               "%s/image-prepare-stamp" % build)
        self.assertEqual(prepare.returncode, 0, prepare.stderr)
        cache_dir = self.argument(prepare.stdout, "--cache-dir")
        vendor_key = self.argument(prepare.stdout, "--vendor-key")
        self.assertEqual(vendor_key, os.path.join(cache_dir, "public-key.asc"))
        self.assertIn(VENDOR_KEY_URL, prepare.stdout,
                      "the key must be produced before the rootfs is prepared")

        # `-o` keeps make from descending into the components the stamps stand for, so the
        # dry run prints the customize recipe alone.
        customize = self.dry_run(
            "duo256", "sd", build, rootfs, "%s/image-customize-stamp" % build,
            "-o", "%s/image-addons-stamp" % build,
            "-o", "%s/linux-compile-stamp" % build,
            "-o", "%s/fsbl-package-stamp" % build,
        )
        self.assertEqual(customize.returncode, 0, customize.stderr)
        self.assertIn(VENDOR_KEY_URL, customize.stdout,
                      "the key must be produced before the image is customized")
        copied = re.search(r"cp -v (\S*public-key\.asc) ", customize.stdout)
        self.assertIsNotNone(copied, customize.stdout)
        self.assertEqual(copied.group(1), vendor_key)

    def test_a_board_without_the_vendor_repo_gets_no_key(self) -> None:
        build = self.root / "build-novendor"
        rootfs = self.root / "rootfs-novendor"
        build.mkdir()
        rootfs.mkdir()
        prepare = self.dry_run("maixcam", "sd", build, rootfs,
                               "%s/image-prepare-stamp" % build)
        self.assertEqual(prepare.returncode, 0, prepare.stderr)
        self.assertNotIn("--vendor-key", prepare.stdout)
        self.assertNotIn(VENDOR_KEY_URL, prepare.stdout)

        customize = self.dry_run(
            "maixcam", "sd", build, rootfs, "%s/image-customize-stamp" % build,
            "-o", "%s/image-addons-stamp" % build,
            "-o", "%s/linux-compile-stamp" % build,
            "-o", "%s/fsbl-package-stamp" % build,
        )
        self.assertEqual(customize.returncode, 0, customize.stderr)
        self.assertNotIn("public-key.asc", customize.stdout)


if __name__ == "__main__":
    unittest.main()

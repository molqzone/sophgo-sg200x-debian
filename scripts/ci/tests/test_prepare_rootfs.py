"""The rootfs cache: fast when it is right, never fast when it is stale.

Bootstrap is the slowest step of the image pipeline and has nothing to do with the
change being built, so its result is cached under a hash of the five inputs it depends
on.  The dangerous failure mode is not slowness, it is a cache that cannot tell an
input changed and silently builds every later image from a stale rootfs.  So these
tests use a stub `mmdebstrap` that counts calls: a cache hit must not call it at all,
and any input change must.

The stub also proves the failure path: a bootstrap that dies leaves no key behind, so
the next build tries again instead of restoring a half-written rootfs.
"""

from __future__ import annotations

import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT = REPO_ROOT / "scripts/ci/prepare-rootfs.sh"

STUB = """#!/bin/bash
set -e
echo "call" >> "$STUB_LOG"
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
        self.rootfs = self.root / "rootfs"
        self.cache = self.root / "cache"
        self.log = self.root / "stub.log"
        self.log.write_text("")

    def tearDown(self) -> None:
        self.directory.cleanup()

    def run_script(self, *extra: str, packages: str = "a b", fail: bool = False):
        environment = dict(os.environ)
        environment["PATH"] = "%s:%s" % (self.bin, environment["PATH"])
        environment["STUB_LOG"] = str(self.log)
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


if __name__ == "__main__":
    unittest.main()

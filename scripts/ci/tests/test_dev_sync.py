"""dev sync: the fast loop must write the same bytes the slow loop would.

`make sync` exists so that changing a shell script does not cost a 25-minute image
rebuild.  That is only safe if it installs exactly what the image would install, so
these tests compare the two rather than trusting the copy: the manifest is checked
against the tree, the rendered drop-in against what the real addon produces, and the
whole thing is exercised against a fake mounted rootfs.

Two policies are asserted, not just implemented: it never writes to the boot partition
(that is where the boot log ring lives, preallocated precisely so nothing writes there
at runtime), and it never restarts `load-systemko.service` (this image has no stop path
for the C906L -- stopping a streaming core wedges the board).
"""

from __future__ import annotations

import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "scripts/ci"))

import dev_sync  # noqa: E402

ADDON = REPO_ROOT / "scripts/addons/guidance-fw/addon.mk"
# 按目标路径找，别按下标：manifest 会增删条目，下标会让测试悄悄测错文件。
# Look up by destination, never by index: the manifest grows, and an index would make
# these tests quietly check the wrong file.
DROPIN = next(target for target in dev_sync.TARGETS if target.render)
SCRIPTS = REPO_ROOT / "scripts"


class ManifestTests(unittest.TestCase):
    def test_every_source_exists(self) -> None:
        for target in dev_sync.TARGETS:
            with self.subTest(source=target.source):
                self.assertTrue((REPO_ROOT / target.source).is_file())

    def test_every_destination_is_an_absolute_path(self) -> None:
        for target in dev_sync.TARGETS:
            with self.subTest(destination=target.destination):
                self.assertTrue(target.destination.startswith("/"))
                self.assertNotIn("..", target.destination)

    def test_nothing_is_written_to_the_boot_partition(self) -> None:
        # 环由镜像预分配，运行时（这里是“开发时”）也不往里写：那张 FAT 分区是 U-Boot
        # 读内核和 DTB 的地方。见 docs/boot-log.md。
        for target in dev_sync.TARGETS:
            with self.subTest(destination=target.destination):
                self.assertFalse(target.destination.startswith("/boot"))

    def test_the_core_is_never_stopped_or_restarted(self) -> None:
        # 没有 stop 路径：对正在跑 SPI 采样的核做 stop/restart 会把整块板子挂死。
        for target in dev_sync.TARGETS:
            with self.subTest(destination=target.destination):
                self.assertNotEqual(target.restart, "load-systemko.service")

    def test_only_what_needs_a_power_cycle_says_so(self) -> None:
        # ELF 与 drop-in 只在放核时被读，而这个镜像没有 stop 路径 → 只能下次上电生效。
        # 契约 JSON 相反：cop/bootlogd 每次调用都重新读，改完立刻生效。
        # The ELF and the drop-in are read when the core is released, and there is no stop
        # path, so they need a power cycle; the contract JSON is read per invocation, so it
        # takes effect immediately.
        by_destination = {t.destination: t for t in dev_sync.TARGETS}
        for destination in ("/lib/firmware/c906-mcu.elf",
                            "/etc/systemd/system/load-systemko.service.d/"
                            "20-guidance-fw.conf"):
            with self.subTest(destination=destination):
                self.assertIn("power cycle", by_destination[destination].note)
        self.assertIn("already in effect",
                      by_destination["/lib/firmware/guidance_contract.json"].note)


class RenderTests(unittest.TestCase):
    def test_load_renders_remoteproc(self) -> None:
        text = dev_sync.render(DROPIN.source,
                               {"GUIDANCE_FW_RPROC_MODE": "load"}).decode()
        self.assertIn("ExecStart=/usr/bin/rtos-mode remoteproc\n", text)
        self.assertNotIn("@GUIDANCE_FW_RPROC_COMMAND@", text)

    def test_attach_renders_attach(self) -> None:
        text = dev_sync.render(DROPIN.source,
                               {"GUIDANCE_FW_RPROC_MODE": "attach"}).decode()
        self.assertIn("ExecStart=/usr/bin/rtos-mode attach\n", text)

    @unittest.skipIf(shutil.which("make") is None, "make is not available")
    def test_the_rendered_drop_in_matches_what_the_image_builds(self) -> None:
        # 两条路（镜像与 dev sync）必须装出同一份文件，否则“快路径”会悄悄偏离“发布路径”。
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / "build"
            rootfs = root / "rootfs"
            build.mkdir()
            makefile = root / "addon.mk"
            makefile.write_text(
                f"BUILDDIR := {build}\n"
                f"ROOTFS := {rootfs}\n"
                f"COMPONENTS_ROOT := {REPO_ROOT / 'components'}\n"
                "BOARD := maixcam\n"
                "GUIDANCE_FW_RPROC_MODE := load\n"
                f"include {ADDON}\n",
                encoding="utf-8",
            )
            subprocess.run(
                ["make", "--no-print-directory", "-f", str(makefile),
                 str(build / "guidance-fw-stamp")],
                cwd=SCRIPTS, check=True, capture_output=True, text=True,
            )
            installed = (rootfs / "etc/systemd/system/load-systemko.service.d/"
                         "20-guidance-fw.conf").read_bytes()
        rendered = dev_sync.render(DROPIN.source,
                                   {"GUIDANCE_FW_RPROC_MODE": "load"})
        self.assertEqual(rendered, installed)


class CardRootTests(unittest.TestCase):
    """`--root` is how a card-mounted rootfs is updated; it is also testable."""

    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        (self.root / "etc").mkdir()

    def tearDown(self) -> None:
        self.directory.cleanup()

    def run_sync(self, *extra: str) -> str:
        output = io.StringIO()
        with redirect_stdout(output):
            dev_sync.main(["dev_sync.py", "--root", str(self.root),
                           "--board-name", "maixcam", *extra])
        return output.getvalue()

    def test_it_installs_the_runtime_files_with_their_modes(self) -> None:
        self.run_sync()
        daemon = self.root / "usr/local/sbin/bootlogd"
        self.assertTrue(daemon.is_file())
        self.assertTrue(daemon.stat().st_mode & 0o111)
        tool = self.root / "usr/bin/rtos-mode"
        self.assertTrue(tool.stat().st_mode & 0o111)
        contract = self.root / "lib/firmware/guidance_contract.json"
        self.assertTrue(contract.is_file())
        dropin = (self.root / "etc/systemd/system/load-systemko.service.d/"
                  "20-guidance-fw.conf")
        self.assertIn("ExecStart=/usr/bin/rtos-mode remoteproc\n",
                      dropin.read_text(encoding="utf-8"))

    def test_a_new_unit_is_enabled_not_just_copied(self) -> None:
        # boot-time.service 只有被 enable 过才会在下次启动跑；把文件拷过去而不 enable，
        # 是"装了但没生效"的经典形态。
        self.run_sync()
        link = (self.root / "etc/systemd/system/multi-user.target.wants/"
                "boot-time.service")
        self.assertTrue(link.is_symlink())
        self.assertEqual(link.readlink(), Path("/etc/systemd/system/boot-time.service"))
        self.assertTrue((self.root / "usr/local/sbin/boot-time").stat().st_mode & 0o111)

    def test_a_second_run_changes_nothing(self) -> None:
        self.run_sync()
        self.assertIn("nothing changed", self.run_sync())

    def test_dry_run_writes_nothing(self) -> None:
        self.assertIn("would write", self.run_sync("--dry-run"))
        self.assertFalse((self.root / "usr/bin/rtos-mode").exists())

    def test_it_refuses_a_directory_that_is_not_a_rootfs(self) -> None:
        empty = Path(self.directory.name) / "empty"
        empty.mkdir()
        with self.assertRaises(SystemExit):
            dev_sync.main(["dev_sync.py", "--root", str(empty)])


if __name__ == "__main__":
    unittest.main()

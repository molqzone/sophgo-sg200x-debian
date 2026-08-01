from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


CI_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(CI_DIR))
import cache  # noqa: E402


class CacheInvalidationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tempdir.name)

    def tearDown(self) -> None:
        self.tempdir.cleanup()

    def create(self, *names: str) -> None:
        for name in names:
            path = self.root / name
            if "." in path.name or path.name.endswith("stamp"):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("present\n", encoding="utf-8")
            else:
                path.mkdir(parents=True, exist_ok=True)

    def test_linux_invalidates_downstream_trees(self) -> None:
        self.create("kernel", "osdrv", "middleware", "rootfs", "u-boot")
        cache.invalidate(self.root, "linux")
        for name in ("kernel", "osdrv", "middleware", "rootfs"):
            self.assertFalse((self.root / name).exists())
        self.assertTrue((self.root / "u-boot").exists())

    def test_firmware_invalidates_fsbl_and_rootfs(self) -> None:
        self.create("sg2002-ipc-build-stamp", "fsbl", "rootfs", "kernel")
        cache.invalidate(self.root, "firmware")
        for name in ("sg2002-ipc-build-stamp", "fsbl", "rootfs"):
            self.assertFalse((self.root / name).exists())
        self.assertTrue((self.root / "kernel").exists())

    def test_remove_path_rejects_parent_escape(self) -> None:
        with self.assertRaises(cache.PlanError):
            cache.remove_path(self.root, "../outside")

    def test_boot_inputs_follow_storage_partition(self) -> None:
        repo = self.root / "repo"
        config = repo / "configs"
        (config / "common").mkdir(parents=True)
        (config / "board/u-boot").mkdir(parents=True)
        (config / "settings.mk").write_text("", encoding="utf-8")
        (config / "board/settings.mk").write_text(
            "PARTITION_FILE=partition_$(STORAGE_TYPE).xml\n", encoding="utf-8"
        )
        for relative in (
            "memmap.py",
            "partition_sd.xml",
            "partition_emmc.xml",
            "u-boot/defconfig",
            "u-boot/cvitek.h",
            "u-boot/cvi_board_init.c",
        ):
            path = config / "board" / relative
            path.write_text(relative, encoding="utf-8")

        inputs = cache.layer_inputs(repo, config, "board", "emmc", "boot")
        self.assertIn(config / "board/partition_emmc.xml", inputs)
        self.assertNotIn(config / "board/partition_sd.xml", inputs)


if __name__ == "__main__":
    unittest.main()

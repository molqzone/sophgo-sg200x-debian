from __future__ import annotations

import sys
import subprocess
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
        self.create("guidance-fw-build-stamp", "fsbl", "rootfs", "kernel")
        cache.invalidate(self.root, "firmware")
        for name in ("guidance-fw-build-stamp", "fsbl", "rootfs"):
            self.assertFalse((self.root / name).exists())
        self.assertTrue((self.root / "kernel").exists())

    def test_remove_path_rejects_parent_escape(self) -> None:
        with self.assertRaises(cache.PlanError):
            cache.remove_path(self.root, "../outside")

    def test_layer_completion_rejects_partial_checkout(self) -> None:
        self.create("linux-compile-stamp")
        head = self.root / "kernel/.git/HEAD"
        head.parent.mkdir(parents=True)
        head.write_text("ref: refs/heads/test\n", encoding="utf-8")
        self.assertTrue(cache.layer_complete(self.root, "linux"))

        head.unlink()
        self.assertFalse(cache.layer_complete(self.root, "linux"))

    def test_layer_completion_requires_build_stamp(self) -> None:
        head = self.root / "kernel/.git/HEAD"
        head.parent.mkdir(parents=True)
        head.write_text("ref: refs/heads/test\n", encoding="utf-8")
        self.assertFalse(cache.layer_complete(self.root, "linux"))

    def test_rootfs_completion_requires_customization(self) -> None:
        self.create("rootfs", "image-compile-stamp", "images/sdcard.img")
        self.assertFalse(cache.layer_complete(self.root, "rootfs"))

        self.create("image-customize-stamp")
        self.assertTrue(cache.layer_complete(self.root, "rootfs"))

    def test_rootfs_completion_requires_normal_image(self) -> None:
        self.create("rootfs", "image-customize-stamp", "image-compile-stamp")
        self.assertFalse(cache.layer_complete(self.root, "rootfs"))

    def test_rootfs_invalidation_preserves_complete_component_graphs(self) -> None:
        reusable = (
            "linux-prepare-checkout-stamp",
            "linux-prepare-patch-stamp",
            "linux-prepare-configure-stamp",
            "linux-compile-stamp",
            "osdrv-prepare-checkout-stamp",
            "osdrv-package-stamp",
            "middleware-prepare-checkout-stamp",
            "middleware-package-stamp",
            "vcodec-firmware-prepare-checkout-stamp",
            "vcodec-firmware-package-stamp",
            "uboot-prepare-checkout-stamp",
            "uboot-compile-stamp",
            "opensbi-prepare-checkout-stamp",
            "opensbi-compile-stamp",
            "fsbl-prepare-checkout-stamp",
            "fsbl-package-stamp",
            "cvi-pinmux-package-stamp",
            "guidance-fw-build-stamp",
        )
        rootfs_dependent = (
            "image-prepare-stamp",
            "image-addons-stamp",
            "image-customize-stamp",
            "image-compile-stamp",
            "maixcam-sensor-config-package-stamp",
            "maixcam-sensor-config-stamp",
            "guidance-fw-stamp",
        )
        self.create("rootfs", *reusable, *rootfs_dependent)

        cache.invalidate(self.root, "rootfs")

        self.assertFalse((self.root / "rootfs").exists())
        for name in reusable:
            self.assertTrue((self.root / name).exists(), name)
        for name in rootfs_dependent:
            self.assertFalse((self.root / name).exists(), name)

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

    def test_component_inputs_are_split_by_build_layer(self) -> None:
        repo = self.root / "repo"
        config = repo / "configs"
        component = repo / "components/guidance-fw"
        (config / "common").mkdir(parents=True)
        (config / "board").mkdir(parents=True)

        firmware_inputs = cache.layer_inputs(
            repo, config, "board", "sd", "firmware"
        )
        rootfs_inputs = cache.layer_inputs(repo, config, "board", "sd", "rootfs")

        self.assertIn(component / "firmware", firmware_inputs)
        self.assertIn(component / "ports/osdrv/patches", firmware_inputs)
        self.assertNotIn(component / "linux", firmware_inputs)
        self.assertNotIn(component / "tools", firmware_inputs)
        self.assertNotIn(component / "README.md", firmware_inputs)

        self.assertIn(component / "linux", rootfs_inputs)
        self.assertIn(component / "tools", rootfs_inputs)
        self.assertIn(component / "firmware", rootfs_inputs)
        self.assertNotIn(component / "README.md", rootfs_inputs)

    def test_board_settings_invalidate_middleware(self) -> None:
        repo = self.root / "repo"
        config = repo / "configs"
        (config / "common").mkdir(parents=True)
        (config / "base").mkdir()
        (config / "child").mkdir()
        (config / "settings.mk").write_text("GLOBAL=1\n", encoding="utf-8")
        (config / "base/board.mk").write_text("", encoding="utf-8")
        (config / "base/settings.mk").write_text("BASE=1\n", encoding="utf-8")
        (config / "child/board.mk").write_text(
            "BASE_BOARD := base\n", encoding="utf-8"
        )
        (config / "child/settings.mk").write_text(
            "SENSOR_ENV_EXTRA=CONFIG_SENSOR_SMS_SC035HGS=y\n",
            encoding="utf-8",
        )

        inputs = cache.layer_inputs(repo, config, "child", "sd", "middleware")
        self.assertIn(config / "settings.mk", inputs)
        self.assertIn(config / "base/settings.mk", inputs)
        self.assertIn(config / "child/settings.mk", inputs)

    def test_offline_launcher_verifies_every_image_layer(self) -> None:
        launcher = CI_DIR / "local-build.sh"
        source = launcher.read_text(encoding="utf-8")
        self.assertIn('cache_args+=(--verify)', source)
        self.assertIn(
            "run_cache firmware linux osdrv middleware boot rootfs", source
        )

    def test_offline_image_copies_the_completed_normal_image(self) -> None:
        makefile = (CI_DIR.parents[0] / "Makefile").read_text(encoding="utf-8")
        self.assertIn('test -s "$(BUILDDIR)/images/sdcard.img"', makefile)
        self.assertIn(
            'cp "$(BUILDDIR)/images/sdcard.img" '
            '"$(OUTPUT_DIR)/$(BOARD)_$(STORAGE_TYPE).img"',
            makefile,
        )
        self.assertIn(
            'cmp -s "$(BUILDDIR)/images/sdcard.img" '
            '"$(OUTPUT_DIR)/$(BOARD)_$(STORAGE_TYPE).img"',
            makefile,
        )

    def test_offline_image_rejects_missing_fixed_sensor_defaults(self) -> None:
        repo = CI_DIR.parents[1]
        build_root = self.root / "build"
        rootfs = build_root / "rootfs"
        status = rootfs / "var/lib/dpkg/status"
        status.parent.mkdir(parents=True)
        status.write_text(
            "\n".join(
                (
                    "Package: cvitek-fsbl-maixcam-sc035hgs",
                    "Package: linux-image-maixcam-sc035hgs-test",
                    "Package: cvitek-osdrv-maixcam-sc035hgs-test",
                    "Package: cvitek-middleware-maixcam-sc035hgs",
                    "Package: firmware-vcodec-cv181x",
                    "Package: sensor-config-maixcam-sc035hgs",
                )
            )
            + "\n",
            encoding="utf-8",
        )
        (rootfs / "etc/init.d").mkdir(parents=True)
        sensor_script = rootfs / "etc/init.d/sensor-config.sh"
        sensor_script.write_text("#!/bin/sh\n", encoding="utf-8")
        sensor_script.chmod(0o755)
        service = rootfs / "etc/systemd/system/sensor-config.service"
        service.parent.mkdir(parents=True)
        service.write_text("[Service]\n", encoding="utf-8")
        (rootfs / "mnt/data").mkdir(parents=True)
        (rootfs / "mnt/data/sensor_cfg.ini.SC035HGS").write_text(
            "sensor\n", encoding="utf-8"
        )
        (rootfs / "mnt/cfg/param").mkdir(parents=True)
        (rootfs / "mnt/cfg/param/cvi_sdr_bin").write_bytes(b"pq")
        (rootfs / "lib/modules").mkdir(parents=True)
        boot = rootfs / "boot"
        (boot / "fdt/test").mkdir(parents=True)
        (boot / "fip.bin").write_bytes(b"fip")
        (boot / "uboot.env").write_bytes(b"env")
        (boot / "vmlinuz-test").write_bytes(b"kernel")
        (boot / "fdt/test/board.dtb").write_bytes(b"dtb")
        extlinux = boot / "extlinux/extlinux.conf"
        extlinux.parent.mkdir(parents=True)
        extlinux.write_text("label l0\n linux /vmlinuz-test\n", encoding="utf-8")
        for name in ("image-customize-stamp", "image-compile-stamp"):
            (build_root / name).write_text("done\n", encoding="utf-8")
        cached_image = build_root / "images/sdcard.img"
        cached_image.parent.mkdir(parents=True)
        cached_image.write_bytes(b"normal image")
        (build_root / "u-boot.bin").write_bytes(b"uboot")
        (build_root / "fip.bin").write_bytes(b"fip")

        result = subprocess.run(
            [
                "make",
                "-C",
                str(repo / "scripts"),
                "offline-image",
                "BOARD=maixcam-sc035hgs",
                "STORAGE_TYPE=sd",
                f"BUILDDIR={build_root}",
                f"ROOTFS={rootfs}",
                f"CONFIG_ROOT={repo / 'configs'}",
                f"VERSION_FILE={repo / 'versions.env'}",
                f"PLAN_TOOL={repo / 'scripts/ci/plan.py'}",
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "offline rootfs has the wrong fixed sensor profile",
            result.stderr,
        )

        defaults = rootfs / "etc/default/maixcam-sensor-config"
        defaults.parent.mkdir(parents=True)
        defaults.write_text(
            "SENSOR_CONFIG_DEFAULT=/mnt/data/sensor_cfg.ini.SC035HGS\n"
            "SENSOR_CONFIG_FIXED=1\n",
            encoding="utf-8",
        )
        output = self.root / "output"
        output.mkdir()
        copy_result = subprocess.run(
            [*result.args, f"OUTPUT_DIR={output}"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        self.assertEqual(copy_result.returncode, 0, copy_result.stderr)
        self.assertEqual(
            (output / "maixcam-sc035hgs_sd.img").read_bytes(),
            cached_image.read_bytes(),
        )


if __name__ == "__main__":
    unittest.main()

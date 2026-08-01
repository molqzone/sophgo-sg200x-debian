from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


CI_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(CI_DIR))
import plan  # noqa: E402
import validate as validation  # noqa: E402


class PlanTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.config = Path(self.tempdir.name)
        for name in ("common", "base", "child"):
            (self.config / name).mkdir()
        (self.config / "base/board.mk").write_text("", encoding="utf-8")
        (self.config / "child/board.mk").write_text("BASE_BOARD := base\n", encoding="utf-8")
        (self.config / "settings.mk").write_text('IMAGE_ADDITIONS="usb-gadget"\n', encoding="utf-8")
        (self.config / "base/settings.mk").write_text('IMAGE_ADDITIONS += "base-addon"\n', encoding="utf-8")
        (self.config / "child/settings.mk").write_text('IMAGE_ADDITIONS += "child-addon"\n', encoding="utf-8")
        matrix = {"include": [{"board": "child", "storage": "sd", "format": "img"}]}
        (self.config / "build-matrix.json").write_text(json.dumps(matrix), encoding="utf-8")

    def tearDown(self) -> None:
        self.tempdir.cleanup()

    def test_inheritance_and_settings(self) -> None:
        self.assertEqual(plan.board_chain(self.config, "child"), ["child", "base", "common"])
        additions = plan.assignment_words(
            plan.effective_assignments(self.config, "child")["IMAGE_ADDITIONS"]
        )
        self.assertEqual(additions, ["usb-gadget", "base-addon", "child-addon"])

    def test_exact_file_and_patch_tombstone(self) -> None:
        (self.config / "base/memmap.py").write_text("base\n", encoding="utf-8")
        self.assertEqual(plan.resolve_file(self.config, "child", "memmap.py"), self.config / "base/memmap.py")
        for layer in ("common", "base", "child"):
            (self.config / layer / "patches/linux").mkdir(parents=True)
        (self.config / "common/patches/linux/0001.patch").write_text("common", encoding="utf-8")
        (self.config / "base/patches/linux/0002.patch").write_text("base", encoding="utf-8")
        (self.config / "child/patches/linux/0001.patch.skip").write_text("", encoding="utf-8")
        patches = plan.patch_files(self.config, "child", "linux")
        self.assertEqual(patches, [self.config / "base/patches/linux/0002.patch"])

    def test_exact_directory_shadows_base_directory(self) -> None:
        (self.config / "base/dts").mkdir()
        (self.config / "child/dts").mkdir()
        (self.config / "base/dts/base.dts").write_text("base\n", encoding="utf-8")
        (self.config / "child/dts/child.dts").write_text("child\n", encoding="utf-8")
        self.assertEqual(
            plan.resolve_directory(self.config, "child", "dts"),
            self.config / "child/dts",
        )

    def test_patches_follow_application_layer_order(self) -> None:
        for layer in ("common", "base", "child"):
            (self.config / layer / "patches/linux").mkdir(parents=True)
        common = self.config / "common/patches/linux/9000.patch"
        base = self.config / "base/patches/linux/8000.patch"
        child = self.config / "child/patches/linux/0001.patch"
        common.write_text("common", encoding="utf-8")
        base.write_text("base", encoding="utf-8")
        child.write_text("child", encoding="utf-8")
        self.assertEqual(
            plan.patch_files(self.config, "child", "linux"),
            [common, base, child],
        )

    def test_addon_change_selects_only_consumers(self) -> None:
        with mock.patch.object(plan, "changed_files", return_value=["scripts/addons/child-addon/addon.mk"]):
            affected = plan.affected_matrix(self.config, "base", "head")
        self.assertEqual([entry["board"] for entry in affected], ["child"])

    def test_validation_change_rebuilds_all_boards(self) -> None:
        with mock.patch.object(
            plan,
            "changed_files",
            return_value=["scripts/ci/validate.py"],
        ):
            affected = plan.affected_matrix(self.config, "base", "head")
        self.assertEqual([entry["board"] for entry in affected], ["child"])

    def test_validation_test_change_does_not_rebuild_boards(self) -> None:
        with mock.patch.object(
            plan,
            "changed_files",
            return_value=["scripts/ci/tests/test_plan.py"],
        ):
            self.assertEqual(plan.affected_matrix(self.config, "base", "head"), [])

    def test_image_generation_inputs_rebuild_all_boards(self) -> None:
        paths = (
            "scripts/python/mmap_conv.py",
            "scripts/python/raw2cimg.py",
            "scripts/genimage_sd.cfg",
            "scripts/genimage_emmc.cfg",
        )
        for path in paths:
            with self.subTest(path=path), mock.patch.object(
                plan, "changed_files", return_value=[path]
            ):
                affected = plan.affected_matrix(self.config, "base", "head")
                self.assertEqual([entry["board"] for entry in affected], ["child"])

    def test_board_build_script_change_rebuilds_all_boards(self) -> None:
        with mock.patch.object(
            plan, "changed_files", return_value=["scripts/ci/build-board.sh"]
        ):
            affected = plan.affected_matrix(self.config, "base", "head")
        self.assertEqual([entry["board"] for entry in affected], ["child"])

    def test_matrix_validation(self) -> None:
        entries = plan.validate(self.config)
        self.assertEqual(entries[0]["format"], "img")

    def test_pin_scan_ignores_untracked_build_output(self) -> None:
        root = self.config / "repository"
        root.mkdir()
        subprocess.run(["git", "init", "--quiet", str(root)], check=True)
        (root / "versions.env").write_text("PIN_COMMIT=abc123\n", encoding="utf-8")
        subprocess.run(["git", "-C", str(root), "add", "versions.env"], check=True)
        (root / "build-output").mkdir()
        (root / "build-output/image.img").write_text("abc123\n", encoding="utf-8")
        with mock.patch.object(validation, "REPO_ROOT", root):
            self.assertEqual(
                validation.tracked_occurrences({"abc123"}),
                {"abc123": ["versions.env"]},
            )

    def test_pin_scan_includes_tracked_files_in_excluded_named_directories(self) -> None:
        root = self.config / "repository"
        root.mkdir()
        subprocess.run(["git", "init", "--quiet", str(root)], check=True)
        (root / "versions.env").write_text("PIN_COMMIT=abc123\n", encoding="utf-8")
        (root / "build-output").mkdir()
        maintained = root / "build-output/maintained.txt"
        maintained.write_text("abc123\n", encoding="utf-8")
        subprocess.run(
            ["git", "-C", str(root), "add", "versions.env", "build-output/maintained.txt"],
            check=True,
        )
        with mock.patch.object(validation, "REPO_ROOT", root):
            self.assertEqual(
                validation.tracked_occurrences({"abc123"}),
                {"abc123": ["build-output/maintained.txt", "versions.env"]},
            )

    def test_pin_scan_falls_back_when_worktree_metadata_is_unavailable(self) -> None:
        root = self.config / "repository"
        root.mkdir()
        (root / "versions.env").write_text("PIN_COMMIT=abc123\n", encoding="utf-8")
        (root / "build-output").mkdir()
        (root / "build-output/image.img").write_text("abc123\n", encoding="utf-8")
        failure = subprocess.CalledProcessError(128, ["git", "ls-files"])
        with (
            mock.patch.object(validation, "REPO_ROOT", root),
            mock.patch.object(validation.subprocess, "run", side_effect=failure),
        ):
            self.assertEqual(
                validation.tracked_occurrences({"abc123"}),
                {"abc123": ["versions.env"]},
            )

    def test_pin_scan_excludes_custom_output_path(self) -> None:
        root = self.config / "repository"
        root.mkdir()
        (root / "versions.env").write_text("PIN_COMMIT=abc123\n", encoding="utf-8")
        custom_output = root / "artifacts" / "current"
        custom_output.mkdir(parents=True)
        (custom_output / "image.img").write_text("abc123\n", encoding="utf-8")
        failure = subprocess.CalledProcessError(128, ["git", "ls-files"])
        with (
            mock.patch.object(validation, "REPO_ROOT", root),
            mock.patch.object(validation.subprocess, "run", side_effect=failure),
        ):
            self.assertEqual(
                validation.tracked_occurrences(
                    {"abc123"}, excluded_paths=(custom_output,)
                ),
                {"abc123": ["versions.env"]},
            )

    def test_pin_scan_matches_across_read_chunks(self) -> None:
        root = self.config / "repository"
        root.mkdir()
        (root / "versions.env").write_text("1234567abc123\n", encoding="utf-8")
        failure = subprocess.CalledProcessError(128, ["git", "ls-files"])
        with (
            mock.patch.object(validation, "REPO_ROOT", root),
            mock.patch.object(validation, "SCAN_CHUNK_SIZE", 8),
            mock.patch.object(validation.subprocess, "run", side_effect=failure),
        ):
            self.assertEqual(
                validation.tracked_occurrences({"abc123"}),
                {"abc123": ["versions.env"]},
            )

    def test_fallback_scan_skips_non_regular_files(self) -> None:
        root = self.config / "repository"
        root.mkdir()
        regular = root / "regular.txt"
        regular.write_text("source\n", encoding="utf-8")
        special = root / "special"
        with (
            mock.patch.object(validation, "REPO_ROOT", root),
            mock.patch.object(
                validation.os,
                "walk",
                return_value=[(str(root), [], [regular.name, special.name])],
            ),
            mock.patch.object(
                Path,
                "is_file",
                autospec=True,
                side_effect=lambda path: path.name == regular.name,
            ),
        ):
            self.assertEqual(validation.fallback_files(()), [regular])

    def test_validate_output_checks_zip_and_component_artifacts(self) -> None:
        output = self.config / "output"
        output.mkdir()
        zip_entry = [{"board": "child", "storage": "emmc", "format": "zip"}]
        with validation.zipfile.ZipFile(output / "child_emmc.zip", "w") as archive:
            archive.writestr("fip.bin", b"fip")
        validation.validate_output(zip_entry, "child", "emmc", output, False)

        component_entry = [
            {
                "board": "child",
                "storage": "sd",
                "format": "img",
                "components": ["sg2002-ipc"],
            }
        ]
        (output / "child_sd.img").write_bytes(b"image")
        for suffix in (
            "c906-mcu.elf",
            "c906-mcu.bin",
            "rtos-cmd",
            "rtos-bench",
            "libsg2002-rtos.a",
        ):
            (output / f"child_{suffix}").write_bytes(b"artifact")
        validation.validate_output(component_entry, "child", "sd", output, False)

        (output / "child_rtos-cmd").unlink()
        with self.assertRaisesRegex(plan.PlanError, "component artifact is missing"):
            validation.validate_output(component_entry, "child", "sd", output, False)

        (output / "child_emmc.zip").write_bytes(b"not a zip")
        with self.assertRaises(validation.zipfile.BadZipFile):
            validation.validate_output(zip_entry, "child", "emmc", output, False)

    def test_uboot_defconfig_requires_exactly_one_target(self) -> None:
        defconfig = self.config / "u-boot-defconfig"
        defconfig.write_text("CONFIG_RISCV=y\n", encoding="utf-8")
        with self.assertRaisesRegex(plan.PlanError, "exactly one target"):
            validation.validate_uboot_defconfig("child", defconfig)

        defconfig.write_text(
            "CONFIG_RISCV=y\nCONFIG_TARGET_CVITEK_CV181X=y\n",
            encoding="utf-8",
        )
        validation.validate_uboot_defconfig("child", defconfig)

    def test_mailbox_only_remoteproc_regions(self) -> None:
        dts_directory = self.config / "maixcam-dts"
        dts_directory.mkdir()
        dts = dts_directory / "maixcam.dts"
        valid = """
        /delete-node/ &vdev0vring0;
        /delete-node/ &vdev0vring1;
        /delete-node/ &vdev0buffer;
        cv181x-c906_1 {
            memory-region = <&fast_image &rtos_boot_trace>;
        };
        """
        self.assertEqual(
            validation.remoteproc_memory_regions(valid),
            ["fast_image", "rtos_boot_trace"],
        )
        dts.write_text(valid, encoding="utf-8")
        validation.validate_maixcam_remoteproc_dts(dts_directory)

        invalid = """
        /delete-node/ &vdev0vring0;
        /delete-node/ &vdev0vring1;
        /delete-node/ &vdev0buffer;
        cv181x-c906_1 {
            memory-region = <&fast_image &rtos_boot_trace
                             &vdev0vring0 &vdev0vring1 &vdev0buffer>;
        };
        """
        self.assertEqual(
            validation.remoteproc_memory_regions(invalid),
            [
                "fast_image",
                "rtos_boot_trace",
                "vdev0vring0",
                "vdev0vring1",
                "vdev0buffer",
            ],
        )
        dts.write_text(invalid, encoding="utf-8")
        with self.assertRaisesRegex(plan.PlanError, "mailbox-only remoteproc"):
            validation.validate_maixcam_remoteproc_dts(dts_directory)

        dts.write_text(
            """
            cv181x-c906_1 {
                memory-region = <&fast_image &rtos_boot_trace>;
            };
            """,
            encoding="utf-8",
        )
        with self.assertRaisesRegex(plan.PlanError, "delete legacy RPMsg pool"):
            validation.validate_maixcam_remoteproc_dts(dts_directory)

    def test_compiled_mailbox_only_remoteproc_regions(self) -> None:
        valid = """
        reserved-memory {
            rproc { phandle = <0x10>; };
            boot-trace@8ffff000 { phandle = <0x11>; };
        };
        cv181x-c906_1 {
            memory-region = <0x10 0x11>;
        };
        """
        validation.validate_compiled_maixcam_dts(valid)

        with self.assertRaisesRegex(plan.PlanError, "retains legacy RPMsg pool"):
            validation.validate_compiled_maixcam_dts(
                valid + "\nvdev0buffer { compatible = \"shared-dma-pool\"; };\n"
            )

        with self.assertRaisesRegex(plan.PlanError, "reference two regions"):
            validation.validate_compiled_maixcam_dts(
                valid.replace("<0x10 0x11>", "<0x10 0x11 0x12>")
            )

    def test_reserved_memory_names_use_boot_safe_storage(self) -> None:
        patch = self.config / "reserved-memory.patch"
        patch.write_text(
            "+\tchar\t\t\t\tname[64];\n"
            "+\tif (strscpy(rmem->name, uname, sizeof(rmem->name)) < 0)\n"
            "+\t\tpanic(\"%s: Reserved-memory name is too long: %s\\n\",\n"
            "+\t\t      __func__, uname);\n"
            "+\trmem = __find_rmem(np);\n"
            "+\tif (rmem)\n"
            "+\t\treturn rmem;\n",
            encoding="utf-8",
        )
        validation.validate_reserved_memory_name_patch(patch)

        invalid_patches = {
            "too_small": (
                "+\tchar\t\t\t\tname[32];\n"
                "+\tif (strscpy(rmem->name, uname, sizeof(rmem->name)) < 0)\n"
                "+\t\tpanic(\"%s: Reserved-memory name is too long: %s\\n\",\n"
                "+\t\t      __func__, uname);\n"
            ),
            "truncated": "+\tchar\t\t\t\tname[64];\n+\tstrncpy(rmem->name, uname, 64);\n",
            "early_fdt_pointer": "+\tconst char\t\t\t*name;\n+\trmem->name = uname;\n",
            "early_memblock_pointer": (
                "+\tchar\t\t\t\t*name;\n"
                "+\trmem->name = memblock_alloc(strlen(uname) + 1, SMP_CACHE_BYTES);\n"
            ),
        }
        for name, text in invalid_patches.items():
            with self.subTest(name=name):
                patch.write_text(text, encoding="utf-8")
                with self.assertRaises(plan.PlanError):
                    validation.validate_reserved_memory_name_patch(patch)

        patch.write_text(
            "+\tchar\t\t\t\tname[64];\n"
            "+\tif (strscpy(rmem->name, uname, sizeof(rmem->name)) < 0)\n"
            "+\t\tpanic(\"%s: Reserved-memory name is too long: %s\\n\",\n"
            "+\t\t      __func__, uname);\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(plan.PlanError, "prefer the device-tree phandle"):
            validation.validate_reserved_memory_name_patch(patch)


if __name__ == "__main__":
    unittest.main()

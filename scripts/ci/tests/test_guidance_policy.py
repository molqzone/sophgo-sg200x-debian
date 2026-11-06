from __future__ import annotations

import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import sys

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "scripts/ci"))

import validate  # noqa: E402
from plan import PlanError, assignment_words, effective_assignments  # noqa: E402

CONFIGS = REPO_ROOT / "configs"
COMPONENT = REPO_ROOT / "components/guidance-fw"
ADDON = REPO_ROOT / "scripts/addons/guidance-fw/addon.mk"
DTS = next((CONFIGS / "maixcam/dts").glob("*.dts")).read_text(encoding="utf-8")
PRODUCTION = {"GUIDANCE_FW_LOADER": "remoteproc"}


def policy(**overrides: str) -> dict[str, str]:
    settings = dict(PRODUCTION)
    settings.update(overrides)
    return settings


def generate_dropin(loader: str) -> str:
    """Run the real addon into a temp rootfs and return the installed drop-in."""
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
            f"GUIDANCE_FW_LOADER := {loader}\n"
            f"include {ADDON}\n",
            encoding="utf-8",
        )
        subprocess.run(
            ["make", "--no-print-directory", "-f", str(makefile),
             str(build / "guidance-fw-stamp")],
            check=True, capture_output=True, text=True,
        )
        return (rootfs / "etc/systemd/system/load-systemko.service.d/"
                "20-guidance-fw.conf").read_text(encoding="utf-8")


class BootPolicyTests(unittest.TestCase):
    def test_production_policy_is_the_shipped_one(self) -> None:
        settings = effective_assignments(CONFIGS, "maixcam")
        self.assertEqual(settings.get("GUIDANCE_FW_LOADER"), "remoteproc")
        for gone in validate.GUIDANCE_RETIRED_SETTINGS:
            self.assertNotIn(gone, settings)
        validate.check_guidance_boot_policy(settings, DTS)

    def test_fsbl_is_accepted(self) -> None:
        validate.check_guidance_boot_policy(policy(GUIDANCE_FW_LOADER="fsbl"), DTS)

    def test_the_retired_knobs_are_refused_not_ignored(self) -> None:
        # 独立变量能配出工具不认识的东西，这正是 rtos-mode load 那次事故的形状。
        for gone in validate.GUIDANCE_RETIRED_SETTINGS:
            with self.subTest(setting=gone):
                with self.assertRaises(PlanError) as caught:
                    validate.check_guidance_boot_policy(policy(**{gone: "whatever"}), DTS)
                self.assertIn("is gone", str(caught.exception))

    def test_unknown_loader_is_refused(self) -> None:
        with self.assertRaises(PlanError):
            validate.check_guidance_boot_policy(policy(GUIDANCE_FW_LOADER="uboot"), DTS)

    def test_dts_must_keep_the_firmware_band(self) -> None:
        text = DTS.replace("memory-region\t= <&fast_image &rtos_boot_trace>;",
                           "memory-region\t= <&rtos_boot_trace>;")
        if text == DTS:
            self.skipTest("DTS memory-region spelling changed; update this test")
        with self.assertRaises(PlanError) as caught:
            validate.check_guidance_boot_policy(PRODUCTION, text)
        self.assertIn("fast_image", str(caught.exception))

    def test_dts_must_keep_the_doorbell_irq(self) -> None:
        text = DTS.replace("interrupts\t= <101 IRQ_TYPE_LEVEL_HIGH>;",
                           "interrupts\t= <38 IRQ_TYPE_LEVEL_HIGH>;")
        if text == DTS:
            self.skipTest("DTS mailbox IRQ spelling changed; update this test")
        with self.assertRaises(PlanError) as caught:
            validate.check_guidance_boot_policy(PRODUCTION, text)
        self.assertIn("IRQ 101", str(caught.exception))

    def test_dts_must_keep_the_cmdqu_node(self) -> None:
        text = DTS.replace("\trtos_cmdqu {", "\tcmdqu-node {")
        with self.assertRaises(PlanError):
            validate.check_guidance_boot_policy(PRODUCTION, text)


class DropinGenerationTests(unittest.TestCase):
    """The drop-in is generated from the policy, not committed with a baked-in mode."""

    def setUp(self) -> None:
        if shutil.which("make") is None:
            self.skipTest("make is not available")

    def test_remoteproc_policy_generates_remoteproc(self) -> None:
        dropin = generate_dropin("remoteproc")
        self.assertIn("ExecStart=/usr/bin/rtos-mode remoteproc\n", dropin)
        self.assertNotIn("@GUIDANCE_FW_RPROC_MODE@", dropin)

    def test_fsbl_loader_generates_attach(self) -> None:
        dropin = generate_dropin("fsbl")
        self.assertIn("ExecStart=/usr/bin/rtos-mode attach\n", dropin)

    def test_the_firmware_owns_the_page_format(self) -> None:
        # 页由固件在 bring-up 里建立（`Format()` 以 clean+invalidate 收尾）。Linux 不再
        # 格式化：那一步曾经是必需的，因为固件只校验、不建立非全零页。
        dropin = generate_dropin("remoteproc")
        self.assertNotIn("ExecStartPre=", dropin)
        self.assertNotIn("cop format", dropin)

    def test_core_has_no_stop_path(self) -> None:
        # 小核的生命周期由 SoC 复位兜底，镜像**不**写 `stop`：实测对正在跑 SPI 采样的
        # C906L 写 `stop` 会把整块板子挂死（只能断电），所以这里连“可以停一下”的
        # 接口都不留。见 docs/shutdown-behaviour.md。
        dropin = generate_dropin("remoteproc")
        self.assertNotIn("ExecStop=", dropin.split("ExecStopPost=")[0])
        tool = (COMPONENT / "tools/rtos-mode").read_text(encoding="utf-8")
        self.assertNotIn("CHABORT", tool)
        self.assertNotIn("\tstop)\n", tool)
        self.assertNotIn("printf stop", tool)

    def test_the_core_is_not_gated_on_a_target_that_cycles(self) -> None:
        # `After=network-online.target` 与清掉的 `Before=NetworkManager.service` 成环，
        # systemd 会把 load-systemko 的 start 任务删掉——核永远不起来。
        directives = [
            line for line in generate_dropin("remoteproc").splitlines()
            if line and not line.startswith("#")
        ]
        self.assertNotIn("After=network-online.target", directives)
        self.assertIn("Before=", directives)

    def test_there_is_no_park_command_left(self) -> None:
        # 停机路径不需要固件配合（见 docs/shutdown-behaviour.md），所以契约与工具里
        # 都不该再出现一条停机命令。
        contract = json.loads(
            (COMPONENT / "firmware/guidance_contract.json").read_text(encoding="utf-8"))
        self.assertEqual(sorted(contract["command"]), ["led", "magic", "ping", "rejected"])
        self.assertFalse((COMPONENT / "tools/guidance-cmd.py").exists())

    def test_template_is_the_committed_artifact(self) -> None:
        # 模板留的是**命令**占位符：策略里的 load/attach 不是 rtos-mode 的子命令，直接
        # 填进去会生成 `rtos-mode load`（usage、exit 2），核永远不会被放行。
        # The placeholder is the *command*: the policy's load/attach are not rtos-mode
        # subcommands, and substituting one produced `rtos-mode load` -- usage, exit 2,
        # core never released.
        template = REPO_ROOT / validate.GUIDANCE_DROPIN_TEMPLATE
        self.assertTrue(template.is_file())
        text = template.read_text(encoding="utf-8")
        self.assertIn("@GUIDANCE_FW_RPROC_COMMAND@", text)
        self.assertNotIn("@GUIDANCE_FW_RPROC_MODE@", text)
        self.assertFalse(template.with_suffix("").exists(),
                         "the baked-in drop-in must not come back")

    def test_rtos_mode_implements_the_mode_the_policy_can_pick(self) -> None:
        tool = (COMPONENT / "tools/rtos-mode").read_text(encoding="utf-8")
        for mode in ("remoteproc", "attach"):
            self.assertIn(f"\t{mode})\n", tool)
        self.assertIn("attach needs the loader", tool)


class SettingsSurfaceTests(unittest.TestCase):
    def test_guidance_boards_declare_the_policy(self) -> None:
        settings = effective_assignments(CONFIGS, "maixcam")
        additions = assignment_words(settings.get("IMAGE_ADDITIONS", ""))
        self.assertIn("guidance-fw", additions)
        for key in PRODUCTION:
            self.assertIn(key, settings)


if __name__ == "__main__":
    unittest.main()

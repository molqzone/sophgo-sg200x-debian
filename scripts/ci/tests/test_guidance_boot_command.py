"""The drop-in's rtos-mode argument must be a subcommand rtos-mode actually has.

`GUIDANCE_FW_RPROC_MODE` is a policy value (`load` = Linux writes the firmware and
starts the core, `attach` = the core is already running).  Those are not rtos-mode's
subcommand names: its arms are cmdqu/remoteproc/attach/status.  Substituting the policy
value straight into `ExecStart` produced `rtos-mode load`, which prints the usage text
and exits 2 -- the unit failed, the core was never released, and every later symptom
("the core is blinking but Linux is gone") was read as a different problem.

These tests drive the real addon so the rendered command is what the image will run,
and compare it against the tool's own case arms.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "scripts/ci"))

import validate  # noqa: E402
from plan import PlanError  # noqa: E402

ADDON = REPO_ROOT / "scripts/addons/guidance-fw/addon.mk"
TOOL = REPO_ROOT / "components/guidance-fw/tools/rtos-mode"
TEMPLATE = REPO_ROOT / validate.GUIDANCE_DROPIN_TEMPLATE
SCRIPTS = REPO_ROOT / "scripts"


def render(mode: str) -> str:
    """Install the real addon into a temp rootfs and return the installed drop-in."""
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
            f"GUIDANCE_FW_RPROC_MODE := {mode}\n"
            f"include {ADDON}\n",
            encoding="utf-8",
        )
        subprocess.run(
            ["make", "--no-print-directory", "-f", str(makefile),
             str(build / "guidance-fw-stamp")],
            cwd=SCRIPTS, check=True, capture_output=True, text=True,
        )
        return (rootfs / "etc/systemd/system/load-systemko.service.d/"
                "20-guidance-fw.conf").read_text(encoding="utf-8")


def tool_arguments() -> set[str]:
    """The case arms rtos-mode implements, read from the tool itself."""
    arms = set(re.findall(r"^\t([a-z][a-z0-9-]*)\)$", TOOL.read_text(encoding="utf-8"),
                          re.MULTILINE))
    if "remoteproc" not in arms:
        raise AssertionError(f"cannot read rtos-mode's case arms: {arms}")
    return arms


@unittest.skipIf(shutil.which("make") is None, "make is not available")
class DropinRenderingTests(unittest.TestCase):
    def test_load_renders_remoteproc(self) -> None:
        # 策略说“Linux 写固件并起核”，rtos-mode 里那条分支叫 remoteproc。
        self.assertIn("ExecStart=/usr/bin/rtos-mode remoteproc\n", render("load"))

    def test_attach_renders_attach(self) -> None:
        self.assertIn("ExecStart=/usr/bin/rtos-mode attach\n", render("attach"))

    def test_nothing_is_left_unsubstituted(self) -> None:
        self.assertNotIn("@GUIDANCE_FW_RPROC_COMMAND@", render("load"))

    def test_every_rendered_argument_exists_in_rtos_mode(self) -> None:
        arms = tool_arguments()
        self.assertNotIn("load", arms)
        for mode in ("load", "attach"):
            command = re.search(
                r"^ExecStart=/usr/bin/rtos-mode (\S+)$", render(mode), re.MULTILINE
            )
            self.assertIsNotNone(command)
            self.assertIn(command.group(1), arms)

    def test_the_template_leaves_the_argument_to_the_mapping(self) -> None:
        text = TEMPLATE.read_text(encoding="utf-8")
        self.assertIn("@GUIDANCE_FW_RPROC_COMMAND@", text)
        self.assertNotIn("ExecStart=/usr/bin/rtos-mode load", text)


class PolicyValidationTests(unittest.TestCase):
    def test_the_shipped_policy_is_the_production_one(self) -> None:
        settings = validate.effective_assignments(REPO_ROOT / "configs", "maixcam") \
            if hasattr(validate, "effective_assignments") else None
        if settings is None:
            self.skipTest("validate.py does not expose effective_assignments here")
        self.assertEqual(settings.get("GUIDANCE_FW_LOADER"), "remoteproc")


if __name__ == "__main__":
    unittest.main()

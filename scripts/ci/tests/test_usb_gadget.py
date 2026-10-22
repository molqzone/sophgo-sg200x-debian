from __future__ import annotations

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
USB_ADDON = REPO_ROOT / "scripts/addons/maixcam-usb"
GADGET_SCRIPT = USB_ADDON / "run_usb.sh"
MODE_SCRIPT = USB_ADDON / "usb-mode-switch"
AUTHORIZE_SCRIPT = USB_ADDON / "maixcam-ssh-authorize"
ADDON_MK = USB_ADDON / "addon.mk"
NCM_UNIT = USB_ADDON / "usb-gadget-ncm.service"


class UsbGadgetTests(unittest.TestCase):
    def test_ncm_stop_unlinks_os_descriptor_without_following_it(self) -> None:
        source = GADGET_SCRIPT.read_text(encoding="utf-8")
        stop_function = "stop() {" + source.split("stop() {", 1)[1].split(
            '\ncase "$1" in', 1
        )[0]
        with tempfile.TemporaryDirectory() as directory:
            gadget = Path(directory) / "gadget"
            config = gadget / "configs/c.1"
            (config / "strings/0x409").mkdir(parents=True)
            (gadget / "strings/0x409").mkdir(parents=True)
            function = gadget / "functions/ncm.usb0"
            (function / "os_desc/interface.ncm").mkdir(parents=True)
            (config / "ncm.usb0").symlink_to(function, target_is_directory=True)
            (gadget / "os_desc").mkdir()
            os_link = gadget / "os_desc/c.1"
            os_link.symlink_to(config, target_is_directory=True)
            marker = config / "configfs-attribute"
            marker.write_text("must not traverse the OS descriptor symlink\n")
            # Configfs removes its generated attributes when rmdir is called.
            # Keep those directories here to isolate removal of the symlinks.
            script = (
                f"CVI_GADGET={shlex.quote(str(gadget))}\n"
                "rmdir() { :; }\n"
                f"{stop_function}\nstop\n"
            )
            result = subprocess.run(
                ["sh", "-c", script], capture_output=True, text=True, check=False
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(os_link.is_symlink(), "OS descriptor still pins c.1")
            self.assertFalse((config / "ncm.usb0").is_symlink())
            self.assertTrue(marker.is_file(), "cleanup followed the OS link")

    def test_acm_recovery_keeps_ncm_as_persistent_boot_mode(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bin_dir = root / "bin"
            bin_dir.mkdir()
            log = root / "systemctl.log"
            self._write_systemctl_stub(bin_dir, log)
            self._write_stub(bin_dir, "ifdown", "")
            self._write_stub(bin_dir, "ifup", "")
            self._write_stub(bin_dir, "ip", "")
            systemd_dir = root / "systemd"
            systemd_dir.mkdir()

            env = {
                **os.environ,
                "PATH": f"{bin_dir}:/usr/bin:/bin",
                "SYSTEMCTL_LOG": str(log),
                "MAIXCAM_SYSTEMD_DIR": str(systemd_dir),
                "MAIXCAM_RUN_USB": str(root / "missing-run-usb.sh"),
            }
            result = subprocess.run(
                ["sh", str(MODE_SCRIPT), "acm"],
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            calls = log.read_text(encoding="utf-8").splitlines()
            self.assertIn(
                "enable usb-gadget-ncm.service usb-gadget-ncm-usb0.service",
                calls,
            )
            self.assertIn("disable usb-shell.service usb-gadget-acm.service", calls)
            self.assertIn("start usb-gadget-acm.service", calls)
            self.assertIn("start usb-shell.service", calls)
            self.assertFalse(
                [call for call in calls if call.startswith("enable usb-gadget-acm")],
                "ACM recovery must not persist across a reboot",
            )
            self.assertFalse(
                [call for call in calls if call.startswith("enable usb-shell")],
                "the serial shell must not persist across a reboot",
            )

    def test_legacy_autorevert_is_retired_on_boot(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bin_dir = root / "bin"
            bin_dir.mkdir()
            log = root / "systemctl.log"
            self._write_systemctl_stub(bin_dir, log)
            systemd_dir = root / "systemd"
            (systemd_dir / "timers.target.wants").mkdir(parents=True)
            timer = systemd_dir / "usb-mode-autorevert.timer"
            timer.write_text("[Timer]\n", encoding="utf-8")
            (systemd_dir / "timers.target.wants" / timer.name).symlink_to(timer)
            (systemd_dir / "usb-gadget-ncm.service").write_text(
                "[Service]\n", encoding="utf-8"
            )

            env = {
                **os.environ,
                "PATH": f"{bin_dir}:/usr/bin:/bin",
                "SYSTEMCTL_LOG": str(log),
                "MAIXCAM_SYSTEMD_DIR": str(systemd_dir),
                "MAIXCAM_AUTHORIZED_KEYS_FILE": str(root / "missing-keys"),
                "MAIXCAM_SSH_TARGET_DIR": str(root / "ssh"),
            }
            result = subprocess.run(
                ["sh", str(AUTHORIZE_SCRIPT)],
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(timer.exists(), "legacy timer unit still installed")
            self.assertFalse(
                (systemd_dir / "timers.target.wants" / timer.name).is_symlink(),
                "legacy timer still enabled",
            )
            calls = log.read_text(encoding="utf-8").splitlines()
            self.assertIn(
                "enable usb-gadget-ncm.service usb-gadget-ncm-usb0.service",
                calls,
            )
            self.assertIn("start usb-gadget-ncm.service", calls)
            self.assertFalse(
                [
                    call
                    for call in calls
                    if call.startswith("enable ") and "usb-mode-autorevert" in call
                ],
                "boot must not arm the ACM fallback",
            )

    def test_legacy_rndis_image_falls_back_to_its_network_gadget(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bin_dir = root / "bin"
            bin_dir.mkdir()
            log = root / "systemctl.log"
            self._write_systemctl_stub(bin_dir, log)
            systemd_dir = root / "systemd"
            (systemd_dir / "timers.target.wants").mkdir(parents=True)
            (systemd_dir / "usb-gadget-rndis.service").write_text(
                "[Service]\n", encoding="utf-8"
            )
            # The old image had this enabled after the timer flipped it.
            (systemd_dir / "multi-user.target.wants").mkdir()
            (systemd_dir / "multi-user.target.wants" / "usb-gadget-acm.service").symlink_to(
                "/etc/systemd/system/usb-gadget-acm.service"
            )

            env = {
                **os.environ,
                "PATH": f"{bin_dir}:/usr/bin:/bin",
                "SYSTEMCTL_LOG": str(log),
                "MAIXCAM_SYSTEMD_DIR": str(systemd_dir),
                "MAIXCAM_AUTHORIZED_KEYS_FILE": str(root / "missing-keys"),
                "MAIXCAM_SSH_TARGET_DIR": str(root / "ssh"),
            }
            result = subprocess.run(
                ["sh", str(AUTHORIZE_SCRIPT)],
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            calls = log.read_text(encoding="utf-8").splitlines()
            self.assertIn(
                "enable usb-gadget-rndis.service usb-gadget-rndis-usb0.service",
                calls,
            )
            self.assertIn("start usb-gadget-rndis.service", calls)
            self.assertFalse(
                [call for call in calls if call.startswith("enable usb-gadget-ncm")],
                "a pre-NCM image must not be told to start a missing NCM unit",
            )

    def test_image_does_not_install_legacy_usb_units(self) -> None:
        source = ADDON_MK.read_text(encoding="utf-8")
        installed = [
            line
            for line in source.splitlines()
            if "@cp" in line and "usb-mode-autorevert" in line
        ]
        self.assertEqual(installed, [])
        enable_line = next(
            line for line in source.splitlines() if "systemd-enable" in line and "echo" in line
        )
        self.assertIn("usb-gadget-ncm.service", enable_line)
        self.assertNotIn("usb-gadget-acm.service", enable_line)
        self.assertNotIn("usb-gadget-rndis.service", enable_line)

    def test_ncm_unit_wins_over_legacy_acm_and_rndis(self) -> None:
        source = NCM_UNIT.read_text(encoding="utf-8")
        self.assertIn("After=usb-gadget-acm.service usb-gadget-rndis.service", source)
        self.assertIn("Conflicts=usb-gadget-acm.service", source)
        self.assertIn("Conflicts=usb-gadget-rndis.service", source)

    @staticmethod
    def _write_stub(bin_dir: Path, name: str, body: str) -> None:
        path = bin_dir / name
        path.write_text("#!/bin/sh\n" + body + "\nexit 0\n", encoding="utf-8")
        path.chmod(0o755)

    @classmethod
    def _write_systemctl_stub(cls, bin_dir: Path, log: Path) -> None:
        cls._write_stub(
            bin_dir,
            "systemctl",
            'printf "%s\\n" "$*" >> "$SYSTEMCTL_LOG"',
        )


if __name__ == "__main__":
    unittest.main()

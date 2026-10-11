from __future__ import annotations

import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
GADGET_SCRIPT = REPO_ROOT / "scripts/addons/maixcam-usb/run_usb.sh"


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


if __name__ == "__main__":
    unittest.main()

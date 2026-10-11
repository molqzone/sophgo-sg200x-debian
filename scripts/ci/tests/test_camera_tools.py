from __future__ import annotations

import ast
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
VIEWER_ROOT = REPO_ROOT / "tools/maixcam-camera"


class CameraToolTests(unittest.TestCase):
    def test_failed_remoteproc_start_preserves_boot_blacklist(self) -> None:
        source = (REPO_ROOT / "components/guidance-fw/tools/rtos-mode").read_text(
            encoding="utf-8"
        )
        definitions = source.split('\n[ $# -eq 1 ] || usage', 1)[0]
        with tempfile.TemporaryDirectory() as directory:
            blacklist = Path(directory) / "blacklist.conf"
            blacklist.write_text(
                "blacklist cvitek_mailbox\nblacklist cvitek_remoteproc\n"
                "blacklist aic8800_fdrv\n"
            )
            # Run the real startup failure path without touching any modules.
            # On hosts without the firmware, its existence check fails first;
            # otherwise the first (stubbed) insmod fails.
            script = (
                definitions
                + f"\nBLACKLIST={shlex.quote(str(blacklist))}\n"
                + "stop_media_chain() { :; }\ninsmod() { return 1; }\n"
                + "start_remoteproc\n"
            )
            result = subprocess.run(
                ["sh", "-c", script], capture_output=True, text=True, check=False
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(
                set(blacklist.read_text().splitlines()),
                {"blacklist cvitek_mailbox", "blacklist cvitek_remoteproc",
                 "blacklist aic8800_fdrv"},
            )

    def test_camera_startup_scripts_have_linux_line_endings(self) -> None:
        for relative_path in (
            "components/guidance-fw/tools/rtos-mode",
            "scripts/addons/load-systemko/load-systemko.sh",
            "scripts/addons/maixcam-sensor-config/sensor-config.sh",
        ):
            with self.subTest(script=relative_path):
                script = REPO_ROOT / relative_path
                self.assertNotIn(b"\r", script.read_bytes())
                result = subprocess.run(
                    ["sh", "-n", str(script)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_viewer_is_valid_python(self) -> None:
        source = (VIEWER_ROOT / "viewer.py").read_text(encoding="utf-8")
        ast.parse(source)

    def test_low_latency_decoder_contract(self) -> None:
        source = (VIEWER_ROOT / "viewer.py").read_text(encoding="utf-8")
        for fragment in (
            '"fflags": "nobuffer"',
            '"flags": "low_delay"',
            '"max_delay": "0"',
            '"reorder_queue_size": "0"',
            "thread_count = 1",
            "ThreadType.SLICE",
            "Flags.low_delay",
        ):
            self.assertIn(fragment, source)

    def test_dependencies_are_pinned(self) -> None:
        requirements = (VIEWER_ROOT / "requirements.txt").read_text(
            encoding="utf-8"
        )
        self.assertEqual(
            requirements.splitlines(),
            ["av==18.0.0", "opencv-python==4.13.0.92"],
        )


if __name__ == "__main__":
    unittest.main()

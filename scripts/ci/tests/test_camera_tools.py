from __future__ import annotations

import ast
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
VIEWER_ROOT = REPO_ROOT / "tools/maixcam-camera"


class CameraToolTests(unittest.TestCase):
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

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


CI_DIR = Path(__file__).resolve().parents[1]
REPO_ROOT = CI_DIR.parents[1]
CONFIG_ROOT = REPO_ROOT / "configs"
MMF_SOURCE = "sample/test_mmf/maix_mmf/sophgo_middleware.c"
SAMPLE_VIO_SOURCE = "sample/test_mmf/sample_vio.c"
SAMPLE_VIO_MAIN_SOURCE = "sample/test_mmf/sample_vio_main.c"
ISP_CONTROL_SOURCE = "sample/test_mmf/isp_control.c"
HGS_VB_PATCH = (
    CONFIG_ROOT
    / "maixcam-sc035hgs/patches/middleware/0002-increase-h26x-vb-pool.patch"
)
HGS_FPS_PATCH = (
    CONFIG_ROOT
    / "maixcam-sc035hgs/patches/middleware/0003-run-sc035hgs-at-30fps.patch"
)
HGS_ISP_CONTROL_PATCH = (
    CONFIG_ROOT
    / "maixcam-sc035hgs/patches/middleware/0004-add-runtime-isp-control.patch"
)
MAIXCAM_SIGPIPE_PATCH = (
    CONFIG_ROOT
    / "maixcam/patches/middleware/0014-sample-ignore-sigpipe-in-rtsp-server.patch"
)
HGS_H26X_BLOCK_COUNT = 4
H26X_POOL_SIZE = re.compile(
    r"astCommPool\[priv\.vb_enc_h26x_id\]\.u32BlkSize\s*=\s*(?P<value>[^;]+);"
)
H26X_POOL_COUNT = re.compile(
    r"astCommPool\[priv\.vb_enc_h26x_id\]\.u32BlkCnt\s*=\s*(?P<value>\d+);"
)

sys.path.insert(0, str(CI_DIR))
import plan  # noqa: E402


def materialize_source(board: str, source_path: str, root: Path) -> str:
    for patch in plan.patch_files(CONFIG_ROOT, board, "middleware"):
        patch_text = patch.read_text(encoding="utf-8")
        if f"a/{source_path}" not in patch_text:
            continue
        result = subprocess.run(
            ["git", "apply", f"--include={source_path}", str(patch)],
            cwd=root,
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        if result.returncode != 0:
            raise AssertionError(
                f"failed to apply {patch} for {board}:\n{result.stderr}"
            )
    return (root / source_path).read_text(encoding="utf-8")


class MiddlewarePatchTests(unittest.TestCase):
    def h26x_pool(self, board: str) -> tuple[str, int]:
        with tempfile.TemporaryDirectory() as tempdir:
            source = materialize_source(board, MMF_SOURCE, Path(tempdir))
        size_matches = list(H26X_POOL_SIZE.finditer(source))
        count_matches = list(H26X_POOL_COUNT.finditer(source))
        self.assertEqual(len(size_matches), 1, f"{board}: H.26x size must be unique")
        self.assertEqual(len(count_matches), 1, f"{board}: H.26x count must be unique")
        size = re.sub(r"\s+", "", size_matches[0].group("value"))
        return size, int(count_matches[0].group("value"))

    def test_hgs_h26x_pool_contract_preserves_parent(self) -> None:
        expected_size = "ALIGN(1280,DEFAULT_ALIGN)*ALIGN(720,DEFAULT_ALIGN)*3/2"
        self.assertEqual(self.h26x_pool("maixcam"), (expected_size, 1))
        self.assertEqual(
            self.h26x_pool("maixcam-sc035hgs"),
            (expected_size, HGS_H26X_BLOCK_COUNT),
        )

    def test_hgs_vb_patch_is_isolated_to_hgs_matrix_entry(self) -> None:
        for entry in plan.load_matrix(CONFIG_ROOT):
            board = str(entry["board"])
            selected = HGS_VB_PATCH in plan.patch_files(
                CONFIG_ROOT, board, "middleware"
            )
            self.assertEqual(
                selected,
                "maixcam-sc035hgs" in plan.board_chain(CONFIG_ROOT, board),
                f"unexpected HGS VB patch selection for {board}",
            )

    def test_hgs_fps_patch_is_isolated_to_hgs_matrix_entry(self) -> None:
        for entry in plan.load_matrix(CONFIG_ROOT):
            board = str(entry["board"])
            selected = HGS_FPS_PATCH in plan.patch_files(
                CONFIG_ROOT, board, "middleware"
            )
            self.assertEqual(
                selected,
                "maixcam-sc035hgs" in plan.board_chain(CONFIG_ROOT, board),
                f"unexpected HGS FPS patch selection for {board}",
            )

    def test_hgs_isp_control_patch_is_isolated_to_hgs_matrix_entry(self) -> None:
        for entry in plan.load_matrix(CONFIG_ROOT):
            board = str(entry["board"])
            selected = HGS_ISP_CONTROL_PATCH in plan.patch_files(
                CONFIG_ROOT, board, "middleware"
            )
            self.assertEqual(
                selected,
                "maixcam-sc035hgs" in plan.board_chain(CONFIG_ROOT, board),
                f"unexpected HGS ISP control patch selection for {board}",
            )

    def test_hgs_fps_contract_uses_effective_input_rate(self) -> None:
        patch_text = HGS_FPS_PATCH.read_text(encoding="utf-8")
        self.assertRegex(
            patch_text,
            r"(?m)^\+\tcase SMS_SC035HGS_MIPI_480P_120FPS_12BIT:\n"
            r"^\+\t\tpstPubAttr->f32FrameRate = 30;\n",
        )
        self.assertIn(
            "-\tcase SMS_SC035HGS_MIPI_480P_120FPS_12BIT:", patch_text
        )

        with tempfile.TemporaryDirectory() as tempdir:
            source = materialize_source("maixcam-sc035hgs", MMF_SOURCE, Path(tempdir))
        self.assertEqual(
            source.count("static int _mmf_get_sensor_fps(bool *is_live)"), 1
        )
        self.assertIn("CVI_ISP_GetPubAttr(0, &pub_attr)", source)
        self.assertEqual(
            source.count("stFrameRate.s32SrcFrameRate = src_fps;"), 4
        )
        self.assertEqual(
            source.count("stFrameRate.s32DstFrameRate = dst_fps;"), 4
        )
        self.assertIn(
            "format_out, src_fps, fps, depth, mirror, flip, fit", source
        )

    def test_maixcam_rtsp_ignores_sigpipe_for_all_descendants(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            source = materialize_source("maixcam", SAMPLE_VIO_SOURCE, Path(tempdir))
        self.assertEqual(source.count("signal(SIGPIPE, SIG_IGN);"), 1)

        for entry in plan.load_matrix(CONFIG_ROOT):
            board = str(entry["board"])
            selected = MAIXCAM_SIGPIPE_PATCH in plan.patch_files(
                CONFIG_ROOT, board, "middleware"
            )
            self.assertEqual(
                selected,
                "maixcam" in plan.board_chain(CONFIG_ROOT, board),
                f"unexpected MaixCAM SIGPIPE patch selection for {board}",
            )

    def test_hgs_runtime_isp_control_contract(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            root = Path(tempdir)
            control_source = materialize_source(
                "maixcam-sc035hgs", ISP_CONTROL_SOURCE, root
            )
            vio_source = materialize_source(
                "maixcam-sc035hgs", SAMPLE_VIO_SOURCE, root
            )
            main_source = materialize_source(
                "maixcam-sc035hgs", SAMPLE_VIO_MAIN_SOURCE, root
            )

        self.assertIn('#define CONTROL_MAX_REQUESTS_PER_FRAME 8', control_source)
        self.assertIn('S_IRUSR | S_IWUSR', control_source)
        self.assertIn('CVI_ISP_QueryExposureInfo', control_source)
        self.assertIn('MANUAL_ISP_DGAIN_MAX 0x40000U', control_source)
        self.assertIn('gain_mode = "auto";', control_source)
        self.assertIn('"OK applied"', control_source)
        self.assertNotIn('mmf_set_exp_mode(', control_source)
        self.assertEqual(
            vio_source.count("maixcam_isp_control_server_open("), 1
        )
        self.assertEqual(
            vio_source.count("maixcam_isp_control_server_poll("), 1
        )
        self.assertEqual(
            vio_source.count("maixcam_isp_control_server_close("), 1
        )
        self.assertIn('!strcmp(argv[1], "--ispctl")', main_source)


if __name__ == "__main__":
    unittest.main()

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "tools"))

import buildwatch  # noqa: E402


LOG_SAMPLE = (
    "\x1b[0;32mPreparing Image for maixcam-sc035hgs\x1b[0m\n"
    "I: success in 1009.1292 seconds\n"
    "\x1b[0;32mChecking out U-Boot for maixcam-sc035hgs\x1b[0m\n"
    "Cloning into '/build-cache/boards/maixcam-sc035hgs-sd/u-boot'...\n"
    "\x1b[0;32mBuilding U-Boot for maixcam-sc035hgs\x1b[0m\n"
    "  CC      lib/sbi/sbi_console.o\n"
    "Updating files:  45% (8210/17948)\rUpdating files:  46% (8257/17948)\n"
    "drivers/input/touchscreen/gt9xx/gt9xx.c:12:5: error: 'gt9xx' undeclared\n"
    "297 | #define GTP_ERROR(fmt,arg...)  printk(\"<<-GTP-ERROR->> \"fmt\"\\n\",##arg)\n"
    "make[1]: *** [Makefile:20: u-boot] Error 1\n"
    "\x1b[0;32mCompiling Image for maixcam-sc035hgs\x1b[0m\n"
    "Image for maixcam-sc035hgs is maixcam-sc035hgs_sd.img\n"
    "build_rc=0\n"
)


# TemporaryDirectory objects are kept alive: they delete their tree when
# collected, which would break tests that stat the log afterwards.
_KEEP_DIRS: list[tempfile.TemporaryDirectory] = []


PROGRESS_SAMPLE = (
    "\x1b[0;32mBuilding U-Boot for maixcam-sc035hgs\x1b[0m\n"
    "Updating files:  45% (8210/17948)\rUpdating files:  46% (8257/17948)\n"
)


def make_log(text: str = LOG_SAMPLE) -> tuple[buildwatch.BuildLog, Path]:
    directory = tempfile.TemporaryDirectory()
    _KEEP_DIRS.append(directory)
    path = Path(directory.name) / "build.log"
    path.write_text(text, encoding="utf-8")
    log = buildwatch.BuildLog(path)
    log.poll()
    return log, path


class BuildLogTests(unittest.TestCase):
    def test_stages_and_completion_are_parsed(self) -> None:
        log, _path = make_log()
        self.assertEqual(log.stage, "Compiling Image for maixcam-sc035hgs")
        self.assertEqual(log.board, "maixcam-sc035hgs")
        self.assertEqual(log.artifact, "maixcam-sc035hgs_sd.img")
        self.assertEqual(log.rc, 0)
        # Preparing Image, Checking out U-Boot, Building U-Boot, Compiling Image
        self.assertEqual(log.stage_count, 4)

    def test_progress_and_mmdebstrap_are_parsed(self) -> None:
        log, _path = make_log(PROGRESS_SAMPLE)
        self.assertEqual(log.progress, ("Updating files", 46))
        log, _path = make_log()
        self.assertEqual(log.mmdebstrap, "success in 1009.1292 seconds")

    def test_compile_progress_keeps_a_short_label(self) -> None:
        log, _path = make_log("  CC      lib/sbi/sbi_console.o  [ 52%]\n")
        self.assertEqual(log.progress, ("CC", 52))

    def test_source_identifiers_are_not_counted_as_errors(self) -> None:
        log, _path = make_log()
        # Only the compiler diagnostic and the make failure count.
        self.assertEqual(log.error_count, 2)

    def test_following_appends_only_new_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "build.log"
            path.write_text("Preparing Image for maixcam-sc035hgs\n", encoding="utf-8")
            log = buildwatch.BuildLog(path)
            log.poll()
            self.assertEqual(log.stage, "Preparing Image for maixcam-sc035hgs")
            path.write_text(
                "Preparing Image for maixcam-sc035hgs\n"
                "Building FSBL for maixcam-sc035hgs\n",
                encoding="utf-8",
            )
            log.poll()
            self.assertEqual(log.stage, "Building FSBL for maixcam-sc035hgs")
            self.assertEqual(log.stage_count, 2)

    def test_truncated_log_is_reread(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "build.log"
            path.write_text(
                "Building FSBL for maixcam-sc035hgs\nBuilding OpenSBI for maixcam-sc035hgs\n",
                encoding="utf-8",
            )
            log = buildwatch.BuildLog(path)
            log.poll()
            path.write_text("Building FSBL for maixcam-sc035hgs\n", encoding="utf-8")
            log.poll()
            self.assertEqual(log.stage, "Building FSBL for maixcam-sc035hgs")


class CommandParsingTests(unittest.TestCase):
    def test_host_wrapper_command(self) -> None:
        info = buildwatch._parse_build_cmd(
            "bash -c cd /work/sophgo-sg200x-debian && ./scripts/ci/local-build.sh image "
            "--board maixcam-sc035hgs --storage sd > /tmp/build.log 2>&1; "
            'echo "build_rc=$?" >> /tmp/build.log'
        )
        self.assertEqual(info["target"], "image")
        self.assertEqual(info["board"], "maixcam-sc035hgs")
        self.assertEqual(info["storage"], "sd")
        self.assertEqual(info["log"], "/tmp/build.log")
        self.assertEqual(info["repo"], "/work/sophgo-sg200x-debian")

    def test_docker_run_command(self) -> None:
        info = buildwatch._parse_build_cmd(
            "docker run --rm --privileged -v /work/sophgo-sg200x-debian:/workspace:ro "
            "-w /workspace --entrypoint /bin/bash sg2002-toolchain:1 "
            "/workspace/scripts/ci/local-build.sh middleware --inside "
            "--board maixcam-sc035hgs --storage sd --output /output"
        )
        self.assertEqual(info["target"], "middleware")
        self.assertEqual(info["board"], "maixcam-sc035hgs")
        self.assertEqual(info["repo"], "/work/sophgo-sg200x-debian")
        self.assertIsNone(info["log"])


class InnerProcessTests(unittest.TestCase):
    def test_most_specific_process_wins(self) -> None:
        commands = [
            "make -j12 BOARD=cv181x maixcam-sc035hgs_defconfig",
            "gcc -I include -c lib/sbi/sbi_console.c",
            "cc1 -I include -o /tmp/sbi_console.o",
        ]
        self.assertTrue(buildwatch.pick_inner(commands).startswith("cc1"))

    def test_make_is_the_fallback(self) -> None:
        self.assertTrue(buildwatch.pick_inner(["make -j12 image"]).startswith("make"))

    def test_uninteresting_processes_are_ignored(self) -> None:
        self.assertIsNone(buildwatch.pick_inner(["sleep 300", "tail -f /dev/null"]))


DOCKER_TOP_SAMPLE = (
    "UID                 PID                 PPID                C                   "
    "STIME               TTY                 TIME                CMD\n"
    "root                1234                1200                 54                  "
    "21:42               ?                   00:00:01            gzip -c /dev/urandom\n"
    "root                1200                1                    0                   "
    "21:42               ?                   00:00:00            sh -c sleep 300\n"
)


class DockerTopTests(unittest.TestCase):
    def test_commands_are_extracted(self) -> None:
        self.assertEqual(
            buildwatch.parse_top(DOCKER_TOP_SAMPLE),
            ["gzip -c /dev/urandom", "sh -c sleep 300"],
        )

    def test_empty_output(self) -> None:
        self.assertEqual(buildwatch.parse_top(""), [])
        self.assertEqual(buildwatch.parse_top("UID PID CMD\n"), [])


class RenderTests(unittest.TestCase):
    def test_shorten_clips_long_lines(self) -> None:
        self.assertEqual(buildwatch._shorten("  a   b  "), "a b")
        self.assertEqual(buildwatch._shorten("x" * 200, 10), "x" * 9 + "…")

    def test_text_render_mentions_stage_and_result(self) -> None:
        builds = [buildwatch.Build(log=buildwatch.BuildLog("/tmp/does-not-exist.log"))]
        poll = buildwatch.snapshot(builds, use_docker=False)
        text = buildwatch.render_text(poll)
        self.assertIn("GONE", text)
        self.assertIn("does-not-exist.log", text)

    def test_state_detection(self) -> None:
        log, _path = make_log()
        build = buildwatch.Build(log=log)
        self.assertEqual(buildwatch.build_state(build), "ok")
        log.rc = 1
        self.assertEqual(buildwatch.build_state(build), "failed")

    def test_finished_build_reports_age(self) -> None:
        log, path = make_log()
        build = buildwatch.Build(log=log)
        row = buildwatch.snapshot([build], use_docker=False)[0]
        self.assertEqual(row["state"], "ok")
        self.assertEqual(row["board"], "maixcam-sc035hgs")
        self.assertGreaterEqual(row["ended_s"], 0.0)
        self.assertIsNone(row["pid"])


if __name__ == "__main__":
    unittest.main()

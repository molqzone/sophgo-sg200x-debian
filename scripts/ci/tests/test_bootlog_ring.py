"""The boot log ring: crash-safe by shape, not by hoping for a clean shutdown.

The daemon writes to the FAT partition U-Boot reads the kernel and DTB from, so the
tests here are about *shape*: whole records written in place (`pwrite`), the file's
size and cluster chain never touched, and a torn record that is skipped rather than
guessed at.  The last test runs the real addon, because "the image preallocates the
ring" is the half of the contract the daemon cannot enforce by itself.
"""

from __future__ import annotations

import importlib.machinery
import importlib.util
import io
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "scripts/ci"))

import validate  # noqa: E402
from plan import PlanError  # noqa: E402

DAEMON = REPO_ROOT / validate.BOOTLOG_DAEMON
ADDON = REPO_ROOT / validate.BOOTLOG_ADDON
SERVICE = REPO_ROOT / validate.BOOTLOG_SERVICE
SCRIPTS = REPO_ROOT / "scripts"


def load_daemon():
    """The daemon is installed without a .py suffix; import it by path."""
    loader = importlib.machinery.SourceFileLoader("bootlogd", str(DAEMON))
    spec = importlib.util.spec_from_loader("bootlogd", loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


bootlogd = load_daemon()


class RecordTests(unittest.TestCase):
    def test_a_record_round_trips(self) -> None:
        payload = "hello\nworld\n".encode()
        blob = bootlogd.pack_record(7, payload)
        self.assertEqual(len(blob), bootlogd.RECORD)
        self.assertEqual(bootlogd.unpack_record(blob), (7, payload))

    def test_an_empty_payload_round_trips(self) -> None:
        self.assertEqual(bootlogd.unpack_record(bootlogd.pack_record(1, b"")), (1, b""))

    def test_an_oversized_payload_is_cut_to_one_record(self) -> None:
        blob = bootlogd.pack_record(1, b"x" * (bootlogd.PAYLOAD + 10))
        self.assertEqual(len(blob), bootlogd.RECORD)
        self.assertEqual(len(bootlogd.unpack_record(blob)[1]), bootlogd.PAYLOAD)

    def test_a_torn_record_is_not_returned(self) -> None:
        blob = bytearray(bootlogd.pack_record(3, b"payload"))
        blob[bootlogd.HEADER.size + 1] ^= 0xFF
        self.assertIsNone(bootlogd.unpack_record(bytes(blob)))

    def test_foreign_data_is_not_returned(self) -> None:
        self.assertIsNone(bootlogd.unpack_record(bytes(bootlogd.RECORD)))
        self.assertIsNone(bootlogd.unpack_record(b"short"))


class RingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.path = str(Path(self.directory.name) / "bootlog.bin")
        self.slots = 4
        Path(self.path).write_bytes(
            bytes(bootlogd.RECORD * self.slots)
        )

    def tearDown(self) -> None:
        self.directory.cleanup()

    def write(self, count: int) -> None:
        index = 0
        for number in range(count):
            index = bootlogd.append(self.path, b"payload %d" % number, index, self.slots)

    def test_the_ring_holds_one_record_per_slot(self) -> None:
        self.write(6)
        records = bootlogd.complete_records(self.path)
        self.assertEqual(sorted(index for index, _ in records), [2, 3, 4, 5])

    def test_the_index_survives_a_restart(self) -> None:
        self.write(3)
        self.assertEqual(bootlogd.resume_index(self.path), 3)
        index = bootlogd.append(self.path, b"after restart", 3, self.slots)
        self.assertEqual(index, 4)
        self.assertEqual(bootlogd.complete_records(self.path)[3][1], b"after restart")

    def test_writing_never_changes_the_file_size(self) -> None:
        size_before = Path(self.path).stat().st_size
        self.write(9)
        self.assertEqual(Path(self.path).stat().st_size, size_before)

    def test_a_torn_slot_does_not_hide_its_neighbours(self) -> None:
        self.write(4)
        blob = bytearray(Path(self.path).read_bytes())
        blob[bootlogd.HEADER.size + 2] ^= 0xFF  # first slot's payload
        Path(self.path).write_bytes(bytes(blob))
        self.assertEqual(len(bootlogd.complete_records(self.path)), 3)

    def test_a_missing_ring_is_refused_instead_of_created(self) -> None:
        Path(self.path).unlink()
        with self.assertRaises(SystemExit) as caught:
            bootlogd.slots_in(self.path)
        self.assertIn("preallocate", str(caught.exception))
        self.assertFalse(Path(self.path).exists())

    def test_a_ring_that_is_not_whole_records_is_refused(self) -> None:
        Path(self.path).write_bytes(bytes(bootlogd.RECORD + 1))
        with self.assertRaises(SystemExit) as caught:
            bootlogd.slots_in(self.path)
        self.assertIn("records", str(caught.exception))


class ShowTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.path = str(Path(self.directory.name) / "bootlog.bin")
        Path(self.path).write_bytes(bytes(bootlogd.RECORD * 8))
        index = 0
        for boot in ("aaaaaaaa", "bbbbbbbb"):
            for snap in range(3):
                payload = "boot_id=%s snapshot=%d\n" % (boot, snap)
                index = bootlogd.append(self.path, payload.encode(), index, 8)

    def tearDown(self) -> None:
        self.directory.cleanup()

    def test_show_prints_records_oldest_first(self) -> None:
        output = io.StringIO()
        with redirect_stdout(output):
            self.assertEqual(bootlogd.show(self.path), 0)
        text = output.getvalue()
        self.assertLess(text.index("snapshot=0"), text.index("snapshot=2"))
        self.assertLess(text.index("aaaaaaaa"), text.index("bbbbbbbb"))
        self.assertIn("=== 6 record(s) ===", text)

    def test_show_works_as_a_command(self) -> None:
        done = subprocess.run(
            [sys.executable, str(DAEMON), "--show", self.path],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("snapshot=2", done.stdout)

    def test_show_reports_a_missing_ring(self) -> None:
        done = subprocess.run(
            [sys.executable, str(DAEMON), "--show", self.path + ".missing"],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(done.returncode, 2)
        self.assertIn("missing", done.stderr)


class FitTests(unittest.TestCase):
    def test_a_snapshot_that_fits_is_untouched(self) -> None:
        self.assertEqual(bootlogd.fit(b"short"), b"short")

    def test_a_snapshot_that_does_not_fit_says_so(self) -> None:
        payload = bootlogd.fit(b"x" * (bootlogd.PAYLOAD + 100))
        self.assertEqual(len(payload), bootlogd.PAYLOAD)
        self.assertIn(b"[snapshot truncated", payload)


class SnapshotTests(unittest.TestCase):
    def test_a_snapshot_names_the_sections_a_post_mortem_needs(self) -> None:
        # 冻结的核、消失的网络、坏掉的卡：这三类问题必须在快照里有名字，否则拔出卡来只能看到
        # 一堆 uptime。卡片错误单独一节，因为它才是“卡坏了”的证据，写好者造成的 FAT 不一致
        # 不会出现在那里。
        text = bootlogd.snapshot()
        for section in ("--- dmesg tail:", "--- net:", "--- mmc:",
                        "--- c906 console ring:", "boot_id="):
            self.assertIn(section, text)
        # 装不下时被砍掉的是 dmesg，不是小核的 console 环：后者必须在 dmesg 之前。
        self.assertLess(text.index("--- c906 console ring:"),
                        text.index("--- dmesg tail:"))


class ContractTests(unittest.TestCase):
    def test_the_tree_satisfies_the_ring_contract(self) -> None:
        validate.validate_bootlog_ring()

    def test_the_daemon_may_not_allocate_on_the_boot_partition(self) -> None:
        daemon = DAEMON.read_text(encoding="utf-8")
        for edit in (
            daemon.replace("os.open(path, os.O_RDWR)", 'open(path, "ab")'),
            daemon.replace("os.fsync(fd)", "os.ftruncate(fd, 0)"),
            daemon.replace("os.pwrite(fd,", "os.truncate(path,"),
        ):
            with self.subTest(edit=edit[:40]):
                with self.assertRaises(PlanError):
                    validate.check_bootlog_ring(
                        edit,
                        ADDON.read_text(encoding="utf-8"),
                        SERVICE.read_text(encoding="utf-8"),
                    )

    def test_the_image_must_preallocate_the_ring(self) -> None:
        addon = ADDON.read_text(encoding="utf-8")
        with self.assertRaises(PlanError) as caught:
            validate.check_bootlog_ring(
                DAEMON.read_text(encoding="utf-8"),
                addon.replace("dd if=/dev/zero", "touch"),
                SERVICE.read_text(encoding="utf-8"),
            )
        self.assertIn("preallocate", str(caught.exception))

    def test_the_record_size_is_one_number(self) -> None:
        addon = ADDON.read_text(encoding="utf-8")
        with self.assertRaises(PlanError) as caught:
            validate.check_bootlog_ring(
                DAEMON.read_text(encoding="utf-8"),
                addon.replace("BOOTLOG_RECORD := 8192", "BOOTLOG_RECORD := 4096"),
                SERVICE.read_text(encoding="utf-8"),
            )
        self.assertIn("record size", str(caught.exception))

    def test_the_service_must_restart_the_logger(self) -> None:
        with self.assertRaises(PlanError) as caught:
            validate.check_bootlog_ring(
                DAEMON.read_text(encoding="utf-8"),
                ADDON.read_text(encoding="utf-8"),
                SERVICE.read_text(encoding="utf-8").replace("Restart=always", ""),
            )
        self.assertIn("Restart", str(caught.exception))


class AddonTests(unittest.TestCase):
    """The preallocation half: run the real addon into a temp rootfs."""

    def setUp(self) -> None:
        if shutil.which("make") is None:
            self.skipTest("make is not available")

    def test_the_addon_preallocates_a_whole_number_of_records(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / "build"
            rootfs = root / "rootfs"
            build.mkdir()
            makefile = root / "addon.mk"
            makefile.write_text(
                f"BUILDDIR := {build}\n"
                f"ROOTFS := {rootfs}\n"
                f"COLOUR_GREEN :=\nEND_COLOUR :=\n"
                f"include {ADDON}\n",
                encoding="utf-8",
            )
            subprocess.run(
                ["make", "--no-print-directory", "-f", str(makefile),
                 str(build / "bootlog-stamp")],
                cwd=SCRIPTS, check=True, capture_output=True, text=True,
            )
            ring = rootfs / "boot/bootlog.bin"
            self.assertTrue(ring.is_file())
            self.assertEqual(ring.stat().st_size % bootlogd.RECORD, 0)
            self.assertGreater(ring.stat().st_size, 0)
            self.assertEqual(ring.read_bytes(), bytes(ring.stat().st_size))
            self.assertTrue((rootfs / "usr/local/sbin/bootlogd").is_file())


if __name__ == "__main__":
    unittest.main()

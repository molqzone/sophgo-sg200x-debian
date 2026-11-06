"""`cop` against synthetic pages: the page tool is tested without a board.

It had no tests at all.  Every verb except `list` (which reads the device tree) can run
against a page file via `--page`, so the tests build the exact bytes the firmware
writes -- header, ring, name service, command region -- and then ask the CLI what it
sees.  That covers the two things that actually went wrong in this project: a tool that
read the wrong offset (telemetry head frozen while the ring advanced) and a tool that
offered to "fix" a page its owner had not built.
"""

from __future__ import annotations

import importlib.machinery
import importlib.util
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
COMPONENT = REPO_ROOT / "components/guidance-fw"
COP = COMPONENT / "tools/cop"
CONTRACT = COMPONENT / "firmware/guidance_contract.json"

# These come from the contract, but the tests want to state them independently: a test
# that reads the offsets from the same file it is checking proves nothing.
PAGE = 4096
MAGIC = 0x31506461
ABI = 1
SLOTS = 58
SLOT_BYTES = 64
PAYLOAD = 24
REGION = 4064
TAG = 6
TABLE = 3840
TABLE_MAGIC = 0x3130534E
ENTRY = 42
NAME = 28


def load_cop():
    loader = importlib.machinery.SourceFileLoader("cop", str(COP))
    spec = importlib.util.spec_from_loader("cop", loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


cop = load_cop()


def formatted_page(channels=(("imu", 1024, 1),), head=42) -> bytes:
    """Exactly what a core that builds the page leaves behind."""
    page = bytearray(PAGE)
    struct.pack_into("<8I", page, 0, MAGIC, ABI, PAGE, SLOTS, SLOT_BYTES, PAYLOAD,
                     REGION, TAG)
    struct.pack_into("<I", page, 3776, head)
    struct.pack_into("<I", page, 3780, 0)
    struct.pack_into("<4I", page, TABLE, TABLE_MAGIC, len(channels), 3, 0)
    for index, (name, endpoint, flags) in enumerate(channels):
        raw = name.encode()
        struct.pack_into("<IIIH%ds" % NAME, page, TABLE + 16 + index * ENTRY,
                         0x1234 + index, endpoint, flags, len(raw), raw)
    struct.pack_into("<I", page, REGION, 0x31444D47)  # "GMD1"
    struct.pack_into("<I", page, REGION + 4, 1)
    struct.pack_into("<I", page, REGION + 8, 7)
    return bytes(page)


class PageFixture(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.path = Path(self.directory.name) / "page.bin"

    def tearDown(self) -> None:
        self.directory.cleanup()

    def write(self, blob: bytes) -> None:
        self.path.write_bytes(blob)

    def run_cop(self, *args: str, timeout: float = 20) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, str(COP), "--page", str(self.path), *args],
            capture_output=True, text=True, check=False, timeout=timeout,
        )


class HeaderTests(unittest.TestCase):
    def test_a_cold_page_is_unformatted(self) -> None:
        header = cop.parse_header(cop.open_file_page.__globals__["Page"].__new__(object)
                                  if False else _BytesPage(bytes(PAGE)))
        self.assertEqual(cop.state_name(header), "UNFORMATTED")

    def test_a_foreign_page_is_reported_as_foreign(self) -> None:
        page = bytearray(PAGE)
        struct.pack_into("<I", page, 0, 0xDEADBEEF)
        self.assertEqual(cop.state_name(cop.parse_header(_BytesPage(bytes(page)))),
                         "FOREIGN")

    def test_a_formatted_page_carries_its_geometry(self) -> None:
        header = cop.parse_header(_BytesPage(formatted_page()))
        self.assertEqual(cop.state_name(header), "FORMATTED")
        self.assertEqual(header["abi"], ABI)
        self.assertEqual(header["tag"], TAG)
        self.assertEqual(header["slot_count"], SLOTS)

    def test_the_derived_offsets_match_the_layout(self) -> None:
        # 3776/3780/3840 是固件写的地方；工具自己推导出来的必须是同一组数。
        geometry = cop.geometry(cop.parse_header(_BytesPage(formatted_page())))
        self.assertEqual(geometry, {"telemetry": 64, "head": 3776,
                                    "write_state": 3780, "channels": 3840})


class ChannelTests(unittest.TestCase):
    def test_the_name_service_is_decoded(self) -> None:
        page = _BytesPage(formatted_page((("imu", 1024, 1), ("gyro", 1025, 2))))
        offset, count, generation, entries = cop.channels(
            page, cop.parse_header(page))
        self.assertEqual((offset, count, generation), (3840, 2, 3))
        self.assertEqual([entry[0] for entry in entries], ["imu", "gyro"])
        self.assertEqual(entries[1][1], 1025)

    def test_a_page_without_a_table_reports_none(self) -> None:
        page = _BytesPage(formatted_page(channels=()))
        offset, count, generation, entries = cop.channels(page, cop.parse_header(page))
        self.assertEqual(count, 0)
        self.assertEqual(entries, [])


class CliTests(PageFixture):
    def test_status_reports_the_page_it_sees(self) -> None:
        self.write(formatted_page())
        done = self.run_cop("status")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("FORMATTED", done.stdout)
        self.assertIn("slots=58", done.stdout)
        self.assertIn("channels  : 1 announced", done.stdout)
        self.assertIn("imu", done.stdout)

    def test_status_on_a_cold_page_says_so_and_does_not_guess(self) -> None:
        self.write(bytes(PAGE))
        done = self.run_cop("status")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("UNFORMATTED", done.stdout)

    def test_dump_shows_bytes_at_an_offset(self) -> None:
        self.write(formatted_page())
        done = self.run_cop("dump", "--offset", "0", "--bytes", "16")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("61 64 50 31", done.stdout)  # little-endian page magic

    def test_send_needs_a_contract_and_a_page(self) -> None:
        self.write(formatted_page())
        done = self.run_cop("send", "--type", "1", "--payload", "")
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("--contract", done.stderr)

    def test_send_refuses_a_page_without_an_owner(self) -> None:
        self.write(bytes(PAGE))
        done = self.run_cop("send", "--contract", str(CONTRACT), "--type", "1",
                            "--payload", "")
        self.assertEqual(done.returncode, 1)
        self.assertIn("UNFORMATTED", done.stderr)

    def test_watch_refuses_a_page_without_an_owner(self) -> None:
        self.write(bytes(PAGE))
        done = self.run_cop("watch", timeout=5)
        self.assertEqual(done.returncode, 1)
        self.assertIn("UNFORMATTED", done.stderr)

    def test_there_is_no_format_verb(self) -> None:
        # 页的所有权在写它的人手里：工具不该提供"从这边覆盖"的动词。
        self.write(bytes(PAGE))
        done = self.run_cop("format")
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("invalid choice", done.stderr)


class ContractAgreementTests(unittest.TestCase):
    """The contract and the tool must describe the same page."""

    def setUp(self) -> None:
        self.contract = json.loads(CONTRACT.read_text(encoding="utf-8"))

    def test_the_page_constants_agree(self) -> None:
        page = self.contract["page"]
        self.assertEqual(cop.PAGE_MAGIC, page["magic"])
        self.assertEqual(cop.HEADER_BYTES, page["header_bytes"])
        self.assertEqual(self.contract["channels"]["magic"], cop.CHANNEL_TABLE_MAGIC)
        self.assertEqual(self.contract["channels"]["entry_bytes"],
                         cop.CHANNEL_ENTRY_BYTES)
        self.assertEqual(self.contract["channels"]["name_bytes"],
                         cop.CHANNEL_NAME_BYTES)

    def test_the_declared_offsets_are_what_the_tool_derives(self) -> None:
        page = self.contract["page"]
        telemetry = self.contract["telemetry"]
        header = {"magic": page["magic"], "abi": page["abi_version"],
                  "page_size": page["bytes"], "slot_count": telemetry["slot_count"],
                  "slot_bytes": telemetry["slot_bytes"],
                  "payload_bytes": self.contract["frame"]["payload_bytes"],
                  "region_offset": self.contract["region"]["offset"],
                  "tag": page["wire_tag"]}
        geometry = cop.geometry(header)
        self.assertEqual(geometry["telemetry"], telemetry["offset"])
        self.assertEqual(geometry["head"], telemetry["head_offset"])
        self.assertEqual(geometry["write_state"], telemetry["write_state_offset"])
        self.assertEqual(geometry["channels"], self.contract["channels"]["offset"])

    def test_the_region_and_the_table_fit_inside_the_page(self) -> None:
        page = self.contract["page"]["bytes"]
        channels = self.contract["channels"]
        region = self.contract["region"]
        self.assertLessEqual(channels["offset"] + channels["bytes"], page)
        self.assertEqual(region["offset"] + region["payload_bytes"],
                         region["write_state_offset"])
        self.assertEqual(region["seq_offset"] + 4, page)
        self.assertEqual(region["seq_offset"], region["write_state_offset"] + 4)


class _BytesPage:
    """The read/u32 slice of cop.Page, over an in-memory page."""

    def __init__(self, blob: bytes):
        self.blob = blob

    def read(self, offset: int, length: int) -> bytes:
        return self.blob[offset:offset + length]

    def u32(self, offset: int) -> int:
        return struct.unpack("<I", self.read(offset, 4))[0]


if __name__ == "__main__":
    unittest.main()

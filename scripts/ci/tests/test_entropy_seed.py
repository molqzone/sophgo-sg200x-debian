"""`seed-entropy` must build the kernel's `rand_pool_info` in the kernel's order.

The script credits the image's entropy seed through `RNDADDENTROPY`, and the payload is a
`struct rand_pool_info`:

    struct rand_pool_info { int entropy_count; int buf_size; __u32 buf[]; };

The two ints were once swapped.  Nothing failed loudly: the ioctl still returned, the unit
still reported `Finished`, and the only symptom was a boot that worked when the kernel's own
jitter entropy arrived before the NCM gadget and hung when it did not -- a random-looking
fake brick that took a day to attribute.  So the field order is pinned here, as data.
"""

from __future__ import annotations

import importlib.machinery
import importlib.util
import struct
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
SCRIPT = REPO / "scripts/addons/entropy/seed-entropy"

# Stated independently of the script: the kernel's ABI, not this repository's choice.
STRUCT = struct.Struct("<ii")
RNDADDENTROPY = 0x40085203


def load_seed_entropy():
    loader = importlib.machinery.SourceFileLoader("seed_entropy", str(SCRIPT))
    spec = importlib.util.spec_from_loader("seed_entropy", loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


seed_entropy = load_seed_entropy()


class PoolInfoTests(unittest.TestCase):
    def test_entropy_count_comes_first(self) -> None:
        data = bytes(range(256))
        payload = seed_entropy.pool_info(data)
        entropy_count, buf_size = STRUCT.unpack_from(payload, 0)
        self.assertEqual(buf_size, len(data))
        self.assertEqual(entropy_count, 8 * len(data))

    def test_the_seed_follows_the_header(self) -> None:
        data = b"\xa5" * 128
        payload = seed_entropy.pool_info(data)
        self.assertEqual(len(payload), STRUCT.size + len(data))
        self.assertEqual(payload[STRUCT.size:], data)

    def test_a_short_seed_still_reports_its_real_length(self) -> None:
        # The header describes the buffer actually supplied; a swapped field reads past it.
        data = b"\x5a" * 64
        payload = seed_entropy.pool_info(data)
        _, buf_size = STRUCT.unpack_from(payload, 0)
        self.assertEqual(STRUCT.size + buf_size, len(payload))

    def test_the_ioctl_number_is_rndaddentropy(self) -> None:
        # _IOW('R', 0x03, int[2]) -- the value this script has always used.
        self.assertEqual(seed_entropy.RNDADDENTROPY, RNDADDENTROPY)

    def test_it_always_credits_even_with_a_hwrng_node(self) -> None:
        # "the node exists but yields nothing" is the failure mode worth avoiding; the
        # credit costs tens of microseconds, so there is no shortcut.
        source = SCRIPT.read_text(encoding="utf-8")
        body = source.split("def main", 1)[1]
        self.assertNotIn('os.path.exists("/dev/hwrng")', body)


if __name__ == "__main__":
    unittest.main()

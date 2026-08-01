#!/usr/bin/env python3
"""Compile-time independent checks for the public SG2002 shared-memory ABI."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "include" / "sg2002_rtos_shm.h"


def macro(text: str, name: str) -> int:
    match = re.search(rf"^#define\s+{re.escape(name)}\s+(0x[0-9a-fA-F]+|[0-9]+)U?\s*$", text, re.MULTILINE)
    if not match:
        raise RuntimeError(f"missing integer macro {name}")
    return int(match.group(1), 0)


def main() -> int:
    text = HEADER.read_text(encoding="utf-8")
    expected = {
        "SG2002_RTOS_SHM_ABI_VERSION": 2,
        "SG2002_RTOS_SHM_SLOT_SIZE": 1024,
        "SG2002_RTOS_SHM_SLOT_COUNT": 64,
        "SG2002_RTOS_SHM_SLOT_HEADER_SIZE": 6,
        "SG2002_RTOS_SHM_CONTROL_SIZE": 0x1000,
    }
    for name, value in expected.items():
        actual = macro(text, name)
        if actual != value:
            print(f"{name}: expected {value}, got {actual}", file=sys.stderr)
            return 1
    region_size = 0x1000 + 2 * 1024 * 64
    if region_size != 0x21000:
        print(f"unexpected shared-memory region size: {region_size:#x}", file=sys.stderr)
        return 1
    print("abi-layout=PASS version=2 slot=1024 count=64 payload=1018 region=0x21000")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

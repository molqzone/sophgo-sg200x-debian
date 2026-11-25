#!/usr/bin/env python3
"""Emit the firmware projection of the image's board memory map.

The image owns DRAM, the FreeRTOS region and the cross-core pages.  The
firmware build consumes this small, versioned JSON instead of importing the
Python memmap: the image may change ION/U-Boot/bootlogo layout without
touching the firmware, as long as the firmware-facing values stay the same.
"""

import argparse
import importlib.util
import json
from pathlib import Path


def load_memmap(path: Path):
    spec = importlib.util.spec_from_file_location("board_memmap", path)
    if spec is None or spec.loader is None:
        raise SystemExit(f"cannot load memmap: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.MemoryMap


def build_view(memmap) -> dict:
    shared = int(memmap.GUIDANCE_SHARED_PAGE_ADDR)
    page = int(memmap.GUIDANCE_PAGE_SIZE)
    console = int(memmap.GUIDANCE_CONSOLE_PAGE_ADDR)
    host_addr = int(memmap.RTOS_BOOT_TRACE_ADDR)
    host_size = int(memmap.RTOS_BOOT_TRACE_SIZE)
    ipc_addr = int(memmap.IPC_SHARED_ADDR)
    ipc_size = int(memmap.IPC_SHARED_SIZE)
    fw_addr = int(memmap.FREERTOS_ADDR)
    fw_size = int(memmap.FREERTOS_SIZE)

    assert console == shared - page
    assert host_addr + host_size == fw_addr + fw_size
    assert ipc_addr + ipc_size == host_addr

    return {
        "schema": 1,
        "board": "maixcam",
        "dram": {"base": int(memmap.DRAM_BASE), "size": int(memmap.DRAM_SIZE)},
        "freertos": {"addr": fw_addr, "size": fw_size},
        "pages": {
            "page_size": page,
            "shared": shared,
            "console": console,
            "host_sentinel": host_addr,
            "host_sentinel_size": host_size,
        },
        "ipc_shared": {"addr": ipc_addr, "size": ipc_size},
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    view = build_view(load_memmap(args.board_dir / "memmap.py"))
    text = json.dumps(view, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
        print(f"wrote {args.output}")
    else:
        print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

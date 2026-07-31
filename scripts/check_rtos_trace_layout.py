#!/usr/bin/env python3
import argparse
import json
import re
import subprocess
import sys


def run_readelf(readelf, option, elf):
    result = subprocess.run(
        [readelf, option, str(elf)],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    )
    return result.stdout


def main():
    parser = argparse.ArgumentParser(
        description="Verify the C906L firmware, shared-memory, and boot-trace layout."
    )
    parser.add_argument("elf")
    parser.add_argument("--readelf", default="readelf")
    parser.add_argument("--trace-start", type=lambda value: int(value, 0), default=0x8FFFF000)
    parser.add_argument("--shm-start", type=lambda value: int(value, 0), default=0x8FFEE000)
    args = parser.parse_args()

    program_headers = run_readelf(args.readelf, "-lW", args.elf)
    load_end = 0
    for line in program_headers.splitlines():
        fields = line.split()
        if fields and fields[0] == "LOAD" and len(fields) >= 6:
            physical_address = int(fields[3], 0)
            memory_size = int(fields[5], 0)
            load_end = max(load_end, physical_address + memory_size)

    symbols = run_readelf(args.readelf, "-sW", args.elf)
    trace_symbol = None
    for line in symbols.splitlines():
        if re.search(r"\b__boot_trace_start$", line):
            fields = line.split()
            trace_symbol = int(fields[1], 16)
            break

    header = run_readelf(args.readelf, "-h", args.elf)
    entry_match = re.search(r"Entry point address:\s*(0x[0-9a-fA-F]+)", header)
    entry = int(entry_match.group(1), 0) if entry_match else None

    errors = []
    if not load_end:
        errors.append("ELF has no LOAD segment")
    if load_end > args.shm_start:
        errors.append(
            f"LOAD end 0x{load_end:x} overlaps shared memory at 0x{args.shm_start:x}"
        )
    if trace_symbol != args.trace_start:
        actual = "missing" if trace_symbol is None else f"0x{trace_symbol:x}"
        errors.append(
            f"__boot_trace_start is {actual}, expected 0x{args.trace_start:x}"
        )
    if entry is None or not 0x8FE00000 <= entry < args.shm_start:
        actual = "missing" if entry is None else f"0x{entry:x}"
        errors.append(f"entry point {actual} is outside the C906L firmware window")

    result = {
        "elf": str(args.elf),
        "entry": None if entry is None else f"0x{entry:x}",
        "load_end": f"0x{load_end:x}",
        "shm_start": f"0x{args.shm_start:x}",
        "trace_start": f"0x{args.trace_start:x}",
        "trace_symbol": None if trace_symbol is None else f"0x{trace_symbol:x}",
        "status": "fail" if errors else "pass",
    }
    print(json.dumps(result, sort_keys=True))
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

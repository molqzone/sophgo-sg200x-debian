#!/usr/bin/env python3
"""Validate repository layering and, when present, one board build output."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import zipfile
from pathlib import Path

from plan import (
    PlanError,
    assignment_words,
    effective_assignments,
    resolve_directory,
    resolve_file,
    validate,
)


REPO_ROOT = Path(__file__).resolve().parents[2]
REQUIRED_BOARD_FILES = ("linux/defconfig", "memmap.py", "u-boot/defconfig", "u-boot/cvitek.h", "u-boot/cvi_board_init.c")
UBOOT_TARGET = re.compile(r"^CONFIG_TARGET_[A-Z0-9_]+=y$", re.MULTILINE)
FALLBACK_EXCLUDED_DIRS = {
    ".cache",
    ".git",
    ".mypy_cache",
    ".pytest_cache",
    ".venv",
    "__pycache__",
    "build",
    "dist",
    "image",
    "node_modules",
    "out",
    "output",
}


def fail(message: str) -> None:
    raise PlanError(message)


def maintained_files() -> list[Path]:
    try:
        result = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "ls-files", "-z"],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except (OSError, subprocess.CalledProcessError):
        print(
            "validate.py: Git metadata unavailable; scanning maintained source files",
            file=sys.stderr,
        )
        return sorted(
            path
            for path in REPO_ROOT.rglob("*")
            if path.is_file()
            and not any(
                part in FALLBACK_EXCLUDED_DIRS
                for part in path.relative_to(REPO_ROOT).parts
            )
        )
    return [REPO_ROOT / relative for relative in result.stdout.split("\0") if relative]


def tracked_occurrences(values: set[str]) -> dict[str, list[str]]:
    needles = {value: value.encode("utf-8") for value in values}
    locations = {value: [] for value in values}
    for path in maintained_files():
        try:
            content = path.read_bytes()
        except OSError as exc:
            fail(f"cannot scan {path}: {exc}")
        relative = path.relative_to(REPO_ROOT).as_posix()
        for value, needle in needles.items():
            if needle in content:
                locations[value].append(relative)
    return {value: sorted(paths) for value, paths in locations.items()}


def validate_pins() -> None:
    pin_files = (
        REPO_ROOT / "versions.env",
        REPO_ROOT / "toolchain.env",
        REPO_ROOT / "components/sg2002-ipc/versions.env",
    )
    expected: dict[str, str] = {}
    for pin_file in pin_files:
        relative = pin_file.relative_to(REPO_ROOT).as_posix()
        for raw_line in pin_file.read_text(encoding="utf-8").splitlines():
            if "_COMMIT=" not in raw_line:
                continue
            value = raw_line.split("=", 1)[1].strip()
            if not value:
                fail(f"empty commit pin in {pin_file}")
            if value in expected and expected[value] != relative:
                fail(f"commit pin {value} is duplicated in {expected[value]} and {relative}")
            expected[value] = relative
    locations = tracked_occurrences(set(expected))
    for value, source in expected.items():
        if locations[value] != [source]:
            fail(f"pin {value} must occur only in {source}: {locations[value]}")


def validate_addons(config_root: Path, entries: list[dict[str, object]]) -> None:
    for entry in entries:
        board = str(entry["board"])
        settings = effective_assignments(config_root, board)
        additions = assignment_words(settings.get("IMAGE_ADDITIONS", ""))
        for addon in additions:
            addon_makefile = REPO_ROOT / "scripts/addons" / addon / "addon.mk"
            if not addon_makefile.is_file():
                fail(f"{board}: addon {addon!r} has no addon.mk")
            target = re.compile(rf"^\$\(BUILDDIR\)/{re.escape(addon)}-stamp\s*:", re.MULTILINE)
            if not target.search(addon_makefile.read_text(encoding="utf-8")):
                fail(f"{board}: addon {addon!r} has no matching stamp target")
        if board != "maixcam" and any(addon.startswith("maixcam-") for addon in additions):
            fail(f"{board}: inherits a MaixCAM-only addon")


def validate_uboot_defconfig(board: str, path: Path) -> None:
    targets = UBOOT_TARGET.findall(path.read_text(encoding="utf-8"))
    if len(targets) != 1:
        fail(f"{board}: U-Boot defconfig must enable exactly one target, got {targets}")


def validate_boards(config_root: Path, entries: list[dict[str, object]]) -> None:
    for entry in entries:
        board = str(entry["board"])
        storage = str(entry["storage"])
        settings = effective_assignments(config_root, board)
        resolve_directory(config_root, board, "dts")
        resolved = {relative: resolve_file(config_root, board, relative) for relative in REQUIRED_BOARD_FILES}
        validate_uboot_defconfig(board, resolved["u-boot/defconfig"])
        partition = settings.get("PARTITION_FILE", "").replace("$(STORAGE_TYPE)", storage).strip('"')
        if not partition:
            fail(f"{board}: PARTITION_FILE is not defined")
        resolve_file(config_root, board, partition)


def validate_workflows() -> None:
    workflow_dir = REPO_ROOT / ".github/workflows"
    expected = {"ci.yml", "images.yml", "toolchain.yml"}
    actual = {path.name for path in workflow_dir.glob("*.yml")}
    if actual != expected:
        fail(f"workflow set must be {sorted(expected)}, got {sorted(actual)}")
    board_if = re.compile(r"^\s*if:.*\b(maixcam|licheervnano|duo256|duos)\b", re.IGNORECASE | re.MULTILINE)
    for path in workflow_dir.glob("*.yml"):
        if board_if.search(path.read_text(encoding="utf-8")):
            fail(f"board-specific workflow conditional in {path}")


def validate_component() -> None:
    component = REPO_ROOT / "components/sg2002-ipc"
    required = ("Makefile", "versions.env", "include", "core", "firmware", "linux", "ports", "tools", "tests")
    for name in required:
        if not (component / name).exists():
            fail(f"component path is missing: {component / name}")
    if (REPO_ROOT / "scripts/addons/rtos-firmware").exists():
        fail("legacy scripts/addons/rtos-firmware still exists")
    subprocess.run([sys.executable, str(component / "tools/verify_layout.py")], check=True)


def validate_output(entries: list[dict[str, object]], board: str, storage: str, output: Path, allow_missing: bool) -> None:
    matches = [entry for entry in entries if entry["board"] == board and entry["storage"] == storage]
    if len(matches) != 1:
        fail(f"matrix has no unique entry for {board}/{storage}")
    artifact = output / f"{board}_{storage}.{matches[0]['format']}"
    if not artifact.is_file():
        if allow_missing:
            return
        fail(f"expected artifact is missing: {artifact}")
    if artifact.stat().st_size == 0:
        fail(f"artifact is empty: {artifact}")
    if artifact.suffix == ".zip":
        with zipfile.ZipFile(artifact) as archive:
            bad_member = archive.testzip()
            if bad_member:
                fail(f"corrupt zip member: {bad_member}")
    if "sg2002-ipc" in matches[0].get("components", []):
        for suffix in ("c906-mcu.elf", "c906-mcu.bin", "rtos-cmd", "rtos-bench", "libsg2002-rtos.a"):
            candidate = output / f"{board}_{suffix}"
            if not candidate.is_file() or candidate.stat().st_size == 0:
                fail(f"component artifact is missing: {candidate}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--board", default="maixcam")
    parser.add_argument("--storage", default="sd")
    parser.add_argument("--output", type=Path, default=REPO_ROOT / "output")
    parser.add_argument("--allow-missing-image", action="store_true")
    args = parser.parse_args()
    try:
        config_root = REPO_ROOT / "configs"
        entries = validate(config_root)
        validate_boards(config_root, entries)
        validate_addons(config_root, entries)
        validate_pins()
        validate_component()
        validate_workflows()
        validate_output(entries, args.board, args.storage, args.output, args.allow_missing_image)
    except (OSError, PlanError, subprocess.CalledProcessError, zipfile.BadZipFile) as exc:
        print(f"validate.py: {exc}", file=sys.stderr)
        return 2
    print(f"validate=PASS board={args.board} storage={args.storage}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

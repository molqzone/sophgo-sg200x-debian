#!/usr/bin/env python3
"""Resolve board inheritance and emit the build matrix used by local and CI jobs."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Iterable


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CONFIG_ROOT = REPO_ROOT / "configs"
ASSIGNMENT = re.compile(
    r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*([:+?]?=)\s*(.*?)\s*$"
)


class PlanError(RuntimeError):
    pass


def apply_assignments(path: Path, values: dict[str, str]) -> None:
    if not path.is_file():
        return
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.split("#", 1)[0].strip()
        if not line:
            continue
        match = ASSIGNMENT.match(line)
        if match:
            key, operator, raw_value = match.groups()
            value = raw_value.strip()
            if operator == "+=":
                values[key] = f"{values.get(key, '')} {value}".strip()
            elif operator != "?=" or key not in values:
                values[key] = value


def read_assignments(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    apply_assignments(path, values)
    return values


def effective_assignments(config_root: Path, board: str) -> dict[str, str]:
    values: dict[str, str] = {}
    apply_assignments(config_root / "settings.mk", values)
    for path in board_settings(config_root, board):
        apply_assignments(path, values)
    return values


def assignment_words(value: str) -> list[str]:
    return value.replace('"', "").split()


def board_chain(config_root: Path, board: str) -> list[str]:
    if not board or board == "common" or "/" in board or "\\" in board:
        raise PlanError(f"invalid board name: {board!r}")
    chain: list[str] = []
    current = board
    while current:
        if current in chain:
            raise PlanError("board inheritance cycle: " + " -> ".join(chain + [current]))
        board_dir = config_root / current
        if not board_dir.is_dir():
            raise PlanError(f"board configuration does not exist: {board_dir}")
        chain.append(current)
        current = read_assignments(board_dir / "board.mk").get("BASE_BOARD", "")
    chain.append("common")
    return chain


def application_layers(config_root: Path, board: str) -> list[str]:
    return list(reversed(board_chain(config_root, board)))


def resolve_file(config_root: Path, board: str, relative: str) -> Path:
    relative_path = Path(relative)
    if relative_path.is_absolute() or ".." in relative_path.parts:
        raise PlanError(f"relative path required: {relative!r}")
    for layer in board_chain(config_root, board):
        candidate = config_root / layer / relative_path
        if candidate.is_file():
            return candidate
    raise PlanError(f"{relative!r} is not defined for board {board!r}")


def resolve_directory(config_root: Path, board: str, relative: str) -> Path:
    relative_path = Path(relative)
    if relative_path.is_absolute() or ".." in relative_path.parts:
        raise PlanError(f"relative path required: {relative!r}")
    for layer in board_chain(config_root, board):
        candidate = config_root / layer / relative_path
        if candidate.is_dir():
            return candidate
    raise PlanError(f"directory {relative!r} is not defined for board {board!r}")


def patch_files(config_root: Path, board: str, component: str) -> list[Path]:
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", component):
        raise PlanError(f"invalid patch component: {component!r}")
    selected: dict[str, Path | None] = {}
    for layer in board_chain(config_root, board):
        patch_dir = config_root / layer / "patches" / component
        if not patch_dir.is_dir():
            continue
        for tombstone in sorted(patch_dir.glob("*.patch.skip")):
            selected.setdefault(tombstone.name.removesuffix(".skip"), None)
        for patch in sorted(patch_dir.glob("*.patch")):
            selected.setdefault(patch.name, patch)
    ordered: list[Path] = []
    for layer in application_layers(config_root, board):
        patch_dir = config_root / layer / "patches" / component
        ordered.extend(
            sorted(
                path
                for path in selected.values()
                if path is not None and path.parent == patch_dir
            )
        )
    return ordered


def board_settings(config_root: Path, board: str) -> list[Path]:
    layers = application_layers(config_root, board)
    return [config_root / layer / "settings.mk" for layer in layers if layer != "common"]


def load_matrix(config_root: Path) -> list[dict[str, object]]:
    path = config_root / "build-matrix.json"
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise PlanError(f"cannot read {path}: {exc}") from exc
    entries = document.get("include") if isinstance(document, dict) else None
    if not isinstance(entries, list):
        raise PlanError("build-matrix.json must contain an include array")
    return entries


def validate(config_root: Path) -> list[dict[str, object]]:
    entries = load_matrix(config_root)
    seen: set[tuple[str, str]] = set()
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            raise PlanError(f"matrix entry {index} is not an object")
        board = entry.get("board")
        storage = entry.get("storage")
        output_format = entry.get("format")
        if not all(isinstance(value, str) and value for value in (board, storage, output_format)):
            raise PlanError(f"matrix entry {index} lacks board/storage/format")
        if storage not in {"sd", "emmc"}:
            raise PlanError(f"matrix entry {index} has unsupported storage {storage!r}")
        if output_format not in {"img", "zip"}:
            raise PlanError(f"matrix entry {index} has unsupported format {output_format!r}")
        if storage == "emmc" and output_format != "zip":
            raise PlanError(f"eMMC entry {board!r} must use zip format")
        if storage == "sd" and output_format != "img":
            raise PlanError(f"SD entry {board!r} must use img format")
        key = (board, storage)
        if key in seen:
            raise PlanError(f"duplicate matrix entry: {board}/{storage}")
        seen.add(key)
        board_chain(config_root, board)
        components = entry.get("components", [])
        if not isinstance(components, list) or not all(
            isinstance(component, str) and component for component in components
        ):
            raise PlanError(f"matrix entry {index} has invalid components")
    return entries


def changed_files(base: str, head: str) -> list[str]:
    result = subprocess.run(
        ["git", "-C", str(REPO_ROOT), "diff", "--name-only", f"{base}...{head}"],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    )
    return [line for line in result.stdout.splitlines() if line]


def derived_boards(config_root: Path, board: str, entries: Iterable[dict[str, object]]) -> set[str]:
    result: set[str] = set()
    for entry in entries:
        candidate = str(entry["board"])
        if board in board_chain(config_root, candidate):
            result.add(candidate)
    return result


def affected_matrix(config_root: Path, base: str, head: str) -> list[dict[str, object]]:
    entries = validate(config_root)
    files = changed_files(base, head)
    if not files:
        return []
    global_prefixes = (
        ".github/workflows/",
        "configs/common/",
    )
    global_files = {
        "scripts/ci/build-board.sh",
        "scripts/ci/cache.py",
        "scripts/ci/local-build.sh",
        "scripts/ci/package-board.sh",
        "scripts/ci/plan.py",
        "scripts/ci/prepare-ccache-toolchains.sh",
        "scripts/ci/toolchain-ref.sh",
        "scripts/Makefile",
        "scripts/Dockerfile",
        "scripts/setup_rootfs.sh",
        "configs/settings.mk",
        "configs/build-matrix.json",
        "versions.env",
        "toolchain.env",
        "Makefile",
    }
    if any(path in global_files or path.startswith(global_prefixes) for path in files):
        return entries
    affected: set[str] = set()
    for path in files:
        match = re.match(r"configs/([^/]+)/", path)
        if match:
            affected.update(derived_boards(config_root, match.group(1), entries))
        if path.startswith("components/"):
            component = path.split("/", 2)[1]
            for entry in entries:
                if component in entry.get("components", []):
                    affected.add(str(entry["board"]))
        if path.startswith("scripts/addons/"):
            parts = path.split("/", 3)
            if len(parts) >= 3:
                addon = parts[2]
                for entry in entries:
                    candidate = str(entry["board"])
                    additions = assignment_words(
                        effective_assignments(config_root, candidate).get("IMAGE_ADDITIONS", "")
                    )
                    if addon in additions:
                        affected.add(candidate)
        if path.startswith("scripts/deb/"):
            return entries
    return [entry for entry in entries if str(entry["board"]) in affected]


def print_json_matrix(entries: list[dict[str, object]]) -> None:
    print(json.dumps({"include": entries}, separators=(",", ":")))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config-root", type=Path, default=DEFAULT_CONFIG_ROOT)
    subparsers = parser.add_subparsers(dest="command", required=True)

    subparsers.add_parser("validate")
    subparsers.add_parser("matrix")

    chain_parser = subparsers.add_parser("board-chain")
    chain_parser.add_argument("--board", required=True)
    chain_parser.add_argument("--order", choices=("precedence", "application"), default="precedence")
    chain_parser.add_argument("--separator", default=" ")

    settings_parser = subparsers.add_parser("settings")
    settings_parser.add_argument("--board", required=True)

    resolve_parser = subparsers.add_parser("resolve")
    resolve_parser.add_argument("--board", required=True)
    resolve_parser.add_argument("--path", required=True)

    resolve_dir_parser = subparsers.add_parser("resolve-dir")
    resolve_dir_parser.add_argument("--board", required=True)
    resolve_dir_parser.add_argument("--path", required=True)

    patches_parser = subparsers.add_parser("patches")
    patches_parser.add_argument("--board", required=True)
    patches_parser.add_argument("--component", required=True)

    artifact_parser = subparsers.add_parser("artifact")
    artifact_parser.add_argument("--board", required=True)
    artifact_parser.add_argument("--storage", required=True)

    affected_parser = subparsers.add_parser("affected")
    affected_parser.add_argument("--base", required=True)
    affected_parser.add_argument("--head", default="HEAD")

    args = parser.parse_args()
    config_root = args.config_root.resolve()
    try:
        if args.command == "validate":
            entries = validate(config_root)
            print(f"matrix=PASS entries={len(entries)}")
        elif args.command == "matrix":
            print_json_matrix(validate(config_root))
        elif args.command == "board-chain":
            chain = board_chain(config_root, args.board)
            if args.order == "application":
                chain.reverse()
            print(args.separator.join(chain))
        elif args.command == "settings":
            print(" ".join(str(path) for path in board_settings(config_root, args.board)))
        elif args.command == "resolve":
            print(resolve_file(config_root, args.board, args.path))
        elif args.command == "resolve-dir":
            print(resolve_directory(config_root, args.board, args.path))
        elif args.command == "patches":
            print(" ".join(str(path) for path in patch_files(config_root, args.board, args.component)))
        elif args.command == "artifact":
            entries = validate(config_root)
            matches = [
                entry
                for entry in entries
                if entry["board"] == args.board and entry["storage"] == args.storage
            ]
            if len(matches) != 1:
                raise PlanError(f"matrix has no unique entry for {args.board}/{args.storage}")
            print(f"{args.board}_{args.storage}.{matches[0]['format']}")
        elif args.command == "affected":
            print_json_matrix(affected_matrix(config_root, args.base, args.head))
    except (PlanError, subprocess.CalledProcessError) as exc:
        print(f"plan.py: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

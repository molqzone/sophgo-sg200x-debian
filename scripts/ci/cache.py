#!/usr/bin/env python3
"""Invalidate persistent SG2002 build layers when their maintained inputs change."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
from pathlib import Path

from plan import PlanError, board_chain, effective_assignments, patch_files, resolve_file


LAYERS = ("firmware", "linux", "osdrv", "middleware", "boot", "rootfs")
LAYER_COMPLETION_PATHS = {
    "firmware": ("sg2002-ipc-build-stamp",),
    "linux": ("linux-compile-stamp", "kernel/.git/HEAD"),
    "osdrv": ("osdrv-package-stamp", "osdrv/.git/HEAD"),
    "middleware": ("middleware-package-stamp", "middleware/.git/HEAD"),
    "boot": (
        "fsbl-package-stamp",
        "u-boot/.git/HEAD",
        "opensbi/.git/HEAD",
        "fsbl/.git/HEAD",
    ),
    "rootfs": ("image-compile-stamp", "rootfs"),
}
ROOTFS_REUSABLE_STAMP_PREFIXES = (
    "linux-",
    "osdrv-",
    "middleware-",
    "vcodec-firmware-",
    "uboot-",
    "opensbi-",
    "fsbl-",
)
ROOTFS_REUSABLE_STAMPS = {
    "cvi-pinmux-package-stamp",
    "sg2002-ipc-build-stamp",
}


def add_path(digest: "hashlib._Hash", root: Path, path: Path) -> None:
    if not path.exists():
        return
    files = [path] if path.is_file() else sorted(item for item in path.rglob("*") if item.is_file())
    for file_path in files:
        if "__pycache__" in file_path.parts or file_path.suffix == ".pyc":
            continue
        try:
            relative = file_path.relative_to(root)
        except ValueError:
            relative = file_path
        digest.update(str(relative).replace("\\", "/").encode("utf-8"))
        digest.update(b"\0")
        digest.update(file_path.read_bytes())
        digest.update(b"\0")


def layer_inputs(
    repo: Path, config_root: Path, board: str, storage: str, layer: str
) -> list[Path]:
    common = [repo / "versions.env", repo / "toolchain.env", repo / "scripts" / "Makefile"]
    if layer == "firmware":
        component = repo / "components" / "sg2002-ipc"
        return common + [
            component / "Makefile",
            component / "versions.env",
            component / "include",
            component / "core",
            component / "firmware",
            component / "ports" / "duo-sdk",
            resolve_file(config_root, board, "memmap.py"),
        ]
    if layer == "linux":
        paths = common + [
            resolve_file(config_root, board, "linux/defconfig"),
            resolve_file(config_root, board, "memmap.py"),
            config_root / "common" / "dts",
            repo / "scripts" / "python" / "mmap_conv.py",
        ]
        paths.extend(config_root / name / "dts" for name in board_chain(config_root, board) if name != "common")
        paths.extend(patch_files(config_root, board, "linux"))
        return paths
    if layer == "osdrv":
        return common + patch_files(config_root, board, "osdrv") + [
            repo / "components" / "sg2002-ipc" / "ports" / "osdrv"
        ]
    if layer == "middleware":
        return common + patch_files(config_root, board, "middleware") + patch_files(
            config_root, board, "sensorsupportlist"
        )
    if layer == "boot":
        settings = effective_assignments(config_root, board)
        partition = settings.get("PARTITION_FILE", "").replace(
            "$(STORAGE_TYPE)", storage
        ).strip('"')
        paths = common + [
            resolve_file(config_root, board, "memmap.py"),
            config_root / "common" / "dts",
            repo / "scripts" / "python" / "mmap_conv.py",
            repo / "scripts" / "python" / "mk_imgHeader.py",
            repo / "scripts" / "python" / "mkcvipart.py",
        ]
        if partition:
            paths.append(resolve_file(config_root, board, partition))
        paths.extend(
            config_root / name / "dts"
            for name in board_chain(config_root, board)
            if name != "common"
        )
        for relative in ("u-boot/defconfig", "u-boot/cvitek.h", "u-boot/cvi_board_init.c"):
            paths.append(resolve_file(config_root, board, relative))
        for component in ("u-boot", "opensbi", "fsbl"):
            paths.extend(patch_files(config_root, board, component))
        return paths
    if layer == "rootfs":
        component = repo / "components" / "sg2002-ipc"
        paths = common + [
            repo / "scripts" / "addons",
            repo / "scripts" / "deb",
            repo / "scripts" / "genimage_sd.cfg",
            repo / "scripts" / "genimage_emmc.cfg",
            repo / "scripts" / "python" / "raw2cimg.py",
            repo / "scripts" / "setup_rootfs.sh",
            component / "Makefile",
            component / "include",
            component / "linux",
            component / "tools",
            config_root / "settings.mk",
        ]
        paths.extend(config_root / name / "settings.mk" for name in board_chain(config_root, board) if name != "common")
        return paths
    raise PlanError(f"unknown cache layer: {layer}")


def digest_paths(repo: Path, paths: list[Path]) -> str:
    digest = hashlib.sha256()
    for path in sorted(set(paths), key=lambda item: str(item)):
        add_path(digest, repo, path)
    return digest.hexdigest()


def remove_path(build_root: Path, relative: str) -> None:
    target = (build_root / relative).resolve()
    root = build_root.resolve()
    if target == root or root not in target.parents:
        raise PlanError(f"refusing to remove unsafe cache path: {target}")
    if target.is_dir() and not target.is_symlink():
        shutil.rmtree(target)
    elif target.exists() or target.is_symlink():
        target.unlink()


def invalidate(build_root: Path, layer: str) -> None:
    mappings = {
        "firmware": ["sg2002-ipc-build-stamp", "sg2002-ipc-stamp"],
        "fsbl": ["fsbl", "fsbl-prepare-checkout-stamp", "fsbl-prepare-patch-stamp", "fsbl-compile-stamp", "fsbl-package-stamp"],
        "linux": ["kernel", "linux-prepare-checkout-stamp", "linux-prepare-patch-stamp", "linux-prepare-configure-stamp", "linux-compile-stamp"],
        "osdrv": ["osdrv", "osdrv-prepare-checkout-stamp", "osdrv-prepare-patch-stamp", "osdrv-prepare-configure-stamp", "osdrv-compile-stamp", "osdrv-package-stamp"],
        "middleware": ["middleware", "middleware-prepare-checkout-stamp", "middleware-prepare-patch-stamp", "middleware-prepare-configure-stamp", "middleware-compile-stamp", "middleware-package-stamp"],
        "boot": ["u-boot", "opensbi", "fsbl", "uboot-prepare-checkout-stamp", "uboot-prepare-patch-stamp", "uboot-prepare-configure-stamp", "uboot-compile-stamp", "opensbi-prepare-checkout-stamp", "opensbi-prepare-patch-stamp", "opensbi-compile-stamp", "fsbl-prepare-checkout-stamp", "fsbl-prepare-patch-stamp", "fsbl-compile-stamp", "fsbl-package-stamp"],
        "rootfs": ["rootfs", "image-prepare-stamp", "image-addons-stamp", "image-customize-stamp", "image-compile-stamp"],
    }
    downstream = {
        "firmware": ("firmware", "fsbl", "rootfs"),
        "linux": ("linux", "osdrv", "middleware", "rootfs"),
        "osdrv": ("osdrv", "middleware", "rootfs"),
        "middleware": ("middleware", "rootfs"),
        "boot": ("boot", "rootfs"),
        "rootfs": ("rootfs",),
    }
    affected = downstream[layer]
    for affected_layer in affected:
        for relative in mappings[affected_layer]:
            remove_path(build_root, relative)
    if "rootfs" in affected:
        for stamp in build_root.glob("*-stamp"):
            if not rootfs_stamp_is_reusable(stamp.name):
                stamp.unlink()


def layer_complete(build_root: Path, layer: str) -> bool:
    try:
        paths = LAYER_COMPLETION_PATHS[layer]
    except KeyError as exc:
        raise PlanError(f"unknown cache layer: {layer}") from exc
    return all((build_root / relative).exists() for relative in paths)


def rootfs_stamp_is_reusable(name: str) -> bool:
    return name in ROOTFS_REUSABLE_STAMPS or name.startswith(
        ROOTFS_REUSABLE_STAMP_PREFIXES
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--config-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--board", required=True)
    parser.add_argument("--storage", choices=("sd", "emmc"), default="sd")
    parser.add_argument("--layers", nargs="+", choices=LAYERS, required=True)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    repo = args.repo.resolve()
    config_root = args.config_root.resolve()
    build_root = args.build_root.resolve()
    if str(build_root) in {"/", ""} or not str(build_root).startswith("/build-cache/"):
        print(f"cache.py: build root must be below /build-cache: {build_root}", file=sys.stderr)
        return 2
    build_root.mkdir(parents=True, exist_ok=True)
    state_path = build_root / ".source-hashes.json"
    try:
        old_state = json.loads(state_path.read_text(encoding="utf-8")) if state_path.is_file() else {}
        new_state = dict(old_state)
        for layer in args.layers:
            current = digest_paths(
                repo, layer_inputs(repo, config_root, args.board, args.storage, layer)
            )
            previous = old_state.get(layer)
            changed = current != previous
            complete = layer_complete(build_root, layer)
            print(
                f"cache layer={layer} changed={str(changed).lower()} "
                f"complete={str(complete).lower()} hash={current}"
            )
            if (changed or not complete) and not args.dry_run:
                invalidate(build_root, layer)
                new_state[layer] = current
        if not args.dry_run:
            state_path.write_text(json.dumps(new_state, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    except (OSError, ValueError, PlanError) as exc:
        print(f"cache.py: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

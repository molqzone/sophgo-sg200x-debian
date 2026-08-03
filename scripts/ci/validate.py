#!/usr/bin/env python3
"""Validate repository layering and, when present, one board build output."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

from plan import (
    PlanError,
    assignment_words,
    board_chain,
    effective_assignments,
    patch_files,
    resolve_directory,
    resolve_file,
    validate,
)


REPO_ROOT = Path(__file__).resolve().parents[2]
REQUIRED_BOARD_FILES = ("linux/defconfig", "memmap.py", "u-boot/defconfig", "u-boot/cvitek.h", "u-boot/cvi_board_init.c")
UBOOT_TARGET = re.compile(r"^CONFIG_TARGET_[A-Z0-9_]+=y$", re.MULTILINE)
REMOTEPROC_NODE = re.compile(r"\bcv181x-c906_1\s*\{(?P<body>.*?)\};", re.DOTALL)
MEMORY_REGION = re.compile(r"\bmemory-region\s*=\s*<(?P<value>.*?)>;", re.DOTALL)
PHANDLE = re.compile(r"&([A-Za-z_][A-Za-z0-9_]*)")
LEGACY_RPMSG_REGIONS = ("vdev0vring0", "vdev0vring1", "vdev0buffer")
RESERVED_MEM_OWNED_NAME = re.compile(
    r"^\+\s*char\s+name\[(?P<size>\d+)\];", re.MULTILINE
)
RESERVED_MEM_NAME_COPY = re.compile(
    r"^\+\s*if\s*\(strscpy\(rmem->name,\s*uname,\s*sizeof\(rmem->name\)\)\s*<\s*0\)\s*\n"
    r'^\+\s*panic\("%s: Reserved-memory name is too long: %s\\n",\s*\n'
    r"^\+\s*__func__,\s*uname\);",
    re.MULTILINE,
)
RESERVED_MEM_PHANDLE_LOOKUP = re.compile(
    r"^\+\s*rmem\s*=\s*__find_rmem\(np\);\s*\n"
    r"^\+\s*if\s*\(rmem\)\s*\n"
    r"^\+\s*return rmem;",
    re.MULTILINE,
)
FALLBACK_EXCLUDED_DIRS = {
    ".cache",
    ".git",
    ".mypy_cache",
    ".pytest_cache",
    ".venv",
    "__pycache__",
    "build",
    "build-output",
    "component-output",
    "dist",
    "image",
    "node_modules",
    "out",
    "output",
    "package-output",
}
SCAN_CHUNK_SIZE = 1024 * 1024


def fail(message: str) -> None:
    raise PlanError(message)


def resolved_exclusions(paths: tuple[Path, ...]) -> tuple[Path, ...]:
    return tuple(
        (path if path.is_absolute() else Path.cwd() / path).resolve(strict=False)
        for path in paths
    )


def is_excluded(path: Path, excluded_roots: tuple[Path, ...]) -> bool:
    resolved = path.resolve(strict=False)
    if any(resolved == root or root in resolved.parents for root in excluded_roots):
        return True
    relative = path.relative_to(REPO_ROOT)
    if any(part in FALLBACK_EXCLUDED_DIRS for part in relative.parts):
        return True
    return bool(relative.parts and relative.parts[0].endswith("-output"))


def fallback_files(excluded_roots: tuple[Path, ...]) -> list[Path]:
    files: list[Path] = []
    for current, directories, filenames in os.walk(REPO_ROOT):
        current_path = Path(current)
        directories[:] = sorted(
            name
            for name in directories
            if not is_excluded(current_path / name, excluded_roots)
        )
        files.extend(
            current_path / name
            for name in sorted(filenames)
            if (current_path / name).is_file()
            and not is_excluded(current_path / name, excluded_roots)
        )
    return files


def maintained_files(excluded_paths: tuple[Path, ...] = ()) -> list[Path]:
    excluded_roots = resolved_exclusions(excluded_paths)
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
        return fallback_files(excluded_roots)
    return [REPO_ROOT / relative for relative in result.stdout.split("\0") if relative]


def file_occurrences(path: Path, needles: dict[str, bytes]) -> set[str]:
    if not needles:
        return set()
    found: set[str] = set()
    overlap = max(len(needle) for needle in needles.values()) - 1
    tail = b""
    with path.open("rb") as stream:
        while chunk := stream.read(SCAN_CHUNK_SIZE):
            content = tail + chunk
            for value, needle in needles.items():
                if value not in found and needle in content:
                    found.add(value)
            if len(found) == len(needles):
                break
            tail = content[-overlap:] if overlap else b""
    return found


def tracked_occurrences(
    values: set[str], excluded_paths: tuple[Path, ...] = ()
) -> dict[str, list[str]]:
    needles = {value: value.encode("utf-8") for value in values}
    locations = {value: [] for value in values}
    for path in maintained_files(excluded_paths):
        try:
            found = file_occurrences(path, needles)
        except OSError as exc:
            fail(f"cannot scan {path}: {exc}")
        relative = path.relative_to(REPO_ROOT).as_posix()
        for value in found:
            locations[value].append(relative)
    return {value: sorted(paths) for value, paths in locations.items()}


def validate_pins(excluded_paths: tuple[Path, ...] = ()) -> None:
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
    locations = tracked_occurrences(set(expected), excluded_paths)
    for value, source in expected.items():
        if locations[value] != [source]:
            fail(f"pin {value} must occur only in {source}: {locations[value]}")


def validate_addons(config_root: Path, entries: list[dict[str, object]]) -> None:
    for entry in entries:
        board = str(entry["board"])
        maixcam_family = is_board_or_descendant(config_root, board, "maixcam")
        settings = effective_assignments(config_root, board)
        additions = assignment_words(settings.get("IMAGE_ADDITIONS", ""))
        for addon in additions:
            addon_makefile = REPO_ROOT / "scripts/addons" / addon / "addon.mk"
            if not addon_makefile.is_file():
                fail(f"{board}: addon {addon!r} has no addon.mk")
            target = re.compile(rf"^\$\(BUILDDIR\)/{re.escape(addon)}-stamp\s*:", re.MULTILINE)
            if not target.search(addon_makefile.read_text(encoding="utf-8")):
                fail(f"{board}: addon {addon!r} has no matching stamp target")
        if not maixcam_family and any(
            addon.startswith("maixcam-") for addon in additions
        ):
            fail(f"{board}: inherits a MaixCAM-only addon")


def is_board_or_descendant(config_root: Path, board: str, ancestor: str) -> bool:
    return ancestor in board_chain(config_root, board)


def validate_maixcam_sensor_settings(
    config_root: Path, entries: list[dict[str, object]]
) -> None:
    addon = REPO_ROOT / "scripts/addons/maixcam-sensor-config/overlay/mnt"
    for entry in entries:
        board = str(entry["board"])
        if not is_board_or_descendant(config_root, board, "maixcam"):
            continue
        settings = effective_assignments(config_root, board)
        fixed = settings.get("MAIXCAM_SENSOR_FIXED", "0").strip().strip('"')
        if fixed not in {"0", "1"}:
            fail(f"{board}: MAIXCAM_SENSOR_FIXED must be 0 or 1, got {fixed!r}")
        if fixed == "1" and not settings.get("MAIXCAM_SENSOR_CONFIG", "").strip():
            fail(f"{board}: fixed sensor profile requires MAIXCAM_SENSOR_CONFIG")
        selections = {
            "MAIXCAM_SENSOR_CONFIG": addon / "data",
            "MAIXCAM_SENSOR_PQ": addon / "cfg/param",
        }
        for setting, directory in selections.items():
            filename = settings.get(setting, "").strip().strip('"')
            if not filename:
                continue
            if not re.fullmatch(r"[A-Za-z0-9_.-]+", filename):
                fail(f"{board}: invalid {setting} filename {filename!r}")
            if not (directory / filename).is_file():
                fail(f"{board}: {setting} file is missing: {directory / filename}")
        profile = settings.get("MAIXCAM_SENSOR_CONFIG", "").strip().strip('"')
        if not profile:
            continue
        profile_text = (addon / "data" / profile).read_text(encoding="utf-8")
        if "SMS_SC035HGS_" in profile_text:
            middleware_patches = patch_files(config_root, board, "middleware")
            patch_text = "\n".join(
                patch.read_text(encoding="utf-8") for patch in middleware_patches
            )
            for expected in (
                "case SMS_SC035HGS_MIPI_480P_120FPS_12BIT:",
                'snprintf(name, sizeof(name), "sms_sc035hgs");',
                '!strcmp(sensor_name, "sms_sc035hgs")',
            ):
                if expected not in patch_text:
                    fail(f"{board}: Maix MMF has no SC035HGS route: {expected}")


def validate_uboot_defconfig(board: str, path: Path) -> None:
    targets = UBOOT_TARGET.findall(path.read_text(encoding="utf-8"))
    if len(targets) != 1:
        fail(f"{board}: U-Boot defconfig must enable exactly one target, got {targets}")


def remoteproc_memory_regions(text: str) -> list[str]:
    node = REMOTEPROC_NODE.search(text)
    if not node:
        fail("cv181x-c906_1 node is missing")
    memory_region = MEMORY_REGION.search(node.group("body"))
    if not memory_region:
        fail("cv181x-c906_1 memory-region property is missing")
    return PHANDLE.findall(memory_region.group("value"))


def validate_maixcam_remoteproc_dts(dts_directory: Path) -> None:
    candidates = sorted(dts_directory.glob("*.dts"))
    if len(candidates) != 1:
        fail(f"maixcam: expected one DTS source, got {candidates}")
    text = candidates[0].read_text(encoding="utf-8")
    regions = remoteproc_memory_regions(text)
    expected = ["fast_image", "rtos_boot_trace"]
    if regions != expected:
        fail(f"maixcam: mailbox-only remoteproc memory-region must be {expected}, got {regions}")
    for name in LEGACY_RPMSG_REGIONS:
        directive = re.compile(
            rf"^\s*/delete-node/\s+&{re.escape(name)}\s*;", re.MULTILINE
        )
        if not directive.search(text):
            fail(f"maixcam: mailbox-only DTS must delete legacy RPMsg pool {name}")


def validate_compiled_maixcam_dts(text: str) -> None:
    node = REMOTEPROC_NODE.search(text)
    if not node:
        fail("compiled MaixCAM DTB has no cv181x-c906_1 node")
    memory_region = MEMORY_REGION.search(node.group("body"))
    if not memory_region:
        fail("compiled MaixCAM remoteproc has no memory-region property")
    cells = re.findall(r"\b(?:0x[0-9a-fA-F]+|[0-9]+)\b", memory_region.group("value"))
    if len(cells) != 2:
        fail(f"compiled MaixCAM remoteproc must reference two regions, got {cells}")
    for name in LEGACY_RPMSG_REGIONS:
        active_node = re.compile(
            rf"^\s*{re.escape(name)}(?:@[0-9a-fA-F]+)?\s*\{{", re.MULTILINE
        )
        if active_node.search(text):
            fail(f"compiled MaixCAM DTB retains legacy RPMsg pool {name}")


def validate_compiled_maixcam_dtb(path: Path) -> None:
    result = subprocess.run(
        ["dtc", "-I", "dtb", "-O", "dts", str(path)],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    validate_compiled_maixcam_dts(result.stdout)


def validate_reserved_memory_name_patch(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    owned_name = RESERVED_MEM_OWNED_NAME.search(text)
    if not owned_name:
        fail("reserved-memory patch must use boot-safe owned node-name storage")
    if int(owned_name.group("size")) < 64:
        fail("reserved-memory owned node-name storage must be at least 64 bytes")
    if not RESERVED_MEM_NAME_COPY.search(text):
        fail("reserved-memory patch must reject names that do not fit owned storage")
    if not RESERVED_MEM_PHANDLE_LOOKUP.search(text):
        fail("reserved-memory lookup must prefer the device-tree phandle")
    if re.search(
        r"^\+.*(?:strncpy\(rmem->name|rmem->name\s*=\s*(?:uname|memblock_alloc))",
        text,
        re.MULTILINE,
    ):
        fail("reserved-memory patch must not use truncating or early-pointer storage")


def validate_boards(config_root: Path, entries: list[dict[str, object]]) -> None:
    for entry in entries:
        board = str(entry["board"])
        storage = str(entry["storage"])
        settings = effective_assignments(config_root, board)
        dts_directory = resolve_directory(config_root, board, "dts")
        if is_board_or_descendant(config_root, board, "maixcam"):
            validate_maixcam_remoteproc_dts(dts_directory)
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


def one_package(output: Path, pattern: str, label: str) -> Path:
    matches = sorted(output.glob(pattern))
    if len(matches) != 1:
        fail(f"expected one {label} package, got {matches}")
    return matches[0]


def extract_deb(package: Path, destination: Path) -> None:
    subprocess.run(
        ["dpkg-deb", "-x", str(package), str(destination)],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def validate_fixed_sensor_output(board: str, output: Path, settings: dict[str, str]) -> None:
    profile = settings.get("MAIXCAM_SENSOR_CONFIG", "").strip().strip('"')
    source_profile = (
        REPO_ROOT
        / "scripts/addons/maixcam-sensor-config/overlay/mnt/data"
        / profile
    )
    sensor_deb = one_package(
        output, f"sensor-config-{board}_*.deb", f"{board} sensor-config"
    )
    middleware_deb = one_package(
        output, f"cvitek-middleware-{board}_*.deb", f"{board} middleware"
    )

    with tempfile.TemporaryDirectory() as tempdir:
        extracted = Path(tempdir)
        sensor_root = extracted / "sensor"
        middleware_root = extracted / "middleware"
        extract_deb(sensor_deb, sensor_root)
        extract_deb(middleware_deb, middleware_root)

        packaged_profile = sensor_root / "mnt/data" / profile
        if not packaged_profile.is_file() or packaged_profile.read_bytes() != source_profile.read_bytes():
            fail(f"{board}: packaged sensor profile does not match {source_profile}")
        defaults = sensor_root / "etc/default/maixcam-sensor-config"
        expected_defaults = (
            f"SENSOR_CONFIG_DEFAULT=/mnt/data/{profile}\n"
            "SENSOR_CONFIG_FIXED=1\n"
        )
        if not defaults.is_file() or defaults.read_text(encoding="utf-8") != expected_defaults:
            fail(f"{board}: fixed sensor defaults are missing or incorrect")
        if (sensor_root / "mnt/cfg/param/cvi_sdr_bin").exists():
            fail(f"{board}: sensor package installs an unselected cvi_sdr_bin")

        profile_text = packaged_profile.read_text(encoding="utf-8")
        if "SMS_SC035HGS_" in profile_text:
            if not any(middleware_root.rglob("libsns_sc035hgs.so")):
                fail(f"{board}: middleware package has no libsns_sc035hgs.so")
            mmf_library = middleware_root / "usr/lib/libmaix_mmf.a"
            if not mmf_library.is_file() or b"sms_sc035hgs" not in mmf_library.read_bytes():
                fail(f"{board}: libmaix_mmf.a has no SC035HGS application route")


def validate_output(
    entries: list[dict[str, object]],
    board: str,
    storage: str,
    output: Path,
    allow_missing: bool,
    config_root: Path | None = None,
) -> None:
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
        for suffix in (
            "c906-mcu.elf",
            "c906-mcu.bin",
            "rtos-cmd",
            "rtos-bench",
            "rtos-thread-bench",
            "libsg2002-rtos.a",
        ):
            candidate = output / f"{board}_{suffix}"
            if not candidate.is_file() or candidate.stat().st_size == 0:
                fail(f"component artifact is missing: {candidate}")
    if config_root is not None:
        settings = effective_assignments(config_root, board)
        if settings.get("MAIXCAM_SENSOR_FIXED", "0").strip().strip('"') == "1":
            validate_fixed_sensor_output(board, output, settings)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--board", default="maixcam")
    parser.add_argument("--storage", default="sd")
    parser.add_argument("--output", type=Path, default=REPO_ROOT / "output")
    parser.add_argument("--exclude-path", type=Path, action="append", default=[])
    parser.add_argument("--allow-missing-image", action="store_true")
    parser.add_argument("--compiled-dtb", type=Path)
    args = parser.parse_args()
    if args.compiled_dtb:
        try:
            validate_compiled_maixcam_dtb(args.compiled_dtb)
        except (OSError, PlanError, subprocess.CalledProcessError) as exc:
            print(f"validate.py: {exc}", file=sys.stderr)
            return 2
        print(f"compiled-dtb=PASS path={args.compiled_dtb}")
        return 0
    try:
        config_root = REPO_ROOT / "configs"
        entries = validate(config_root)
        validate_boards(config_root, entries)
        validate_addons(config_root, entries)
        validate_maixcam_sensor_settings(config_root, entries)
        validate_reserved_memory_name_patch(
            REPO_ROOT
            / "configs/common/patches/linux/0002-Add-Reset-for-C906L-ignore-clock-status-for-C906L-an.patch"
        )
        validate_pins((args.output, *args.exclude_path))
        validate_component()
        validate_workflows()
        validate_output(
            entries,
            args.board,
            args.storage,
            args.output,
            args.allow_missing_image,
            config_root,
        )
    except (OSError, PlanError, subprocess.CalledProcessError, zipfile.BadZipFile) as exc:
        print(f"validate.py: {exc}", file=sys.stderr)
        return 2
    print(f"validate=PASS board={args.board} storage={args.storage}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

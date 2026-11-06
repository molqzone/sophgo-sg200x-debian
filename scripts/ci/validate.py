#!/usr/bin/env python3
"""Validate repository layering and, when present, one board build output."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
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
GUIDANCE_PAGES_NODE = re.compile(
    r"\bguidance-pages@[0-9a-fA-F]+\s*\{(?P<body>.*?)\};", re.DOTALL
)
GUIDANCE_PAGES_REG = re.compile(r"\breg\s*=\s*(?P<value>.*?);", re.DOTALL)
GUIDANCE_PAGES_REG_NAMES = re.compile(
    r"\breg-names\s*=\s*(?P<value>.*?);", re.DOTALL
)
GUIDANCE_CONTRACT_FILES = (
    "c906-mcu.elf",
    "c906-mcu.bin",
    "guidance_contract.json",
)
GUIDANCE_FW_ADDON = "guidance-fw"
GUIDANCE_LOADERS = ("remoteproc", "fsbl")
GUIDANCE_RPROC_MODES = ("load", "attach")
GUIDANCE_DOORBELL_OWNERS = ("rtos_cmdqu", "uio")
BOOTLOG_DAEMON = "scripts/addons/bootlog/bootlogd"
BOOTLOG_ADDON = "scripts/addons/bootlog/addon.mk"
BOOTLOG_SERVICE = "scripts/addons/bootlog/bootlog-snap.service"
GUIDANCE_DROPIN_TEMPLATE = (
    "components/guidance-fw/linux/systemd/load-systemko.service.d/"
    "20-guidance-fw.conf.in"
)
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
    ".codex-build-lf",
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
    # A file deleted in the working tree but not yet staged is still listed by
    # `git ls-files`; scanning it would fail on a normal in-progress deletion.  Skip
    # what is not on disk: the build sees the same set.
    return [
        REPO_ROOT / relative
        for relative in result.stdout.split("\0")
        if relative and (REPO_ROOT / relative).is_file()
    ]


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


def validate_entropy_seed() -> None:
    """The image must carry a seed and credit it before sysinit.

    The board spent ~11 s before `crng init done` with every early unit queued behind
    it, and systemd-random-seed only *waits* for the pool before loading its seed -- so
    a seed file alone changes nothing on a cold boot.  The build therefore fails if the
    unit, the script or the seed is missing.
    """
    unit = REPO_ROOT / "scripts/addons/entropy/seed-entropy.service"
    script = REPO_ROOT / "scripts/addons/entropy/seed-entropy"
    addon = REPO_ROOT / "scripts/addons/entropy/addon.mk"
    for path in (unit, script, addon):
        if not path.is_file():
            fail(f"entropy seeding is incomplete: {path} is missing")
    text = unit.read_text(encoding="utf-8")
    if "Before=sysinit.target systemd-random-seed.service" not in text:
        fail("seed-entropy must run before sysinit and before systemd-random-seed")
    if "RNDADDENTROPY" not in script.read_text(encoding="utf-8"):
        fail("seed-entropy must credit the pool (RNDADDENTROPY), not just write bytes")
    if "random-seed" not in (REPO_ROOT / "scripts/setup_rootfs.sh").read_text(
            encoding="utf-8"):
        fail("setup_rootfs.sh must write /var/lib/systemd/random-seed into the image")


def validate_bootlog_ring() -> None:
    check_bootlog_ring(
        (REPO_ROOT / BOOTLOG_DAEMON).read_text(encoding="utf-8"),
        (REPO_ROOT / BOOTLOG_ADDON).read_text(encoding="utf-8"),
        (REPO_ROOT / BOOTLOG_SERVICE).read_text(encoding="utf-8"),
    )


def check_bootlog_ring(daemon: str, addon: str, service: str) -> None:
    """The boot log must never allocate on the partition U-Boot boots from.

    FAT has no journal, so a directory entry, a file size or a cluster chain that the
    writer changes at runtime is a chance to leave the boot partition inconsistent
    after a power cut -- and this daemon runs on exactly the partition U-Boot reads
    the kernel and DTB from.  It already happened once (`fsck.fat`: snap.log claiming
    23418 bytes with a cluster chain over 24576, then short reads out of U-Boot), so
    the shape is enforced here rather than trusted: the ring is preallocated at image
    build time and the daemon may only `pwrite` whole records into it and `fsync`.

    这条检查不看“有没有 sync”，而是看写入的形状：环在镜像里预分配，运行时只按整条记录
    就地覆盖。文件大小和簇链一旦可能在运行时变化，掉电就可能把启动分区带坏。
    """
    for token in ("os.pwrite", "os.fsync", "RING_PATH"):
        if token not in daemon:
            fail(f"{BOOTLOG_DAEMON}: the crash-safe ring needs {token!r}")

    for call in re.findall(r"os\.open\(([^)]*)\)", daemon):
        if "RING_PATH" not in call:
            continue
        for flag in ("O_CREAT", "O_TRUNC", "O_APPEND", "O_WRONLY"):
            if flag in call:
                fail(
                    f"{BOOTLOG_DAEMON}: the ring must be opened O_RDWR without "
                    f"{flag}, or the write can change its size or cluster chain"
                )

    # `open(path, "ab")` would allocate just as well as O_APPEND, and reading needs no
    # write mode at all: only "r"/"rb" are allowed anywhere in the daemon.
    for mode in re.findall(r'"([rwaxb+]{1,3})"', daemon):
        if set(mode) & set("aw+"):
            fail(
                f"{BOOTLOG_DAEMON}: open(..., {mode!r}) can create, grow or truncate; "
                f"the ring is written with pwrite and read with 'rb'"
            )

    for token in ("os.truncate", "os.ftruncate", "shutil."):
        if token in daemon:
            fail(
                f"{BOOTLOG_DAEMON}: {token} can change the ring's size; it is "
                f"preallocated by the image and only ever overwritten in place"
            )
    # Unlinking is fine for /run (a one-off boot report is spent once), never for the
    # ring: the whole point is that the boot partition's metadata does not change.
    for call in re.findall(r"os\.(?:unlink|remove|rename|mkdir)\(([^)]*)\)", daemon):
        if "RING_PATH" in call:
            fail(
                f"{BOOTLOG_DAEMON}: removing or replacing the ring is exactly what "
                f"corrupts the boot partition; it must only be overwritten"
            )

    record = re.search(r"^RECORD = (\d+)$", daemon, re.MULTILINE)
    if record is None:
        fail(f"{BOOTLOG_DAEMON}: RECORD size is not declared")
    if "bootlog.bin" not in addon or "dd if=/dev/zero" not in addon:
        fail(
            f"{BOOTLOG_ADDON}: the ring must be preallocated here, because creating "
            f"it at runtime is what corrupts the boot partition"
        )
    if "stat -c %s" not in addon:
        fail(f"{BOOTLOG_ADDON}: the preallocated ring's size must be asserted")
    addon_record = re.search(r"^BOOTLOG_RECORD := (\d+)$", addon, re.MULTILINE)
    if addon_record is None:
        fail(f"{BOOTLOG_ADDON}: BOOTLOG_RECORD is not declared")
    if addon_record.group(1) != record.group(1):
        fail(
            "the preallocated ring and the daemon disagree about the record size: "
            f"{addon_record.group(1)} vs {record.group(1)}"
        )

    for name in ("boot-time", "boot-time.service"):
        if name not in addon:
            fail(
                f"{BOOTLOG_ADDON}: {name} must be installed and enabled: without it "
                f"boot time is a feeling instead of a measurement"
            )
    if "ExecStart=/usr/local/sbin/bootlogd" not in service:
        fail(f"{BOOTLOG_SERVICE}: must run /usr/local/sbin/bootlogd")
    if "Restart=always" not in service:
        fail(
            f"{BOOTLOG_SERVICE}: Restart=always is required: a crash must bring the "
            f"logger back, the boot must not lose its snapshots because one write failed"
        )


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


def check_maixcam_reserved_map(dts_text: str, memmap_text: str) -> None:
    """The three touching reservations must stay three, with the sizes memmap states.

    实测：把命令队列窗口并进 fast_image（相机桥退役时干过）会让这块板子再也不进系统，
    而所有静态检查都通过——DTB 只是"另一个描述"，没有人核对它和开机的关系。所以这里
    把那次改动会碰到的三处钉住：窗口必须还在、必须还是独立 no-map 节点、cmdqu 必须还
    声明它的 shared-memory 资源。
    """
    if "shared-memory@8ffde000" not in dts_text:
        fail(
            "the maixcam DTS must keep the command-queue window as its own reserved "
            "node: folding it into fast_image is how this board stopped booting"
        )
    if 'reg-names = "mailbox", "shared-memory";' not in dts_text:
        fail(
            "rtos_cmdqu must declare its shared-memory resource; the second reg is "
            "the command queue's data window, not the retired camera ring"
        )
    if "no-map;" not in dts_text.split("shared-memory@8ffde000", 1)[1][:200]:
        fail("the command-queue window must stay no-map")
    if "IPC_SHARED_SIZE" not in memmap_text:
        fail("memmap.py must account for the 132 KiB command-queue window")
    if "RTOS_FIRMWARE_SIZE = FREERTOS_SIZE - RTOS_BOOT_TRACE_SIZE\n" in memmap_text:
        fail(
            "RTOS_FIRMWARE_SIZE must leave the command-queue window out; without it "
            "the firmware band overlaps the cmdqu data area"
        )


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
            check_maixcam_reserved_map(
                "\n".join(path.read_text(encoding="utf-8")
                           for path in sorted(dts_directory.glob("*.dts"))),
                (REPO_ROOT / "configs/maixcam/memmap.py").read_text(encoding="utf-8"),
            )
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


def check_vendored_cop() -> None:
    """The shipped `cop` must be exactly the published revision it claims.

    工具现在有自己的仓库（molqzone/cop），镜像只是**vendored 副本**：一份文件、一份 pin
    （commit + sha256）。两者一旦不一致，板子上装的就是一个没有出处的东西，所以构建期就
    比对，而不是靠谁记得同步。
    """
    tool = REPO_ROOT / "components/guidance-fw/tools/cop"
    pin_path = REPO_ROOT / "components/guidance-fw/tools/cop.upstream"
    if not tool.is_file() or not pin_path.is_file():
        fail("the vendored cop tool or its provenance file is missing")
    pin = {}
    for line in pin_path.read_text(encoding="utf-8").splitlines():
        if "=" in line and not line.startswith("#"):
            key, value = line.split("=", 1)
            pin[key.strip()] = value.strip()
    for key in ("repo", "commit", "sha256"):
        if not pin.get(key):
            fail(f"cop.upstream is missing {key}")
    actual = hashlib.sha256(tool.read_bytes()).hexdigest()
    if actual != pin["sha256"]:
        fail(
            "the shipped cop does not match its pin: %s is %s, cop.upstream says %s "
            "(%s@%s); re-vendor it instead of editing the file in place"
            % (tool.name, actual[:12], pin["sha256"][:12], pin["repo"], pin["commit"][:12])
        )


def validate_component() -> None:
    component = REPO_ROOT / "components/guidance-fw"
    required = (
        "firmware/c906-mcu.elf",
        "firmware/c906-mcu.bin",
        "firmware/guidance_contract.json",
        "firmware/sha256sums.txt",
        "ports/osdrv/patches/0001-cmdqu-allow-user-commands.patch",
        "tools/rtos-mode",
        "tools/cop",
        "linux/systemd/load-systemko.service.d/20-guidance-fw.conf.in",
        "README.md",
    )
    for name in required:
        if not (component / name).is_file():
            fail(f"component path is missing: {component / name}")
    for name in ("tools/rtos-mode", "tools/cop"):
        if not (component / name).stat().st_mode & 0o111:
            fail(f"component tool is not executable: {component / name}")
    if (REPO_ROOT / "scripts/addons/rtos-firmware").exists():
        fail("legacy scripts/addons/rtos-firmware still exists")
    check_vendored_cop()


def check_guidance_boot_policy(settings: dict[str, str], dts_text: str) -> None:
    """The C906L startup facts are build-time decisions; check them against the DTS.

    Three knobs decide who releases the core, whether remoteproc loads firmware or
    attaches to a running one, and who owns the doorbell IRQ.  They used to be
    implicit in two places -- the FSBL input flipped when the guidance-fw addon was
    added, and the drop-in hardcoded the rtos-mode argument -- so nothing compared
    them with the hardware description at all.
    """
    loader = settings.get("GUIDANCE_FW_LOADER", "").strip().strip('"') or "remoteproc"
    mode = settings.get("GUIDANCE_FW_RPROC_MODE", "").strip().strip('"') or "load"
    owner = (
        settings.get("GUIDANCE_FW_DOORBELL_OWNER", "").strip().strip('"')
        or "rtos_cmdqu"
    )

    if loader not in GUIDANCE_LOADERS:
        fail(f"GUIDANCE_FW_LOADER={loader!r} must be one of {GUIDANCE_LOADERS}")
    if mode not in GUIDANCE_RPROC_MODES:
        fail(f"GUIDANCE_FW_RPROC_MODE={mode!r} must be one of {GUIDANCE_RPROC_MODES}")
    if owner not in GUIDANCE_DOORBELL_OWNERS:
        fail(
            "GUIDANCE_FW_DOORBELL_OWNER=%r must be one of %s"
            % (owner, GUIDANCE_DOORBELL_OWNERS)
        )
    if owner == "uio":
        fail(
            "GUIDANCE_FW_DOORBELL_OWNER=uio is not implemented: the vendor rtos_cmdqu "
            "driver owns IRQ 101 with an exclusive request_irq and uio_pdrv_genirq "
            "cannot share it, so the firmware notification rides RTOS_CMDQU_WAIT_EVENT"
        )
    if loader == "fsbl" and mode == "load":
        fail(
            "GUIDANCE_FW_LOADER=fsbl with GUIDANCE_FW_RPROC_MODE=load is the DMA-race "
            "hang recorded in the README: the FSBL releases the C906L while U-Boot and "
            "the early kernel still own the shared AXI DMA controller; FSBL boot must "
            "use attach"
        )
    if loader == "remoteproc" and mode == "attach":
        fail(
            "GUIDANCE_FW_RPROC_MODE=attach needs GUIDANCE_FW_LOADER=fsbl: with "
            "loader=remoteproc nothing has released the core before Linux starts"
        )

    regions = remoteproc_memory_regions(dts_text)
    if "fast_image" not in regions:
        fail(
            "the C906L firmware band (fast_image) must stay in the rproc memory-region; "
            "got %s" % regions
        )
    if owner == "rtos_cmdqu":
        if not re.search(r"\brtos_cmdqu\s*\{", dts_text):
            fail(
                "GUIDANCE_FW_DOORBELL_OWNER=rtos_cmdqu but the DTS has no rtos_cmdqu node"
            )
        if not re.search(r"\binterrupts\s*=\s*<101\b", dts_text):
            fail(
                "GUIDANCE_FW_DOORBELL_OWNER=rtos_cmdqu but the DTS mailbox has no "
                "IRQ 101"
            )


def validate_guidance_boot_policy(
    config_root: Path, entries: list[dict[str, object]]
) -> None:
    template = REPO_ROOT / GUIDANCE_DROPIN_TEMPLATE
    if not template.is_file():
        fail(f"guidance drop-in template is missing: {template}")
    if "@GUIDANCE_FW_RPROC_COMMAND@" not in template.read_text(encoding="utf-8"):
        fail(
            "the guidance drop-in template must leave the rtos-mode argument to the "
            "board's boot policy"
        )
    for entry in entries:
        board = str(entry["board"])
        settings = effective_assignments(config_root, board)
        if GUIDANCE_FW_ADDON not in assignment_words(settings.get("IMAGE_ADDITIONS", "")):
            continue
        dts_directory = resolve_directory(config_root, board, "dts")
        candidates = sorted(dts_directory.glob("*.dts"))
        if len(candidates) != 1:
            fail(f"{board}: expected one DTS source, got {candidates}")
        check_guidance_boot_policy(
            settings, candidates[0].read_text(encoding="utf-8")
        )


def dts_cells(value: str) -> list[int]:
    return [int(cell, 0) for cell in re.findall(r"0x[0-9a-fA-F]+|\b\d+\b", value)]


def load_memmap(path: Path):
    spec = importlib.util.spec_from_file_location("maixcam_memmap", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.MemoryMap


def check_guidance_contract_page(contract: dict[str, object]) -> None:
    """The contract's declared offsets must be the ones the page tool derives.

    `cop` reads the page's own header, so its offsets are a *derivation*; the contract
    states them.  Two independent statements of one layout is exactly the shape that
    drifts quietly, so the build asks the tool what it would compute and compares.  The
    rest is arithmetic that a human should not have to re-check by eye: every region
    inside the page, the command slot ending exactly at `write_state`, the sequence word
    ending the page, and a name-service table that fits its declared capacity.

    这里用 `cop` 自己的推导结果对账，而不是把偏移量再抄一遍：工具读的是页头，契约是声明，
    两边不一致时构建期就红，而不是等板子上错位。
    """
    page = contract["page"]
    telemetry = contract["telemetry"]
    channels = contract["channels"]
    region = contract["region"]
    frame = contract["frame"]
    page_bytes = page["bytes"]

    tool = _load_cop_module()
    if tool.PAGE_MAGIC != page["magic"]:
        fail("cop and the contract disagree about the page magic")
    if tool.HEADER_BYTES != page["header_bytes"]:
        fail("cop and the contract disagree about the header size")
    if tool.CHANNEL_TABLE_MAGIC != channels["magic"]:
        fail("cop and the contract disagree about the name-service magic")
    if tool.CHANNEL_ENTRY_BYTES != channels["entry_bytes"]:
        fail("cop and the contract disagree about the channel entry size")
    if tool.CHANNEL_NAME_BYTES != channels["name_bytes"]:
        fail("cop and the contract disagree about the channel name size")

    header = {
        "magic": page["magic"],
        "abi": page["abi_version"],
        "page_size": page_bytes,
        "slot_count": telemetry["slot_count"],
        "slot_bytes": telemetry["slot_bytes"],
        "payload_bytes": frame["payload_bytes"],
        "region_offset": region["offset"],
        "tag": page["wire_tag"],
    }
    geometry = tool.geometry(header)
    for name, declared in (
        ("telemetry", telemetry["offset"]),
        ("head", telemetry["head_offset"]),
        ("write_state", telemetry["write_state_offset"]),
        ("channels", channels["offset"]),
    ):
        if geometry[name] != declared:
            fail(
                "cop derives %s at %d but the contract declares %d"
                % (name, geometry[name], declared)
            )

    if channels["offset"] + channels["bytes"] > page_bytes:
        fail("the name-service table runs past the end of the page")
    if 16 + channels["capacity"] * channels["entry_bytes"] > channels["bytes"]:
        fail(
            "the name-service area is %d bytes but its capacity needs %d"
            % (channels["bytes"],
               16 + channels["capacity"] * channels["entry_bytes"])
        )
    if region["offset"] + region["payload_bytes"] != region["write_state_offset"]:
        fail("the command slot does not end where write_state begins")
    if region["write_state_offset"] + 4 != region["seq_offset"]:
        fail("write_state and the sequence word are not adjacent")
    if region["seq_offset"] + 4 > page_bytes:
        fail("the command sequence word runs past the end of the page")
    if region["offset"] < channels["offset"] + channels["bytes"]:
        fail("the command region overlaps the name-service table")

    names: set[str] = set()
    keys: set[int] = set()
    for entry in channels["entries"]:
        name = str(entry["name"])
        if not name:
            fail("a channel entry has an empty name")
        if len(name.encode()) > channels["name_bytes"]:
            fail("channel name %r does not fit in %d bytes" % (name, channels["name_bytes"]))
        if name in names:
            fail("channel %r is announced twice" % name)
        if int(entry["key"]) in keys:
            fail("two channels share key 0x%08x" % int(entry["key"]))
        names.add(name)
        keys.add(int(entry["key"]))
        if not 0 <= int(entry["endpoint"]) < page_bytes:
            fail("channel %r has endpoint %r outside the page"
                 % (name, entry["endpoint"]))


def _load_cop_module():
    """Import `cop` (no .py suffix) so the build can ask what it would compute."""
    import importlib.machinery
    import importlib.util

    path = REPO_ROOT / "components/guidance-fw/tools/cop"
    loader = importlib.machinery.SourceFileLoader("guidance_cop", str(path))
    spec = importlib.util.spec_from_loader("guidance_cop", loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


def validate_guidance_contract(config_root: Path) -> None:
    """The page layout has one owner: enforce that everything else agrees with it.

    The firmware publishes `guidance_contract.json` with the ELF and pins both; the
    DTS and `memmap.py` describe the same two pages on the image side.  Three silent
    copies is how a layout drift first shows up as a misaligned page on a board, so
    the build compares them instead: any disagreement is a red build.
    """
    component = REPO_ROOT / "components/guidance-fw"
    contract_path = component / "firmware/guidance_contract.json"
    if not contract_path.is_file():
        fail(f"firmware contract is missing: {contract_path}")
    try:
        contract = json.loads(contract_path.read_text(encoding="utf-8"))
    except ValueError as error:
        fail(f"{contract_path} is not valid JSON: {error}")
    if contract.get("schema") != 1:
        fail(f"unsupported firmware contract schema: {contract.get('schema')}")
    check_guidance_contract_page(contract)
    page = contract["page"]
    console = contract["console"]

    dts_directory = resolve_directory(config_root, "maixcam", "dts")
    candidates = sorted(dts_directory.glob("*.dts"))
    if len(candidates) != 1:
        fail(f"maixcam: expected one DTS source, got {candidates}")
    node = GUIDANCE_PAGES_NODE.search(candidates[0].read_text(encoding="utf-8"))
    if not node:
        fail("maixcam: the DTS has no guidance-pages node")
    body = node.group("body")
    reg = GUIDANCE_PAGES_REG.search(body)
    names = GUIDANCE_PAGES_REG_NAMES.search(body)
    if not reg or not names:
        fail("maixcam: guidance-pages needs reg and reg-names")
    cells = dts_cells(reg.group("value"))
    if len(cells) != 8:
        fail(f"maixcam: guidance-pages needs two <addr size> pairs, got {cells}")
    regions = [
        ((cells[0] << 32) | cells[1], (cells[2] << 32) | cells[3]),
        ((cells[4] << 32) | cells[5], (cells[6] << 32) | cells[7]),
    ]
    names_value = GUIDANCE_PAGES_REG_NAMES.search(body).group("value")
    reg_names = re.findall(r'"([^"]+)"', names_value)
    if reg_names != ["console", "shared"]:
        fail(f"maixcam: guidance-pages reg-names must be console/shared, got {reg_names}")
    if regions[0] != (console["phys"], console["bytes"]):
        fail(
            "maixcam: guidance-pages console map (%#x+%#x) disagrees with the "
            "firmware contract (%#x+%#x)"
            % (regions[0][0], regions[0][1], console["phys"], console["bytes"])
        )
    if regions[1] != (page["phys"], page["bytes"]):
        fail(
            "maixcam: guidance-pages shared map (%#x+%#x) disagrees with the "
            "firmware contract (%#x+%#x)"
            % (regions[1][0], regions[1][1], page["phys"], page["bytes"])
        )

    memory_map = load_memmap(resolve_file(config_root, "maixcam", "memmap.py"))
    if memory_map.GUIDANCE_SHARED_PAGE_ADDR != page["phys"]:
        fail(
            "maixcam: memmap.py GUIDANCE_SHARED_PAGE_ADDR %#x disagrees with the "
            "firmware contract %#x"
            % (memory_map.GUIDANCE_SHARED_PAGE_ADDR, page["phys"])
        )
    if memory_map.GUIDANCE_CONSOLE_PAGE_ADDR != console["phys"]:
        fail(
            "maixcam: memmap.py GUIDANCE_CONSOLE_PAGE_ADDR %#x disagrees with the "
            "firmware contract %#x"
            % (memory_map.GUIDANCE_CONSOLE_PAGE_ADDR, console["phys"])
        )

    pins = {}
    for line in (component / "firmware/sha256sums.txt").read_text(
        encoding="utf-8"
    ).splitlines():
        digest, name = line.split()
        pins[name] = digest
    if set(pins) != set(GUIDANCE_CONTRACT_FILES):
        fail(
            "firmware/sha256sums.txt must pin %s, got %s"
            % (sorted(GUIDANCE_CONTRACT_FILES), sorted(pins))
        )
    for name, digest in pins.items():
        actual = hashlib.sha256((component / "firmware" / name).read_bytes()).hexdigest()
        if actual != digest:
            fail(f"firmware pin mismatch for {name}: {actual} != {digest}")


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
        validate_guidance_boot_policy(config_root, entries)
        validate_maixcam_sensor_settings(config_root, entries)
        validate_reserved_memory_name_patch(
            REPO_ROOT
            / "configs/common/patches/linux/0002-Add-Reset-for-C906L-ignore-clock-status-for-C906L-an.patch"
        )
        validate_pins((args.output, *args.exclude_path))
        validate_component()
        validate_guidance_contract(config_root)
        validate_bootlog_ring()
        validate_entropy_seed()
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

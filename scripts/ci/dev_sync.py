#!/usr/bin/env python3
"""dev_sync.py - put the runtime files onto a board that is already booted.

The image pipeline rebuilds a 4.6 GB artifact to change a shell script, which costs
25-35 minutes and answers a question nobody asked.  Almost every Linux-side change in
this repository is one of a handful of files: the boot log daemon, the C906L boot
drop-in, `rtos-mode`, `cop`, and the firmware contract.  This pushes those and
restarts what can be restarted, in seconds.

Two policies it encodes, both deliberate:

* It never writes to the boot partition.  The boot log ring is preallocated by the
  image for the same reason; see docs/boot-log.md.
* It never restarts `load-systemko.service`.  This image has no stop path for the
  C906L because stopping a streaming core wedges the board (docs/shutdown-behaviour.md),
  so a new firmware ELF or drop-in takes effect on the next power cycle -- and the
  script says so instead of pretending it applied.

Usage:
    dev_sync.py --board debian@10.42.0.1      # over ssh (default)
    dev_sync.py --root /mnt                    # into a mounted card rootfs
    dev_sync.py --dry-run                      # say what would change
"""

from __future__ import annotations

import argparse
import os
import hashlib
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BOARD = "debian@10.42.0.1"
DEFAULT_PASSWORD = "rv"
REMOTE_STAGE = "/tmp/dev-sync"


@dataclass(frozen=True)
class Target:
    """One file: where it comes from, where it goes, and what it needs afterwards."""

    source: str
    destination: str
    mode: int = 0o644
    restart: str = ""
    render: bool = False
    note: str = ""


TARGETS = (
    Target("scripts/addons/bootlog/bootlogd", "/usr/local/sbin/bootlogd", 0o755,
           restart="bootlog-snap.service"),
    Target("scripts/addons/bootlog/bootlog-snap.service",
           "/etc/systemd/system/bootlog-snap.service", 0o644),
    Target("components/guidance-fw/tools/rtos-mode", "/usr/bin/rtos-mode", 0o755),
    Target("components/guidance-fw/tools/cop", "/usr/bin/cop", 0o755),
    Target("components/guidance-fw/linux/systemd/load-systemko.service.d/"
           "20-guidance-fw.conf.in",
           "/etc/systemd/system/load-systemko.service.d/20-guidance-fw.conf",
           0o644, render=True,
           note="takes effect on the next power cycle (there is no stop path)"),
    Target("components/guidance-fw/firmware/guidance_contract.json",
           "/lib/firmware/guidance_contract.json", 0o644,
           note="read per invocation by cop and bootlogd: already in effect"),
    Target("components/guidance-fw/firmware/c906-mcu.elf",
           "/lib/firmware/c906-mcu.elf", 0o644,
           note="takes effect on the next power cycle (there is no stop path)"),
)


def board_settings(board: str) -> dict[str, str]:
    """The board's `KEY=value` settings, read the same way the build reads them."""
    path = REPO_ROOT / "configs" / board / "settings.mk"
    settings: dict[str, str] = {}
    if not path.is_file():
        return settings
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if "=" in line:
            key, value = line.split("=", 1)
            settings[key.strip()] = value.strip().strip('"')
    return settings


def remote_command(mode: str) -> str:
    """The rtos-mode subcommand the drop-in must run.

    `load` means "Linux writes the firmware and starts the core", which rtos-mode spells
    `remoteproc`; `attach` is its own name.  addons/guidance-fw/addon.mk holds the same
    mapping for the image build, and a test keeps the two honest.
    """
    return "attach" if mode == "attach" else "remoteproc"


def render(source: str, settings: dict[str, str]) -> bytes:
    """Substitute the build-time placeholders the image build would substitute."""
    text = (REPO_ROOT / source).read_text(encoding="utf-8")
    command = remote_command(settings.get("GUIDANCE_FW_RPROC_MODE", "load"))
    text = text.replace("@GUIDANCE_FW_RPROC_COMMAND@", command)
    if "@" in text and "@GUIDANCE_FW" in text:
        raise SystemExit(
            "%s still has a build-time placeholder after rendering; update "
            "dev_sync.render() to know about it" % source
        )
    return text.encode("utf-8")


def payload(target: Target, settings: dict[str, str]) -> bytes:
    if target.render:
        return render(target.source, settings)
    return (REPO_ROOT / target.source).read_bytes()


def install_local(target: Target, data: bytes, root: Path, dry_run: bool) -> bool:
    """Write one file under a mounted rootfs; returns True when it changed."""
    destination = root / target.destination.lstrip("/")
    if destination.is_file() and destination.read_bytes() == data:
        return False
    if dry_run:
        print("  would write %s" % destination)
        return True
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(data)
    destination.chmod(target.mode)
    print("  wrote %s" % destination)
    return True


def ssh(command: list[str], password: str) -> subprocess.CompletedProcess:
    if shutil.which("sshpass") is None:
        raise SystemExit("sshpass is required to sync over ssh without a prompt")
    return subprocess.run(
        ["sshpass", "-p", password, *command],
        capture_output=True, text=True, check=False,
    )


def install_remote(board: str, password: str, changes: list[tuple[Target, bytes]],
                   dry_run: bool) -> None:
    if not changes:
        print("nothing changed")
        return
    if dry_run:
        for target, _ in changes:
            print("  would write %s:%s" % (board, target.destination))
        return
    stage = Path(tempfile.mkdtemp(prefix="dev-sync-"))
    try:
        names: list[str] = []
        for index, (target, data) in enumerate(changes):
            local = stage / str(index)
            local.write_bytes(data)
            local.chmod(target.mode)
            names.append(local.name)
        ssh(["ssh", board, "rm", "-rf", REMOTE_STAGE], password)
        ssh(["ssh", board, "mkdir", "-p", REMOTE_STAGE], password)
        copied = ssh(["scp", "-r", str(stage) + "/.", "%s:%s" % (board, REMOTE_STAGE)],
                     password)
        if copied.returncode != 0:
            raise SystemExit("scp failed: %s" % copied.stderr.strip())
        script = ["set -e"]
        for index, (target, _) in enumerate(changes):
            script.append('install -D -m %04o "%s/%d" "%s"'
                          % (target.mode, REMOTE_STAGE, index, target.destination))
            print("  wrote %s:%s" % (board, target.destination))
        script.append("rm -rf %s" % REMOTE_STAGE)
        script.append("systemctl daemon-reload")
        restarts = sorted({t.restart for t, _ in changes if t.restart})
        for unit in restarts:
            script.append("systemctl restart %s" % unit)
            print("  restarted %s" % unit)
        done = ssh(["ssh", board, "sudo -S sh -c '%s'" % "; ".join(script)], password)
        if done.returncode != 0:
            raise SystemExit("remote install failed: %s" % done.stderr.strip())
    finally:
        shutil.rmtree(stage, ignore_errors=True)


def notes(changes: list[tuple[Target, bytes]]) -> None:
    said = sorted({t.note for t, _ in changes if t.note})
    for note in said:
        print("note: %s" % note)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", default=DEFAULT_BOARD)
    parser.add_argument("--board-name", default="maixcam",
                        help="which configs/<name>/settings.mk to read")
    parser.add_argument("--root", default=None,
                        help="install into a mounted card rootfs instead of over ssh")
    parser.add_argument("--password", default=os.environ.get("BOARD_PASSWORD",
                                                             DEFAULT_PASSWORD))
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv[1:])

    settings = board_settings(args.board_name)
    changes: list[tuple[Target, bytes]] = []
    for target in TARGETS:
        source = REPO_ROOT / target.source
        if not source.is_file():
            raise SystemExit("missing source file: %s" % target.source)
        changes.append((target, payload(target, settings)))

    if args.root is not None:
        root = Path(args.root)
        if not (root / "etc").is_dir():
            raise SystemExit("%s does not look like a root filesystem" % root)
        changed = [target for target, data in changes
                   if install_local(target, data, root, args.dry_run)]
        if not changed:
            print("nothing changed")
        notes([(target, b"") for target in changed])
        return 0

    # Only send what differs: ask the board for the hash of each destination first.
    differing = [(t, data) for t, data in changes
                 if remote_differs(args.board, args.password, t, data, args.dry_run)]
    install_remote(args.board, args.password, differing, args.dry_run)
    notes(differing)
    return 0


def remote_differs(board: str, password: str, target: Target, data: bytes,
                   dry_run: bool) -> bool:
    """True when the board's copy is not byte-identical to what we would install."""
    command = "sha256sum '%s' 2>/dev/null || echo missing" % target.destination
    if dry_run:
        return True
    done = ssh(["ssh", board, command], password)
    if done.returncode != 0:
        raise SystemExit("cannot reach %s: %s" % (board, done.stderr.strip()))
    remote = done.stdout.split()[0] if done.stdout.strip() else "missing"
    return remote != hashlib.sha256(data).hexdigest()


if __name__ == "__main__":
    sys.exit(main(sys.argv))

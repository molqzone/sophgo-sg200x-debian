#!/usr/bin/env python3
"""Watch sophgo SG200x SDK builds in one terminal dashboard.

Builds are launched from the host wrapper ``scripts/ci/local-build.sh`` (by a
human or by an agent) and normally redirected to a log file::

    ./scripts/ci/local-build.sh image --board maixcam-sc035hgs --storage sd \
        > /tmp/build.log 2>&1

``buildwatch`` finds those builds by scanning ``/proc`` for the wrapper and the
``docker run`` it spawns, follows each log, and shows the current build stage
(``Building U-Boot for maixcam-sc035hgs``), the command actually running inside
the build container, and that container's CPU/memory.

Usage::

    tools/buildwatch.py                     # auto-discover running builds
    tools/buildwatch.py /tmp/build.log      # watch one or more logs
    tools/buildwatch.py --once              # single snapshot, no curses
    tools/buildwatch.py --once --json       # machine-readable snapshot

Only the Python standard library is used; ``curses`` is optional.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path


ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)")
STAGE_RE = re.compile(
    r"^(Checking out|Patching|Configuring|Building|Packaging|Preparing|Customizing|"
    r"Compiling|Copying Deb files)\s+(.+?)\s+for\s+(?:installation on\s+)?(\S+)\s*$"
)
IMAGE_RE = re.compile(r"^Image for (\S+) is (\S+?\.(?:img|zip))$")
RC_RE = re.compile(r"^build_rc=(\d+)")
PROGRESS_PREFIX_RE = re.compile(r"^(Updating files:|\[)\s*(?P<pct>\d{1,3})%")
PROGRESS_INLINE_RE = re.compile(r"\[\s*(?P<pct>\d{1,3})%\]")
MMDEBSTRAP_RE = re.compile(r"^[IP]: (.*)$")
# Deliberately strict: kernel sources contain identifiers such as GTP_ERROR
# and "0 errors", which must not be counted as build failures.
ERROR_RE = re.compile(
    r"(?:^|[\s\]])error:|(?:^|[\s\]])ERROR[: ]|(?:^|[\s\]])fatal:|"
    r"(?:^|[\s\]])E: [A-Z]|\*\*\* |\bError \d"
)
NOISE_RE = re.compile(r"^(?:Preparing to unpack|Selecting previously|Unpacking |Setting up )")

# Processes worth reporting as "what the build is doing right now", most
# specific first: the first group with a match wins, and within a group the
# last match (usually the deepest child) is shown.
INNER_PRIORITY = (
    ("compile", re.compile(r"\b(?:cc1plus|cc1)\b")),
    ("link", re.compile(r"\b(?:ld|ld\.bfd|ld\.gold|collect2)\b")),
    ("assemble", re.compile(r"\b(?:as|gcc|g\+\+|clang)\b")),
    ("objcopy", re.compile(r"\b(?:objcopy|objdump|strip|ranlib|ar)\b")),
    ("fit", re.compile(r"\b(?:mkimage|mkimage_fit_atf|fiptool|dtc)\b")),
    ("package", re.compile(r"\b(?:dpkg-deb|dpkg|apt|mmdebstrap|fakeroot)\b")),
    ("chroot", re.compile(r"\b(?:chroot|qemu-riscv64(?:-static)?)\b")),
    ("fetch", re.compile(r"\b(?:git|wget|curl|rsync)\b")),
    ("archive", re.compile(r"\b(?:tar|cpio|gzip|xz|zstd)\b")),
    ("configure", re.compile(r"\b(?:cmake|ninja|meson|autoconf|configure)\b")),
    ("make", re.compile(r"\bmake\b")),
    ("python", re.compile(r"\bpython3?\b")),
)

TAIL_SKIP_RE = re.compile(r"^(?:Updating files:|[\s]*$)")


def strip_ansi(text: str) -> str:
    return ANSI_RE.sub("", text)


def run(cmd: list[str], timeout: float = 5.0) -> str:
    """Run a helper command, returning stdout ("" when it fails or is absent)."""
    try:
        proc = subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout, check=False
        )
    except (OSError, subprocess.SubprocessError):
        return ""
    return proc.stdout or ""


def docker_available() -> bool:
    return bool(run(["docker", "version", "--format", "{{.Server.Version}}"]).strip())


class BuildLog:
    """Incremental follower for one redirected build log."""

    def __init__(self, path: str | Path, name: str | None = None) -> None:
        self.path = Path(path)
        self.name = name or self.path.name
        self._offset = 0
        self._carry = ""
        self.stage: str | None = None
        self.stage_component: str | None = None
        self.board: str | None = None
        self.stage_since: float | None = None
        self.stage_count = 0
        self.progress: tuple[str, int] | None = None
        self.artifact: str | None = None
        self.rc: int | None = None
        self.errors: deque[str] = deque(maxlen=5)
        self.error_count = 0
        self.tail: deque[str] = deque(maxlen=4)
        self.mmdebstrap: str | None = None
        self.size = 0
        self.mtime: float | None = None
        self.missing = False

    def _reset(self) -> None:
        self._offset = 0
        self._carry = ""

    def poll(self) -> None:
        try:
            stat = self.path.stat()
            size = stat.st_size
            self.mtime = stat.st_mtime
            self.missing = False
        except OSError:
            self.missing = True
            return
        if size < self._offset:
            self._reset()
        self.size = size
        if size == self._offset:
            return
        try:
            with self.path.open("rb") as handle:
                handle.seek(self._offset)
                chunk = handle.read(size - self._offset)
        except OSError:
            self._reset()
            return
        self._offset += len(chunk)
        text = self._carry + chunk.decode("utf-8", "replace")
        lines = text.replace("\r", "\n").split("\n")
        self._carry = lines.pop()
        for line in lines:
            self._handle(line)

    def _handle(self, raw: str) -> None:
        line = strip_ansi(raw).rstrip()
        if not line:
            return

        match = STAGE_RE.match(line)
        if match:
            verb, component, board = match.groups()
            self.stage = f"{verb} {component} for {board}"
            self.stage_component = component
            self.board = board
            self.stage_since = time.monotonic()
            self.stage_count += 1
            self.progress = None
            self.tail.append(line)
            return

        image = IMAGE_RE.match(line)
        if image:
            self.artifact = image.group(2)
            self.tail.append(line)
            return

        rc = RC_RE.match(line)
        if rc:
            self.rc = int(rc.group(1))
            return

        progress = PROGRESS_PREFIX_RE.match(line) or PROGRESS_INLINE_RE.search(line)
        if progress:
            if progress.start() == 0:
                label = progress.group(1)
            else:
                label = line[: progress.start()].strip()
            label = label.rstrip(":").strip()
            if not label or label.startswith("["):
                label = "build"
            elif len(label) > 24:
                label = label.split()[0]
            self.progress = (label, int(progress.group("pct")))

        mmdeb = MMDEBSTRAP_RE.match(line)
        if mmdeb:
            self.mmdebstrap = mmdeb.group(1).strip()

        if ERROR_RE.search(line):
            self.error_count += 1
            self.errors.append(line)

        if not NOISE_RE.match(line) and not TAIL_SKIP_RE.match(line):
            self.tail.append(line)


def _pids() -> list[int]:
    try:
        return sorted(int(e) for e in os.listdir("/proc") if e.isdigit())
    except OSError:
        return []


def _cmdline(pid: int) -> str:
    try:
        raw = Path(f"/proc/{pid}/cmdline").read_bytes()
    except OSError:
        return ""
    return raw.replace(b"\0", b" ").decode("utf-8", "replace").strip()


def _opt(cmd: str, flag: str) -> str | None:
    match = re.search(rf"{re.escape(flag)}[=\s]+(\S+)", cmd)
    return match.group(1) if match else None


def proc_started_wall(pid: int) -> float | None:
    """Wall-clock start time of a process, or None when it is gone."""
    try:
        stat = Path(f"/proc/{pid}/stat").read_text()
    except OSError:
        return None
    try:
        # field 22 (1-based) is starttime; comm (field 2) may contain spaces
        tail = stat.rsplit(")", 1)[1].split()
        starttime = float(tail[19])
        hz = os.sysconf("SC_CLK_TCK")
        with open("/proc/stat") as fh:  # noqa: PTH123 - fixed kernel path
            for line in fh:
                if line.startswith("btime "):
                    return float(line.split()[1]) + starttime / hz
    except (OSError, ValueError, IndexError):
        return None
    return None


def descendants(root: int) -> list[int]:
    """All live descendant pids of ``root`` (deepest last)."""
    parents: dict[int, int] = {}
    for pid in _pids():
        try:
            stat = Path(f"/proc/{pid}/stat").read_text()
            ppid = int(stat.rsplit(")", 1)[1].split()[1])
        except (OSError, ValueError, IndexError):
            continue
        parents[pid] = ppid
    out: list[int] = []
    frontier = [root]
    while frontier:
        current = frontier.pop()
        for pid, ppid in parents.items():
            if ppid == current and pid not in out and pid != root:
                out.append(pid)
                frontier.append(pid)
    return out


def pick_inner(commands: list[str]) -> str | None:
    """Choose the most representative running command from a process list."""
    for _label, pattern in INNER_PRIORITY:
        hits = [cmd for cmd in commands if pattern.search(cmd)]
        if hits:
            return hits[-1]
    return None


@dataclass
class Build:
    """One build: a log plus whatever could be learned about its process."""

    log: BuildLog
    target: str = "?"
    board: str = "?"
    storage: str = "?"
    repo: str | None = None
    pid: int | None = None
    container: str | None = None
    host_cmd: str = ""
    started_wall: float | None = None
    seen_at: float = field(default_factory=time.monotonic)

    @property
    def key(self) -> str:
        return str(self.log.path)


def _parse_build_cmd(cmd: str) -> dict[str, str | None]:
    target = re.search(r"local-build\.sh\s+([A-Za-z0-9_.-]+)", cmd)
    redirect = re.search(r">>?\s*(/\S+\.log)", cmd)
    repo = re.search(r"-v\s+(/\S+?):/workspace(?::ro)?(?=\s|$)", cmd) or re.search(
        r"cd\s+(/\S+)\s*&&", cmd
    )
    return {
        "target": target.group(1) if target else None,
        "board": _opt(cmd, "--board"),
        "storage": _opt(cmd, "--storage"),
        "log": redirect.group(1) if redirect else None,
        "repo": repo.group(1) if repo else None,
    }


def discover_builds() -> list[dict[str, str | int | None]]:
    """Find builds from host processes mentioning local-build.sh.

    A build shows up twice: the host wrapper (``bash -c 'cd <repo> &&
    ./scripts/ci/local-build.sh <target> ... > log'``) and the ``docker run`` it
    spawns (``-v <repo>:/workspace ... local-build.sh <target> --inside ...``).
    They are merged into one entry per (target, board, repo); the wrapper keeps
    the lowest pid, which is the one the log redirect belongs to.
    """
    entries: list[dict[str, str | int | None]] = []
    for pid in _pids():
        cmd = _cmdline(pid)
        if "local-build.sh" not in cmd:
            continue
        info = _parse_build_cmd(cmd)
        identity = (info["target"] or "?", info["board"] or "?", info["repo"])
        for entry in entries:
            if (entry["target"], entry["board"], entry["repo"]) == identity:
                for name, value in info.items():
                    if value and not entry.get(name):
                        entry[name] = value
                break
        else:
            entries.append(
                {
                    "log": info["log"],
                    "target": info["target"] or "?",
                    "board": info["board"],
                    "storage": info["storage"],
                    "repo": info["repo"],
                    "pid": pid,
                }
            )
    return [entry for entry in entries if entry.get("log")]


def docker_containers() -> list[tuple[str, str, str]]:
    """(id, name, command) for running containers."""
    out = run(
        ["docker", "ps", "--no-trunc", "--format", "{{.ID}}\t{{.Names}}\t{{.Command}}"]
    )
    rows = []
    for line in out.splitlines():
        parts = line.split("\t", 2)
        if len(parts) == 3:
            rows.append((parts[0], parts[1], parts[2]))
    return rows


def container_for(target: str, board: str | None) -> tuple[str, str] | None:
    for cid, name, cmd in docker_containers():
        if "local-build.sh" not in cmd:
            continue
        if target != "?" and f"local-build.sh {target}" not in cmd:
            continue
        if board and f"--board {board}" not in cmd:
            continue
        return cid, name
    return None


def container_stats(cid: str) -> tuple[str, str]:
    out = run(["docker", "stats", "--no-stream", "--format", "{{.CPUPerc}}\t{{.MemUsage}}", cid])
    parts = out.strip().split("\t")
    if len(parts) == 2:
        return parts[0].strip(), parts[1].strip()
    return "?", "?"


def parse_top(output: str) -> list[str]:
    """Commands from ``docker top`` output (ps-style header plus rows)."""
    lines = output.splitlines()
    if len(lines) < 2:
        return []
    columns = len(lines[0].split())
    commands = []
    for line in lines[1:]:
        parts = line.split(None, columns)
        # maxsplit yields one extra element holding the rest of the CMD column
        if len(parts) == columns + 1:
            commands.append(" ".join(parts[columns - 1 :]))
    return commands


def container_inner(cid: str) -> str | None:
    return pick_inner(parse_top(run(["docker", "top", cid])))


def host_inner(pid: int | None) -> str | None:
    if not pid:
        return None
    commands = []
    for child in descendants(pid):
        cmd = _cmdline(child)
        if cmd:
            commands.append(cmd)
    return pick_inner(commands)


def fallback_logs(limit: int = 4) -> list[Path]:
    """Most recently modified /tmp logs that look like SDK build logs."""
    candidates = []
    for path in Path("/tmp").glob("*.log"):
        try:
            if not path.is_file():
                continue
            stat = path.stat()
            if time.time() - stat.st_mtime > 7 * 86400 or stat.st_size > 8_000_000:
                continue
            with path.open("rb") as fh:
                text = strip_ansi(fh.read().decode("utf-8", "replace"))
        except OSError:
            continue
        if (
            "local-build.sh" in text
            or "build_rc=" in text
            or STAGE_RE.search(text)
            or MMDEBSTRAP_RE.search(text)
        ):
            candidates.append((stat.st_mtime, path))
    candidates.sort(reverse=True)
    return [path for _mtime, path in candidates[:limit]]


def _shorten(text: str, limit: int = 96) -> str:
    """Collapse whitespace and clip long log lines (mmdebstrap --include=...)."""
    text = " ".join(text.split())
    if len(text) <= limit:
        return text
    return text[: limit - 1].rstrip() + "…"


def _ended_seconds(log: BuildLog, build: Build) -> float | None:
    """Seconds since a finished build's log was last written (no pid known)."""
    if log.rc is None or build.started_wall is not None:
        return None
    try:
        return round(time.time() - log.path.stat().st_mtime, 1)
    except OSError:
        return None


def _fmt_duration(seconds: float | None) -> str:
    if seconds is None:
        return "-"
    seconds = max(0, int(seconds))
    hours, rem = divmod(seconds, 3600)
    minutes, secs = divmod(rem, 60)
    if hours:
        return f"{hours}h{minutes:02d}m"
    if minutes:
        return f"{minutes}m{secs:02d}s"
    return f"{secs}s"


def build_state(build: Build) -> str:
    log = build.log
    if log.rc is not None:
        return "ok" if log.rc == 0 else "failed"
    if log.artifact:
        return "finishing"
    if log.missing or not log.path.exists():
        return "missing"
    if log.mtime is not None and time.time() - log.mtime > 300:
        return "quiet"
    return "running"


def snapshot(builds: list[Build], use_docker: bool = True) -> list[dict]:
    """Collect the renderable state of every build."""
    rows = []
    for build in builds:
        log = build.log
        state = build_state(build)
        board = build.board if build.board != "?" else (log.board or "?")

        if build.pid:
            build.started_wall = proc_started_wall(build.pid) or build.started_wall
        elapsed = (
            time.time() - build.started_wall
            if build.started_wall
            else time.monotonic() - build.seen_at
        )

        inner = None
        cpu = mem = None
        if use_docker and build.container:
            cid, _name = build.container
            cpu, mem = container_stats(cid)
            inner = container_inner(cid)
        if inner is None:
            inner = host_inner(build.pid)

        rows.append(
            {
                "name": log.name,
                "log": str(log.path),
                "state": state,
                "target": build.target,
                "board": board,
                "storage": build.storage,
                "repo": build.repo,
                "elapsed_s": round(elapsed, 1),
                "pid": build.pid,
                "ended_s": _ended_seconds(log, build),
                "stage": log.stage,
                "stage_elapsed_s": (
                    round(time.monotonic() - log.stage_since, 1)
                    if log.stage_since
                    else None
                ),
                "stage_count": log.stage_count,
                "progress": log.progress,
                "inner": inner,
                "container": build.container[1] if build.container else None,
                "cpu": cpu,
                "mem": mem,
                "size_kb": round(log.size / 1024, 1),
                "last_write_s": (
                    round(time.time() - log.mtime, 1) if log.mtime is not None else None
                ),
                "artifact": log.artifact,
                "rc": log.rc,
                "mmdebstrap": log.mmdebstrap,
                "error_count": log.error_count,
                "errors": list(log.errors),
                "tail": list(log.tail),
            }
        )
    return rows


def poll_all(builds: list[Build]) -> None:
    for build in builds:
        build.log.poll()


def collect_builds(args: argparse.Namespace, use_docker: bool) -> list[Build]:
    builds: list[Build] = []
    seen: set[str] = set()

    def add(path: str, **meta) -> None:
        if path in seen:
            return
        seen.add(path)
        builds.append(Build(log=BuildLog(path), **meta))

    if args.logs:
        for path in args.logs:
            add(path)
    else:
        for info in discover_builds():
            if info.get("log"):
                add(
                    str(info["log"]),
                    target=str(info.get("target") or "?"),
                    board=str(info.get("board") or "?"),
                    storage=str(info.get("storage") or "?"),
                    repo=info.get("repo"),
                    pid=info.get("pid"),
                )
        if not builds:
            for path in fallback_logs():
                add(str(path))

    if use_docker:
        for build in builds:
            if build.container is None:
                build.container = container_for(build.target, None if build.board == "?" else build.board)
    return builds


STATE_LABEL = {
    "running": ("RUN", "green"),
    "quiet": ("QUIET", "yellow"),
    "finishing": ("DONE", "cyan"),
    "ok": ("OK", "cyan"),
    "failed": ("FAIL", "red"),
    "missing": ("GONE", "red"),
}


def build_rows(row: dict) -> list[tuple[str, str]]:
    """(style, text) lines describing one build."""
    label, colour = STATE_LABEL.get(row["state"], ("?", "white"))
    job = f"{row['target']}/{row['storage']} {row['board']}"
    head = f"{label} {row['name']}  {job}"
    if row["repo"]:
        head += f"  [{Path(row['repo']).name}]"
    if row["state"] in ("ok", "failed", "finishing") and row["ended_s"] is not None:
        head += f"  finished {_fmt_duration(row['ended_s'])} ago"
    else:
        head += f"  {_fmt_duration(row['elapsed_s'])}"
    rows: list[tuple[str, str]] = [(colour, head)]

    if row["stage"]:
        stage = f"  stage   {row['stage']}"
        if row["stage_elapsed_s"] is not None and row["state"] in ("running", "quiet"):
            stage += f"   ({_fmt_duration(row['stage_elapsed_s'])} in stage)"
        rows.append(("bold", stage))

    inner = row["inner"] or "-"
    if row["container"]:
        extra = f"  [{row['container']}"
        if row["cpu"]:
            extra += f" {row['cpu']} cpu"
        if row["mem"]:
            extra += f" {row['mem'].split('/')[0].strip()} mem"
        extra += "]"
        rows.append(("normal", f"  inner   {_shorten(inner)}{extra}"))
    else:
        rows.append(("normal", f"  inner   {_shorten(inner)}"))

    if row["progress"]:
        label_, percent = row["progress"]
        rows.append(("normal", f"  progress {label_} {percent}%"))

    meta = f"{row['size_kb']} KB"
    if row["last_write_s"] is not None:
        meta += f" · wrote {_fmt_duration(row['last_write_s'])} ago"
    if row["mmdebstrap"]:
        meta += f" · {_shorten(row['mmdebstrap'], 60)}"
    if row["stage_count"]:
        meta += f" · {row['stage_count']} stage" + ("s" if row["stage_count"] != 1 else "")
    rows.append(("dim", f"  log     {meta}"))

    if row["state"] == "finishing" and row["artifact"]:
        rows.append(("cyan", f"  output  {row['artifact']}"))
    if row["rc"] is not None:
        style = "green" if row["rc"] == 0 else "red"
        rows.append((style, f"  build_rc={row['rc']}"))
    if row["error_count"]:
        # A finished build can still carry "(ignored)" make noise; only treat it
        # as a failure signal while the build is running or has failed.
        style = "red" if row["state"] in ("running", "quiet", "failed") else "yellow"
        word = "errors" if style == "red" else "noise "
        rows.append((style, f"  {word}  {row['error_count']}: {_shorten(row['errors'][-1])}"))
    elif row["tail"]:
        rows.append(("dim", f"  last    {_shorten(row['tail'][-1])}"))
    return rows


def render_text(rows: list[dict]) -> str:
    out = [f"buildwatch — {time.strftime('%H:%M:%S')} — {len(rows)} build(s)"]
    for row in rows:
        out.append("-" * 72)
        for _style, text in build_rows(row):
            out.append(text)
    if not rows:
        out.append("no builds found (pass a log path, or start one with local-build.sh)")
    return "\n".join(out)


def render_json(rows: list[dict]) -> str:
    return json.dumps({"generated": time.time(), "builds": rows}, indent=2)


def curses_main(args: argparse.Namespace, use_docker: bool) -> int:
    import curses

    builds = collect_builds(args, use_docker)
    colours = {
        "normal": 0,
        "dim": curses.A_DIM,
        "bold": curses.A_BOLD,
    }

    def loop(stdscr) -> None:
        curses.curs_set(0)
        stdscr.timeout(int(args.interval * 1000))
        pairs = {}
        for index, name in enumerate(("green", "yellow", "cyan", "red", "white"), start=1):
            base = {
                "green": curses.COLOR_GREEN,
                "yellow": curses.COLOR_YELLOW,
                "cyan": curses.COLOR_CYAN,
                "red": curses.COLOR_RED,
                "white": curses.COLOR_WHITE,
            }[name]
            try:
                curses.init_pair(index, base, -1)
            except curses.error:
                continue
            pairs[name] = curses.color_pair(index)
        colours.update(pairs)

        while True:
            poll_all(builds)
            rows = snapshot(builds, use_docker)
            stdscr.erase()
            height, width = stdscr.getmaxyx()
            line = 0

            def put(text: str, style: str = "normal") -> None:
                nonlocal line
                if line >= height - 1:
                    return
                try:
                    stdscr.addnstr(line, 0, text, max(0, width - 1), colours.get(style, 0))
                except curses.error:
                    pass
                line += 1

            put(
                f"buildwatch {time.strftime('%H:%M:%S')}  ·  {len(rows)} build(s)  "
                f"·  refresh {args.interval:g}s  ·  q quit",
                "bold",
            )
            for row in rows:
                for style, text in build_rows(row):
                    put(text, style)
                put("")
            if not rows:
                put("no builds found — pass a log path or start local-build.sh", "dim")
            stdscr.refresh()
            key = stdscr.getch()
            if key in (ord("q"), ord("Q")):
                return

    curses.wrapper(loop)
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Watch sophgo SG200x SDK builds (stages, inner command, container stats).",
    )
    parser.add_argument("logs", nargs="*", help="build log paths (default: auto-discover)")
    parser.add_argument("--interval", type=float, default=2.0, help="refresh seconds (default 2)")
    parser.add_argument("--once", action="store_true", help="print one snapshot and exit")
    parser.add_argument("--json", action="store_true", help="with --once, emit JSON")
    parser.add_argument("--no-docker", action="store_true", help="skip container stats")
    args = parser.parse_args(argv)

    use_docker = not args.no_docker and docker_available()
    builds = collect_builds(args, use_docker)
    if args.once:
        poll_all(builds)
        rows = snapshot(builds, use_docker)
        print(render_json(rows) if args.json else render_text(rows))
        return 0
    if not sys.stdout.isatty():
        poll_all(builds)
        rows = snapshot(builds, use_docker)
        print(render_text(rows))
        return 0
    return curses_main(args, use_docker)


if __name__ == "__main__":
    raise SystemExit(main())

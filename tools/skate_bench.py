#!/usr/bin/env python3
"""The dedicated server's whole-game CPU benchmark: runs a built server
(HALO_DEDICATED=1) with 128 bots on a fixed map and fixed seeds, measures
CPU time per game tick per thread group over a window after a warm-up, and
keeps the run's log (with debug.skate_fingerprint's hashes, port/skate's
real_assets_fingerprint.rs) for comparing two builds.

    python tools/skate_bench.py GAME OUT [--warmup S] [--seconds S] [--env K=V ...]

Run it against two builds (skate_perf_bench.py build-server's --output, or
any halo binary with --skate and dedicated-server support built in) to see
what a change costs in the whole game, not only the fingerprint test's
synthetic crowd.

With --profile-seconds, once the window's warm-up marker appears in the
log, it also attaches `perf record -g` to the game's process for that many
seconds (port/skate/README.md's method, profile.sh's one-off version of
this), which needs a frame-pointer build of the binary
(skate_perf_bench.py build-server --frame-pointers) to get a readable call
graph.

The window's grouping and summary (which thread group spent what, how many
ticks the window covers) are plain functions with no side effects - see
tools/test_skate_bench.py. Starting the game, reading /proc, and running
perf are the only parts that touch the outside world, and a non-zero exit
from the game or from perf is raised, not swallowed.
"""

from __future__ import annotations

import argparse
import dataclasses
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

REPORT = re.compile(
    r"skate: the bots. skaters: (\d+) skating on average, the game waited ([\d.]+) ms a "
    r"tick.*?the game played ([\d.]+) ticks a second"
)

DEFAULT_WARMUP = 60.0
DEFAULT_SECONDS = 60.0
DEFAULT_MAP = "bloodgulch:infection"
DEFAULT_PERF_FREQ = 299


class CommandFailed(RuntimeError):
    pass


# --------------------------------------------------------------------------
# pure functions

@dataclasses.dataclass(frozen=True)
class SkateReport:
    skating: int
    waited_ms: float
    ticks_per_second: float


def parse_reports(text: str) -> List[SkateReport]:
    """Every "skate: the bots' skaters: ..." summary line (skate.c, once
    every 30 seconds) in a slice of the run's log."""
    return [SkateReport(int(s), float(w), float(t)) for s, w, t in REPORT.findall(text)]


def estimate_ticks_per_second(reports: Sequence[SkateReport], default: float = 30.0) -> float:
    """The window's average tick rate from the reports seen in it, or a
    default (the game's usual 30 Hz) if none landed in the window."""
    if not reports:
        return default
    return sum(r.ticks_per_second for r in reports) / len(reports)


def thread_cpu_seconds(stat_fields: Sequence[str], hz: int) -> float:
    """utime + stime (fields 14 and 15 of /proc/<pid>/task/<tid>/stat,
    0-indexed from the first field after the closing ')'), in seconds."""
    return (int(stat_fields[11]) + int(stat_fields[12])) / hz


def thread_group_name(comm: str) -> str:
    """Collapses a pool worker's thread name (e.g. "skate_pool_3") into its
    group ("skate_pool_N"), so the summary has one line per kind of thread,
    not one per worker."""
    return re.sub(r"\d+$", "N", comm)


def group_thread_cpu(before: Dict[str, Tuple[str, float]], after: Dict[str, Tuple[str, float]]) -> Dict[str, float]:
    """CPU seconds spent by each thread group between two /proc snapshots
    (tid -> (comm, cpu_seconds)). A thread that exited mid-window is counted
    from 0 (it is new to `after` relative to `before`'s view of it, not to
    the process); a thread only in `before` contributes nothing further."""
    groups: Dict[str, float] = {}
    for tid, (comm, cpu) in after.items():
        before_cpu = before.get(tid, (comm, 0.0))[1]
        name = thread_group_name(comm)
        groups[name] = groups.get(name, 0.0) + cpu - before_cpu
    return groups


@dataclasses.dataclass(frozen=True)
class BenchSummary:
    window_seconds: float
    reports: List[SkateReport]
    ticks: float
    total_cpu_seconds: float
    groups: Dict[str, float]  # name -> cpu seconds, descending


def summarize(window_seconds: float, reports: Sequence[SkateReport], groups: Dict[str, float]) -> BenchSummary:
    rate = estimate_ticks_per_second(reports)
    ticks = rate * window_seconds
    total = sum(groups.values())
    ordered = dict(sorted(groups.items(), key=lambda kv: -kv[1]))
    return BenchSummary(window_seconds, list(reports), ticks, total, ordered)


def format_summary(summary: BenchSummary) -> str:
    lines = [
        f"window {summary.window_seconds:.1f}s; reports in it: "
        f"{[(r.skating, r.waited_ms, r.ticks_per_second) for r in summary.reports]} "
        "(skating, waited ms, ticks/s)",
        f"total {summary.total_cpu_seconds:.2f} cpu-s = "
        f"{summary.total_cpu_seconds / summary.window_seconds:.3f} cores, "
        f"{summary.total_cpu_seconds / summary.ticks * 1000:.3f} cpu-ms/tick" if summary.ticks else
        f"total {summary.total_cpu_seconds:.2f} cpu-s (no ticks observed)",
    ]
    for name, cpu in summary.groups.items():
        if cpu > 0.005:
            per_tick = cpu / summary.ticks * 1000 if summary.ticks else float("nan")
            lines.append(f"  {name:24s} {cpu:8.2f} cpu-s  {per_tick:7.3f} ms/tick")
    return "\n".join(lines)


def parse_env_overrides(pairs: Sequence[str]) -> Dict[str, str]:
    out = {}
    for pair in pairs:
        key, _, value = pair.partition("=")
        if not key or "=" not in pair:
            raise ValueError(f"--env wants KEY=VALUE, got {pair!r}")
        out[key] = value
    return out


def server_env(base_env: Dict[str, str], out: Path, map_name: str, overrides: Dict[str, str]) -> Dict[str, str]:
    """The dedicated server's fixed-seed environment: no SDL_/HALO_ passed
    through from the caller's own shell, so a stray HALO_DATA_ROOT etc. from
    the dev container can't leak in."""
    env = {k: v for k, v in base_env.items() if not k.startswith(("SDL_", "HALO_"))}
    env.update(
        HALO_DEDICATED="1",
        HALO_DATA_ROOT=str(out / "data"),
        HALO_SAVE_ROOT=str(out / "saves"),
        HALO_SKATE_ASSETS="/skate-assets/skate-data/assets",
        HALO_NET_ONLINE="false",
        HALO_UPDATE_ANSWER="never",
        HALO_SERVER_BOTS="128",
        HALO_SERVER_BOTS_FILL="true",
        HALO_SERVER_MAPS=map_name,
        HALO_SERVER_TIME_LIMIT="15",
        HALO_GAME_SEED="1011",
        HALO_BOT_SEED="11",
        HALO_SKATE_BOT_PERIOD="1",
    )
    env.update(overrides)
    return env


# --------------------------------------------------------------------------
# the run: starting the game, reading /proc, optionally attaching perf

def run(cmd: Sequence[str], **kwargs) -> subprocess.CompletedProcess:
    try:
        return subprocess.run(cmd, check=True, **kwargs)
    except subprocess.CalledProcessError as e:
        raise CommandFailed(f"command failed ({e.returncode}): {' '.join(cmd)}") from e
    except FileNotFoundError as e:
        raise CommandFailed(f"command not found: {cmd[0]}") from e


def prepare_out_dir(out: Path) -> None:
    shutil.rmtree(out, ignore_errors=True)
    (out / "data").mkdir(parents=True)
    (out / "data" / "maps").symlink_to("/skate-assets/halo/maps")
    template = Path("/src/bench-saves-template")
    if template.is_dir():
        shutil.copytree(template, out / "saves")
    else:
        (out / "saves").mkdir()


def read_thread_cpu(pid: int, hz: int) -> Dict[str, Tuple[str, float]]:
    got: Dict[str, Tuple[str, float]] = {}
    task_dir = Path(f"/proc/{pid}/task")
    if not task_dir.is_dir():
        return got
    for entry in task_dir.iterdir():
        try:
            stat = (entry / "stat").read_text()
        except OSError:
            continue
        comm = stat[stat.index("(") + 1 : stat.rindex(")")]
        fields = stat[stat.rindex(")") + 2 :].split()
        got[entry.name] = (comm, thread_cpu_seconds(fields, hz))
    return got


def attach_perf_record(pid: int, seconds: float, out_data: Path, freq: int) -> None:
    """perf record -g of `pid` for `seconds`. Raises on a non-zero exit
    (profile.sh's version let this fail silently in the background)."""
    run(["perf", "record", "-F", str(freq), "-g", "-p", str(pid), "-o", str(out_data), "--", "sleep", str(seconds)])


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("game", help="path to the built dedicated-server binary")
    parser.add_argument("out", help="a run directory to create (removed first if it exists)")
    parser.add_argument("--warmup", type=float, default=DEFAULT_WARMUP)
    parser.add_argument("--seconds", type=float, default=DEFAULT_SECONDS)
    parser.add_argument("--map", default=DEFAULT_MAP)
    parser.add_argument("--env", action="append", default=[], help="KEY=VALUE, overrides the fixed server env")
    parser.add_argument("--profile-seconds", type=float, default=None, dest="profile_seconds",
                         help="once the window starts, perf record -g the game for this many seconds")
    parser.add_argument("--profile-out", default=None, dest="profile_out",
                         help="perf record's output file (default: OUT.data)")
    parser.add_argument("--profile-freq", type=int, default=DEFAULT_PERF_FREQ, dest="profile_freq")
    parser.add_argument("--start-marker", default="network test: game 1", dest="start_marker",
                         help="the log line that means the game has started (end of warm-up wait)")
    parser.add_argument("--start-timeout", type=float, default=300.0, dest="start_timeout")
    args = parser.parse_args(argv)

    out = Path(args.out)
    try:
        overrides = parse_env_overrides(args.env)
        prepare_out_dir(out)
    except (OSError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    env = server_env(dict(os.environ), out, args.map, overrides)
    log_path = out / "run.log"
    log = open(log_path, "w")
    game = subprocess.Popen([args.game], cwd=out, env=env, stdout=log, stderr=subprocess.STDOUT)
    hz = os.sysconf("SC_CLK_TCK")

    def text() -> str:
        return log_path.read_text(errors="replace")

    try:
        start = time.time()
        while args.start_marker not in text():
            if game.poll() is not None:
                print(f"error: the game exited ({game.returncode}) before it started", file=sys.stderr)
                return 1
            if time.time() - start > args.start_timeout:
                print("error: the game never started", file=sys.stderr)
                return 1
            time.sleep(1)
        time.sleep(args.warmup)
        before_len, before_time, before_cpu = len(text()), time.time(), read_thread_cpu(game.pid, hz)
        if args.profile_seconds:
            out_data = Path(args.profile_out) if args.profile_out else out.with_suffix(".data")
            attach_perf_record(game.pid, args.profile_seconds, out_data, args.profile_freq)
            print(f"recorded {out_data}")
        time.sleep(max(0.0, args.seconds - (args.profile_seconds or 0.0)))
        after_len, after_time, after_cpu = len(text()), time.time(), read_thread_cpu(game.pid, hz)
    except CommandFailed as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    finally:
        game.send_signal(signal.SIGTERM)
        try:
            game.wait(15)
        except subprocess.TimeoutExpired:
            game.kill()
        log.close()

    groups = group_thread_cpu(before_cpu, after_cpu)
    reports = parse_reports(text()[before_len:after_len])
    summary = summarize(after_time - before_time, reports, groups)
    print(format_summary(summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())

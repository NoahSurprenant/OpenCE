#!/usr/bin/env python3
"""Measures the skate engine's dedicated-server cost with the real assets,
in the perf-bench dev containers (not CI): builds and runs
halo-skate/tests/real_assets_fingerprint.rs under `perf stat`/`perf record`,
compares a change's stepping cost against a baseline over several rounds,
and builds a frame-pointer copy of the server binary for profiling.

Four things, one tool each:

    python tools/skate_perf_bench.py sync --repo DIR --container NAME
    python tools/skate_perf_bench.py fingerprint --container NAME [--perf stat|record]
    python tools/skate_perf_bench.py step --container NAME --ref REF --files F [F ...]
    python tools/skate_perf_bench.py build-server --container NAME --label NAME [--frame-pointers]

All four assume the engine's source is already synced into the container
(`sync`, or your own `docker cp`/bind mount) at --src (default /src), and
the real Skate 3 assets are mounted under --assets-root. See
port/skate/README.md, "Cheaper steps must ride the same" for what the
fingerprint test measures and why instruction counts (not cycles or wall
time) are the reliable number on a shared, CPU-capped build host.

The parsing and arithmetic (perf stat output, the fingerprint hash and its
timing line, stepping deltas, percent changes, which binary is newest, which
files need syncing, rewriting a ninja link command for a profiling relink)
are plain functions with no side effects - see tools/test_skate_perf_bench.py.
Everything that runs a command fails loudly: every subprocess call checks
its exit code, and the shell fragments this module generates use `set -e`
rather than the `grep ... || true` style that let earlier, one-off versions
of these scripts swallow a build failure.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import re
import subprocess
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

DEFAULT_CONTAINER = "opence-build"
DEFAULT_SRC = "/src"
DEFAULT_ASSETS = "/skate-assets/skate-data/assets"
DEFAULT_MAPS = "/skate-assets/halo/maps"
DEFAULT_TARGET = "i686-unknown-linux-gnu"
DEFAULT_CPU = "x86-64-v3"
DEFAULT_LTO = "false"
DEFAULT_CGU = "16"
DEFAULT_TICKS = 600
# the CI hash at 600 ticks, 12 riders, fork main: a quick sanity check, not
# a default assertion (HALO_SKATE_TEST_FINGERPRINT is left for the caller)
CI_FINGERPRINT_600 = "d4685a26c8358646"


class CommandFailed(RuntimeError):
    """A command run in a container or on the host exited non-zero."""


# --------------------------------------------------------------------------
# pure functions: parsing, arithmetic, and the shell/ninja text this module
# generates. None of these touch the filesystem, docker, or a subprocess.

FINGERPRINT_LINE = re.compile(
    r"fingerprint (?P<hash>[0-9a-f]{16}): (?P<riders>\d+) riders, (?P<ticks>\d+) ticks, "
    r"(?P<held_hits>\d+) held hits told, (?P<bails>\d+) bails given, (?P<recovered>\d+) steps recovered"
)
TICK_LINE = re.compile(r"^a tick of .*$", re.MULTILINE)
TEST_RESULT_LINE = re.compile(r"^test result: .*$", re.MULTILINE)


@dataclasses.dataclass(frozen=True)
class FingerprintResult:
    """What a run of real_assets_fingerprint.rs printed."""

    hash: Optional[str]
    riders: Optional[int]
    ticks: Optional[int]
    held_hits: Optional[int]
    tick_line: Optional[str]
    test_result: Optional[str]
    panicked: bool
    ok: bool


def parse_fingerprint_log(text: str) -> FingerprintResult:
    """Pulls the fingerprint hash, the per-tick timing line, and the test
    outcome out of a real_assets_fingerprint run's combined stdout+stderr.
    `ok` is true only when a test result line says it passed and nothing
    panicked - a missing fingerprint line (the test skipped, no assets) is
    not `ok` either."""
    m = FINGERPRINT_LINE.search(text)
    tick_m = TICK_LINE.search(text)
    result_m = TEST_RESULT_LINE.search(text)
    panicked = "panicked" in text
    result = result_m.group(0) if result_m else None
    ok = bool(m) and not panicked and result is not None and "ok" in result and "0 failed" in result
    return FingerprintResult(
        hash=m.group("hash") if m else None,
        riders=int(m.group("riders")) if m else None,
        ticks=int(m.group("ticks")) if m else None,
        held_hits=int(m.group("held_hits")) if m else None,
        tick_line=tick_m.group(0) if tick_m else None,
        test_result=result,
        panicked=panicked,
        ok=ok,
    )


def parse_perf_stat(text: str) -> Dict[str, float]:
    """Parses `perf stat -x, -o FILE ...`'s output: comment lines start with
    '#', real lines are comma-separated with the counter's value first and
    its event name third (`man perf-stat`, -x). Returns {event: value},
    keyed by the event exactly as perf printed it (e.g. "instructions:u")."""
    out: Dict[str, float] = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split(",")
        if len(fields) < 3 or not fields[0] or not fields[2]:
            continue
        try:
            out[fields[2]] = float(fields[0])
        except ValueError:
            continue
    return out


def counter(stats: Dict[str, float], base_name: str) -> Optional[float]:
    """Looks up a perf stat counter by its base name (e.g. "instructions"
    matches "instructions:u" or "instructions:k")."""
    for event, value in stats.items():
        if event.split(":")[0] == base_name:
            return value
    return None


def stepping_cost(long_stats: Dict[str, float], short_stats: Dict[str, float]) -> Dict[str, float]:
    """The stepping cost of each counter both runs share: the long run's
    count minus the short run's (map/engine load cancels out, since both
    runs pay it once and then step for their own tick count)."""
    return {
        event: long_stats[event] - short_stats[event]
        for event in long_stats
        if event in short_stats
    }


def percent_change(baseline: float, new: float) -> Optional[float]:
    """(new - baseline) / baseline, as a percentage. None if baseline is 0
    (division is meaningless, not "infinite% change")."""
    if baseline == 0:
        return None
    return (new - baseline) / baseline * 100.0


def ab_schedule(rounds: int, labels: Sequence[str] = ("A", "B")) -> List[Tuple[int, str]]:
    """The run order for an N-round A/B comparison: every label runs once
    per round, alternating (A1, B1, A2, B2, ...), rather than all of A then
    all of B, so a host getting busier or quieter over time does not favour
    one side. `labels=("current",)` gives a single-state measurement (no
    comparison, N repeats)."""
    if rounds < 1:
        raise ValueError("rounds must be at least 1")
    if not labels:
        raise ValueError("labels must not be empty")
    return [(round_, label) for round_ in range(1, rounds + 1) for label in labels]


def pick_latest_binary(names: Iterable[str]) -> Optional[str]:
    """Of `ls -t`'s output (already newest-first) for the fingerprint test's
    deps/ directory, the first that is not a .d dependency file - the same
    binary fp-test.sh and friends picked with `grep -v '\\.d$' | head -1`."""
    for name in names:
        if not name.endswith(".d"):
            return name
    return None


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def parse_sha256sum_output(text: str) -> Dict[str, str]:
    """Parses `sha256sum FILE...`'s output ("HASH  PATH" per line) into
    {path: hash}."""
    out: Dict[str, str] = {}
    for line in text.splitlines():
        line = line.rstrip("\n")
        if not line.strip():
            continue
        digest, _, path = line.partition("  ")
        if digest and path:
            out[path] = digest
    return out


def select_changed_files(local_hashes: Dict[str, str], remote_hashes: Dict[str, str]) -> List[str]:
    """Of the files we might sync (local_hashes' keys), the ones whose
    content differs from the container's (or that the container does not
    have at all), sorted so the result is stable to look at and to test."""
    return sorted(path for path, digest in local_hashes.items() if remote_hashes.get(path) != digest)


def cargo_lto_env(lto: str, cgu: str) -> Dict[str, str]:
    return {"CARGO_PROFILE_RELEASE_LTO": lto, "CARGO_PROFILE_RELEASE_CODEGEN_UNITS": cgu}


def build_rustflags(target_cpu: Optional[str], frame_pointers: bool, extra: str = "") -> str:
    """target_cpu=None (or "base") omits -C target-cpu, matching 64/build.sh's
    baseline leg."""
    flags: List[str] = []
    if target_cpu and target_cpu != "base":
        flags.append(f"-C target-cpu={target_cpu}")
    if frame_pointers:
        flags.append("-C force-frame-pointers=yes")
    if extra:
        flags.append(extra)
    return " ".join(flags)


def default_target_dir(target: str, lto: str, cgu: str, frame_pointers: bool) -> str:
    suffix = "fp" if frame_pointers else f"{lto}-{cgu}"
    return f"target-bench-{target}-{suffix}"


PERF_MODES = ("none", "stat", "record")


def fingerprint_build_script(
    *,
    crate_dir: str,
    target: str,
    target_dir: str,
    rustflags: str,
    lto: str,
    cgu: str,
) -> str:
    """The shell fragment that builds (but does not run) the fingerprint
    test binary. `set -e` so a compile error stops the script instead of
    being swallowed by a later `grep` the way the one-off scripts' pipelines
    did."""
    return (
        "set -e\n"
        f"cd {crate_dir}\n"
        f"RUSTFLAGS='{rustflags}' "
        f"CARGO_PROFILE_RELEASE_LTO={lto} CARGO_PROFILE_RELEASE_CODEGEN_UNITS={cgu} \\\n"
        f"  cargo test -q --release --no-run --target {target} --target-dir {target_dir} "
        "-p halo-skate --test real_assets_fingerprint\n"
        f'bin=$(ls -t {target_dir}/{target}/release/deps/real_assets_fingerprint-* | grep -v "\\.d$" | head -1)\n'
        'if [ -z "$bin" ]; then echo "no fingerprint test binary built under '
        f'{target_dir}" >&2; exit 1; fi\n'
        'echo "$bin"\n'
    )


def fingerprint_run_script(
    *,
    crate_dir: str,
    bin_var: str,
    assets_root: str,
    maps_root: str,
    ticks: int,
    riders: Optional[int],
    env_extra: Dict[str, str],
    perf: str,
    perf_events: str,
    perf_freq: int,
    log_file: str,
    stat_file: str,
    data_file: str,
) -> str:
    """The shell fragment that runs an already-built fingerprint binary
    (`bin_var`, a shell variable already holding its path), optionally under
    `perf stat` or `perf record`. Fails loudly: the run's own exit code is
    checked explicitly (after a `set +e`, since perf's own exit status is
    what we check, not a pipeline's), and a non-zero exit is re-raised after
    printing the log, rather than grepped past."""
    if perf not in PERF_MODES:
        raise ValueError(f"unknown perf mode {perf!r}")
    env = {
        "HALO_SKATE_TEST_ASSETS": assets_root,
        "HALO_SKATE_TEST_HALO_MAPS": maps_root,
        "HALO_SKATE_TEST_FINGERPRINT_TICKS": str(ticks),
    }
    if riders is not None:
        env["HALO_SKATE_TEST_FINGERPRINT_RIDERS"] = str(riders)
    env.update(env_extra)
    env_str = " ".join(f"{k}={v}" for k, v in env.items())
    if perf == "stat":
        wrapper = f"perf stat -e {perf_events} -x, -o {stat_file} "
    elif perf == "record":
        wrapper = f"perf record -F {perf_freq} -g -o {data_file} -- "
    else:
        wrapper = ""
    lines = [
        f"cd {crate_dir}",
        "set +e",
        f'{env_str} {wrapper}"${bin_var}" --nocapture > {log_file} 2>&1',
        "rc=$?",
        "set -e",
        f"grep -E 'fingerprint [0-9a-f]+:|^a tick of|test result|panicked' {log_file} || true",
        "if [ $rc -ne 0 ]; then",
        '  echo "fingerprint run failed (exit $rc); log:" >&2',
        f"  cat {log_file} >&2",
        "  exit $rc",
        "fi",
    ]
    if perf == "stat":
        lines.append(f"grep -v '^#' {stat_file} | grep .")
    return "\n".join(lines) + "\n"


# --- A frame-pointer relink of the full server binary (build-prof.sh /
# prof-link.sh's trick), for whole-game profiling with skate_bench.py.

def parse_ninja_object_inputs(query_output: str) -> List[str]:
    """Parses `ninja -t query TARGET`'s output and returns the object-file
    inputs (the lines between "input:" and "outputs:" that end in .o),
    stripped of leading whitespace, in the order ninja printed them."""
    lines = query_output.splitlines()
    try:
        start = next(i for i, l in enumerate(lines) if l.strip().startswith("input:")) + 1
        end = next(i for i in range(start, len(lines)) if lines[i].strip() == "outputs:")
    except StopIteration:
        return []
    objects = []
    for line in lines[start:end]:
        stripped = line.strip()
        if stripped.endswith(".o") and "|" not in stripped:
            objects.append(stripped)
    return objects


def rewrite_link_command_for_profile(command: str, substitutions: Sequence[Tuple[str, str]]) -> str:
    """Applies each (old, new) substring substitution to a ninja link
    command, in order, and drops anything after a trailing `&& ...` (ninja
    chains the link with a strip/install step we don't want to repeat for a
    one-off profiling binary)."""
    out = command
    for old, new in substitutions:
        out = out.replace(old, new)
    out = re.split(r"\s*&&\s*", out)[0]
    return out


def profile_relink_substitutions(normal_target_dir: str, profile_target_dir: str,
                                  normal_output: str, profile_output: str,
                                  normal_rsp: str, profile_rsp: str) -> List[Tuple[str, str]]:
    return [
        (normal_target_dir, profile_target_dir),
        (f"-o {normal_output}", f"-o {profile_output}"),
        (f"@{normal_rsp}", f"@{profile_rsp}"),
    ]


# --------------------------------------------------------------------------
# thin runners: subprocess only, every failure propagates loudly.

def run(cmd: Sequence[str], **kwargs) -> subprocess.CompletedProcess:
    try:
        return subprocess.run(cmd, check=True, **kwargs)
    except subprocess.CalledProcessError as e:
        raise CommandFailed(f"command failed ({e.returncode}): {' '.join(cmd)}") from e
    except FileNotFoundError as e:
        raise CommandFailed(f"command not found: {cmd[0]}") from e


def docker_exec(container: str, script: str, *, extra_env: Optional[Dict[str, str]] = None) -> str:
    cmd = ["docker", "exec"]
    for k, v in (extra_env or {}).items():
        cmd += ["-e", f"{k}={v}"]
    cmd += [container, "sh", "-c", script]
    result = run(cmd, capture_output=True, text=True)
    sys.stderr.write(result.stderr)
    return result.stdout


def docker_exec_i(container: str, stdin_bytes: bytes, script: str) -> None:
    run(["docker", "exec", "-i", container, "sh", "-c", script], input=stdin_bytes)


# --- sync: copy the host worktree's changed/untracked files into the container

def host_tracked_and_untracked_files(repo: Path, ref: str) -> List[str]:
    diff = run(["git", "-C", str(repo), "diff", "--name-only", ref], capture_output=True, text=True).stdout
    untracked = run(
        ["git", "-C", str(repo), "ls-files", "-o", "--exclude-standard"], capture_output=True, text=True
    ).stdout
    return sorted({*diff.splitlines(), *untracked.splitlines()} - {""})


def sync_files(repo: Path, container: str, container_src: str, ref: str = "fork/main",
                extra_files: Sequence[str] = ()) -> List[str]:
    """Copies every file that differs from the container (or is missing
    there) into container_src, preserving relative paths. Returns the list
    of files it copied."""
    candidates = sorted({*host_tracked_and_untracked_files(repo, ref), *extra_files})
    existing = [f for f in candidates if (repo / f).is_file()]
    if not existing:
        return []
    local_hashes = {f: sha256_file(repo / f) for f in existing}
    remote_out = docker_exec(
        container,
        f"cd {container_src} && for f in {' '.join(existing)}; do [ -f \"$f\" ] && sha256sum \"$f\"; done",
    )
    remote_hashes = parse_sha256sum_output(remote_out)
    changed = select_changed_files(local_hashes, remote_hashes)
    if not changed:
        return []
    tar = run(["tar", "-c", "-C", str(repo), *changed], capture_output=True).stdout
    docker_exec_i(container, tar, f"tar -x -m -C {container_src}")
    return changed


# --------------------------------------------------------------------------
# CLI

def cmd_sync(args: argparse.Namespace) -> None:
    changed = sync_files(Path(args.repo), args.container, args.container_src, args.ref, args.extra_files)
    if changed:
        print(f"synced {len(changed)} file(s): {', '.join(changed)}")
    else:
        print("nothing to sync")


def _build_and_run_fingerprint(args: argparse.Namespace, *, frame_pointers: bool, label: str) -> str:
    crate_dir = f"{args.container_src}/port/skate"
    target_dir = args.target_dir or default_target_dir(args.target, args.lto, args.cgu, frame_pointers)
    rustflags = build_rustflags(args.cpu, frame_pointers, args.extra_rustflags)
    build_script = fingerprint_build_script(
        crate_dir=crate_dir, target=args.target, target_dir=target_dir,
        rustflags=rustflags, lto=args.lto, cgu=args.cgu,
    )
    bin_path = docker_exec(args.container, build_script).strip().splitlines()[-1]
    # (the test binary lands in target_dir/TARGET/release/deps/, a path
    # relative to crate_dir; it is run from crate_dir/halo-skate the way the
    # fingerprint test expects its working directory, so one more "../")
    rel_bin = bin_path if bin_path.startswith("/") else f"../{bin_path}"
    run_script = fingerprint_run_script(
        crate_dir=f"{crate_dir}/halo-skate",
        bin_var="bin",
        assets_root=args.assets_root,
        maps_root=args.maps_root,
        ticks=args.ticks,
        riders=args.riders,
        env_extra=dict(e.split("=", 1) for e in args.env),
        perf=args.perf,
        perf_events=args.perf_events,
        perf_freq=args.perf_freq,
        log_file=f"/tmp/fp-{label}.log",
        stat_file=f"/tmp/fp-{label}.stat",
        data_file=args.perf_data_out or f"/tmp/fp-{label}.data",
    )
    script = f'bin="{rel_bin}"\n' + run_script
    return docker_exec(args.container, script)


def cmd_fingerprint(args: argparse.Namespace) -> None:
    if args.sync_repo:
        sync_files(Path(args.sync_repo), args.container, args.container_src)
    out = _build_and_run_fingerprint(args, frame_pointers=(args.perf == "record"), label=args.label)
    print(out)
    result = parse_fingerprint_log(out)
    if not result.ok:
        raise CommandFailed(f"fingerprint run did not pass: {result}")
    if args.perf == "stat":
        stats = parse_perf_stat(out)
        print(f"parsed counters: {stats}")


def cmd_step(args: argparse.Namespace) -> None:
    files = args.files or []
    repo = Path(args.repo) if args.repo else None
    if not files and args.sync_repo:
        sync_files(Path(args.sync_repo), args.container, args.container_src)
    kept: Dict[str, bytes] = {}
    if files:
        if repo is None:
            raise CommandFailed("--files needs --repo")
        for f in files:
            kept[f] = (repo / f).read_bytes()

    def set_state(label: str) -> None:
        if not files:
            return
        if label == "A":
            for f in files:
                content = run(["git", "-C", str(repo), "show", f"{args.ref}:{f}"], capture_output=True).stdout
                (repo / f).write_bytes(content)
        else:
            for f in files:
                (repo / f).write_bytes(kept[f])
        sync_files(repo, args.container, args.container_src, args.ref, extra_files=files)

    labels = ("A", "B") if files else ("current",)
    schedule = ab_schedule(args.rounds, labels)
    results: Dict[Tuple[int, str], Dict[str, float]] = {}
    try:
        for round_, label in schedule:
            set_state(label)
            long_out = _build_and_run_fingerprint(
                with_overrides(args, ticks=args.long_ticks, perf="stat", label=f"{label}{round_}-long"),
                frame_pointers=False, label=f"{label}{round_}-long",
            )
            short_out = _build_and_run_fingerprint(
                with_overrides(args, ticks=args.short_ticks, perf="stat", label=f"{label}{round_}-short"),
                frame_pointers=False, label=f"{label}{round_}-short",
            )
            long_stats, short_stats = parse_perf_stat(long_out), parse_perf_stat(short_out)
            delta = stepping_cost(long_stats, short_stats)
            results[(round_, label)] = delta
            instr, cyc = counter(delta, "instructions"), counter(delta, "cycles")
            print(f"{label}{round_}: stepping instructions={instr} cycles={cyc}")
    finally:
        if files:
            for f in files:
                (repo / f).write_bytes(kept[f])
            sync_files(repo, args.container, args.container_src, args.ref, extra_files=files)

    if files:
        a_instr = [counter(d, "instructions") for (r, l), d in results.items() if l == "A"]
        b_instr = [counter(d, "instructions") for (r, l), d in results.items() if l == "B"]
        a_mean = sum(a_instr) / len(a_instr)
        b_mean = sum(b_instr) / len(b_instr)
        print(f"A mean stepping instructions: {a_mean:.0f}")
        print(f"B mean stepping instructions: {b_mean:.0f}")
        change = percent_change(a_mean, b_mean)
        print(f"change: {change:+.2f}%" if change is not None else "change: n/a (A is zero)")


def with_overrides(args: argparse.Namespace, **overrides) -> argparse.Namespace:
    """A shallow copy of `args` with some fields overridden, so per-run
    tweaks (ticks, perf mode, label) don't mutate the caller's namespace."""
    merged = vars(args).copy()
    merged.update(overrides)
    return argparse.Namespace(**merged)


def cmd_build_server(args: argparse.Namespace) -> None:
    if args.sync_repo:
        sync_files(Path(args.sync_repo), args.container, args.container_src)
    if not args.frame_pointers:
        script = (
            "set -e\n"
            f"cd {args.container_src}\n"
            f"ninja -j{args.jobs} linux\n"
            f"cp build/linux/halo {args.output}\n"
            f'echo "built {args.output}"\n'
        )
        print(docker_exec(args.container, script))
        return

    # frame-pointer relink: rebuild the skate engine with force-frame-pointers,
    # then relink build/linux/halo against it instead of the normal target dir
    crate_dir = f"{args.container_src}/port/skate"
    rustflags = build_rustflags(args.cpu, True)
    build_script = (
        "set -e\n"
        f"cd {crate_dir}\n"
        f"RUSTFLAGS='{rustflags}' CARGO_PROFILE_RELEASE_LTO=fat CARGO_PROFILE_RELEASE_CODEGEN_UNITS=1 \\\n"
        f"  cargo build -q --release --manifest-path Cargo.toml --config .cargo/config.toml "
        f"--target {args.target} --target-dir {args.profile_target_dir} -p halo-skate\n"
    )
    docker_exec(args.container, build_script)
    query_script = f"cd {args.container_src} && ninja -t query {args.ninja_target}"
    query_out = docker_exec(args.container, query_script)
    objects = parse_ninja_object_inputs(query_out)
    if not objects:
        raise CommandFailed(f"ninja -t query {args.ninja_target} listed no object inputs")
    rsp_script = (
        f"cd {args.container_src} && printf '%s\\n' " + " ".join(f'"{o}"' for o in objects)
        + f" > {args.profile_rsp}"
    )
    docker_exec(args.container, rsp_script)
    commands_out = docker_exec(args.container, f"cd {args.container_src} && ninja -t commands {args.ninja_target}")
    last_command = commands_out.strip().splitlines()[-1]
    substitutions = profile_relink_substitutions(
        args.normal_target_dir, args.profile_target_dir,
        args.ninja_target, args.output,
        args.normal_rsp, args.profile_rsp,
    )
    relink_command = rewrite_link_command_for_profile(last_command, substitutions)
    relink_script = f"set -e\ncd {args.container_src}\n{relink_command}\necho built {args.output}\n"
    print(docker_exec(args.container, relink_script))


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--container", default=DEFAULT_CONTAINER, help="docker container to run in")
    common.add_argument("--container-src", default=DEFAULT_SRC, dest="container_src",
                         help="the engine checkout's root inside the container")

    commands = parser.add_subparsers(dest="command", required=True)

    p_sync = commands.add_parser("sync", parents=[common], help="copy changed files into the container")
    p_sync.add_argument("--repo", required=True, help="the host worktree to sync from")
    p_sync.add_argument("--ref", default="fork/main", help="diff against this ref to find changed tracked files")
    p_sync.add_argument("--extra-files", nargs="*", default=[], dest="extra_files")
    p_sync.set_defaults(func=cmd_sync)

    fp_common = argparse.ArgumentParser(add_help=False, parents=[common])
    fp_common.add_argument("--sync-repo", dest="sync_repo", default=None,
                            help="sync this host worktree before building")
    fp_common.add_argument("--target", default=DEFAULT_TARGET)
    fp_common.add_argument("--cpu", default=DEFAULT_CPU, help="RUSTFLAGS target-cpu, or 'base' to omit it")
    fp_common.add_argument("--lto", default=DEFAULT_LTO, help="CARGO_PROFILE_RELEASE_LTO")
    fp_common.add_argument("--cgu", default=DEFAULT_CGU, help="CARGO_PROFILE_RELEASE_CODEGEN_UNITS")
    fp_common.add_argument("--extra-rustflags", default="", dest="extra_rustflags")
    fp_common.add_argument("--target-dir", default=None, dest="target_dir")
    fp_common.add_argument("--assets-root", default=DEFAULT_ASSETS, dest="assets_root")
    fp_common.add_argument("--maps-root", default=DEFAULT_MAPS, dest="maps_root")
    fp_common.add_argument("--riders", type=int, default=None)
    fp_common.add_argument("--env", action="append", default=[], help="EXTRA=VALUE, forwarded to the test run")

    p_fp = commands.add_parser("fingerprint", parents=[fp_common], help="build and run the fingerprint test")
    p_fp.add_argument("--ticks", type=int, default=DEFAULT_TICKS)
    p_fp.add_argument("--perf", choices=PERF_MODES, default="none")
    p_fp.add_argument("--perf-events", dest="perf_events", default="instructions:u,cycles:u")
    p_fp.add_argument("--perf-freq", dest="perf_freq", type=int, default=2000)
    p_fp.add_argument("--perf-data-out", dest="perf_data_out", default=None,
                       help="where perf record writes its profile (perf=record)")
    p_fp.add_argument("--label", default="run")
    p_fp.set_defaults(func=cmd_fingerprint)

    p_step = commands.add_parser(
        "step", parents=[fp_common],
        help="A/B (or single-state) stepping-cost measurement: long run minus short run, N rounds",
    )
    p_step.add_argument("--repo", default=None, help="host worktree; required if --files is given")
    p_step.add_argument("--ref", default="fork/main", help="the 'A' state's git ref")
    p_step.add_argument("--files", nargs="*", default=[], help="files to compare at --ref ('A') vs now ('B')")
    p_step.add_argument("--long-ticks", type=int, default=900, dest="long_ticks")
    p_step.add_argument("--short-ticks", type=int, default=300, dest="short_ticks")
    p_step.add_argument("--rounds", type=int, default=1)
    p_step.set_defaults(func=cmd_step, perf_data_out=None, label="step",
                         perf_events="instructions:u,cycles:u", perf_freq=2000)

    p_build = commands.add_parser(
        "build-server", parents=[common], help="build (optionally frame-pointer) the full server binary",
    )
    p_build.add_argument("--sync-repo", dest="sync_repo", default=None)
    p_build.add_argument("--output", required=True, help="where to copy the built binary, inside the container")
    p_build.add_argument("--jobs", type=int, default=6)
    p_build.add_argument("--frame-pointers", action="store_true", dest="frame_pointers")
    p_build.add_argument("--target", default=DEFAULT_TARGET)
    p_build.add_argument("--cpu", default=DEFAULT_CPU)
    p_build.add_argument("--ninja-target", default="build/linux/halo", dest="ninja_target")
    p_build.add_argument("--normal-target-dir", default="port/skate/target-x86-64-v3", dest="normal_target_dir")
    p_build.add_argument("--profile-target-dir", default="port/skate/target-prof", dest="profile_target_dir")
    p_build.add_argument("--normal-rsp", default="build/linux/halo.rsp", dest="normal_rsp")
    p_build.add_argument("--profile-rsp", default="/tmp/profile.rsp", dest="profile_rsp")
    p_build.set_defaults(func=cmd_build_server)

    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)
    try:
        args.func(args)
    except CommandFailed as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

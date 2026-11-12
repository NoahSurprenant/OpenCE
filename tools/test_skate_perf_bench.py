"""Unit tests for the pure parsing/arithmetic/text-generation functions in
skate_perf_bench.py. No docker, no cargo, no perf: these run anywhere."""

import pytest

from tools import skate_perf_bench as spb


# ---------- parsing a real_assets_fingerprint.rs run's output

FINGERPRINT_LOG = """\
Compiling halo-skate v0.1.0
   Running tests/real_assets_fingerprint.rs
fingerprint d4685a26c8358646: 12 riders, 600 ticks, 311 held hits told, 4 bails given, 2 steps recovered
a tick of 12 steps on the pool's 3 threads 0.652 ms; a step alone 0.071 ms; 7200 steps on the pool, 0.163 ms each; 1.204 ms of processor time a step
test result: ok. 1 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out; finished in 21.44s
"""


def test_parse_fingerprint_log_reads_the_hash_and_tick_line():
    result = spb.parse_fingerprint_log(FINGERPRINT_LOG)
    assert result.hash == "d4685a26c8358646"
    assert result.riders == 12
    assert result.ticks == 600
    assert result.held_hits == 311
    assert result.tick_line.startswith("a tick of 12 steps")
    assert result.ok is True
    assert result.panicked is False


def test_parse_fingerprint_log_a_panic_is_never_ok():
    text = FINGERPRINT_LOG + "\nthread 'a_crowds_rides_fingerprint' panicked at ...\n"
    result = spb.parse_fingerprint_log(text)
    assert result.panicked is True
    assert result.ok is False


def test_parse_fingerprint_log_a_failed_test_is_not_ok():
    text = FINGERPRINT_LOG.replace("0 failed", "1 failed").replace("ok.", "FAILED.")
    result = spb.parse_fingerprint_log(text)
    assert result.ok is False


def test_parse_fingerprint_log_missing_hash_is_not_ok():
    result = spb.parse_fingerprint_log("test result: ok. 1 passed; 0 failed;\n")
    assert result.hash is None
    assert result.ok is False


# ---------- parsing `perf stat -x,` output

PERF_STAT_TEXT = """\
# started on Thu Nov 12 08:00:00 2026

39954312345,,instructions:u,41000000,100.00,1.17,insn per cycle
34110223344,,cycles:u,41000000,100.00,,
1204.556789,msec,task-clock:u,1204556789,100.00,1.002,CPUs utilized

# a comment at the end
"""


def test_parse_perf_stat_reads_every_counter():
    stats = spb.parse_perf_stat(PERF_STAT_TEXT)
    assert stats["instructions:u"] == 39954312345.0
    assert stats["cycles:u"] == 34110223344.0
    assert stats["task-clock:u"] == 1204.556789


def test_parse_perf_stat_ignores_comments_and_blank_lines():
    stats = spb.parse_perf_stat("# nothing here\n\n   \n")
    assert stats == {}


def test_parse_perf_stat_skips_malformed_lines():
    stats = spb.parse_perf_stat("not,a,real,perf,line\n,,missing-value\n")
    assert "missing-value" not in stats


def test_counter_matches_by_base_name_ignoring_the_modifier():
    stats = {"instructions:u": 100.0, "cycles:k": 50.0}
    assert spb.counter(stats, "instructions") == 100.0
    assert spb.counter(stats, "cycles") == 50.0
    assert spb.counter(stats, "task-clock") is None


# ---------- stepping cost and percent change

def test_stepping_cost_is_long_minus_short_per_counter():
    long = {"instructions:u": 1000.0, "cycles:u": 2000.0}
    short = {"instructions:u": 400.0, "cycles:u": 900.0}
    assert spb.stepping_cost(long, short) == {"instructions:u": 600.0, "cycles:u": 1100.0}


def test_stepping_cost_only_keeps_counters_present_in_both():
    long = {"instructions:u": 1000.0, "task-clock:u": 5.0}
    short = {"instructions:u": 400.0}
    assert spb.stepping_cost(long, short) == {"instructions:u": 600.0}


def test_percent_change_basic():
    assert spb.percent_change(100.0, 90.0) == pytest.approx(-10.0)
    assert spb.percent_change(100.0, 110.0) == pytest.approx(10.0)


def test_percent_change_zero_baseline_is_undefined_not_infinite():
    assert spb.percent_change(0.0, 10.0) is None


# ---------- the A/B round schedule

def test_ab_schedule_alternates_within_each_round():
    assert spb.ab_schedule(2) == [(1, "A"), (1, "B"), (2, "A"), (2, "B")]


def test_ab_schedule_one_round():
    assert spb.ab_schedule(1) == [(1, "A"), (1, "B")]


def test_ab_schedule_single_label_for_a_single_state_measurement():
    assert spb.ab_schedule(3, labels=("current",)) == [(1, "current"), (2, "current"), (3, "current")]


def test_ab_schedule_rejects_zero_rounds():
    with pytest.raises(ValueError):
        spb.ab_schedule(0)


# ---------- picking the newest fingerprint test binary

def test_pick_latest_binary_skips_dot_d_files():
    names = [
        "target/i686/release/deps/real_assets_fingerprint-abc123.d",
        "target/i686/release/deps/real_assets_fingerprint-abc123",
        "target/i686/release/deps/real_assets_fingerprint-old456",
    ]
    assert spb.pick_latest_binary(names) == "target/i686/release/deps/real_assets_fingerprint-abc123"


def test_pick_latest_binary_none_when_only_dot_d_files():
    assert spb.pick_latest_binary(["x.d", "y.d"]) is None


def test_pick_latest_binary_empty_list():
    assert spb.pick_latest_binary([]) is None


# ---------- sha256sum output and sync file selection

def test_parse_sha256sum_output():
    text = (
        "d41d8cd98f00b204e9800998ecf8427e  port/skate/vendor/skate-core/src/lib.rs\n"
        "0123456789abcdef0123456789abcdef  configure.py\n"
    )
    hashes = spb.parse_sha256sum_output(text)
    assert hashes["port/skate/vendor/skate-core/src/lib.rs"] == "d41d8cd98f00b204e9800998ecf8427e"
    assert hashes["configure.py"] == "0123456789abcdef0123456789abcdef"


def test_select_changed_files_includes_missing_and_different_only():
    local = {"a.rs": "hash-a", "b.rs": "hash-b", "c.rs": "hash-c"}
    remote = {"a.rs": "hash-a", "b.rs": "different"}
    assert spb.select_changed_files(local, remote) == ["b.rs", "c.rs"]


def test_select_changed_files_nothing_changed():
    local = {"a.rs": "hash-a"}
    remote = {"a.rs": "hash-a"}
    assert spb.select_changed_files(local, remote) == []


# ---------- building the shell/env fragments (string content only, never run)

def test_build_rustflags_default_cpu_and_no_frame_pointers():
    assert spb.build_rustflags("x86-64-v3", False) == "-C target-cpu=x86-64-v3"


def test_build_rustflags_base_cpu_omits_target_cpu():
    assert spb.build_rustflags("base", False) == ""
    assert spb.build_rustflags(None, False) == ""


def test_build_rustflags_frame_pointers_and_extra():
    flags = spb.build_rustflags("x86-64-v3", True, "-C lto=no")
    assert flags == "-C target-cpu=x86-64-v3 -C force-frame-pointers=yes -C lto=no"


def test_default_target_dir_differs_for_frame_pointer_builds():
    normal = spb.default_target_dir("i686-unknown-linux-gnu", "false", "16", False)
    fp = spb.default_target_dir("i686-unknown-linux-gnu", "false", "16", True)
    assert normal != fp
    assert "fp" in fp


def test_fingerprint_build_script_fails_loudly_and_sets_the_right_envs():
    script = spb.fingerprint_build_script(
        crate_dir="/src/port/skate", target="i686-unknown-linux-gnu", target_dir="target-bench",
        rustflags="-C target-cpu=x86-64-v3", lto="false", cgu="16",
    )
    assert script.startswith("set -e\n")
    assert "CARGO_PROFILE_RELEASE_LTO=false" in script
    assert "CARGO_PROFILE_RELEASE_CODEGEN_UNITS=16" in script
    assert "--target-dir target-bench" in script
    assert "--test real_assets_fingerprint" in script
    # a build that produced no binary must fail the script, not continue silently
    assert "exit 1" in script


def test_fingerprint_run_script_rejects_unknown_perf_mode():
    with pytest.raises(ValueError):
        spb.fingerprint_run_script(
            crate_dir="x", bin_var="bin", assets_root="a", maps_root="m", ticks=1, riders=None,
            env_extra={}, perf="bogus", perf_events="instructions:u", perf_freq=99,
            log_file="l", stat_file="s", data_file="d",
        )


def test_fingerprint_run_script_stat_mode_wraps_with_perf_stat_and_checks_exit_code():
    script = spb.fingerprint_run_script(
        crate_dir="/src/port/skate/halo-skate", bin_var="bin", assets_root="/assets", maps_root="/maps",
        ticks=600, riders=12, env_extra={}, perf="stat", perf_events="instructions:u,cycles:u",
        perf_freq=2000, log_file="/tmp/fp.log", stat_file="/tmp/fp.stat", data_file="/tmp/fp.data",
    )
    assert "perf stat -e instructions:u,cycles:u -x, -o /tmp/fp.stat" in script
    assert "HALO_SKATE_TEST_FINGERPRINT_TICKS=600" in script
    assert "HALO_SKATE_TEST_FINGERPRINT_RIDERS=12" in script
    assert "rc=$?" in script and "exit $rc" in script  # fails loudly, not grepped past


def test_fingerprint_run_script_record_mode_wraps_with_perf_record():
    script = spb.fingerprint_run_script(
        crate_dir="/src", bin_var="bin", assets_root="/a", maps_root="/m", ticks=900, riders=None,
        env_extra={"FOO": "bar"}, perf="record", perf_events="instructions:u", perf_freq=2000,
        log_file="/tmp/l", stat_file="/tmp/s", data_file="/tmp/prof.data",
    )
    assert "perf record -F 2000 -g -o /tmp/prof.data" in script
    assert "FOO=bar" in script
    assert "HALO_SKATE_TEST_FINGERPRINT_RIDERS" not in script


def test_fingerprint_run_script_none_mode_runs_bare():
    script = spb.fingerprint_run_script(
        crate_dir="/src", bin_var="bin", assets_root="/a", maps_root="/m", ticks=600, riders=None,
        env_extra={}, perf="none", perf_events="x", perf_freq=1, log_file="/tmp/l", stat_file="/tmp/s",
        data_file="/tmp/d",
    )
    assert "perf stat" not in script
    assert "perf record" not in script


# ---------- the profile relink's ninja text parsing/rewriting

NINJA_QUERY_OUTPUT = """\
build/linux/halo:
  input: link_linux
    port/skate/target-x86-64-v3/i686-unknown-linux-gnu/release/libhalo_skate.a
    build/linux/obj/source/game/game.o
    build/linux/obj/port/linux/src/p2p.o
    build/linux/obj/port/linux/src/main.o | build/linux/obj/some_generated_header.h
  outputs:
"""


def test_parse_ninja_object_inputs_only_takes_plain_o_files():
    objects = spb.parse_ninja_object_inputs(NINJA_QUERY_OUTPUT)
    assert objects == [
        "build/linux/obj/source/game/game.o",
        "build/linux/obj/port/linux/src/p2p.o",
    ]


def test_parse_ninja_object_inputs_no_input_section():
    assert spb.parse_ninja_object_inputs("nothing to see here\n") == []


def test_rewrite_link_command_for_profile_applies_substitutions_in_order():
    cmd = (
        "clang -o build/linux/halo @build/linux/halo.rsp "
        "port/skate/target-x86-64-v3/i686-unknown-linux-gnu/release/libhalo_skate.a "
        "&& strip build/linux/halo"
    )
    subs = spb.profile_relink_substitutions(
        normal_target_dir="port/skate/target-x86-64-v3",
        profile_target_dir="port/skate/target-prof",
        normal_output="build/linux/halo",
        profile_output="/src/bins/halo-prof-main",
        normal_rsp="build/linux/halo.rsp",
        profile_rsp="/tmp/profile.rsp",
    )
    rewritten = spb.rewrite_link_command_for_profile(cmd, subs)
    assert "port/skate/target-prof/i686-unknown-linux-gnu" in rewritten
    assert "-o /src/bins/halo-prof-main" in rewritten
    assert "@/tmp/profile.rsp" in rewritten
    assert "strip" not in rewritten  # the trailing && step is dropped


# ---------- A/B run-order helper used by `with_overrides`

def test_with_overrides_does_not_mutate_the_original():
    import argparse

    original = argparse.Namespace(ticks=600, perf="none")
    overridden = spb.with_overrides(original, ticks=900, perf="stat")
    assert original.ticks == 600 and original.perf == "none"
    assert overridden.ticks == 900 and overridden.perf == "stat"

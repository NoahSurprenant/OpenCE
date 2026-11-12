"""Unit tests for skate_bench.py's pure functions: report-line parsing,
thread-group CPU accounting and the summary. No game, no /proc, no perf."""

import pytest

from tools import skate_bench as sb


LOG_SLICE = (
    "skate: the bots' skaters: 108 skating on average, the game waited 0.40 ms a tick, "
    "the game played 29.9 ticks a second\n"
    "some unrelated line\n"
    "skate: the bots' skaters: 110 skating on average, the game waited 0.38 ms a tick, "
    "the game played 30.0 ticks a second\n"
)


def test_parse_reports_reads_every_summary_line():
    reports = sb.parse_reports(LOG_SLICE)
    assert reports == [
        sb.SkateReport(108, 0.40, 29.9),
        sb.SkateReport(110, 0.38, 30.0),
    ]


def test_parse_reports_empty_text():
    assert sb.parse_reports("") == []


def test_estimate_ticks_per_second_averages_the_reports():
    reports = [sb.SkateReport(1, 0.1, 29.0), sb.SkateReport(1, 0.1, 31.0)]
    assert sb.estimate_ticks_per_second(reports) == pytest.approx(30.0)


def test_estimate_ticks_per_second_falls_back_when_no_reports_landed_in_the_window():
    assert sb.estimate_ticks_per_second([]) == 30.0
    assert sb.estimate_ticks_per_second([], default=25.0) == 25.0


def test_thread_group_name_collapses_numbered_workers():
    assert sb.thread_group_name("skate_pool_3") == "skate_pool_N"
    assert sb.thread_group_name("skate_pool_42") == "skate_pool_N"
    assert sb.thread_group_name("halo") == "halo"


def test_group_thread_cpu_sums_by_group_and_subtracts_the_baseline():
    before = {"100": ("halo", 10.0), "101": ("skate_pool_1", 1.0), "102": ("skate_pool_2", 1.5)}
    after = {"100": ("halo", 12.0), "101": ("skate_pool_1", 1.6), "102": ("skate_pool_2", 2.1)}
    groups = sb.group_thread_cpu(before, after)
    assert groups["halo"] == pytest.approx(2.0)
    assert groups["skate_pool_N"] == pytest.approx(1.2)


def test_group_thread_cpu_counts_a_thread_new_since_the_baseline_from_zero():
    before = {}
    after = {"200": ("skate_pool_7", 0.5)}
    groups = sb.group_thread_cpu(before, after)
    assert groups["skate_pool_N"] == pytest.approx(0.5)


def test_thread_cpu_seconds_adds_user_and_system_time():
    # fields are 0-indexed from the first field after ')'; utime=index 11, stime=index 12
    fields = ["S"] * 11 + ["150", "50"]
    assert sb.thread_cpu_seconds(fields, hz=100) == pytest.approx(2.0)


def test_summarize_and_format_report_a_quiet_window():
    groups = {"halo": 1.0, "skate_pool_N": 0.5}
    summary = sb.summarize(10.0, [sb.SkateReport(12, 0.5, 30.0)], groups)
    assert summary.ticks == pytest.approx(300.0)
    assert summary.total_cpu_seconds == pytest.approx(1.5)
    text = sb.format_summary(summary)
    assert "cpu-ms/tick" in text
    assert "halo" in text and "skate_pool_N" in text


def test_summarize_handles_a_window_with_no_reports():
    summary = sb.summarize(5.0, [], {"halo": 0.1})
    assert summary.ticks == pytest.approx(150.0)  # the 30 Hz default
    assert "total" in sb.format_summary(summary)


def test_format_summary_hides_negligible_groups():
    summary = sb.summarize(10.0, [], {"halo": 1.0, "tiny_thread": 0.001})
    text = sb.format_summary(summary)
    assert "halo" in text
    assert "tiny_thread" not in text


def test_parse_env_overrides():
    assert sb.parse_env_overrides(["HALO_SERVER_BOTS=64", "FOO=bar=baz"]) == {
        "HALO_SERVER_BOTS": "64",
        "FOO": "bar=baz",
    }


def test_parse_env_overrides_rejects_a_pair_without_equals():
    with pytest.raises(ValueError):
        sb.parse_env_overrides(["NOVALUE"])


def test_server_env_strips_inherited_sdl_and_halo_vars_but_keeps_overrides():
    base = {"SDL_VIDEODRIVER": "x11", "HALO_SOMETHING": "1", "PATH": "/usr/bin"}
    env = sb.server_env(base, out=__import__("pathlib").Path("/tmp/run"), map_name="bloodgulch:slayer",
                         overrides={"HALO_SERVER_BOTS": "64"})
    assert "SDL_VIDEODRIVER" not in env
    assert "HALO_SOMETHING" not in env
    assert env["PATH"] == "/usr/bin"
    assert env["HALO_DEDICATED"] == "1"
    assert env["HALO_SERVER_MAPS"] == "bloodgulch:slayer"
    assert env["HALO_SERVER_BOTS"] == "64"  # override wins over the fixed default

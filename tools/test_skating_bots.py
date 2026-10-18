"""Bots on boards (port/linux/game/bots_skate.c; port/skate/README.md, "Bots
on boards") in real games on this machine, with Skate 3 mode and its data: a
--skate build (HALO_TEST_GAME), the converted Skate 3 data
(HALO_SKATE_TEST_ASSETS) and the Halo maps (HALO_SKATE_TEST_HALO_MAPS); they
skip without them. Each game's random numbers are fixed (debug.bot_seed,
debug.game_seed), so a run goes the same way as near as the machine's timing
lets it."""

import os
import re
from pathlib import Path

import pytest

from tools.test_bots import REPORT, SCORE, _log, _start, _stop, _wait
from tools.test_linux_port import _test_address

BOARDED = re.compile(r"bots: player (\d+) gets on a board at tick (\d+)")
STEPPED_OFF = re.compile(r"bots: player (\d+) steps off the board \(([^)]*)\)")
ON_A_BOARD = re.compile(r"on a board (\d+) times, (\d+) seconds, (\d+) world units, (\d+) ollies, (\d+) bails, "
                        r"(\d+) people ridden at")
RUN_OVER_DECIDED = re.compile(r"skate verdict (\d+) decided: player (\d+)'s skater \w+ on; player (\d+) on foot run over")
RUN_OVER_KILLED = re.compile(r"skate verdict \d+: the person run over died here, killed by player (\d+)")


def _server():
    """the dedicated server's build (HALO_TEST_SERVER: the server image's, a
    release build with the mode), else HALO_TEST_GAME's"""
    server = os.environ.get("HALO_TEST_SERVER")
    return server if server and Path(server).is_file() else os.environ.get("HALO_TEST_GAME")


def _needs_skate():
    game = os.environ.get("HALO_TEST_GAME")
    maps = os.environ.get("HALO_SKATE_TEST_HALO_MAPS")
    assets = os.environ.get("HALO_SKATE_TEST_ASSETS")
    if not (game and Path(game).is_file() and maps and Path(maps, "bloodgulch.map").is_file() and assets and
            Path(assets, "private").is_dir()):
        pytest.skip("needs a --skate HALO_TEST_GAME, HALO_SKATE_TEST_ASSETS and HALO_SKATE_TEST_HALO_MAPS")
    return game, maps, assets


def _boards(log):
    """each bot's last report of its boards: {name: (times, seconds, world
    units, ollies, bails, people ridden at)}"""
    boards = {}
    for name, _, text in REPORT.findall(log):
        found = ON_A_BOARD.search(text)
        if found:
            boards[name] = tuple(int(n) for n in found.groups())
    return boards


def _bots_alone(tmp_path, name, game, maps, assets, settings, addresses, done=None):
    """a dedicated server of a --skate build, with the Skate 3 data, and bots
    alone, until its game is over with their scores (or done(log) holds); its
    log"""
    bots = int(settings["HALO_SERVER_BOTS"])
    games = {name: _start(tmp_path, name, game, maps, {
        "HALO_DEDICATED": "1", "HALO_NETWORK_TEST_START": "3", "HALO_SKATE_ASSETS": assets,
        "HALO_NET_ADDRESS": addresses[0], "HALO_NET_BROADCAST": addresses[1], **settings})}
    try:
        _wait(lambda: len(SCORE.findall(_log(tmp_path, name))) >= bots or (done and done(_log(tmp_path, name))), 400,
              games, tmp_path, "the bots' game did not end with their scores")
    finally:
        _stop(games, tmp_path, {name: game})
        for cache in (tmp_path / name / "saves").rglob("cache*.map"):
            cache.unlink()
    return _log(tmp_path, name)


def test_skating_bots_ride_and_fight(tmp_path):
    """A listen host with Skate 3 data (debug.network_test "host" on Blood
    Gulch), its own player skating too (debug.network_test_skate), three bots
    (two normal, one peaceful) and a client: the bots get on boards when the
    way ahead rolls and ride a good way, step off to fight (the normal ones
    kill), and the host's own skater rides on beside the bots' (several skate
    sessions in one game); the client sees the bots' skaters posed with their
    boards. About three and a half minutes."""
    game, maps, assets = _needs_skate()
    host_address, join_address = _test_address(249), _test_address(250)
    games = {"host": _start(tmp_path, "host", game, maps, {
        "HALO_NETWORK_TEST": "host:bloodgulch", "HALO_NETWORK_TEST_START": "8", "HALO_SERVER_BOTS": "3",
        "HALO_BOT_DIFFICULTIES": "normal,normal,peaceful", "HALO_BOT_SEED": "11", "HALO_SKATE_ASSETS": assets,
        "HALO_NETWORK_TEST_SKATE": "20", "HALO_NULL_RENDERER": "1", "HALO_NO_AUDIO": "1",
        "HALO_NET_ADDRESS": host_address, "HALO_NET_BROADCAST": join_address})}
    games["join"] = _start(tmp_path, "join", game, maps, {
        "HALO_NETWORK_TEST": "join", "HALO_NULL_RENDERER": "1", "HALO_NO_AUDIO": "1", "HALO_TEST_INPUT": "bot:7",
        "HALO_SKATE_ASSETS": assets, "HALO_NET_ADDRESS": join_address, "HALO_NET_BROADCAST": host_address})
    bot_players = set()

    def ridden():
        host = _log(tmp_path, "host")
        boards = _boards(host)
        return len(boards) == 3 and sum(b[2] for b in boards.values()) >= 150 and \
            sum(1 for b in boards.values() if b[0] >= 1) >= 2 and "(a fight)" in host

    try:
        _wait(ridden, 300, games, tmp_path, "the bots did not ride 150 world units and step off for a fight")
        host = _log(tmp_path, "host")
        bot_players.update(player for player, _ in BOARDED.findall(host))
        # (the client sees each bot that rides as another machine's skater,
        # posed, its board drawn)
        posed = re.compile(r"skate network: player (\d+) skates on another machine at .*?: posed \d+ nodes, "
                           r"the board drawn")
        _wait(lambda: bot_players & set(posed.findall(_log(tmp_path, "join"))), 60, games, tmp_path,
              "the client saw no bot's skater posed with its board")
    finally:
        _stop(games, tmp_path, {"host": game, "join": game})
        for cache in tmp_path.rglob("cache*.map"):
            cache.unlink()
    host = _log(tmp_path, "host")
    boards = _boards(host)
    print(f"boards {boards}; stepped off {STEPPED_OFF.findall(host)[:12]}")
    print(f"the client saw bots {sorted(bot_players)} posed with their boards")
    # each skater in its own session; the host's own rode beside them
    sessions = set(re.findall(r"skate: bot skater (\d+) on \(player", host))
    assert len(sessions) >= 2, f"the bots rode in {sessions} sessions"
    assert "skate: on" in host, "the host's own player never got on a board"
    assert not re.search(r"NOT FINITE|EXCEPTION|fatal error", host), "a pose not finite, or a fatal error"
    # the normal bots fought; the peaceful one never ran anyone over
    players = {name: player for player, name in re.findall(r"network test: player (\d+) is named (\[BOT\] \w+)", host)}
    peaceful = [players.get(name) for name in re.findall(r"bots: (\[BOT\] \w+) joined \([^)]*peaceful\)", host)]
    fired = {name: int(n) for name, n in re.findall(r"bots: (\[BOT\] \w+) at tick \d+: .*?fired (\d+) ticks", host)}
    decided = RUN_OVER_DECIDED.findall(host)
    print(f"run-overs decided {decided}; fired {fired}; the peaceful bot is player {peaceful}")
    assert peaceful and peaceful[0] is not None, "the peaceful bot was not named"
    assert sum(fired.values()) > 0, f"no bot fired: {fired}"
    assert not any(skater in peaceful for _, skater, _ in decided), "the peaceful bot ran someone over"

def test_skating_bots_run_over(tmp_path):
    """A bot that rams (debug.bot_skate "ram": it stays on its board in a
    fight) and a peaceful bot standing still on foot ('stand'), on Blood Gulch's dedicated
    server (the server image's build, HALO_TEST_SERVER) with Skate 3 data: the first rides at the other on foot and runs
    them over, the host deciding it (skate_authority.c) and crediting the
    kill to the skater, as a vehicle's driver. Until the first such kill (at
    most a five-minute game): about two to three minutes."""
    _, maps, assets = _needs_skate()
    log = _bots_alone(tmp_path, "server", _server(), maps, assets, {
        "HALO_SERVER_BOTS": "2", "HALO_BOT_DIFFICULTIES": "hard,peaceful", "HALO_BOT_SKATE": "ram,stand",
        "HALO_BOT_SEED": "1", "HALO_SERVER_MAPS": "bloodgulch", "HALO_SERVER_TIME_LIMIT": "5"},
        (_test_address(251), _test_address(252)), done=lambda log: RUN_OVER_KILLED.search(log))
    boards = _boards(log)
    killed = RUN_OVER_KILLED.findall(log)
    print(f"boards {boards}; run-overs {RUN_OVER_DECIDED.findall(log)}; killed by {killed}; "
          f"scores {SCORE.findall(log)}")
    boarded = {player for player, _ in BOARDED.findall(log)}
    assert boarded == {"0"}, f"not the ramming bot alone (player 0) got on a board: {boarded}"
    assert killed and all(player == "0" for player in killed), \
        f"no person run over died, killed by the skater (player 0): {killed}"


def test_skating_bots_harmless(tmp_path):
    """Two peaceful bots told to ram (debug.bot_skate "ram,ram") on Prisoner's
    dedicated server with Skate 3 data: they get on boards and ride, but never
    ride at anyone nor run anyone over, and nobody dies. About three and a
    half minutes."""
    _, maps, assets = _needs_skate()
    log = _bots_alone(tmp_path, "server", _server(), maps, assets, {
        "HALO_SERVER_BOTS": "2", "HALO_SERVER_BOT_DIFFICULTY": "peaceful", "HALO_BOT_SKATE": "ram,ram",
        "HALO_BOT_SEED": "1", "HALO_SERVER_MAPS": "prisoner", "HALO_SERVER_TIME_LIMIT": "3"},
        (_test_address(253), _test_address(254)))
    boards = _boards(log)
    scores = SCORE.findall(log)
    print(f"boards {boards}; run-overs {RUN_OVER_DECIDED.findall(log)}; scores {scores}")
    assert sum(b[0] for b in boards.values()) >= 2 and sum(b[2] for b in boards.values()) >= 30, \
        f"the peaceful bots hardly rode: {boards}"
    assert all(b[5] == 0 for b in boards.values()), f"a peaceful bot rode at someone: {boards}"
    assert not RUN_OVER_DECIDED.findall(log), "a peaceful bot ran someone over"
    assert all(int(kills) == 0 and int(deaths) == 0 for _, _, kills, deaths in scores), f"someone died: {scores}"


def test_skating_bots_need_data(tmp_path):
    """The same dedicated server without Skate 3 data (HALO_SKATE_ASSETS names
    no folder, as the server image without its skate-data volume): it says
    so, makes no skate session, and its bots stay on foot and play. About a
    minute and a half."""
    _, maps, _ = _needs_skate()
    log = _bots_alone(tmp_path, "server", _server(), maps, str(tmp_path / "no-skate-data" / "assets"), {
        "HALO_SERVER_BOTS": "2", "HALO_BOT_SEED": "1", "HALO_SERVER_MAPS": "prisoner", "HALO_SERVER_TIME_LIMIT": "1"},
        (_test_address(253), _test_address(254)))
    boards = _boards(log)
    went = {name: int(n) for name, n in re.findall(r"bots: (\[BOT\] \w+) at tick \d+: .*?went (\d+) world units", log)}
    print(f"boards {boards}; went {went}")
    assert "skate: a dedicated server without Skate 3 data" in log, log[-3000:]
    assert "halo-skate" not in log and "gets on a board" not in log, "a skate session without the data"
    assert len(boards) == 2 and all(b[0] == 0 for b in boards.values()), f"a bot rode without the data: {boards}"
    assert all(n >= 20 for n in went.values()) and len(went) == 2, f"the bots hardly moved: {went}"

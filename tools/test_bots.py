"""Bots (port/linux/game/bots.c; port/linux/README.md, "Bots") in real games
on this machine: a host with bots, and a client that sees them as other
players. They need a Linux build (HALO_TEST_GAME) and the Halo maps
(HALO_SKATE_TEST_HALO_MAPS), as test_linux_port.py's network tests, and skip
without them. HALO_TEST_SERVER is the dedicated server's build (the server
image's), else HALO_TEST_GAME's; HALO_TEST_OLD_CLIENT a build from before bots
(fork/main at ed14f98c), for the client of test_bots_with_an_older_client."""

import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

import pytest

from tools.test_linux_port import _game_env, _stall_backtraces, _test_address

BOT_NAMED = re.compile(r"network test: player (\d+) is named (\[BOT\] \w+)")
BOT_JOINED = re.compile(r"bots: (\[BOT\] \w+) joined \(machine (\d+), controller (\d+), team (-?\d+)\)")


def _needs(*names):
    """the builds the environment names (HALO_TEST_SERVER falls back on
    HALO_TEST_GAME) and the maps, else the test is skipped"""
    maps = os.environ.get("HALO_SKATE_TEST_HALO_MAPS")
    paths = {name: os.environ.get(name) or (os.environ.get("HALO_TEST_GAME") if name == "HALO_TEST_SERVER" else None)
             for name in names}
    if not (maps and Path(maps, "bloodgulch.map").is_file() and
            all(path and Path(path).is_file() for path in paths.values())):
        pytest.skip(f"needs {', '.join(names)} and HALO_SKATE_TEST_HALO_MAPS")
    return maps, paths


def _start(tmp_path, name, game, maps, settings):
    """a game in its own data and save roots, its output in run.log"""
    folder = tmp_path / name
    (folder / "data").mkdir(parents=True)
    (folder / "saves").mkdir()
    (folder / "data" / "maps").symlink_to(maps)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("SDL_", "HALO_", "DISPLAY", "WAYLAND"))}
    env.update(settings, HALO_NET_ONLINE="false", HALO_UPDATE_ANSWER="never",
               HALO_DATA_ROOT=str(folder / "data"), HALO_SAVE_ROOT=str(folder / "saves"))
    with open(folder / "run.log", "w") as log:
        return subprocess.Popen([game], cwd=folder, env=_game_env(env), stdout=log, stderr=subprocess.STDOUT)


def _log(tmp_path, name):
    path = tmp_path / name / "run.log"
    return path.read_text(errors="replace") if path.is_file() else ""


def _debug(tmp_path, name):
    for path in (tmp_path / name / "data" / "debug.txt", tmp_path / name / "saves" / "debug.txt"):
        if path.is_file():
            return path.read_text(errors="replace")
    return ""


def _samples(log):
    """each player's place every second as the machine's log has it
    (network_test.c): {player index: [(x, y, z, machine index)]}, a dead
    player's seconds left out"""
    samples = {}
    for line in log.splitlines():
        if "network test: tick" not in line:
            continue
        for segment in line.split(" player ")[1:]:
            place = re.match(r"(\d+): \((-?[\d.]+) (-?[\d.]+) (-?[\d.]+)\)", segment)
            machine = re.search(r" m(-?\d+)", segment)
            if place and machine:
                samples.setdefault(int(place[1]), []).append(
                    (float(place[2]), float(place[3]), float(place[4]), int(machine[1])))
    return samples


def _travelled(samples):
    """how far a player went, second to second"""
    return sum(((b[0] - a[0]) ** 2 + (b[1] - a[1]) ** 2) ** 0.5 for a, b in zip(samples, samples[1:]))


def _console(port, *commands):
    """commands for the game's telnet console (debug.telnet_console), on one
    connection (the console takes few at once, and a connection closed is a
    while letting its place go): what it answered"""
    def read(connection):
        text = b""
        try:
            while True:
                text += connection.recv(4096)
        except socket.timeout:
            return text

    answer = b""
    with socket.create_connection(("127.0.0.1", port), timeout=10) as connection:
        connection.settimeout(1.5)
        # (its greeting first: the game takes the connection a frame later)
        answer += read(connection)
        for command in commands:
            connection.sendall(command.encode() + b"\r\n")
            answer += read(connection)
    print(f"the console answered: {answer!r}")
    return answer.decode(errors="replace")


def _wait(condition, seconds, games, tmp_path, what):
    """until condition() holds; fails (with the logs) if a game stops or the time runs out"""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        for name, process in games.items():
            assert process.poll() is None, f"the {name} game stopped ({process.returncode}):\n" \
                f"{_log(tmp_path, name)[-4000:]}\n{_debug(tmp_path, name)[-2000:]}"
        if condition():
            return
        time.sleep(1)
    logs = "\n".join(f"== {name}:\n{_log(tmp_path, name)[-3000:]}" for name in games)
    raise AssertionError(f"{what}, not within {seconds} s:\n{logs}")


def _stop(games, tmp_path, executables):
    failing = sys.exc_info()[0] is not None
    for name, process in games.items():
        if process.poll() is None:
            if failing:
                _stall_backtraces(name, process, executables[name], tmp_path / name / "run.log")
            process.kill()
        process.wait()


def _bots_seen(log, count):
    """the bots a client's log names (index: name), once it names count of
    them and has seen each move a while"""
    named = dict(BOT_NAMED.findall(log))
    samples = _samples(log)
    if len(named) < count or any(len(samples.get(int(index), [])) < 12 for index in named):
        return None
    return named


def _assert_bots_play(log, named, machine_of_bots):
    """the bots, as a client saw them: on bot machines, and each walked about"""
    samples = _samples(log)
    for index, name in named.items():
        seen = samples[int(index)]
        travelled = _travelled(seen)
        machines = {sample[3] for sample in seen}
        print(f"{name} (player {index}, machines {machines}): {len(seen)} seconds seen, went {travelled:.1f} world units")
        assert machines <= machine_of_bots, f"{name} was on machine(s) {machines}, not a bot machine"
        assert travelled >= 3.0, f"{name} hardly moved ({travelled:.1f} world units): {seen[:5]} ... {seen[-5:]}"


def _dedicated_with_bots(tmp_path, client_game, server_game, maps, bots, addresses, telnet_port=None):
    settings = {"HALO_DEDICATED": "1", "HALO_NETWORK_TEST_START": "5", "HALO_SERVER_BOTS": str(bots),
                "HALO_BOT_SEED": "1", "HALO_NET_ADDRESS": addresses[0], "HALO_NET_BROADCAST": addresses[1]}
    if telnet_port:
        settings.update(HALO_TELNET_CONSOLE="1", HALO_TELNET_CONSOLE_PORT=str(telnet_port))
    games = {"server": _start(tmp_path, "server", server_game, maps, settings)}
    games["client"] = _start(tmp_path, "client", client_game, maps, {
        "HALO_NETWORK_TEST": "join", "HALO_NULL_RENDERER": "1", "HALO_NO_AUDIO": "1", "HALO_TEST_INPUT": "bot:7",
        "HALO_NET_ADDRESS": addresses[1], "HALO_NET_BROADCAST": addresses[0]})
    return games


def test_bots_on_a_dedicated_server(tmp_path):
    """A dedicated server with three bots (server.bots) and a client that
    joins it: the bots join the server's lobby before its game starts, on the
    last machine slot (127, controllers 0 to 2); the client sees them named
    "[BOT] ..." as players of that machine, walking about. Then the server's
    console (its telnet console) adds two bots in the game being played, the
    fourth on the same machine and the fifth on the next one down (126), which
    the client sees join; removes one ("bot remove"), kicks another ("kick",
    which removes a bot), and lists them; the client is told each left. About
    two minutes."""
    maps, paths = _needs("HALO_TEST_GAME", "HALO_TEST_SERVER")
    port = 23230 + os.getpid() % 500
    games = _dedicated_with_bots(tmp_path, paths["HALO_TEST_GAME"], paths["HALO_TEST_SERVER"], maps, 3,
                                 (_test_address(221), _test_address(222)), port)
    executables = {"server": paths["HALO_TEST_SERVER"], "client": paths["HALO_TEST_GAME"]}
    try:
        _wait(lambda: _bots_seen(_log(tmp_path, "client"), 3), 150, games, tmp_path,
              "the client did not see three bots play")
        server, client = _log(tmp_path, "server"), _log(tmp_path, "client")
        joined = BOT_JOINED.findall(server)
        print(f"bots joined the server: {joined}")
        assert sorted((machine, controller) for _, machine, controller, _ in joined[:3]) == \
            [("127", "0"), ("127", "1"), ("127", "2")], server[-4000:]
        named = _bots_seen(client, 3)
        assert sorted(named.values()) == sorted(name for name, *_ in joined[:3]), (named, joined)
        _assert_bots_play(client, named, {127})
        assert "server: the game starts: slayer on bloodgulch" in server, server[-4000:]

        # two more in the game being played: the client sees them join
        _console(port, "bot add 2")
        _wait(lambda: len(BOT_JOINED.findall(_log(tmp_path, "server"))) >= 5 and
              len(BOT_NAMED.findall(_log(tmp_path, "client"))) >= 5, 30, games, tmp_path,
              "two bots added in game did not join")
        joined = BOT_JOINED.findall(_log(tmp_path, "server"))
        assert [(machine, controller) for _, machine, controller, _ in joined[3:5]] == \
            [("127", "3"), ("126", "0")], joined
        _wait(lambda: all(len(_samples(_log(tmp_path, "client")).get(int(index), [])) >= 10
                          for index in dict(BOT_NAMED.findall(_log(tmp_path, "client")))), 30, games, tmp_path,
              "the bots added in game were not seen playing")
        client = _log(tmp_path, "client")
        named = dict(BOT_NAMED.findall(client))
        late = {index: name for index, name in named.items() if name in (joined[3][0], joined[4][0])}
        _assert_bots_play(client, late, {127, 126})

        # one removed, one kicked: every machine is told they quit
        first, second = joined[0][0], joined[1][0]
        third = joined[2][0]
        answer = _console(port, f"bot remove {first[6:]}", f"kick {second}", f"bot difficulty peaceful {third[6:]}",
                          "bot list")
        assert f"kick: {second} removed" in answer, answer
        assert f"{third} (team" in answer and re.search(re.escape(third) + r" \(team \d+, peaceful\)", answer), answer
        _wait(lambda: f"server: {first} left" in _log(tmp_path, "server") and
              f"server: {second} left" in _log(tmp_path, "server") and
              _debug(tmp_path, "client").count("quit of of game") >= 2, 30, games, tmp_path,
              "the removed and kicked bots did not leave")
        server = _log(tmp_path, "server")
        listed = re.findall(r"bots: listed (\[BOT\] \w+)", server)
        assert sorted(listed) == sorted(name for name, *_ in joined[2:5]), (listed, joined)
        games["server"].terminate()
        assert games["server"].wait(timeout=15) == 0, _log(tmp_path, "server")[-4000:]
    finally:
        _stop(games, tmp_path, executables)


def test_bots_with_an_older_client(tmp_path):
    """A build from before bots (HALO_TEST_OLD_CLIENT: fork/main at ed14f98c)
    joins a dedicated server with two bots and plays with them: nothing a bot
    is goes on the wire that it does not know (no new message, the network
    version still 28), so it has them as two other players of machine 127,
    named "[BOT] ...", walking about. About a minute and a half."""
    maps, paths = _needs("HALO_TEST_OLD_CLIENT", "HALO_TEST_SERVER")
    games = _dedicated_with_bots(tmp_path, paths["HALO_TEST_OLD_CLIENT"], paths["HALO_TEST_SERVER"], maps, 2,
                                 (_test_address(223), _test_address(224)))
    executables = {"server": paths["HALO_TEST_SERVER"], "client": paths["HALO_TEST_OLD_CLIENT"]}
    try:
        _wait(lambda: _bots_seen(_log(tmp_path, "client"), 2), 150, games, tmp_path,
              "the older client did not see two bots play")
        client = _log(tmp_path, "client")
        _assert_bots_play(client, _bots_seen(client, 2), {127})
        assert "server: Player joined" in _log(tmp_path, "server") or \
            re.search(r"server: (?!\[BOT\])\S.* joined", _log(tmp_path, "server")), _log(tmp_path, "server")[-4000:]
    finally:
        _stop(games, tmp_path, executables)


def test_bots_on_a_listen_host(tmp_path):
    """A host that plays too (debug.network_test "host", not dedicated) with
    two bots, and a client: the host's own player is on its own machine and
    the bots on machine 127; both machines see the bots walk about, the host's
    own player still its local one. About a minute and a half."""
    maps, paths = _needs("HALO_TEST_GAME")
    game = paths["HALO_TEST_GAME"]
    host_address, join_address = _test_address(225), _test_address(226)
    games = {"host": _start(tmp_path, "host", game, maps, {
        "HALO_NETWORK_TEST": "host:bloodgulch", "HALO_NETWORK_TEST_START": "8", "HALO_SERVER_BOTS": "2",
        "HALO_BOT_SEED": "2", "HALO_NULL_RENDERER": "1", "HALO_NO_AUDIO": "1", "HALO_TEST_INPUT": "bot:3",
        "HALO_NET_ADDRESS": host_address, "HALO_NET_BROADCAST": join_address})}
    games["join"] = _start(tmp_path, "join", game, maps, {
        "HALO_NETWORK_TEST": "join", "HALO_NULL_RENDERER": "1", "HALO_NO_AUDIO": "1", "HALO_TEST_INPUT": "bot:7",
        "HALO_NET_ADDRESS": join_address, "HALO_NET_BROADCAST": host_address})
    try:
        # (a host's first start makes its map caches, a gigabyte, slow on a
        # busy runner: the tests' other half beside it)
        _wait(lambda: _bots_seen(_log(tmp_path, "join"), 2) and _bots_seen(_log(tmp_path, "host"), 2), 240, games,
              tmp_path, "the machines did not see two bots play")
        for name in ("host", "join"):
            _assert_bots_play(_log(tmp_path, name), _bots_seen(_log(tmp_path, name), 2), {127})
        # (the host's own player is still its local player, and no bot)
        host = _log(tmp_path, "host")
        local = re.findall(r"\| local (-?\d+) camera", host)
        assert local and local[-1] != "-1" and local[-1] not in _bots_seen(host, 2), host[-3000:]
    finally:
        _stop(games, tmp_path, {"host": game, "join": game})


STOCK_MAPS = ("beavercreek", "bloodgulch", "boardingaction", "carousel", "chillout", "damnation", "hangemhigh",
              "longest", "prisoner", "putput", "ratrace", "sidewinder", "wizard")
# the multiplayer weapons the walking graph reaches on each stock map, at
# least (the rest of each map's are all of them): those it does not, a few,
# need a map's own links checked in the game (bots_nav_game.c's overrides)
# (Damnation's other five lie in a duct behind a slit no biped passes;
# Longest's four are on platforms 2.1 high with nothing to climb from, and
# Chillout's, Hang 'Em High's and Rat Race's one each on ledges too narrow
# for the graph)
WEAPONS_REACHED_AT_LEAST = {"chillout": 32, "damnation": 41, "hangemhigh": 44, "longest": 36, "ratrace": 29}
GRAPH = re.compile(r"bots: walking graph of (\w+) (built|read from the cache) in (\d+) ms: (\d+) nodes, .*? spawns "
                   r"(\d+)/(\d+) reach the main part, weapons (\d+)/(\d+) are reached from it")


def _nav_check(tmp_path, name, server, maps, map_names):
    """a dedicated server through the maps (debug.bot_nav_check ends each
    game once its graph is made); each map's graph line"""
    games = {name: _start(tmp_path, name, server, maps, {
        "HALO_DEDICATED": "1", "HALO_NETWORK_TEST_START": "3", "HALO_BOT_NAV_CHECK": "1",
        "HALO_SERVER_MAPS": ",".join(map_names), "HALO_NET_ADDRESS": _test_address(227),
        "HALO_NET_BROADCAST": _test_address(228)})}
    try:
        _wait(lambda: {m[0] for m in GRAPH.findall(_log(tmp_path, name))} >= set(map_names), 60 + 30 * len(map_names),
              games, tmp_path, f"not every map's walking graph was made ({', '.join(map_names)})")
    finally:
        _stop(games, tmp_path, {name: server})
        # (the game's own caches of the maps, a gigabyte: not the graphs')
        for cache in (tmp_path / name / "saves").rglob("cache*.map"):
            cache.unlink()
    return {m[0]: m for m in GRAPH.findall(_log(tmp_path, name))}


def test_bots_nav_on_every_map(tmp_path):
    """The bots' walking graph (port/linux/game/bots_nav.c) of each stock
    multiplayer map, built by a dedicated server going through all thirteen
    (debug.bot_nav_check): every spawn reaches the spawns' main part (a bot
    spawned anywhere joins the game), and the main part reaches every
    multiplayer weapon but the few WEAPONS_REACHED_AT_LEAST leaves out. The
    graphs go to the save root's cache: a second server, on two of the maps,
    reads them from it, the same. Skips the maps the maps folder lacks.
    About four minutes."""
    maps, paths = _needs("HALO_TEST_SERVER")
    names = [name for name in STOCK_MAPS if Path(maps, f"{name}.map").is_file()]
    built = _nav_check(tmp_path, "server", paths["HALO_TEST_SERVER"], maps, names)
    for name in names:
        _, how, milliseconds, nodes, spawns, spawn_count, weapons, weapon_count = built[name]
        print(f"{name}: {how} in {milliseconds} ms, {nodes} nodes; spawns {spawns}/{spawn_count}, "
              f"weapons {weapons}/{weapon_count}")
        assert how == "built", built[name]
        assert spawns == spawn_count, f"{name}: only {spawns} of {spawn_count} spawns reach the main part"
        assert int(weapons) >= WEAPONS_REACHED_AT_LEAST.get(name, int(weapon_count)), \
            f"{name}: only {weapons} of {weapon_count} weapons are reached:\n" + \
            "\n".join(line for line in _log(tmp_path, "server").splitlines() if f"nav check: {name} " in line)
    # the cache: the same graphs, read, not built (its save root moved over)
    second = names[:2]
    (tmp_path / "again").mkdir()
    (tmp_path / "server" / "saves").rename(tmp_path / "again" / "saves")
    (tmp_path / "server" / "data").rename(tmp_path / "again" / "data")
    read = _nav_check_again(tmp_path, paths["HALO_TEST_SERVER"], maps, second)
    for name in second:
        assert read[name][1] == "read from the cache", read[name]
        assert read[name][3:] == built[name][3:], (read[name], built[name])


def _nav_check_again(tmp_path, server, maps, map_names):
    """_nav_check in tmp_path/again, whose save root is there already"""
    folder = tmp_path / "again"
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("SDL_", "HALO_", "DISPLAY", "WAYLAND"))}
    env.update(HALO_DEDICATED="1", HALO_NETWORK_TEST_START="3", HALO_BOT_NAV_CHECK="1",
               HALO_SERVER_MAPS=",".join(map_names), HALO_NET_ADDRESS=_test_address(229),
               HALO_NET_BROADCAST=_test_address(230), HALO_NET_ONLINE="false", HALO_UPDATE_ANSWER="never",
               HALO_DATA_ROOT=str(folder / "data"), HALO_SAVE_ROOT=str(folder / "saves"))
    with open(folder / "run.log", "w") as log:
        games = {"again": subprocess.Popen([server], cwd=folder, env=_game_env(env), stdout=log,
                                           stderr=subprocess.STDOUT)}
    try:
        _wait(lambda: {m[0] for m in GRAPH.findall(_log(tmp_path, "again"))} >= set(map_names), 120, games, tmp_path,
              "the cached graphs were not read")
    finally:
        _stop(games, tmp_path, {"again": server})
        for cache in (folder / "saves").rglob("cache*.map"):
            cache.unlink()
    return {m[0]: m for m in GRAPH.findall(_log(tmp_path, "again"))}


ROAMED = re.compile(r"bots: (\[BOT\] \w+) at tick (\d+): (\d+) goals reached, (\d+) paths \((\d+) failed\), stuck (\d+) "
                    r"times(?: \(\d+ seconds\))?, went (\d+) world units \((\d+) drops, (\d+) jumps, (\d+) teleporters, (\d+) ladders up, "
                    r"(\d+) down\); \d+ targets seen, fired \d+ ticks, (\d+) weapons taken")


def test_bots_roam(tmp_path):
    """Four peaceful bots (server.bot_difficulty) on Boarding Action's dedicated server, alone, for 80
    seconds of game: each goes from goal to goal along the walking graph
    (bots_brain.c) and to weapons lying about, going a long way, and between
    them they take the ladders. About two minutes."""
    maps, paths = _needs("HALO_TEST_SERVER")
    if not Path(maps, "boardingaction.map").is_file():
        pytest.skip("needs boardingaction.map")
    server = paths["HALO_TEST_SERVER"]
    games = {"server": _start(tmp_path, "server", server, maps, {
        "HALO_DEDICATED": "1", "HALO_NETWORK_TEST_START": "3", "HALO_SERVER_BOTS": "4", "HALO_BOT_SEED": "3",
        "HALO_SERVER_BOT_DIFFICULTY": "peaceful",
        "HALO_SERVER_MAPS": "boardingaction", "HALO_NET_ADDRESS": _test_address(231),
        "HALO_NET_BROADCAST": _test_address(232)})}
    try:
        _wait(lambda: len([m for m in ROAMED.findall(_log(tmp_path, "server")) if int(m[1]) >= 2400]) >= 4, 240, games,
              tmp_path, "the bots did not roam 80 seconds")
    finally:
        _stop(games, tmp_path, {"server": server})
        for cache in (tmp_path / "server" / "saves").rglob("cache*.map"):
            cache.unlink()
    last = {m[0]: m for m in ROAMED.findall(_log(tmp_path, "server")) if int(m[1]) == 2400}
    for name, m in last.items():
        print(f"{name}: {m[2]} goals, {m[3]} paths ({m[4]} failed), stuck {m[5]} times, went {m[6]} world units, "
              f"{m[7]} drops, {m[8]} jumps, {m[9]} teleporters, {m[10]} ladders up, {m[11]} down, {m[12]} weapons taken")
    assert len(last) == 4, last
    stuck = {name: counts.get("seconds", 0) for name, counts in _reports(_log(tmp_path, "server")).items()
             if counts["tick"] == 2400}
    print(f"seconds stuck in 80: {stuck}")
    for name, m in last.items():
        assert int(m[2]) + int(m[12]) >= 1, f"{name} reached no goal and took no weapon: {m}"
        assert int(m[6]) >= 60, f"{name} went only {m[6]} world units: {m}"
        # (Boarding Action's ladders and lips: 9 to 21 seconds stuck in 80 before
        # the climb counted as getting somewhere, 0 to 3 since)
        assert stuck.get(name, 0) <= 12, f"{name} was stuck {stuck.get(name)} seconds of 80: {m}"
    assert sum(int(m[10]) + int(m[11]) for m in last.values()) >= 1, f"no bot took a ladder: {last}"


SCORE = re.compile(r"server: score: (\[BOT\] \w+) (-?\d+), (\d+) kills, (\d+) deaths")
# a bot's score in a game with teams, and the teams'
TEAM_SCORE = re.compile(r"server: score: (\[BOT\] \w+) \((red|blue)\) (-?\d+), (\d+) kills, (\d+) deaths")
TEAMS_SCORE = re.compile(r"server: score: red (-?\d+), blue (-?\d+)")
REPORT = re.compile(r"bots: (\[BOT\] \w+) at tick (\d+): (.*)")


def _reports(log):
    """each bot's last report (bots.c's bots_report): {name: {what: count}},
    the counts named as the report names them ("grenades thrown": 3)"""
    reports = {}
    for name, tick, text in REPORT.findall(log):
        counts = {"tick": int(tick)}
        for number, what in re.findall(r"(\d+) ([a-z][a-z ]*?)(?=[,;()]|$)", text):
            counts.setdefault(what.strip(), int(number))
        reports[name] = counts
    return reports


def _bots_game(tmp_path, name, settings, games_over=1, seconds=300):
    """a dedicated server with bots alone (settings: its HALO_ settings)
    until games_over of its games are over, each with its bots' scores; its
    log"""
    maps, paths = _needs("HALO_TEST_SERVER")
    server = paths["HALO_TEST_SERVER"]
    bots = int(settings.get("HALO_SERVER_BOTS", "1"))
    games = {name: _start(tmp_path, name, server, maps, {
        "HALO_DEDICATED": "1", "HALO_NETWORK_TEST_START": "3", **settings})}
    try:
        _wait(lambda: len(re.findall(r"server: score: \[BOT\]", _log(tmp_path, name))) >= games_over * bots,
              seconds, games, tmp_path, "the bots' game did not end with their scores")
    finally:
        _stop(games, tmp_path, {name: server})
        for cache in (tmp_path / name / "saves").rglob("cache*.map"):
            cache.unlink()
    return _log(tmp_path, name)


def test_bots_slayer(tmp_path):
    """Four bots (normal) play a two-minute Slayer on Hang 'Em High's
    dedicated server, alone: they find each other, shoot (only at what they
    see: bots_combat.c) and kill each other: the game ends with each bot
    having killed and died, and between them they took weapons lying about.
    About three minutes."""
    maps, paths = _needs("HALO_TEST_SERVER")
    server = paths["HALO_TEST_SERVER"]
    games = {"server": _start(tmp_path, "server", server, maps, {
        "HALO_DEDICATED": "1", "HALO_NETWORK_TEST_START": "3", "HALO_SERVER_BOTS": "4", "HALO_BOT_SEED": "5",
        "HALO_SERVER_BOT_DIFFICULTY": "normal", "HALO_SERVER_MAPS": "hangemhigh", "HALO_SERVER_TIME_LIMIT": "2",
        "HALO_NET_ADDRESS": _test_address(233), "HALO_NET_BROADCAST": _test_address(234)})}
    try:
        _wait(lambda: len(SCORE.findall(_log(tmp_path, "server"))) >= 4, 240, games, tmp_path,
              "the bots' game did not end with their scores")
    finally:
        _stop(games, tmp_path, {"server": server})
        for cache in (tmp_path / "server" / "saves").rglob("cache*.map"):
            cache.unlink()
    log = _log(tmp_path, "server")
    scores = SCORE.findall(log)[:4]
    reports = {m[0]: m for m in re.findall(r"bots: (\[BOT\] \w+) at tick (\d+): .*?(\d+) targets seen, fired (\d+) ticks, "
                                           r"(\d+) weapons taken", log)}
    for name, score, kills, deaths in scores:
        print(f"{name}: score {score}, {kills} kills, {deaths} deaths; {reports.get(name)}")
    assert len(scores) == 4, log[-4000:]
    assert sum(int(kills) for _, _, kills, _ in scores) >= 8, scores
    assert all(int(kills) >= 1 and int(deaths) >= 1 for _, _, kills, deaths in scores), scores
    assert sum(int(m[4]) for m in reports.values()) >= 1, f"no bot took a weapon: {reports}"


def test_bots_peaceful(tmp_path):
    """Four peaceful bots (server.bot_difficulty "peaceful") play the same
    two-minute Slayer as test_bots_slayer's: they roam and take weapons as
    the others do, but never fight - not one shot fired, no kills - where
    the normal bots of test_bots_slayer kill each other. About three
    minutes."""
    maps, paths = _needs("HALO_TEST_SERVER")
    server = paths["HALO_TEST_SERVER"]
    games = {"server": _start(tmp_path, "server", server, maps, {
        "HALO_DEDICATED": "1", "HALO_NETWORK_TEST_START": "3", "HALO_SERVER_BOTS": "4", "HALO_BOT_SEED": "5",
        "HALO_SERVER_BOT_DIFFICULTY": "peaceful", "HALO_SERVER_MAPS": "hangemhigh", "HALO_SERVER_TIME_LIMIT": "2",
        "HALO_NET_ADDRESS": _test_address(237), "HALO_NET_BROADCAST": _test_address(238)})}
    try:
        _wait(lambda: len(SCORE.findall(_log(tmp_path, "server"))) >= 4, 240, games, tmp_path,
              "the bots' game did not end with their scores")
    finally:
        _stop(games, tmp_path, {"server": server})
        for cache in (tmp_path / "server" / "saves").rglob("cache*.map"):
            cache.unlink()
    log = _log(tmp_path, "server")
    scores = SCORE.findall(log)[:4]
    reports = {m[0]: m for m in re.findall(r"bots: (\[BOT\] \w+) at tick (\d+): .*?went (\d+) world units.*?(\d+) "
                                           r"targets seen, fired (\d+) ticks, (\d+) weapons taken", log)}
    for name, score, kills, deaths in scores:
        print(f"{name}: score {score}, {kills} kills, {deaths} deaths; {reports.get(name)}")
    assert len(scores) == 4 and len(reports) == 4, log[-4000:]
    assert all(int(kills) == 0 for _, _, kills, _ in scores), f"a peaceful bot killed: {scores}"
    assert all(int(m[4]) == 0 and int(m[3]) == 0 for m in reports.values()), f"a peaceful bot aimed or fired: {reports}"
    assert all(int(m[2]) >= 60 for m in reports.values()), f"a peaceful bot hardly moved: {reports}"
    assert sum(int(m[5]) for m in reports.values()) >= 1, f"no peaceful bot took a weapon: {reports}"
    thrown = {name: counts.get("grenades thrown") for name, counts in _reports(log).items()}
    assert all(count == 0 for count in thrown.values()) and len(thrown) == 4, f"a peaceful bot threw a grenade: {thrown}"


def test_bots_grenades(tmp_path):
    """Six legendary bots play a two-minute Team Slayer on Beaver Creek, three
    against three (everyone spawns with frag grenades there): they throw
    grenades (bots_grenades.c) at enemies in view and where they last saw
    them, and hit enemies with them, but never kill themselves and hardly
    ever a team mate (at most one, who ran into the blast); each hears the fight
    (bots_hearing.c), and they run from grenades. About two and a half
    minutes."""
    maps, _ = _needs("HALO_TEST_SERVER")
    if not Path(maps, "beavercreek.map").is_file():
        pytest.skip("needs beavercreek.map")
    log = _bots_game(tmp_path, "server", {
        "HALO_SERVER_BOTS": "6", "HALO_BOT_SEED": "4", "HALO_SERVER_BOT_DIFFICULTY": "legendary",
        "HALO_SERVER_MAPS": "beavercreek", "HALO_SERVER_GAME_TYPES": "team_slayer", "HALO_SERVER_TIME_LIMIT": "2",
        "HALO_NET_ADDRESS": _test_address(239), "HALO_NET_BROADCAST": _test_address(240)})
    reports = _reports(log)
    scores = TEAM_SCORE.findall(log)[:6]
    for name, counts in sorted(reports.items()):
        print(f"{name}: {counts}")
    print(f"scores: {scores}; teams {TEAMS_SCORE.findall(log)}")
    assert len(reports) == 6 and len(scores) == 6, log[-4000:]

    def total(what):
        return sum(counts.get(what, 0) for counts in reports.values())

    assert total("grenades thrown") >= 4, f"the bots threw only {total('grenades thrown')} grenades"
    assert total("grenade hits") >= 1, "no bot's grenade hit an enemy"
    print(f"grenades: {total('grenades thrown')} thrown, {total('grenade hits')} hits, {total('grenade kills')} kills")
    assert total("grenade suicides") == 0, "a bot killed itself with its grenade:\n" + \
        "\n".join(line for line in log.splitlines() if "killed itself" in line)
    # (a team mate who runs into a grenade after it left the hand: rare, about
    # one game in ten; more than one is the bots' fault)
    assert total("team kills") <= 1, "bots killed team mates:\n" + \
        "\n".join(line for line in log.splitlines() if "killed a team mate" in line)
    assert all(counts.get("noises heard", 0) >= 1 for counts in reports.values()), "a bot heard nothing of the fight"
    assert total("grenades dodged") >= 1, "no bot ran from a grenade"


def test_bots_leave_for_a_person(tmp_path):
    """A dedicated server full of bots (server.max_players 3, server.bots 3)
    and a person who joins it: a bot leaves at once to make room, the person
    plays, and when they leave a bot comes back. About a minute and a half."""
    maps, paths = _needs("HALO_TEST_GAME", "HALO_TEST_SERVER")
    server, game = paths["HALO_TEST_SERVER"], paths["HALO_TEST_GAME"]
    games = {"server": _start(tmp_path, "server", server, maps, {
        "HALO_DEDICATED": "1", "HALO_NETWORK_TEST_START": "3", "HALO_SERVER_BOTS": "3", "HALO_SERVER_MAX_PLAYERS": "3",
        "HALO_BOT_SEED": "6", "HALO_NET_ADDRESS": _test_address(235), "HALO_NET_BROADCAST": _test_address(236)})}
    executables = {"server": server, "client": game}
    try:
        _wait(lambda: len(BOT_JOINED.findall(_log(tmp_path, "server"))) >= 3 and
              "server: the game starts" in _log(tmp_path, "server"), 90, games, tmp_path, "three bots did not join")
        games["client"] = _start(tmp_path, "client", game, maps, {
            "HALO_NETWORK_TEST": "join", "HALO_NULL_RENDERER": "1", "HALO_NO_AUDIO": "1", "HALO_TEST_INPUT": "bot:7",
            "HALO_NET_ADDRESS": _test_address(236), "HALO_NET_BROADCAST": _test_address(235)})
        position = re.compile(r"network test: tick \d+ .* local (\d+) camera")
        _wait(lambda: "left to make room for a person" in _log(tmp_path, "server") and
              len([m for m in position.findall(_log(tmp_path, "client")) if m != "-1"]) >= 5, 90, games, tmp_path,
              "the person did not get into the full game")
        server_log = _log(tmp_path, "server")
        assert re.search(r"server: (?!\[BOT\])\S.* joined", server_log), server_log[-3000:]
        joined_before = len(BOT_JOINED.findall(server_log))
        games["client"].kill()
        games["client"].wait()
        _wait(lambda: len(BOT_JOINED.findall(_log(tmp_path, "server"))) > joined_before, 60, {"server": games["server"]},
              tmp_path, "no bot came back after the person left")
    finally:
        _stop(games, tmp_path, executables)
        for cache in tmp_path.rglob("cache*.map"):
            cache.unlink()

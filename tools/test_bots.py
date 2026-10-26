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
BOT_JOINED = re.compile(r"bots: (\[BOT\] \w+) joined \(machine (\d+), controller (\d+), team (-?\d+)(?:, \w+)?\)")


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
    # (the game's own random numbers fixed too - spawns, weapons' spread -
    # with the bots': a test's games go the same way each run, as near as the
    # machine's timing lets them)
    env.setdefault("HALO_GAME_SEED", str(1000 + int(env.get("HALO_BOT_SEED", "0"))))
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


def test_bots_a_client_loading_while_the_graphs_build(tmp_path):
    """A dedicated server with two bots on Blood Gulch, no graph in its cache,
    and a client joining it: at the game's start the server builds the bots'
    graphs, its walking graph made 20 seconds slower
    (debug.bot_nav_build_delay) - longer than a client waits for a host that
    sends nothing (15 seconds). The graphs are built aside
    (bots_nav_game.c), so the server and its network go on meanwhile, its
    game waiting for them to start its clock as for a machine still loading:
    the client, loading the map meanwhile, is not timed out, and sees the
    bots play once the game starts. About a minute and a half."""
    maps, paths = _needs("HALO_TEST_GAME", "HALO_TEST_SERVER")
    addresses = (_test_address(221), _test_address(222))
    games = {"server": _start(tmp_path, "server", paths["HALO_TEST_SERVER"], maps, {
        "HALO_DEDICATED": "1", "HALO_NETWORK_TEST_START": "5", "HALO_SERVER_BOTS": "2", "HALO_BOT_SEED": "1",
        "HALO_SERVER_MAPS": "bloodgulch", "HALO_BOT_NAV_BUILD_DELAY": "20",
        "HALO_NET_ADDRESS": addresses[0], "HALO_NET_BROADCAST": addresses[1]})}
    games["client"] = _start(tmp_path, "client", paths["HALO_TEST_GAME"], maps, {
        "HALO_NETWORK_TEST": "join", "HALO_NULL_RENDERER": "1", "HALO_NO_AUDIO": "1", "HALO_TEST_INPUT": "bot:7",
        "HALO_NET_ADDRESS": addresses[1], "HALO_NET_BROADCAST": addresses[0]})
    executables = {"server": paths["HALO_TEST_SERVER"], "client": paths["HALO_TEST_GAME"]}
    aside = re.compile(r"bots: walking graph of bloodgulch built aside: (\d+) segments asked of the game's collision, "
                       r"over (\d+) frames")
    try:
        _wait(lambda: aside.search(_log(tmp_path, "server")) and _bots_seen(_log(tmp_path, "client"), 2), 150, games,
              tmp_path, "the client did not see the bots play after their graph was built")
    finally:
        _stop(games, tmp_path, executables)
    server, client = _log(tmp_path, "server"), _log(tmp_path, "client")
    segments, frames = (int(n) for n in aside.search(server).groups())
    built = re.search(r"bots: walking graph of bloodgulch built in (\d+) ms", server)
    print(f"walking graph built in {built and built[1]} ms, aside: {segments} segments asked over {frames} frames")
    assert "server: the game starts: slayer on bloodgulch" in server, server[-4000:]
    # (the server went on, a frame at a time, its network too, while the graph was built)
    assert frames >= 20 * 30 * 3 // 4, f"the server stood still while the graph was built: {frames} frames"
    # (and its game waited for the graph to start: the bots began with it)
    assert "bots: the game waits for the bots' graphs of the map" in server, server[-4000:]
    # (a client that waited for a host sending nothing gives up: "the game host went down", its game log says)
    assert not re.search(r"the game host went down|timeout in network", client + _debug(tmp_path, "client")), \
        _debug(tmp_path, "client")[-3000:]
    _assert_bots_play(client, _bots_seen(client, 2), {127})


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



def test_bots_start_now_in_a_lobby(tmp_path):
    """A host that plays too, with two bots in its lobby and nobody else (the
    owner's game: "[BOT] Alpha, [BOT] Bravo" and the host, Battle Creek,
    Slayer), started as a person starts it: START NOW pressed once in the
    lobby (debug.network_test_start_now: the pregame screen's own request),
    not the tests' start at once. Its countdown starts (a host with bots
    alone starts as a host alone does - before, START NOW did nothing: the
    bots made it no longer alone), the game starts within the countdown, and
    the lobby's reason to wait, if any, is never a bot. About a minute."""
    maps, paths = _needs("HALO_TEST_GAME")
    game = paths["HALO_TEST_GAME"]
    games = {"host": _start(tmp_path, "host", game, maps, {
        "HALO_NETWORK_TEST": "host:beavercreek", "HALO_NETWORK_TEST_START": "5", "HALO_NETWORK_TEST_START_NOW": "1",
        "HALO_SERVER_BOTS": "2", "HALO_BOT_SEED": "4", "HALO_NULL_RENDERER": "1", "HALO_NO_AUDIO": "1",
        "HALO_TEST_INPUT": "bot:3", "HALO_NET_ADDRESS": _test_address(209), "HALO_NET_BROADCAST": _test_address(210)})}
    try:
        _wait(lambda: "START NOW pressed in the lobby" in _log(tmp_path, "host"), 120, games, tmp_path,
              "the host never pressed START NOW")
        _wait(lambda: len(_bots_seen(_log(tmp_path, "host"), 2) or {}) == 2, 70, games, tmp_path,
              "the game did not start within the countdown after START NOW, with two bots")
    finally:
        _stop(games, tmp_path, {"host": game})
        for cache in tmp_path.rglob("cache*.map"):
            cache.unlink()
    log = _log(tmp_path, "host")
    waits = re.findall(r"server: the game's start waits: (.*)", log)
    print(f"the start waited on: {waits}")
    assert "(not sent)" not in log, "START NOW was not sent"
    assert not any("BOT" in reason for reason in waits), f"the start waited on a bot: {waits}"


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
# the stock maps that place vehicles, and how many (each on the drive graph's
# main part: bots_nav_vehicle_parameters)
VEHICLES_PLACED = {"bloodgulch": 8, "sidewinder": 8}
# ... and the Scorpions of each, on the tanks' graph
TANKS_PLACED = {"bloodgulch": 2, "sidewinder": 2}
TANK_GRAPH = re.compile(r"bots: tank graph of (\w+) (built|read from the cache) in (\d+) ms: (\d+) nodes, (\d+) edges .*?"
                        r"tanks (\d+)/(\d+) on its main part")
DRIVE_GRAPH = re.compile(r"bots: drive graph of (\w+) (built|read from the cache) in (\d+) ms: (\d+) nodes, (\d+) edges .*?"
                         r"vehicles (\d+)/(\d+) on its main part")
GRAPH = re.compile(r"bots: walking graph of (\w+) (built|read from the cache) in (\d+) ms: (\d+) nodes, .*? spawns "
                   r"(\d+)/(\d+) reach the main part, weapons (\d+)/(\d+) are reached from it")


def _nav_check(tmp_path, name, server, maps, map_names):
    """a dedicated server through the maps (debug.bot_nav_check ends each
    game once its graph is made; each next game starts a second after, before
    the server would have precached the map had it not waited for that:
    network_server_manager.c); each map's graph line"""
    games = {name: _start(tmp_path, name, server, maps, {
        "HALO_DEDICATED": "1", "HALO_NETWORK_TEST_START": "1", "HALO_BOT_NAV_CHECK": "1",
        "HALO_SERVER_MAPS": ",".join(map_names), "HALO_NET_ADDRESS": _test_address(227),
        "HALO_NET_BROADCAST": _test_address(228)})}
    try:
        _wait(lambda: {m[0] for m in GRAPH.findall(_log(tmp_path, name))} >= set(map_names) and
              {m[0] for m in DRIVE_GRAPH.findall(_log(tmp_path, name))} >= set(map_names) & set(VEHICLES_PLACED) and
              {m[0] for m in TANK_GRAPH.findall(_log(tmp_path, name))} >= set(map_names) & set(TANKS_PLACED),
              60 + 30 * len(map_names), games, tmp_path, f"not every map's graphs were made ({', '.join(map_names)})")
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
    multiplayer weapon but the few WEAPONS_REACHED_AT_LEAST leaves out. On
    the maps that place vehicles (VEHICLES_PLACED), the graph vehicles drive
    has each of them on its main part, and the tanks' graph each Scorpion.
    The graphs go to the save root's
    cache: a second server, on two of the maps, reads them from it, the
    same. Skips the maps the maps folder lacks.
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
    drives = {m[0]: m for m in DRIVE_GRAPH.findall(_log(tmp_path, "server"))}
    print({name: drives.get(name) for name in names})
    assert set(drives) == set(names) & set(VEHICLES_PLACED), f"drive graphs: {sorted(drives)}"
    for name, (_, how, _, nodes, _, on, placed) in drives.items():
        assert how == "built" and int(nodes) > 100, drives[name]
        assert int(on) == int(placed) == VEHICLES_PLACED[name], \
            f"{name}: only {on} of {placed} vehicles on the drive graph:\n" + \
            "\n".join(line for line in _log(tmp_path, "server").splitlines() if f"nav check: {name} vehicle" in line)
    tanks = {m[0]: m for m in TANK_GRAPH.findall(_log(tmp_path, "server"))}
    print({name: tanks.get(name) for name in names})
    assert set(tanks) == set(names) & set(TANKS_PLACED), f"tank graphs: {sorted(tanks)}"
    for name, (_, how, _, nodes, _, on, placed) in tanks.items():
        assert how == "built" and int(nodes) > 100, tanks[name]
        assert int(on) == int(placed) == TANKS_PLACED[name], f"{name}: only {on} of {placed} tanks on the tank graph"
    # the cache: the same graphs, read, not built (its save root moved over)
    second = names[:2]
    (tmp_path / "again").mkdir()
    (tmp_path / "server" / "saves").rename(tmp_path / "again" / "saves")
    (tmp_path / "server" / "data").rename(tmp_path / "again" / "data")
    read = _nav_check_again(tmp_path, paths["HALO_TEST_SERVER"], maps, second)
    for name in second:
        assert read[name][1] == "read from the cache", read[name]
        assert read[name][3:] == built[name][3:], (read[name], built[name])
    again = {m[0]: m for m in DRIVE_GRAPH.findall(_log(tmp_path, "again"))}
    for name in set(second) & set(drives):
        assert again[name][1] == "read from the cache" and again[name][3:] == drives[name][3:], (again[name], drives[name])


def _nav_check_again(tmp_path, server, maps, map_names):
    """_nav_check in tmp_path/again, whose save root is there already"""
    folder = tmp_path / "again"
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("SDL_", "HALO_", "DISPLAY", "WAYLAND"))}
    env.update(HALO_DEDICATED="1", HALO_NETWORK_TEST_START="1", HALO_BOT_NAV_CHECK="1",
               HALO_SERVER_MAPS=",".join(map_names), HALO_NET_ADDRESS=_test_address(229),
               HALO_NET_BROADCAST=_test_address(230), HALO_NET_ONLINE="false", HALO_UPDATE_ANSWER="never",
               HALO_DATA_ROOT=str(folder / "data"), HALO_SAVE_ROOT=str(folder / "saves"))
    with open(folder / "run.log", "w") as log:
        games = {"again": subprocess.Popen([server], cwd=folder, env=_game_env(env), stdout=log,
                                           stderr=subprocess.STDOUT)}
    try:
        _wait(lambda: {m[0] for m in GRAPH.findall(_log(tmp_path, "again"))} >= set(map_names) and
              {m[0] for m in DRIVE_GRAPH.findall(_log(tmp_path, "again"))} >= set(map_names) & set(VEHICLES_PLACED) and
              {m[0] for m in TANK_GRAPH.findall(_log(tmp_path, "again"))} >= set(map_names) & set(TANKS_PLACED),
              120, games, tmp_path, "the cached graphs were not read")
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
        counts = _reports(_log(tmp_path, "server")).get(name, {})
        # (a goal reached, an item taken, or far from where it spawned: Boarding
        # Action is two ships, and a goal on the other one may take longer)
        assert int(m[2]) + int(m[12]) + counts.get("items taken", 0) >= 1 or \
            counts.get("world units from a spawn at most", 0) >= 15, f"{name} got nowhere: {m}, {counts}"
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
    having killed and died, and between them they took weapons lying about,
    a power weapon or a powerup among them; they fell back to cover when
    their shields were gone, and hardly ever killed themselves. About three
    minutes."""
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
    # (fighting as people do: falling back to cover with its shields gone,
    # going for the power weapons, killing itself hardly ever: bots_cover.c,
    # bots_items.c)
    counts = _reports(log)
    print({name: {what: c.get(what) for what in ("retreats", "covers reached", "powerups taken", "weapon switches",
                                                  "power weapons taken", "suicides")} for name, c in counts.items()})

    def total(what):
        return sum(c.get(what, 0) for c in counts.values())

    assert total("retreats") >= 2 and total("covers reached") >= 1, f"the bots did not fall back to cover: {counts}"
    assert total("power weapons taken") + total("powerups taken") >= 1, f"no power weapon nor powerup taken: {counts}"
    assert total("suicides") <= 1, "bots killed themselves:\n" + \
        "\n".join(line for line in log.splitlines() if "killed itself" in line)


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


def test_bots_out_of_a_vehicle(tmp_path):
    """A bot that finds itself in a vehicle (the action button it presses for
    a weapon or a ball gets a player into a vehicle near instead) gets out
    again at once and plays on: two bots on Blood Gulch's dedicated server,
    the last of them seated as a Warthog's or Ghost's driver eight seconds in
    by the host (debug.network_test_vehicle, which would take it out itself
    fifteen seconds on). The bot is out within five seconds, says so, never
    gets into one again (the host stands it at a vehicle's driver's door
    twenty seconds on), and walks on. About a minute and a half."""
    maps, paths = _needs("HALO_TEST_SERVER")
    server = paths["HALO_TEST_SERVER"]
    games = {"server": _start(tmp_path, "server", server, maps, {
        "HALO_DEDICATED": "1", "HALO_NETWORK_TEST_START": "3", "HALO_SERVER_BOTS": "2", "HALO_BOT_SEED": "8",
        "HALO_SERVER_MAPS": "bloodgulch", "HALO_SERVER_TIME_LIMIT": "1", "HALO_NETWORK_TEST_VEHICLE": "8",
        "HALO_SERVER_BOT_VEHICLES": "-1",
        "HALO_NET_ADDRESS": _test_address(219), "HALO_NET_BROADCAST": _test_address(220)})}
    try:
        _wait(lambda: len(SCORE.findall(_log(tmp_path, "server"))) >= 2, 150, games, tmp_path,
              "the bots' game did not end with their scores")
    finally:
        _stop(games, tmp_path, {"server": server})
        for cache in (tmp_path / "server" / "saves").rglob("cache*.map"):
            cache.unlink()
    log = _log(tmp_path, "server")
    seated = re.search(r"network test: the last player drives vehicle", log)
    assert seated, "the host seated no bot in a vehicle:\n" + log[-4000:]
    mistakes = re.findall(r"bots: player (\d+) got into vehicle \w+ by mistake at tick (\d+)", log)
    outs = re.findall(r"bots: player (\d+) got out of the vehicle at tick (\d+)", log)
    print(f"mistakes {mistakes}, out {outs}")
    assert len(mistakes) == 1, f"the seated bot did not notice, or got into a vehicle again: {mistakes}"
    assert outs and outs[0][0] == mistakes[0][0], f"the bot never got out: {outs}"
    assert int(outs[0][1]) - int(mistakes[0][1]) <= 5 * 30, f"the bot took more than five seconds to get out: {outs}"
    # (out before the host would have taken it out: the host's own line is
    # only for a player still in a vehicle)
    assert "network test: the last player leaves the vehicle" not in log, "the bot stayed in the vehicle"
    counts = _reports(log)
    print({name: c.get("times in a vehicle by mistake") for name, c in counts.items()})
    assert sorted(c.get("times in a vehicle by mistake") for c in counts.values()) == [0, 1], counts
    went = {m[0]: int(m[1]) for m in re.findall(r"bots: (\[BOT\] \w+) at tick \d+: .*?went (\d+) world units", log)}
    assert len(went) == 2 and all(n >= 20 for n in went.values()), f"a bot hardly moved: {went}"


def _drives(log):
    """each bot's last report of its vehicles: {name: {what: count}} with
    rides in vehicles, seconds driven, world units driven, reverses, vehicles
    flipped back, people braked for, people driven at and times in a vehicle
    by mistake"""
    keys = ("rides in vehicles", "seconds driven", "world units driven", "reverses", "vehicles flipped back",
            "people braked for", "people driven at", "times in a vehicle by mistake", "rides as a gunner",
            "seconds gunning", "ticks fired riding", "kills riding", "people run over")
    return {name: {key: counts.get(key, 0) for key in keys} for name, counts in _reports(log).items()}


def test_bots_drive(tmp_path):
    """Two bots (normal) on Blood Gulch's dedicated server, alone, each
    driving whenever its goal is a little way off and a Warthog or Ghost is
    near (debug.bot_vehicle "*drive"): they walk to a driver's door, get in,
    drive the drive graph (bots_vehicle.c) toward their goals, back up when
    stuck and get out at their goals. Slayer places Blood Gulch's two
    Warthogs, one for each: both drive, again and again, a good way between
    them, neither gets stuck in one for long, and neither drives at anyone
    on purpose. About two and a half minutes."""
    log = _bots_game(tmp_path, "server", {
        "HALO_SERVER_BOTS": "2", "HALO_BOT_SEED": "21", "HALO_SERVER_BOT_DIFFICULTY": "normal",
        "HALO_BOT_VEHICLE": "*drive", "HALO_SERVER_MAPS": "bloodgulch", "HALO_SERVER_TIME_LIMIT": "2",
        "HALO_NET_ADDRESS": _test_address(201), "HALO_NET_BROADCAST": _test_address(202)})
    drives = _drives(log)
    for name, counts in sorted(drives.items()):
        print(f"{name}: {counts}")
    print("\n".join(line for line in log.splitlines() if re.search(r"bots: player \d+ (drives|gets out|got out|is out|flipped)", line)))
    assert len(drives) == 2, log[-4000:]
    assert all(c["rides in vehicles"] >= 1 for c in drives.values()), f"a bot did not drive: {drives}"
    assert sum(c["rides in vehicles"] for c in drives.values()) >= 4, f"the bots hardly drove: {drives}"
    assert sum(c["world units driven"] for c in drives.values()) >= 150, f"the bots hardly drove: {drives}"
    assert all(c["people driven at"] == 0 and c["times in a vehicle by mistake"] == 0 for c in drives.values()), drives
    assert "cannot get out of vehicle" not in log, "a bot could not get out of its vehicle"
    assert len(re.findall(r"bots: player \d+ gets out of vehicle \w+ \(stuck\)", log)) <= 2, "bots got stuck driving"


def test_bots_drive_harmless(tmp_path):
    """Four peaceful bots drive Sidewinder's Warthogs and Ghosts as
    test_bots_drive's do: travel is no fight, so they drive, but they brake
    for anyone on foot ahead and never drive at anyone (a run-over by
    accident is allowed: none is meant), nor fire. About two and a half
    minutes."""
    maps, _ = _needs("HALO_TEST_SERVER")
    if not Path(maps, "sidewinder.map").is_file():
        pytest.skip("needs sidewinder.map")
    log = _bots_game(tmp_path, "server", {
        "HALO_SERVER_BOTS": "4", "HALO_BOT_SEED": "22", "HALO_SERVER_BOT_DIFFICULTY": "peaceful",
        "HALO_BOT_VEHICLE": "*drive", "HALO_SERVER_MAPS": "sidewinder", "HALO_SERVER_TIME_LIMIT": "2",
        "HALO_NET_ADDRESS": _test_address(203), "HALO_NET_BROADCAST": _test_address(204)})
    drives = _drives(log)
    reports = _reports(log)
    for name, counts in sorted(drives.items()):
        print(f"{name}: {counts}; fired {reports[name].get('ticks')}")
    assert len(drives) == 4, log[-4000:]
    assert sum(1 for c in drives.values() if c["rides in vehicles"] >= 1) >= 2, f"the peaceful bots did not drive: {drives}"
    assert sum(c["world units driven"] for c in drives.values()) >= 100, f"the peaceful bots hardly drove: {drives}"
    assert all(c["people driven at"] == 0 for c in drives.values()), f"a peaceful bot drove at someone: {drives}"
    fired = {m[0]: int(m[1]) for m in re.findall(r"bots: (\[BOT\] \w+) at tick \d+: .*? fired (\d+) ticks", log)}
    assert len(fired) == 4 and all(n == 0 for n in fired.values()), f"a peaceful bot fired: {fired}"


def test_bots_warthog_crew(tmp_path):
    """Four bots play a three-minute Team King of the Hill on Blood Gulch, two
    a team (both crews drive to the hill, so that they meet there: a Team
    Slayer left whether they ever met to the game's random numbers):
    one of each team drives (debug.bot_vehicle "drive") and the other takes
    only a gunner's seat ("gun"); Red's gunner is normal, Blue's peaceful
    (server.bot_difficulty's peaceful may ride a gun, and never fires). A
    driver waits for its team mate at the gun, drives on with them, and with
    an enemy in sight stops for the gunner's shots (bots_vehicle.c); the
    gunner aims and fires the Warthog's chaingun (bots_combat.c). Both
    gunners ride; the normal one fires from the gun, the peaceful one never
    fires; nobody kills a team mate. About three and a half minutes."""
    log = _bots_game(tmp_path, "server", {
        "HALO_SERVER_BOTS": "4", "HALO_BOT_SEED": "31", "HALO_BOT_DIFFICULTIES": "normal,normal,normal,peaceful",
        "HALO_BOT_VEHICLE": "drive,drive,gun,gun", "HALO_SERVER_MAPS": "bloodgulch", "HALO_SERVER_GAME_TYPES": "team_king",
        "HALO_SERVER_TIME_LIMIT": "3", "HALO_NET_ADDRESS": _test_address(207), "HALO_NET_BROADCAST": _test_address(208)},
        seconds=330)
    drives = _drives(log)
    counts = _reports(log)
    for name, c in sorted(drives.items()):
        print(f"{name}: {c}; fired {counts[name].get('ticks')}, team kills {counts[name].get('team kills')}")
    print("\n".join(line for line in log.splitlines() if re.search(r"bots: player \d+ (drives|guns|gets out)|joined", line)))
    assert len(drives) == 4, log[-4000:]
    joined = dict(re.findall(r"bots: (\[BOT\] \w+) joined \(machine \d+, controller \d+, team (\d), \w+\)", log))
    assert joined == {"[BOT] Alpha": "0", "[BOT] Bravo": "1", "[BOT] Delta": "0", "[BOT] Echo": "1"}, joined
    gunner, peaceful = drives["[BOT] Delta"], drives["[BOT] Echo"]
    assert gunner["rides as a gunner"] >= 1 and peaceful["rides as a gunner"] >= 1, f"a gunner never rode: {drives}"
    assert gunner["seconds gunning"] + peaceful["seconds gunning"] >= 20, f"the gunners hardly rode: {drives}"
    # (a crew of two bots: the gunner got in with a bot driving, or first, its
    # driver getting in while it waited at the gun)
    waited = re.findall(r"bots: player \d+ guns vehicle (\w+) \(warthog\), its driver to come at tick (\d+)", log)
    drove = re.findall(r"bots: player \d+ drives vehicle (\w+) \(warthog\) at tick (\d+)", log)
    crews = re.findall(r"bots: player \d+ guns vehicle \w+ \(warthog\), a bot driving", log) + \
        [vehicle for vehicle, tick in waited if any(v == vehicle and 0 <= int(t) - int(tick) <= 3 * 30 for v, t in drove)]
    assert crews, "no crew of two bots"
    assert gunner["ticks fired riding"] >= 30, f"the normal gunner hardly fired the chaingun: {gunner}"
    fired = {m[0]: int(m[1]) for m in re.findall(r"bots: (\[BOT\] \w+) at tick \d+: .*? fired (\d+) ticks", log)}
    assert fired["[BOT] Echo"] == 0 and peaceful["ticks fired riding"] == 0, f"the peaceful gunner fired: {fired}, {peaceful}"
    assert all(c.get("team kills", 0) == 0 for c in counts.values()), f"a team kill: {counts}"


def test_bots_ghost_fight(tmp_path):
    """Four hard bots on Blood Gulch's dedicated server, alone, with all the
    vehicles the map places (debug.network_test_vehicles "all": its four
    Ghosts too), each driving whenever it can ("*drive"): a Ghost's driver is
    its gunner, so a bot in one fights from it, aiming as it steers, closing
    on a far target and backing off a near one (bots_vehicle.c; hard bots
    also run people on foot over: test_bots_ram). At least two drive Ghosts,
    and they fire a good deal from them. About two and a half minutes."""
    log = _bots_game(tmp_path, "server", {
        "HALO_SERVER_BOTS": "4", "HALO_BOT_SEED": "41", "HALO_SERVER_BOT_DIFFICULTY": "hard", "HALO_BOT_VEHICLE": "*drive",
        "HALO_NETWORK_TEST_VEHICLES": "all", "HALO_SERVER_MAPS": "bloodgulch", "HALO_SERVER_TIME_LIMIT": "2",
        "HALO_NET_ADDRESS": _test_address(199), "HALO_NET_BROADCAST": _test_address(200)})
    drives = _drives(log)
    for name, c in sorted(drives.items()):
        print(f"{name}: {c}")
    ghosts = set(re.findall(r"bots: player (\d+) drives vehicle \w+ \(ghost\)", log))
    print(f"Ghost drivers: {ghosts}")
    assert len(drives) == 4, log[-4000:]
    assert len(ghosts) >= 2, f"fewer than two bots drove a Ghost: {ghosts}"
    assert sum(c["ticks fired riding"] for c in drives.values()) >= 200, f"the Ghosts hardly fired: {drives}"


def test_bots_ram(tmp_path):
    """Running people over, by difficulty (bots_vehicle.c's bot_vehicle_may_ram,
    the owner's rule): on Blood Gulch's dedicated server, every vehicle the map
    places, a hard bot and an easy bot that drive whenever they can
    ("drive"), and two peaceful bots that stand still on foot ("stand"). The
    hard bot hunts people on foot: it drives at a standing bot and runs it
    over, the kill its own. The easy bot never drives at anyone (it brakes
    for people), nor do the peaceful ones. About three and a half minutes."""
    log = _bots_game(tmp_path, "server", {
        "HALO_SERVER_BOTS": "4", "HALO_BOT_SEED": "1", "HALO_BOT_DIFFICULTIES": "hard,easy,peaceful,peaceful",
        "HALO_BOT_VEHICLE": "drive,drive,stand,stand", "HALO_NETWORK_TEST_VEHICLES": "all",
        "HALO_SERVER_MAPS": "bloodgulch", "HALO_SERVER_TIME_LIMIT": "3",
        "HALO_NET_ADDRESS": _test_address(197), "HALO_NET_BROADCAST": _test_address(198)}, seconds=330)
    drives = _drives(log)
    for name, c in sorted(drives.items()):
        print(f"{name}: {c}")
    print("\n".join(line for line in log.splitlines() if re.search(r"bots: player \d+ (drives at|ran over)|died of", line)))
    assert len(drives) == 4, log[-4000:]
    hard, easy = drives["[BOT] Alpha"], drives["[BOT] Bravo"]
    assert hard["people driven at"] >= 1, f"the hard bot drove at nobody: {hard}"
    assert hard["people run over"] >= 1, f"the hard bot ran nobody over: {hard}"
    assert easy["people driven at"] == 0, f"the easy bot drove at someone: {easy}"
    assert drives["[BOT] Delta"]["people driven at"] == 0 and drives["[BOT] Echo"]["people driven at"] == 0, drives


def _riding(log, index):
    """the player's places every second the machine's log has them riding
    (network_test.c): [(tick, x, y)]"""
    places = []
    for line in log.splitlines():
        tick = re.search(r"network test: tick (\d+)", line)
        if not tick:
            continue
        for segment in line.split(" player ")[1:]:
            place = re.match(rf"{index}: \((-?[\d.]+) (-?[\d.]+) (-?[\d.]+)\) [^|]*? riding", segment)
            if place:
                places.append((int(tick[1]), float(place[1]), float(place[2])))
    return places


def test_bots_share_with_a_person(tmp_path):
    """Bots and a person share vehicles (bots_vehicle.c; the wire is
    unchanged): a dedicated server with three bots in Team Slayer on Blood
    Gulch and a client: whether the client joins before the bots or after,
    the smaller team rule puts it with the second bot, who drives whenever
    it can. The host shares vehicles between the
    client's player and the Blue bot (debug.network_test_vehicle_share):
    - it seats the person at the gun of the Warthog the bot drives: the bot
      drives with them aboard, on and on (it does not get out at its goal),
      and the client sees itself carried about;
    - then seats the person as the driver of a Warthog near the bot on foot:
      the bot takes its gun ("a person driving");
    - then stands the person beside it: no bot drives that Warthog while a
      person stands by it.
    The bots are peaceful (sharing is the same at every difficulty): a bot
    shot dead on its way to the person's Warthog, or fighting beside it,
    took no gun, and whether an enemy came by was the game's random numbers'.
    About four minutes."""
    maps, paths = _needs("HALO_TEST_SERVER", "HALO_TEST_GAME")
    games = {"server": _start(tmp_path, "server", paths["HALO_TEST_SERVER"], maps, {
        "HALO_DEDICATED": "1", "HALO_NETWORK_TEST_START": "8", "HALO_SERVER_BOTS": "3", "HALO_BOT_SEED": "51",
        "HALO_SERVER_MAPS": "bloodgulch", "HALO_SERVER_GAME_TYPES": "team_slayer", "HALO_SERVER_TIME_LIMIT": "4",
        "HALO_BOT_VEHICLE": "never,drive,never", "HALO_NETWORK_TEST_VEHICLE_SHARE": "10",
        "HALO_SERVER_BOT_DIFFICULTY": "peaceful",
        "HALO_NET_ADDRESS": _test_address(195), "HALO_NET_BROADCAST": _test_address(196)})}
    games["client"] = _start(tmp_path, "client", paths["HALO_TEST_GAME"], maps, {
        "HALO_NETWORK_TEST": "join", "HALO_NULL_RENDERER": "1", "HALO_NO_AUDIO": "1",
        "HALO_NET_ADDRESS": _test_address(196), "HALO_NET_BROADCAST": _test_address(195)})
    stood = re.compile(r"network test: the person gets out of vehicle (\w+) and stands by it at tick (\d+)")
    seen = []

    def done():
        # (12 seconds on from the person standing by the Warthog)
        if stood.search(_log(tmp_path, "server")) and not seen:
            seen.append(time.monotonic())
        return bool(seen) and time.monotonic() - seen[0] > 12

    try:
        _wait(done, 330, games, tmp_path, "the host did not get through the vehicle sharing")
    finally:
        _stop(games, tmp_path, {"server": paths["HALO_TEST_SERVER"], "client": paths["HALO_TEST_GAME"]})
        for name in ("server", "client"):
            for cache in (tmp_path / name / "saves").rglob("cache*.map"):
                cache.unlink()
    server, client = _log(tmp_path, "server"), _log(tmp_path, "client")
    print("\n".join(line for line in server.splitlines()
                    if re.search(r"network test: the person|bots: player \d+ (drives|guns|gets out)|joined", line)))
    gunned = re.search(r"network test: the person \(player (\d+)\) guns vehicle (\w+), a bot driving, at tick (\d+)", server)
    assert gunned, "the person was never seated at a bot's gun:\n" + server[-4000:]
    assert re.search(r"bots: player \d+ drives with a person aboard", server), "the bot did not notice its gunner"
    assert not re.search(rf"bots: player \d+ gets out of vehicle {gunned.group(2)} \(there\)",
                         server[server.index(gunned.group(0)):server.index("the person leaves the gun")]), \
        "the bot got out at its goal with a person aboard"
    # (the client carried about: its own player riding, the places it has)
    person = int(gunned.group(1))
    rides = _riding(client, person)
    went = sum(((b[1] - a[1]) ** 2 + (b[2] - a[2]) ** 2) ** 0.5 for a, b in zip(rides, rides[1:]))
    print(f"the client saw itself riding {len(rides)} seconds, {went:.0f} world units")
    assert len(rides) >= 15 and went >= 15, f"the client was not carried about: {len(rides)} seconds, {went:.0f} world units"
    driven = re.search(r"network test: the person drives vehicle (\w+), a team mate bot", server)
    assert driven, "the person was never seated as a driver near the bot:\n" + server[-4000:]
    assert re.search(rf"bots: player \d+ guns vehicle {driven.group(1)} \(warthog\), a person driving", server), \
        "the bot did not take the gun of the person's Warthog"
    left = stood.search(server)
    later = [int(t) for t in re.findall(rf"bots: player \d+ drives vehicle {left.group(1)} \(\w+\) at tick (\d+)", server)]
    assert not [t for t in later if int(left.group(2)) < t < int(left.group(2)) + 10 * 30], \
        f"a bot drove the Warthog a person stood by: {later}"


def test_bots_tank(tmp_path):
    """Scorpions (bots_vehicle.c: the tanks' own drive graph, bots_nav_game.c):
    on Blood Gulch's dedicated server with every vehicle the map places, a
    hard bot and a peaceful one that drive only Scorpions ("tank"), and two
    normal bots on foot, playing King of the Hill (all four go to the hill,
    so that the tank meets the others: in Slayer whether it ever met anyone
    in range was the game's random numbers'; a Scorpion holds the hill with
    its driver aboard). A Scorpion's cannon is its driver's: the hard bot
    fights from it (aiming as it steers, firing nothing at what is near
    enough for the shell's blast to reach the tank, and hunting people on
    foot, as hard bots do) and never kills itself; the peaceful one drives
    one too and never fires. About two and a half minutes."""
    log = _bots_game(tmp_path, "server", {
        "HALO_SERVER_BOTS": "4", "HALO_BOT_SEED": "63", "HALO_BOT_DIFFICULTIES": "hard,peaceful,normal,normal",
        "HALO_BOT_VEHICLE": "tank,tank,never,never", "HALO_NETWORK_TEST_VEHICLES": "all",
        "HALO_SERVER_MAPS": "bloodgulch:king", "HALO_SERVER_TIME_LIMIT": "2",
        "HALO_NET_ADDRESS": _test_address(193), "HALO_NET_BROADCAST": _test_address(194)})
    drives = _drives(log)
    counts = _reports(log)
    for name, c in sorted(drives.items()):
        print(f"{name}: {c}; fired {counts[name].get('ticks')}, suicides {counts[name].get('suicides')}")
    tanks = {m for m in re.findall(r"bots: player (\d+) drives vehicle \w+ \(scorpion\)", log)}
    print(f"Scorpion drivers: {tanks}")
    assert len(drives) == 4, log[-4000:]
    assert tanks >= {"0", "1"}, f"the hard and the peaceful bot did not both drive a Scorpion: {tanks}"
    hard, peaceful = drives["[BOT] Alpha"], drives["[BOT] Bravo"]
    assert hard["ticks fired riding"] >= 30, f"the hard bot hardly fired from its Scorpion: {hard}"
    assert counts["[BOT] Alpha"].get("suicides", 0) == 0, f"the hard bot killed itself: {counts['[BOT] Alpha']}"
    fired = {m[0]: int(m[1]) for m in re.findall(r"bots: (\[BOT\] \w+) at tick \d+: .*? fired (\d+) ticks", log)}
    assert fired["[BOT] Bravo"] == 0 and peaceful["ticks fired riding"] == 0, f"the peaceful bot fired: {fired}, {peaceful}"
    assert peaceful["people driven at"] == 0, f"the peaceful bot drove at someone: {peaceful}"


def test_bots_vehicle_objectives(tmp_path):
    """Game types with vehicles (bots_vehicle.c: a bot's goal while it drives
    is its game type's objective, bots_objectives.c): four normal bots on
    Blood Gulch's dedicated server, two minutes of Capture the Flag, then two
    of King of the Hill (its Warthogs placed). In each game bots drive toward
    their objectives, and in King of the Hill they spend time on the hill
    (getting out there: no ride to a Warthog standing in the hill). About
    five minutes."""
    log = _bots_game(tmp_path, "server", {
        "HALO_SERVER_BOTS": "4", "HALO_BOT_SEED": "71", "HALO_SERVER_BOT_DIFFICULTY": "normal",
        "HALO_SERVER_MAPS": "bloodgulch:ctf,bloodgulch:king", "HALO_SERVER_TIME_LIMIT": "2",
        "HALO_NET_ADDRESS": _test_address(191), "HALO_NET_BROADCAST": _test_address(192)}, games_over=2, seconds=420)
    # (Capture the Flag is a team game: "ctf (teams) on bloodgulch")
    parts = re.split(r"server: the game starts: (\w+)(?: \(teams\))? on (\w+)", log)
    games = [(parts[index], parts[index + 1], parts[index + 2]) for index in range(1, len(parts) - 2, 3)]
    print([(kind, name) for kind, name, _ in games])
    assert [kind for kind, _, _ in games][:2] == ["ctf", "king"], [(kind, name) for kind, name, _ in games]
    for kind, _, text in games[:2]:
        drove = re.findall(r"bots: player (\d+) drives vehicle \w+ \(\w+\)", text)
        objectives = _objectives(text)
        print(f"{kind}: drives by players {drove}; objectives {objectives}")
        assert drove, f"no bot drove in {kind}"
        if kind == "king":
            assert sum(o[3] for o in objectives.values()) >= 10, f"the bots hardly held the hill: {objectives}"


def test_bots_flip(tmp_path):
    """One bot that drives (debug.bot_vehicle "drive") on Blood Gulch's
    dedicated server; 30 seconds in, the host turns the vehicle it drives (or
    the nearest) over (debug.network_test_vehicle_flip), which puts it out:
    it walks beside the vehicle, flips it back over with the action button as
    a person does, and drives on. About a minute and a half."""
    log = _bots_game(tmp_path, "server", {
        "HALO_SERVER_BOTS": "1", "HALO_BOT_SEED": "23", "HALO_BOT_VEHICLE": "drive",
        "HALO_SERVER_MAPS": "bloodgulch", "HALO_SERVER_TIME_LIMIT": "1", "HALO_NETWORK_TEST_VEHICLE_FLIP": "30",
        "HALO_NET_ADDRESS": _test_address(205), "HALO_NET_BROADCAST": _test_address(206)})
    print("\n".join(line for line in log.splitlines() if re.search(r"bots: player \d+ (drives|gets out|got out|is out|flipped)|"
                                                                    r"network test: .*turn", line)))
    turned = re.search(r"network test: the host turns vehicle (\w+) over at tick (\d+)", log)
    assert turned, "the host turned no vehicle over:\n" + log[-4000:]
    flipped = re.findall(r"bots: player \d+ flipped vehicle (\w+) back over at tick (\d+)", log)
    assert flipped and flipped[0][0] == turned.group(1), f"the bot did not flip the vehicle back: {flipped}"
    drives = _drives(log)
    print(drives)
    (counts,) = drives.values()
    assert counts["vehicles flipped back"] >= 1 and counts["rides in vehicles"] >= 2, counts
    assert any(int(tick) > int(flipped[0][1]) for tick in re.findall(r"bots: player \d+ drives vehicle \w+ \(\w+\) at tick (\d+)", log)), \
        "the bot did not drive again after flipping the vehicle back"


def test_bots_grenades(tmp_path):
    """Six legendary bots play a two-minute Team Slayer on Beaver Creek, three
    against three (everyone spawns with frag grenades there): they throw
    grenades (bots_grenades.c) at enemies in view and where they last saw
    them, and hit enemies with them, and hardly ever kill themselves or a
    team mate (at most one each); each hears the fight
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
    # (a bot killed by its own grenade, or a team mate who ran into one after
    # it left the hand: each about one game in ten of six legendary bots;
    # more than one is the bots' fault)
    assert total("grenade suicides") <= 1, "bots killed themselves with their grenades:\n" + \
        "\n".join(line for line in log.splitlines() if "killed itself" in line)
    assert total("team kills") <= 1, "bots killed team mates:\n" + \
        "\n".join(line for line in log.splitlines() if "killed a team mate" in line)
    assert all(counts.get("noises heard", 0) >= 1 for counts in reports.values()), "a bot heard nothing of the fight"
    assert total("grenades dodged") >= 1, "no bot ran from a grenade"


def test_bots_rotation(tmp_path):
    """Four bots (normal) through a dedicated server's rotation of three
    short Slayer games (Blood Gulch, Hang 'Em High, Blood Gulch, two minutes each),
    as the live server goes through its games: every game has kills, three
    bots of four at least fire in every game, the later games fire at least a
    quarter of what the first did, and few of its ways fail. Each game's bots begin afresh (bots.c):
    kept from the last game, their times, far ahead of the new game's, held
    them back for a whole game - the live server's bots hardly fought after
    its first game. About seven minutes."""
    maps, _ = _needs("HALO_TEST_SERVER")
    if not all(Path(maps, f"{name}.map").is_file() for name in ("bloodgulch", "hangemhigh")):
        pytest.skip("needs bloodgulch and hangemhigh")
    log = _bots_game(tmp_path, "server", {
        "HALO_SERVER_BOTS": "4", "HALO_BOT_SEED": "5", "HALO_SERVER_BOT_DIFFICULTY": "normal",
        "HALO_SERVER_MAPS": "bloodgulch,hangemhigh,bloodgulch", "HALO_SERVER_TIME_LIMIT": "2",
        "HALO_NET_ADDRESS": _test_address(245), "HALO_NET_BROADCAST": _test_address(246)}, games_over=3, seconds=660)
    games = log.split("server: the game starts: ")[1:4]
    assert len(games) == 3, log[-4000:]
    fired_in = []
    for number, text in enumerate(games, 1):
        where = text.splitlines()[0]
        kills = sum(int(k) for _, _, k, _ in SCORE.findall(text))
        reports = _reports(text)
        fired = {name: counts.get("ticks", 0) for name, counts in reports.items()}
        paths = {name: (counts.get("paths", 0), counts.get("failed", 0)) for name, counts in reports.items()}
        last = "\n".join([f"{name}: {counts}" for name, counts in reports.items()] + [str(SCORE.findall(text))])
        print(f"game {number}, {where}: {kills} kills; ticks fired {fired}; paths (failed) {paths}")
        fired_in.append(sum(fired.values()))
        assert kills >= 2, f"game {number} ({where}) had {kills} kills\n{last}"
        # (Blood Gulch is big: once on CI a bot fired at nobody in the first two
        # minutes, the others 900 ticks or more - three of four. The bug this test
        # is for silenced every bot from the second game on: the later games
        # against the first.)
        assert len(fired) == 4 and sum(ticks > 0 for ticks in fired.values()) >= 3, \
            f"two bots never fired in game {number} ({where}): {fired}\n{last}"
        assert number == 1 or fired_in[-1] >= fired_in[0] // 4, \
            f"the bots hardly fired in game {number} ({where}): {fired_in[-1]} ticks against {fired_in[0]} in game 1\n{last}"
        # (the live server's bots, a game on, had two thirds of their ways fail:
        # they went on fighting a target of the last game, where it was then)
        assert all(failed <= max(10, planned // 4) for planned, failed in paths.values()), \
            f"a bot's ways failed in game {number} ({where}): {paths}"


OBJECTIVE = re.compile(r"bots: (\[BOT\] \w+) objective at tick (\d+): (\d+) flag grabs, (\d+) flag returns, (\d+) flag "
                       r"captures, (\d+) hill seconds, (\d+) ball seconds, (\d+) race flags, (\d+) laps, (\d+) world units "
                       r"carried")


def _games(log):
    """the log of each game a server played, in turn: (its game type, map, its
    text)"""
    parts = re.split(r"server: the game starts: (\S+) on (\w+)", log)
    return [(parts[index], parts[index + 1], parts[index + 2]) for index in range(1, len(parts) - 2, 3)]


def _objectives(text):
    """each bot's last objective report in a game's text: {name: (grabs,
    returns, captures, hill, ball, race flags, laps, carried)}"""
    return {m[0]: tuple(int(value) for value in m[2:]) for m in OBJECTIVE.findall(text)}


def _moved(text):
    """each bot's last distance walked in a game's text: {name: world units}"""
    return {m[0]: int(m[2]) for m in re.findall(r"bots: (\[BOT\] \w+) at tick (\d+): .*?went (\d+) world units", text)}


def test_bots_difficulties(tmp_path):
    """Two bots, easy against legendary (debug.bot_difficulties), one
    against the other in two three-minute Slayer games (Hang 'Em High, then
    Chill Out; their games seeded): over both, legendary kills more and dies
    less than easy (the bots_combat.c table; README "Bots"). In eight such
    games locally legendary won every one, 59 kills to 12 (the closest 5 to
    3); the seeds fix the games' random numbers, not the machine's timing, so
    on a busy runner a game can still go 2 to 2, and a few kills a game make
    "twice as many" too tight a test (it failed twice in CI: 11 to 7, and 7
    to 4 with a draw). (Four bots, one of each difficulty, are too noisy for a test of
    two games: the order shows over 16 games, the README's table. Peaceful,
    which never fights, is test_bots_peaceful.) About seven minutes."""
    maps, _ = _needs("HALO_TEST_SERVER")
    if not all(Path(maps, f"{name}.map").is_file() for name in ("hangemhigh", "chillout")):
        pytest.skip("needs hangemhigh and chillout")
    log = _bots_game(tmp_path, "server", {
        "HALO_SERVER_BOTS": "2", "HALO_BOT_SEED": "40", "HALO_BOT_DIFFICULTIES": "easy,legendary",
        "HALO_SERVER_MAPS": "hangemhigh,chillout", "HALO_SERVER_TIME_LIMIT": "3",
        "HALO_NET_ADDRESS": _test_address(247), "HALO_NET_BROADCAST": _test_address(248)}, games_over=2, seconds=540)
    difficulty = {name: level for name, _, _, _, level in
                  re.findall(r"bots: (\[BOT\] \w+) joined \(machine (\d+), controller (\d+), team (-?\d+), (\w+)\)", log)}
    assert sorted(difficulty.values()) == ["easy", "legendary"], f"the bots' difficulties: {difficulty}"
    games = []
    for game in log.split("server: the game starts: ")[1:3]:
        kills = {difficulty[name]: (int(k), int(d)) for name, _, k, d in SCORE.findall(game)}
        print(f"{game.splitlines()[0]}: kills, deaths {kills}")
        games.append(kills)
    assert len(games) == 2 and all(len(kills) == 2 for kills in games), log[-4000:]
    total = {level: (sum(kills[level][0] for kills in games), sum(kills[level][1] for kills in games))
             for level in ("easy", "legendary")}
    assert total["legendary"][0] > total["easy"][0] and total["legendary"][1] < total["easy"][1], \
        f"legendary did not kill more and die less than easy over both games: {games}"


def test_bots_ctf(tmp_path):
    """Two bots (normal) play Capture the Flag on Beaver Creek, one against
    the other (bots_objectives.c), to one capture (server.score_limit 1; at
    most six minutes): each, its team's only bot, attacks - takes the
    enemy's flag, carries it home without the buttons or teleporters that
    would lose it, and scores. The flag is grabbed, carried well away from
    its stand, and captured. The first capture came after 49 to 185 s in
    six games of this. (Three against three, defenders and hunters stop
    most carries in the middle of the map, and a capture took 107 s to more
    than ten minutes: no test of that.)"""
    maps, _ = _needs("HALO_TEST_SERVER")
    if not Path(maps, "beavercreek.map").is_file():
        pytest.skip("needs beavercreek.map")
    log = _bots_game(tmp_path, "server", {
        "HALO_SERVER_BOTS": "2", "HALO_BOT_SEED": "9", "HALO_SERVER_BOT_DIFFICULTY": "normal",
        "HALO_SERVER_MAPS": "beavercreek", "HALO_SERVER_GAME_TYPES": "ctf", "HALO_SERVER_TIME_LIMIT": "6",
        "HALO_SERVER_SCORE_LIMIT": "1",
        "HALO_NET_ADDRESS": _test_address(241), "HALO_NET_BROADCAST": _test_address(242)}, seconds=480)
    objectives = _objectives(log)
    teams = TEAMS_SCORE.findall(log)
    print(f"objectives (grabs, returns, captures, ...): {objectives}; teams {teams}")
    assert len(objectives) == 2 and teams, log[-4000:]
    grabs = sum(o[0] for o in objectives.values())
    carried = max(o[7] for o in objectives.values())
    assert grabs >= 1, "no bot grabbed a flag"
    assert carried >= 10, f"no bot carried a flag 10 world units from its stand (at most {carried})"
    # (the capture ends the game, after the bots' last report: the team's score)
    assert sum(int(score) for score in teams[-1]) >= 1, f"no flag was captured: {objectives}, {teams}"


def test_bots_game_types(tmp_path):
    """Four bots (normal) play two minutes of each of King of the Hill on
    Chillout, Oddball on Beaver Creek, Race on Blood Gulch and Juggernaut on
    Hang 'Em High, a dedicated server's rotation (bots_objectives.c): they
    stand on the hill (three of them at least, 25 s between them, and none
    standing still), take and keep the ball (each some seconds), run to the race's
    flags in turn (some each), and in Juggernaut hunt the juggernaut and fight
    on (kills scored, no bot standing still). Each game's bots begin afresh
    (their reports are the game's). About nine minutes."""
    maps, _ = _needs("HALO_TEST_SERVER")
    names = ("chillout", "beavercreek", "bloodgulch", "hangemhigh")
    if not all(Path(maps, f"{name}.map").is_file() for name in names):
        pytest.skip(f"needs {', '.join(names)}")
    log = _bots_game(tmp_path, "server", {
        "HALO_SERVER_BOTS": "4", "HALO_BOT_SEED": "10", "HALO_SERVER_BOT_DIFFICULTY": "normal",
        "HALO_SERVER_MAPS": "chillout:king,beavercreek:oddball,bloodgulch:race,hangemhigh:juggernaut",
        "HALO_SERVER_TIME_LIMIT": "2",
        "HALO_NET_ADDRESS": _test_address(243), "HALO_NET_BROADCAST": _test_address(244)}, games_over=4, seconds=720)
    games = _games(log)
    print(f"games: {[(kind, where) for kind, where, _ in games]}")
    assert len(games) >= 4, log[-4000:]
    by_kind = {}
    for kind, where, text in games[:4]:
        by_kind[where] = (kind, text)
        print(f"{kind} on {where}: {_objectives(text)}; {SCORE.findall(text)}")
    king = _objectives(by_kind["chillout"][1])
    king_moved = _moved(by_kind["chillout"][1])
    king_reports = "\n".join(re.findall(r"bots: \[BOT\] \w+ at tick 3600: .*", by_kind["chillout"][1]))
    # (Chillout is small: a bot is in a fight most of the time, and one that
    # keeps losing them - killed on the way, back from cover - may get no
    # second on the hill in two minutes: in seven local games 1 to 28 s
    # each, 32 to 59 s between them, every bot walking 129 wu or more. Three
    # of four on it, and all of them about the map, not one standing still.)
    assert len(king) == 4 and sum(o[3] >= 1 for o in king.values()) >= 3, \
        f"two bots were never on the hill: {king}\n{king_reports}"
    assert all(distance >= 40 for distance in king_moved.values()), \
        f"a bot hardly moved in King of the Hill: {king_moved}\n{king_reports}"
    assert sum(o[3] for o in king.values()) >= 25, f"the bots held the hill only {sum(o[3] for o in king.values())} s"
    ball = _objectives(by_kind["beavercreek"][1])
    ball_kills = sum(int(kills) for _, _, kills, _ in SCORE.findall(by_kind["beavercreek"][1]))
    assert len(ball) == 4 and sum(o[4] for o in ball.values()) >= 30, f"the bots hardly had the ball: {ball}"
    assert ball_kills >= 1, f"nobody fought for the ball: {ball}"
    race = _objectives(by_kind["bloodgulch"][1])
    assert len(race) == 4 and sum(o[5] for o in race.values()) >= 3 and sum(o[5] > 0 for o in race.values()) >= 2, \
        f"the bots hardly ran the race: {race}"
    juggernaut = by_kind["hangemhigh"][1]
    scores = SCORE.findall(juggernaut)
    moved = _moved(juggernaut)
    assert len(scores) == 4 and sum(int(kills) for _, _, kills, _ in scores) >= 2, f"no fight in Juggernaut: {scores}"
    assert all(distance >= 40 for distance in moved.values()), f"a bot hardly moved in Juggernaut: {moved}"


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


def test_bots_fill_a_full_lobby(tmp_path):
    """A dedicated server with server.bots_fill and a full lobby (server.bots
    and server.max_players both 6, so it is full at a scale the test can join
    quickly) and a person who joins it: a bot leaves at once to make room
    (bots_make_room, which does that either way; server.bots_fill only keeps
    the bots topped up to server.bots as people come and go), the person
    plays, and when they leave a bot comes back.

    The chart's full-lobby maximum (server.bots 128 with server.bots_fill,
    OpenCE #127) is covered by the chart's schema tests and the engine's
    bots_maximum() clamp (bots.c), not by joining 128 bots over the network
    here: at that scale, on the shared runner the other bots groups also run
    on, joining alone took anywhere from under ten minutes to over fifteen
    across tries (bots join one a frame, and the lobby's settings resend to
    everyone already in it as each one joins) - too slow and too variable for
    a check that runs on every PR (test_skating_bots_crowd's "three minutes"
    for far fewer bots to ride, not just join, is the closest precedent for
    how this scales). About a minute and a half."""
    maps, paths = _needs("HALO_TEST_GAME", "HALO_TEST_SERVER")
    server, game = paths["HALO_TEST_SERVER"], paths["HALO_TEST_GAME"]
    games = {"server": _start(tmp_path, "server", server, maps, {
        "HALO_DEDICATED": "1", "HALO_NETWORK_TEST_START": "3", "HALO_SERVER_BOTS": "6",
        "HALO_SERVER_MAX_PLAYERS": "6", "HALO_SERVER_BOTS_FILL": "1", "HALO_BOT_SEED": "7",
        "HALO_NET_ADDRESS": _test_address(249), "HALO_NET_BROADCAST": _test_address(250)})}
    executables = {"server": server, "client": game}
    try:
        _wait(lambda: len(BOT_JOINED.findall(_log(tmp_path, "server"))) >= 6 and
              "server: the game starts" in _log(tmp_path, "server"), 90, games, tmp_path,
              "a full lobby of 6 bots did not join")
        games["client"] = _start(tmp_path, "client", game, maps, {
            "HALO_NETWORK_TEST": "join", "HALO_NULL_RENDERER": "1", "HALO_NO_AUDIO": "1", "HALO_TEST_INPUT": "bot:7",
            "HALO_NET_ADDRESS": _test_address(250), "HALO_NET_BROADCAST": _test_address(249)})
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

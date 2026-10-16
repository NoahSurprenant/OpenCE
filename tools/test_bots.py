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
        answer = _console(port, f"bot remove {first[6:]}", f"kick {second}", "bot list")
        assert f"kick: {second} removed" in answer, answer
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
        _wait(lambda: _bots_seen(_log(tmp_path, "join"), 2) and _bots_seen(_log(tmp_path, "host"), 2), 150, games,
              tmp_path, "the machines did not see two bots play")
        for name in ("host", "join"):
            _assert_bots_play(_log(tmp_path, name), _bots_seen(_log(tmp_path, name), 2), {127})
        # (the host's own player is still its local player, and no bot)
        host = _log(tmp_path, "host")
        local = re.findall(r"\| local (-?\d+) camera", host)
        assert local and local[-1] != "-1" and local[-1] not in _bots_seen(host, 2), host[-3000:]
    finally:
        _stop(games, tmp_path, {"host": game, "join": game})

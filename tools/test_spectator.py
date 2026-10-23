"""Spectators (port/linux/game/spectate.c; port/linux/README.md, "Spectators")
in real games on this machine: a dedicated server with bots, and a machine
that joins it as a spectator (network.spectate). It takes no player slot,
is on no team and nobody's scoreboard, and sees the bots play, following one
and then the next. They need a Linux build (HALO_TEST_GAME) and the Halo maps
(HALO_SKATE_TEST_HALO_MAPS), as tools/test_bots.py's tests, and skip without
them. HALO_TEST_SERVER is the dedicated server's build, else HALO_TEST_GAME's.

The games are on loopback addresses of their own (127.0.7.*), so that these
tests go beside tools/test_bots.py's."""

import os
import re
import socket
import time

from tools.test_bots import _bots_seen, _debug, _log, _needs, _samples, _start, _stop, _travelled, _wait

TICK = re.compile(r"network test: tick (\d+).*\| local (-?\d+) camera \((-?[\d.]+) (-?[\d.]+) (-?[\d.]+)\)"
                  r"(?: respawn -?\d+)?(?: \| spectating (\w+) watching (-?\d+))?")
WATCHING = re.compile(r"spectate: watching (.+?) \(player (\d+), ([\w ]+)\)")
JOINED = re.compile(r"server: (.+?) joined \((\d+) players\)")


def _address(last):
    """a loopback address of these tests, 127.0.7.<last> (or of
    HALO_TEST_SPECTATOR_NET's network)"""
    return f"{os.environ.get('HALO_TEST_SPECTATOR_NET', '127.0.7')}.{last}"


def _console(port, command):
    """a command for a game's telnet console (debug.telnet_console), on a
    connection of its own: what it answered (until it is quiet, or closes).
    The console takes one connection at a time, and lets one go a frame or
    so after it closes: until then it turns another away, which is tried
    again."""
    for _ in range(20):
        answer = b""
        with socket.create_connection(("127.0.0.1", port), timeout=10) as connection:
            connection.settimeout(1.5)
            deadline = time.monotonic() + 10
            sent = False
            while time.monotonic() < deadline:
                try:
                    data = connection.recv(4096)
                except socket.timeout:
                    if sent:
                        break
                    continue
                if data == b"":
                    break
                answer += data
                if b"maximum number of clients" in answer:
                    break
                if not sent and b"\n" in answer:
                    connection.sendall(command.encode() + b"\r\n")
                    sent = True
        print(f"the console answered {command!r}: {answer!r}")
        if sent:
            return answer.decode(errors="replace")
        time.sleep(1)
    raise AssertionError(f"the console never took {command!r}")


def _ticks(log):
    """the spectator's seconds as its log has them: (tick, local player,
    camera (x, y, z), camera mode, player watched)"""
    return [(int(m[1]), int(m[2]), (float(m[3]), float(m[4]), float(m[5])), m[6], int(m[7]) if m[7] else None)
            for m in TICK.finditer(log)]


def _dedicated_with_a_spectator(tmp_path, game, server, maps, bots, addresses, telnet_port, settings=None):
    """a dedicated server with bots, and a spectator that joins its game in
    progress (its telnet console on telnet_port). It joins once the bots have
    played a while: the server builds the bots' graphs of a map (seconds,
    with no cache) as its game starts, sending nothing meanwhile, and a
    machine joined before could time out waiting."""
    games = {"server": _start(tmp_path, "server", server, maps, {
        "HALO_DEDICATED": "1", "HALO_NETWORK_TEST_START": "5", "HALO_SERVER_BOTS": str(bots),
        "HALO_BOT_SEED": "4", "HALO_NET_ADDRESS": addresses[0], "HALO_NET_BROADCAST": addresses[1]})}
    _wait(lambda: re.search(r"bots: \[BOT\] \w+ at tick \d+", _log(tmp_path, "server")), 180, games, tmp_path,
          "the server's bots did not play")
    games["spectator"] = _start(tmp_path, "spectator", game, maps, dict({
        "HALO_NETWORK_TEST": "join", "HALO_SPECTATE": "1", "HALO_NULL_RENDERER": "1", "HALO_NO_AUDIO": "1",
        "HALO_TELNET_CONSOLE": "1", "HALO_TELNET_CONSOLE_PORT": str(telnet_port),
        "HALO_NET_ADDRESS": addresses[1], "HALO_NET_BROADCAST": addresses[0]}, **(settings or {})))
    return games


def test_spectator_on_a_dedicated_server(tmp_path):
    """A dedicated server with three bots, and a machine that joins it as a
    spectator: it gets the game's world with no player of its own (it sees
    every bot walk about, as a player's machine does), takes no player slot
    (the server still has three players, and no one joined but the bots), is
    on no team nor the scoreboard, and its camera follows a bot, then the next
    and the one before as its console asks ("spectate next", "spectate
    previous"), and flies free ("spectate free"). About a minute and a half."""
    maps, paths = _needs("HALO_TEST_GAME", "HALO_TEST_SERVER")
    port = 23930 + os.getpid() % 50
    games = _dedicated_with_a_spectator(tmp_path, paths["HALO_TEST_GAME"], paths["HALO_TEST_SERVER"], maps, 3,
                                        (_address(11), _address(12)), port)
    executables = {"server": paths["HALO_TEST_SERVER"], "spectator": paths["HALO_TEST_GAME"]}
    try:
        _wait(lambda: _bots_seen(_log(tmp_path, "spectator"), 3), 150, games, tmp_path,
              "the spectator did not see three bots play")
        server, spectator = _log(tmp_path, "server"), _log(tmp_path, "spectator")

        # the world: every bot seen walking about, from no player's machine
        named = _bots_seen(spectator, 3)
        samples = _samples(spectator)
        for index, name in named.items():
            travelled = _travelled(samples[int(index)])
            print(f"the spectator saw {name} (player {index}) go {travelled:.1f} world units")
            assert travelled >= 3.0, f"{name} hardly moved as the spectator saw it: {samples[int(index)][:5]}"
        ticks = _ticks(spectator)
        assert ticks and all(local == -1 for _, local, *_ in ticks), spectator[-3000:]
        assert all(mode for *_, mode, _ in ticks[-5:]), "the spectator's log says it is not spectating"

        # no slot, no team, no score: only the bots joined, and the server
        # counts three players
        joined = JOINED.findall(server)
        print(f"the server's joins: {joined}")
        assert joined and all(name.startswith("[BOT]") for name, _ in joined), server[-4000:]
        assert int(joined[-1][1]) == 3, server[-4000:]
        assert "requesting a player addition" not in spectator, spectator[-3000:]

        # it follows a bot, then the next and the one before
        followed = [int(index) for _, index, _ in WATCHING.findall(spectator)]
        assert followed and str(followed[0]) in named, (followed, named)
        _console(port, "spectate next")
        _wait(lambda: len(WATCHING.findall(_log(tmp_path, "spectator"))) >= 2, 20, games, tmp_path,
              "the spectator did not watch the next bot")
        _console(port, "spectate previous")
        _wait(lambda: len(WATCHING.findall(_log(tmp_path, "spectator"))) >= 3, 20, games, tmp_path,
              "the spectator did not watch the bot before")
        followed = [int(index) for _, index, _ in WATCHING.findall(_log(tmp_path, "spectator"))]
        print(f"the spectator watched players {followed}")
        assert followed[1] != followed[0] and followed[2] == followed[0], followed
        assert all(str(index) in named for index in followed), (followed, named)

        # its camera is behind the bot watched (within a few world units)
        def near_watched():
            log = _log(tmp_path, "spectator")
            ticks, samples = _ticks(log), _samples(log)
            if not ticks or ticks[-1][4] is None or ticks[-1][4] not in samples:
                return False
            camera, bot = ticks[-1][2], samples[ticks[-1][4]][-1]
            return ((camera[0] - bot[0]) ** 2 + (camera[1] - bot[1]) ** 2 + (camera[2] - bot[2]) ** 2) ** 0.5 < 8.0
        _wait(near_watched, 20, games, tmp_path, "the spectator's camera was not near the bot it follows")

        # and flies free
        _console(port, "spectate free")
        _wait(lambda: "spectate: free camera" in _log(tmp_path, "spectator") and
              _ticks(_log(tmp_path, "spectator"))[-1][3] == "free", 20, games, tmp_path,
              "the spectator's camera did not fly free")
    finally:
        _stop(games, tmp_path, executables)


def test_spectator_in_a_lobby(tmp_path):
    """A spectator that joins a dedicated server's lobby, with two bots in it,
    half a minute before its game starts: the server keeps it there with no
    player (a machine with no player used to be refused after fifteen
    seconds), and it starts with the game and sees the bots play. About a
    minute and a half."""
    maps, paths = _needs("HALO_TEST_GAME", "HALO_TEST_SERVER")
    # (the bots stay on foot: no vehicle graphs built as the game starts,
    # when the server sends nothing for a while)
    games = {"server": _start(tmp_path, "server", paths["HALO_TEST_SERVER"], maps, {
        "HALO_DEDICATED": "1", "HALO_NETWORK_TEST_START": "30", "HALO_SERVER_BOTS": "2", "HALO_BOT_SEED": "5",
        "HALO_SERVER_BOT_VEHICLES": "-1", "HALO_NET_ADDRESS": _address(31), "HALO_NET_BROADCAST": _address(32)})}
    games["spectator"] = _start(tmp_path, "spectator", paths["HALO_TEST_GAME"], maps, {
        "HALO_NETWORK_TEST": "join", "HALO_SPECTATE": "1", "HALO_NULL_RENDERER": "1", "HALO_NO_AUDIO": "1",
        "HALO_NET_ADDRESS": _address(32), "HALO_NET_BROADCAST": _address(31)})
    executables = {"server": paths["HALO_TEST_SERVER"], "spectator": paths["HALO_TEST_GAME"]}
    try:
        _wait(lambda: "server: the game starts" in _log(tmp_path, "server"), 120, games, tmp_path,
              "the server's game did not start")
        server = _log(tmp_path, "server")
        joined = re.findall(r"server added machine @ \S+ to the game at machine index #(\d+)", server)
        print(f"machines the server added: {joined}")
        assert len(joined) >= 2, server[-3000:]
        assert "refusing machine" not in server, server[-3000:]
        assert _log(tmp_path, "spectator").count("network test: joining") == 1, "the spectator joined more than once"
        _wait(lambda: _bots_seen(_log(tmp_path, "spectator"), 2), 120, games, tmp_path,
              "the spectator did not see two bots play")
        assert "refusing machine" not in _log(tmp_path, "server")
        assert _log(tmp_path, "spectator").count("network test: joining") == 1, "the spectator joined more than once"
    finally:
        _stop(games, tmp_path, executables)


CUT = re.compile(r"spectate: director cut to (.+?) \(player (-?\d+), (\w+)\) at tick (\d+) after ([\d.]+) s, (glide|hard cut)")


def test_spectator_director(tmp_path):
    """The spectator's auto-director (network.spectate_camera "director",
    spectate_director.c) on a dedicated server with four bots fighting
    (fixed seeds): it cuts between them, each cut with its reason, no shot
    shorter than five seconds but after the one watched died (then the next
    is their killer, or whoever is most worth watching), none longer than
    fifteen (but by the two seconds held on one who died), and it shows more
    than one bot. About two minutes."""
    maps, paths = _needs("HALO_TEST_GAME", "HALO_TEST_SERVER")
    port = 23980 + os.getpid() % 15
    games = _dedicated_with_a_spectator(tmp_path, paths["HALO_TEST_GAME"], paths["HALO_TEST_SERVER"], maps, 4,
                                        (_address(21), _address(22)), port, {"HALO_SPECTATE_CAMERA": "director"})
    executables = {"server": paths["HALO_TEST_SERVER"], "spectator": paths["HALO_TEST_GAME"]}
    try:
        _wait(lambda: len(CUT.findall(_log(tmp_path, "spectator"))) >= 7, 240, games, tmp_path,
              "the director did not cut six times")
        cuts = CUT.findall(_log(tmp_path, "spectator"))
        for name, index, reason, tick, after, how in cuts:
            print(f"cut to {name} (player {index}) at tick {tick} after {after} s: {reason}, {how}")
        assert cuts[0][2] == "start", cuts[0]
        reasons = {"start", "kill", "killer", "fight", "hurt", "objective", "vehicle", "near", "turn", "time", "left",
                   "died"}
        assert all(reason in reasons for _, _, reason, *_ in cuts), cuts
        # (a shot's length in game ticks, from the ticks of its cut and the
        # next: one ends early only when the one watched died - two seconds
        # held on them, then their killer or another - or left)
        for this, following in zip(cuts, cuts[1:]):
            seconds = (int(following[3]) - int(this[3])) / 30.0
            if following[2] in ("killer", "died", "left"):
                assert following[2] == "left" or seconds >= 1.9, f"a death held {seconds:.1f} s: {this}, {following}"
                # (one who dies near the shot's end is held on two seconds more)
                assert seconds <= 15.5 + 2.5, f"a shot of {seconds:.1f} s, over seventeen: {this} then {following}"
            else:
                assert seconds >= 4.9, f"a shot of {seconds:.1f} s, under five: {this} then {following}"
                assert seconds <= 15.5, f"a shot of {seconds:.1f} s, over fifteen: {this} then {following}"
        assert len({index for _, index, *_ in cuts}) >= 2, f"the director showed one bot only: {cuts}"
        assert any(reason in ("kill", "killer", "fight", "hurt") for _, _, reason, *_ in cuts), \
            f"the director never cut to a fight: {cuts}"
    finally:
        _stop(games, tmp_path, executables)


def test_spectator_limit(tmp_path):
    """server.max_spectators 1 on a dedicated server with two bots, and two
    spectators joining its game in progress: the server counts one watching,
    turns the other away as a full game would, and the first watches on. About a minute and a half."""
    maps, paths = _needs("HALO_TEST_GAME", "HALO_TEST_SERVER")
    games = {"server": _start(tmp_path, "server", paths["HALO_TEST_SERVER"], maps, {
        "HALO_DEDICATED": "1", "HALO_NETWORK_TEST_START": "5", "HALO_SERVER_BOTS": "2", "HALO_BOT_SEED": "7",
        "HALO_SERVER_MAX_SPECTATORS": "1", "HALO_NET_ADDRESS": _address(41),
        "HALO_NET_BROADCAST": f"{_address(42)},{_address(43)}"})}
    executables = {"server": paths["HALO_TEST_SERVER"], "first": paths["HALO_TEST_GAME"],
                   "second": paths["HALO_TEST_GAME"]}
    try:
        _wait(lambda: re.search(r"bots: \[BOT\] \w+ at tick \d+", _log(tmp_path, "server")), 180, games, tmp_path,
              "the server's bots did not play")
        for name, last in (("first", 42), ("second", 43)):
            games[name] = _start(tmp_path, name, paths["HALO_TEST_GAME"], maps, {
                "HALO_NETWORK_TEST": "join", "HALO_SPECTATE": "1", "HALO_NULL_RENDERER": "1", "HALO_NO_AUDIO": "1",
                "HALO_NET_ADDRESS": _address(last), "HALO_NET_BROADCAST": _address(41)})
            # (one after the other: the first is the one that stays)
            _wait(lambda: "spectate: watching" in _log(tmp_path, name), 90, games, tmp_path,
                  f"the {name} spectator did not watch")
        _wait(lambda: "server: a spectator is turned away" in _log(tmp_path, "server"), 60, games, tmp_path,
              "the server did not turn the second spectator away")
        server = _log(tmp_path, "server")
        print(re.findall(r"server: (\d+ watching|a spectator is turned away.*)", server))
        assert "server: 1 watching" in server, server[-3000:]
        assert "server: 2 watching" not in server, server[-3000:]
        # the second is let go (a machine in the game hears it as the host
        # going); the first watches on, a tick logged every second
        _wait(lambda: re.search(r"the game host went down|game_is_full", _debug(tmp_path, "second")), 30, games,
              tmp_path, "the second spectator was not let go")
        ticks = len(_ticks(_log(tmp_path, "first")))
        time.sleep(5)
        assert len(_ticks(_log(tmp_path, "first"))) > ticks, "the first spectator stopped watching"
    finally:
        _stop(games, tmp_path, executables)


def test_spectator_with_an_older_client(tmp_path):
    """A build from before spectators (HALO_TEST_OLD_CLIENT, as
    tools/test_bots.py's test_bots_with_an_older_client has it) plays on a
    dedicated server with two bots while a spectator watches: nothing on the
    wire is new, so the older client gets its player and sees the bots as
    before, and the spectator sees the older client's player too. About a
    minute and a half."""
    maps, paths = _needs("HALO_TEST_OLD_CLIENT", "HALO_TEST_GAME", "HALO_TEST_SERVER")
    games = {"server": _start(tmp_path, "server", paths["HALO_TEST_SERVER"], maps, {
        "HALO_DEDICATED": "1", "HALO_NETWORK_TEST_START": "5", "HALO_SERVER_BOTS": "2", "HALO_BOT_SEED": "8",
        "HALO_NET_ADDRESS": _address(51), "HALO_NET_BROADCAST": f"{_address(52)},{_address(53)}"})}
    executables = {"server": paths["HALO_TEST_SERVER"], "spectator": paths["HALO_TEST_GAME"],
                   "older": paths["HALO_TEST_OLD_CLIENT"]}
    try:
        _wait(lambda: re.search(r"bots: \[BOT\] \w+ at tick \d+", _log(tmp_path, "server")), 180, games, tmp_path,
              "the server's bots did not play")
        games["spectator"] = _start(tmp_path, "spectator", paths["HALO_TEST_GAME"], maps, {
            "HALO_NETWORK_TEST": "join", "HALO_SPECTATE": "1", "HALO_NULL_RENDERER": "1", "HALO_NO_AUDIO": "1",
            "HALO_NET_ADDRESS": _address(52), "HALO_NET_BROADCAST": _address(51)})
        _wait(lambda: "spectate: watching" in _log(tmp_path, "spectator"), 90, games, tmp_path,
              "the spectator did not watch")
        games["older"] = _start(tmp_path, "older", paths["HALO_TEST_OLD_CLIENT"], maps, {
            "HALO_NETWORK_TEST": "join", "HALO_NULL_RENDERER": "1", "HALO_NO_AUDIO": "1", "HALO_TEST_INPUT": "bot:9",
            "HALO_NET_ADDRESS": _address(53), "HALO_NET_BROADCAST": _address(51)})

        def older_plays():
            ticks = re.findall(r"\| local (-?\d+) camera", _log(tmp_path, "older"))
            return ticks and ticks[-1] != "-1" and _bots_seen(_log(tmp_path, "older"), 2)
        _wait(older_plays, 150, games, tmp_path, "the older client did not play beside the spectator")
        older_player = re.findall(r"\| local (-?\d+) camera", _log(tmp_path, "older"))[-1]
        print(f"the older client plays as player {older_player}")
        _wait(lambda: len(_samples(_log(tmp_path, "spectator")).get(int(older_player), [])) >= 5, 60, games, tmp_path,
              "the spectator did not see the older client's player")
        server = _log(tmp_path, "server")
        assert re.search(r"server: (?!\[BOT\])\S.* joined \(3 players\)", server), server[-3000:]
    finally:
        _stop(games, tmp_path, executables)

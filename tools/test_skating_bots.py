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
                        r"(\d+) people ridden at, (\d+) rails lined up with, (\d+) grinds")
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
    units, ollies, bails, people ridden at, rails lined up with, grinds)}"""
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
        "HALO_SERVER_BOT_VEHICLES": "-1",
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
        "HALO_NETWORK_TEST_SKATE": "20", "HALO_NULL_RENDERER": "1", "HALO_NO_AUDIO": "1", "HALO_SERVER_BOT_VEHICLES": "-1",
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
    # the normal bots fought; the peaceful one never rode at anyone on purpose
    # (a run-over by accident is allowed)
    players = {name: player for player, name in re.findall(r"network test: player (\d+) is named (\[BOT\] \w+)", host)}
    peaceful = [players.get(name) for name in re.findall(r"bots: (\[BOT\] \w+) joined \([^)]*peaceful\)", host)]
    fired = {name: int(n) for name, n in re.findall(r"bots: (\[BOT\] \w+) at tick \d+: .*?fired (\d+) ticks", host)}
    decided = RUN_OVER_DECIDED.findall(host)
    print(f"run-overs decided {decided}; fired {fired}; the peaceful bot is player {peaceful}")
    assert peaceful and peaceful[0] is not None, "the peaceful bot was not named"
    assert sum(fired.values()) > 0, f"no bot fired: {fired}"
    rams = re.findall(r"bots: player (\d+) rides at a person on foot", host)
    assert not set(rams) & set(peaceful), f"the peaceful bot rode at someone: {rams}"

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
    dedicated server with Skate 3 data: they get on boards, ride and grind the
    rails beside their way (every one they pass, within a hard bot's reach:
    debug.bot_tricks "*hard"; a peaceful bot's own skill takes only some of the
    rails, as an easy one, which left a grind in three minutes to chance), but
    never ride at anyone on purpose -
    no ram, though they see each other. What happens by accident is allowed
    (the owner's call): a run-over decided as any skater's, a fall off a
    ledge, a bail. About three and a half minutes."""
    _, maps, assets = _needs_skate()
    log = _bots_alone(tmp_path, "server", _server(), maps, assets, {
        "HALO_SERVER_BOTS": "2", "HALO_SERVER_BOT_DIFFICULTY": "peaceful", "HALO_BOT_SKATE": "ram,ram",
        "HALO_BOT_TRICKS": "*hard", "HALO_BOT_SEED": "1", "HALO_SERVER_MAPS": "prisoner", "HALO_SERVER_TIME_LIMIT": "3"},
        (_test_address(253), _test_address(254)))
    boards = _boards(log)
    seen = {name: int(n) for name, n in re.findall(r"bots: (\[BOT\] \w+) at tick \d+: .*?(\d+) targets seen", log)}
    deaths = re.findall(r"bots: player (\d+) died of (\S+), by player (-?\d+)", log)
    print(f"boards {boards}; targets seen {seen}; run-overs {RUN_OVER_DECIDED.findall(log)}; deaths {deaths}; "
          f"scores {SCORE.findall(log)}")
    assert len(boards) == 2, log[-3000:]
    assert sum(b[0] for b in boards.values()) >= 2 and sum(b[2] for b in boards.values()) >= 30, \
        f"the peaceful bots hardly rode: {boards}"
    assert sum(b[7] for b in boards.values()) >= 1, f"no bot grinded a rail: {boards}"
    # never on purpose: no ram, nor riding at anyone (bots_skate.c logs each)
    assert all(b[5] == 0 for b in boards.values()), f"a peaceful bot rode at someone: {boards}"
    assert "rides at a person on foot" not in log, "a peaceful bot rode at a person"


def test_skating_bots_take_a_vehicle(tmp_path):
    """Boards and vehicles together (bots_vehicle.c weighs walking, a board and
    a vehicle: port/linux/README.md, "Bots in vehicles"): the dedicated server
    with Skate 3 data, four normal bots on Blood Gulch, three minutes of Slayer
    with every vehicle the map places (debug.network_test_vehicles "all"). Outside a fight, a bot takes a Warthog only where it is
    quicker than its board would be (each choice logged with both), steps off its board for one
    ("a vehicle") when on it, and the bots both ride boards and drive. About
    three and a half minutes."""
    _, maps, assets = _needs_skate()
    log = _bots_alone(tmp_path, "server", _server(), maps, assets, {
        "HALO_SERVER_BOTS": "4", "HALO_BOT_SEED": "73", "HALO_SERVER_MAPS": "bloodgulch", "HALO_SERVER_TIME_LIMIT": "3",
        "HALO_SERVER_BOT_VEHICLES": "0", "HALO_NETWORK_TEST_VEHICLES": "all"}, (_test_address(189), _test_address(190)))
    boards = _boards(log)
    choices = re.findall(r"bots: player \d+ takes vehicle \w+: (\d+) world units of walking by it, (\d+) (on a board|on foot)",
                         log)
    fights = re.findall(r"bots: player \d+ takes vehicle \w+ in a fight", log)
    drove = re.findall(r"bots: player (\d+) drives vehicle", log)
    stepped = STEPPED_OFF.findall(log)
    print(f"boards {boards}\nvehicle choices {choices} (and {len(fights)} in fights)\ndrives {drove}\nsteps off {stepped}")
    assert sum(b[0] for b in boards.values()) >= 1, f"no bot rode a board: {boards}"
    assert drove, "no bot drove"
    assert choices or fights, "no vehicle chosen"
    assert all(kind == "on a board" for _, _, kind in choices), f"a vehicle chosen without weighing a board: {choices}"
    assert all(int(by_vehicle) <= 0.75 * int(by_board) for by_vehicle, by_board, _ in choices), \
        f"a vehicle chosen though a board was quicker: {choices}"


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


def test_skating_bots_crowd(tmp_path):
    """A crowd on boards: a dedicated server with Skate 3 data and many bots
    that all ride (debug.bot_skate "*ram": on a board whenever the way rolls
    a little, fighting from it), and a client. Nothing caps them: every bot
    gets a skate session of its own (copies of the first: a few milliseconds
    each), most ride, the client sees them posed with their boards, and
    nothing fails; a machine with too few processors for them lets them skate
    slower, and says so, rather than refusing anyone. HALO_TEST_SKATE_CROWD
    sets how many bots (16 at first, as the CI runner has room for; 64 or 127
    to measure a machine: port/skate/README.md, "Several skaters"). About
    three minutes."""
    _, maps, assets = _needs_skate()
    bots = int(os.environ.get("HALO_TEST_SKATE_CROWD", "16"))
    server = _server()
    game = os.environ.get("HALO_TEST_GAME")
    games = {"server": _start(tmp_path, "server", server, maps, {
        "HALO_DEDICATED": "1", "HALO_NETWORK_TEST_START": "3", "HALO_SKATE_ASSETS": assets,
        "HALO_SERVER_BOTS": str(bots), "HALO_SERVER_MAX_PLAYERS": "128", "HALO_BOT_SKATE": "*ram",
        "HALO_BOT_SEED": "3", "HALO_SERVER_MAPS": "bloodgulch", "HALO_SERVER_TIME_LIMIT": "5",
        "HALO_SERVER_BOT_VEHICLES": "-1",
        "HALO_NET_ADDRESS": _test_address(255), "HALO_NET_BROADCAST": _test_address(211)})}
    games["client"] = _start(tmp_path, "client", game, maps, {
        "HALO_NETWORK_TEST": "join", "HALO_NULL_RENDERER": "1", "HALO_NO_AUDIO": "1", "HALO_TEST_INPUT": "bot:7",
        "HALO_SKATE_ASSETS": assets, "HALO_NET_ADDRESS": _test_address(211), "HALO_NET_BROADCAST": _test_address(255)})
    posed = re.compile(r"skate network: player (\d+) skates on another machine at .*?: posed \d+ nodes, the board drawn")
    rode = lambda: {player for player, _ in BOARDED.findall(_log(tmp_path, "server"))}
    try:
        _wait(lambda: len(rode()) >= bots * 3 // 4 and len(set(posed.findall(_log(tmp_path, "client")))) >= bots // 2,
              240, games, tmp_path, f"three quarters of the {bots} bots did not ride, or the client saw too few of them")
    finally:
        _stop(games, tmp_path, {"server": server, "client": game})
        for cache in tmp_path.rglob("cache*.map"):
            cache.unlink()
    log = _log(tmp_path, "server")
    sessions = {int(n) for n in re.findall(r"skate: bot skater (\d+) made for player", log)}
    copied = re.findall(r"preload: session in (\d+)ms \(a copy of the first\)", log)
    slower = re.findall(r"skate: the bots' skaters take .*", log)
    seen = set(posed.findall(_log(tmp_path, "client")))
    print(f"{bots} bots: {len(rode())} rode, {len(sessions)} skate sessions made ({len(copied)} copies, "
          f"{max(map(int, copied)) if copied else 0} ms at most), the client saw {len(seen)} posed with boards; "
          f"load: {slower[-1] if slower else 'never slowed'}")
    assert len(sessions) >= bots * 3 // 4, f"only {len(sessions)} skate sessions for {bots} bots"
    assert copied and max(map(int, copied)) < 2000, f"sessions were not copied quickly: {copied[:5]}"
    assert not re.search(r"NOT FINITE|EXCEPTION|fatal error|session could not be made", log), \
        [line for line in log.splitlines() if re.search(r"NOT FINITE|EXCEPTION|fatal|could not be made", line)][:5]


TRICKS = re.compile(r"bots: (\[BOT\] \w+)'s tricks at tick \d+: (\d+) landed, (\d+) points, (\d+) combos \(best (\d+) tricks, "
                    r"(\d+) points\), (\d+) bails; (\d+) flips, (\d+) late flips, (\d+) grabs, (\d+) manuals, "
                    r"(\d+) flips off grinds, (\d+) pops over things in the way")
TRICK_FIELDS = ("landed", "points", "combos", "best_tricks", "best_points", "bails", "flips", "late_flips", "grabs",
                "manuals", "pops", "obstacle_pops")


def _tricks(log):
    """each bot's last report of its tricks: {name: {field: number}}"""
    return {name: dict(zip(TRICK_FIELDS, (int(n) for n in numbers))) for name, *numbers in TRICKS.findall(log)}


def _difficulties(log):
    """each bot's difficulty, by its name"""
    return dict(re.findall(r"bots: (\[BOT\] \w+) joined \([^)]*?(peaceful|easy|normal|hard|legendary)", log))


def test_skating_bots_tricks(tmp_path):
    """Tricks (port/linux/game/bots_tricks.c): six bots on Prisoner's
    dedicated server with Skate 3 data (its ledges and railings are rails),
    three minutes, three tricking as hard bots do and three as easy ones do
    (debug.bot_tricks; all peaceful, so that they ride rather than fight,
    and the numbers come from riding). Riding, they flick flips through the right
    stick, grab and late-flip in the air, grind the rails they pass and pop
    off them, and the skate engine's scoring (Skate 3's) counts what they
    land: tricks and grinds landed, the hard bots chaining combos and scoring
    far above the easy ones, who trick rarely. About three and a half
    minutes."""
    _, maps, assets = _needs_skate()
    log = _bots_alone(tmp_path, "server", _server(), maps, assets, {
        "HALO_SERVER_BOTS": "6", "HALO_SERVER_BOT_DIFFICULTY": "peaceful", "HALO_BOT_TRICKS": "hard,easy,hard,easy,hard,easy",
        "HALO_BOT_SEED": "5",
        "HALO_SERVER_MAPS": "prisoner", "HALO_SERVER_TIME_LIMIT": "3"}, (_test_address(251), _test_address(252)))
    boards = _boards(log)
    tricks = _tricks(log)
    difficulty = _difficulties(log)
    landed = re.findall(r"bots: player (\d+) lands (\d+) tricks for (\d+) points \(last (\w+)\)", log)
    print(f"boards {boards}\ndifficulties {difficulty}\ntricks {tricks}\nsequences landed {len(landed)}: {landed[:30]}")
    print(f"bails {re.findall(r'bots: player (\d+) bails out of a trick[^\n]*', log)}")
    assert len(tricks) == 6, f"not every bot told of its tricks: {tricks}"
    # (in the order they join: hard, easy, hard, easy ...)
    names = re.findall(r"bots: (\[BOT\] \w+) joined", log)
    hard = [tricks[name] for name in names[0::2]]
    easy = [tricks[name] for name in names[1::2]]
    total = lambda bots, field: sum(b[field] for b in bots)
    tried = lambda bots: sum(total(bots, f) for f in ("flips", "late_flips", "grabs", "manuals", "pops"))
    landed = total(hard + easy, "landed")
    grinds = sum(b[7] for b in boards.values())
    print(f"landed {landed}, grinds {grinds}; hard: tried {tried(hard)}, {total(hard, 'points')} points, "
          f"{total(hard, 'combos')} combos, {total(hard, 'bails')} bails; easy: tried {tried(easy)}, "
          f"{total(easy, 'points')} points, {total(easy, 'combos')} combos, {total(easy, 'bails')} bails")
    # (locally, over runs: 43 to 75 landed, 2 to 7 grinds; the hard pair 3 to 4
    # times the easy pair's points, 5 to 13 combos)
    assert landed >= 30, f"only {landed} tricks landed"
    assert grinds >= 1, f"no grind: {boards}"
    assert tried(hard) >= 2 * tried(easy) + 5, f"the hard bots tried hardly more tricks than the easy ones: {tricks}"
    assert total(hard, "points") >= 1.5 * total(easy, "points"), f"the hard bots did not outscore the easy ones: {tricks}"
    assert total(hard, "combos") >= 2, f"the hard bots chained no combos: {tricks}"
    assert total(hard, "bails") * 3 <= total(hard, "landed"), f"the hard bots bailed too often: {tricks}"
    assert not re.search(r"NOT FINITE|EXCEPTION|fatal error", log), "a pose not finite, or a fatal error"


def test_skating_bots_tricks_in_a_fight(tmp_path):
    """Tricks give way to fights: two hard and two normal bots playing Slayer
    on Blood Gulch's dedicated server with Skate 3 data, three minutes. They
    ride and trick between fights, but begin no trick with an enemy about (a
    target seen or gunfire heard lately), so a fight seldom finds one in the
    middle of a trick: they step off their boards, fire and kill as without
    tricks. About three and a half minutes."""
    _, maps, assets = _needs_skate()
    log = _bots_alone(tmp_path, "server", _server(), maps, assets, {
        "HALO_SERVER_BOTS": "4", "HALO_BOT_DIFFICULTIES": "hard,normal,hard,normal", "HALO_BOT_SEED": "7",
        "HALO_SERVER_MAPS": "bloodgulch", "HALO_SERVER_TIME_LIMIT": "3", "HALO_SERVER_BOT_VEHICLES": "-1"},
        (_test_address(251), _test_address(252)))
    boards = _boards(log)
    tricks = _tricks(log)
    fights = len(re.findall(r"bots: player \d+ steps off the board \(a fight\)", log))
    interrupted = len(re.findall(r"bots: player \d+ stops a trick for a fight", log))
    fired = {name: int(n) for name, n in re.findall(r"bots: (\[BOT\] \w+) at tick \d+: .*?fired (\d+) ticks", log)}
    scores = SCORE.findall(log)
    print(f"boards {boards}\ntricks {tricks}\nsteps off for a fight {fights}, of them in the middle of a trick "
          f"{interrupted}\nfired {fired}; scores {scores}")
    kills = sum(int(score[2]) for score in scores)
    assert sum(b["landed"] for b in tricks.values()) >= 5, f"the bots hardly tricked: {tricks}"
    assert fights >= 2, f"the bots stepped off their boards for {fights} fights"
    assert interrupted <= max(1, fights // 3), f"{interrupted} of {fights} fights found a bot in the middle of a trick"
    assert sum(1 for n in fired.values() if n > 0) >= 3 and kills >= 4, f"the bots hardly fought: fired {fired}, scores {scores}"

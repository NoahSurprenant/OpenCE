"""Infection (port/linux/game/infection.c; port/linux/README.md's
"Infection") in real games of bots alone on a dedicated server with Skate 3
mode and its data: a --skate build (HALO_TEST_SERVER, else HALO_TEST_GAME),
the converted Skate 3 data (HALO_SKATE_TEST_ASSETS) and the Halo maps
(HALO_SKATE_TEST_HALO_MAPS); they skip without them. Each game's random
numbers are fixed (debug.bot_seed, debug.game_seed)."""

import re

from tools.test_bots import _log, _start, _stop, _wait
from tools.test_linux_port import _test_address
from tools.test_skating_bots import BOARDED, RUN_OVER_KILLED, _bots_alone, _needs_skate, _server

INFECTED = re.compile(r"infection: player (\d+) is infected by player (\d+) at tick (\d+)")
STARTS = re.compile(r"infection: player (\d+) starts infected")
OVER = re.compile(r"infection: the game is over at tick (\d+): the (survivors|infected) win")
SEEN = re.compile(r"infection: infected player (\d+) at tick (\d+): (on a board|not on a board), (\d+) weapons, "
                  r"(\d+) grenades, (in a vehicle|no vehicle)")
TALLY = re.compile(r"infection: at tick (\d+): (\d+) survivors, (\d+) infected; the infected's presses refused (\d+) "
                   r"times, (\d+) rules broken")
FINAL = re.compile(r"infection: player (\d+) (infected|a survivor), score (-?\d+), survived (\d+) seconds")


def _over(log):
    return OVER.search(log) and len(FINAL.findall(log)) >= 1


def test_infection_a_run_over_infects(tmp_path):
    """A hard infected bot (debug.infection_first 0) and four peaceful
    survivors standing still (debug.bot_skate 'stand') on Blood Gulch: the
    infected rides at them and runs them over, which infects them (the host
    moves them to the infected); each comes back infected, on a board; once
    the last survivor is infected the infected win. About three minutes."""
    _, maps, assets = _needs_skate()
    log = _bots_alone(tmp_path, "server", _server(), maps, assets, {
        "HALO_SERVER_BOTS": "5", "HALO_BOT_DIFFICULTIES": "hard,peaceful,peaceful,peaceful,peaceful",
        "HALO_BOT_SKATE": "ram,stand,stand,stand,stand", "HALO_INFECTION_FIRST": "0", "HALO_BOT_SEED": "1", "HALO_GAME_SEED": "1",
        "HALO_SERVER_MAPS": "bloodgulch:infection", "HALO_SERVER_TIME_LIMIT": "5"},
        (_test_address(181), _test_address(182)), done=_over)
    infected = INFECTED.findall(log)
    over = OVER.search(log)
    print(f"starts {STARTS.findall(log)}; infected {infected}; run-over kills by {RUN_OVER_KILLED.findall(log)}; "
          f"over {over and over.groups()}; final {FINAL.findall(log)}")
    assert STARTS.findall(log) == ["0"], f"not player 0 alone started infected: {STARTS.findall(log)}"
    assert infected, "nobody was infected"
    first_victim, first_by, first_tick = infected[0]
    assert first_by == "0" and first_victim in ("1", "2", "3", "4"), f"the first infection: {infected[0]}"
    assert "0" in RUN_OVER_KILLED.findall(log), "the infected's kill was no run-over"
    # the victim came back infected, on a board
    later = [s for s in SEEN.findall(log) if s[0] == first_victim and int(s[1]) > int(first_tick)]
    assert any(s[2] == "on a board" for s in later), f"player {first_victim} never rode infected: {later}"
    assert first_victim in {p for p, _ in BOARDED.findall(log)}, "the victim's bot never got on a board"
    assert over and over.group(2) == "infected", f"the infected did not win: {over and over.groups()}"
    scores = {p: (side, int(score)) for p, side, score, _ in FINAL.findall(log)}
    assert scores["0"][1] >= 1, f"the infected's infections not scored: {scores}"


def test_infection_the_infected_keep_the_rules(tmp_path):
    """Two peaceful infected (debug.infection_first 0,1) and two peaceful
    survivors on Blood Gulch with every vehicle it places, a one-minute round,
    and debug.infection_test_press: the infected press fire, melee, grenade,
    action, swap and reload every tick and their bots ask off their boards
    every five seconds. None of it happens: they ride on boards throughout,
    unarmed, never in a vehicle (the host checks every tick); nobody is
    infected (a peaceful infected never rides at anyone on purpose), so the
    survivors win when the time runs out. About two minutes."""
    _, maps, assets = _needs_skate()
    log = _bots_alone(tmp_path, "server", _server(), maps, assets, {
        "HALO_SERVER_BOTS": "4", "HALO_BOT_DIFFICULTIES": "peaceful,peaceful,peaceful,peaceful",
        "HALO_INFECTION_FIRST": "0,1", "HALO_INFECTION_TEST_PRESS": "1", "HALO_NETWORK_TEST_VEHICLES": "all",
        "HALO_BOT_SEED": "2", "HALO_GAME_SEED": "2", "HALO_SERVER_MAPS": "bloodgulch:infection",
        "HALO_SERVER_TIME_LIMIT": "1"},
        (_test_address(183), _test_address(184)), done=_over)
    seen = SEEN.findall(log)
    tallies = TALLY.findall(log)
    over = OVER.search(log)
    print(f"seen {seen[-8:]}; tallies {tallies[-3:]}; over {over and over.groups()}")
    assert "RULE BROKEN" not in log, re.findall(r"infection: RULE BROKEN.*", log)[:5]
    assert tallies and int(tallies[-1][3]) > 1000, f"the presses were not refused: {tallies[-1:]}"
    assert re.search(r"skate: infected player [01] asked off the board, stays on", log), "no dismount was refused"
    # (each infected seen from its first ride on: on a board, unarmed, no vehicle)
    for player in ("0", "1"):
        mine = [s for s in seen if s[0] == player]
        riding = [s for s in mine if s[2] == "on a board"]
        assert len(riding) >= 3, f"infected player {player} did not ride: {mine}"
        first = mine.index(riding[0])
        assert all(s[2] == "on a board" for s in mine[first:]), f"infected player {player} got off: {mine}"
        assert all(s[3] == "0" and s[4] == "0" and s[5] == "no vehicle" for s in mine), mine
    assert not INFECTED.findall(log), f"a peaceful infected infected someone: {INFECTED.findall(log)}"
    assert over and over.group(2) == "survivors", f"the survivors did not win: {over and over.groups()}"
    assert 55 * 30 <= int(over.group(1)) <= 70 * 30, f"the round did not last a minute: {over.group(1)}"


def test_infection_a_person_infected_on_a_client(tmp_path):
    """A listen host on Blood Gulch (its player, two peaceful bots) and a
    client joining it whose player starts infected (debug.infection_first "client"): the
    client learns it is
    infected from the host's unit, its own skater gets on a board by itself
    and the host sees it ride; the client's scripted input (fire, the
    action button, moves: debug.test_input) does nothing it may not, and its
    player is never armed nor in a vehicle. About two minutes."""
    game, maps, assets = _needs_skate()
    host_address, join_address = _test_address(187), _test_address(188)
    games = {"host": _start(tmp_path, "host", game, maps, {
        "HALO_NETWORK_TEST": "host:bloodgulch:infection", "HALO_NETWORK_TEST_START": "8", "HALO_SERVER_BOTS": "2",
        "HALO_BOT_DIFFICULTIES": "peaceful", "HALO_BOT_SEED": "3", "HALO_GAME_SEED": "3", "HALO_INFECTION_FIRST": "client",
        "HALO_SKATE_ASSETS": assets, "HALO_NULL_RENDERER": "1", "HALO_NO_AUDIO": "1", "HALO_NETWORK_TEST_VEHICLES": "all",
        "HALO_NET_ADDRESS": host_address, "HALO_NET_BROADCAST": join_address})}
    games["join"] = _start(tmp_path, "join", game, maps, {
        "HALO_NETWORK_TEST": "join", "HALO_NULL_RENDERER": "1", "HALO_NO_AUDIO": "1", "HALO_TEST_INPUT": "bot:7",
        "HALO_SKATE_ASSETS": assets, "HALO_NET_ADDRESS": join_address, "HALO_NET_BROADCAST": host_address})

    def client_player(host):
        found = re.search(r"infection: player (\d+) starts infected \(the test's client\)", host)
        return found.group(1) if found else None

    def ridden():
        host = _log(tmp_path, "host")
        player = client_player(host)
        return player and len([s for s in SEEN.findall(host) if s[0] == player and s[2] == "on a board"]) >= 4

    try:
        _wait(ridden, 240, games, tmp_path, "the host did not see the client's infected player ride a board")
    finally:
        _stop(games, tmp_path, {"host": game, "join": game})
        for cache in tmp_path.rglob("cache*.map"):
            cache.unlink()
    host, join = _log(tmp_path, "host"), _log(tmp_path, "join")
    player = client_player(host)
    seen = [s for s in SEEN.findall(host) if s[0] == player]
    print(f"host saw the client's player {player} {seen[-6:]}; client: {re.findall(r'infection: this machine.*', join)[:3]}")
    assert player and STARTS.findall(host) == [player], f"not the client's player alone started infected: {STARTS.findall(host)}"
    assert re.search(r"infection: this machine's player \d+ is infected", join), "the client never learnt it is infected"
    assert "skate: on" in join, "the client's own skater never got on a board"
    assert "RULE BROKEN" not in host, re.findall(r"infection: RULE BROKEN.*", host)[:5]
    assert all(s[3] == "0" and s[4] == "0" and s[5] == "no vehicle" for s in seen), seen


def _balance_game(
tmp_path, seed, run_over, minutes, name="server", damage=0):
    """an Infection game of eight normal bots on Blood Gulch (one starts
    infected, chosen by the seed): (winner, infections, the final lines)"""
    _, maps, assets = _needs_skate()
    settings = {
        "HALO_SERVER_BOTS": "8", "HALO_BOT_DIFFICULTIES": "normal", "HALO_BOT_SEED": str(seed),
        "HALO_GAME_SEED": str(seed), "HALO_SERVER_MAPS": "bloodgulch:infection",
        "HALO_SERVER_TIME_LIMIT": str(minutes)}
    if run_over:
        settings["HALO_SERVER_INFECTION_RUN_OVER"] = str(run_over)
    if damage:
        settings["HALO_SERVER_INFECTION_DAMAGE_TAKEN"] = str(damage)
    # (INFECTION_TRIAL_ENV "K=V,K=V": more settings, for the tuning)
    import os
    for pair in filter(None, (os.environ.get("INFECTION_TRIAL_ENV") or "").split(",")):
        key, value = pair.split("=", 1)
        settings[key] = value

    log = _bots_alone(tmp_path, name, _server(), maps, assets, settings, (_test_address(185), _test_address(186)),
                      done=_over)
    over = OVER.search(log)
    return (over.group(2) if over else None), INFECTED.findall(log), FINAL.findall(log), log


def test_infection_bots_balance(tmp_path):
    """Eight normal bots play a five-minute round of Infection on Blood Gulch
    (seed 3) with the game type's run-over damage: the infected (one at the
    start) infect at least two survivors by running them over, and the
    survivors kill infected with their weapons (the round's outcome varies
    with the seed: tools/test_infection.py's INFECTION_TRIAL loop measured the
    share each side wins). About six minutes."""
    import os
    seed = int(os.environ.get("INFECTION_TRIAL_SEED") or "3")
    run_over = int(os.environ.get("INFECTION_TRIAL_RUN_OVER") or "0")
    minutes = int(os.environ.get("INFECTION_TRIAL_MINUTES") or "5")
    damage = int(os.environ.get("INFECTION_TRIAL_DAMAGE") or "0")
    winner, infected, final, log = _balance_game(tmp_path, seed, run_over, minutes, damage=damage)
    # (the infected shot dead: the survivors' weapons, as bots log a death)
    kills = len(re.findall(r"bots: player \d+ died of weapons\\", log))
    infections = len(infected)
    print(f"INFECTION TRIAL seed {seed} run-over {run_over or 'default'} damage taken {damage or 'default'}: "
          f"winner {winner}, {infections} infected by "

          f"run-overs of {len(RUN_OVER_KILLED.findall(log))}, deaths as survivors "
          f"{len(re.findall(r'dies a survivor', log))}, shot dead {kills}, final {final}")
    if os.environ.get("INFECTION_TRIAL"):
        return
    assert winner in ("survivors", "infected"), "the round did not end"
    assert infections >= 2, f"the infected infected {infections} survivors: {infected}"
    assert kills >= 1, "nobody was shot dead"


VARIANT = re.compile(r"infection: the variant: round (\d+) seconds, run-overs (\d+)%, speed (\d+)%, damage taken (\d+)%, "
                     r"reach (\d+), one infected in (\d+), turning (\d+)%, out of reach (\d+)")


def test_infection_an_edited_gametype(tmp_path):
    """A listen host whose Infection gametype is edited through the gametype
    editor's own spinners (debug.gametype_edit: menu_functions.c's options
    table, as a person sets the Infection Options screen) - a one-minute
    round, boards at 140%, run-overs at 500%, the infected taking 25%, a
    reach of 0.4, one in 4 infected, turning at 175%, out of reach after 20
    seconds - with two peaceful bots and a client whose player starts
    infected: both machines play by the edited values (the client has them
    from the host's game), the client's board steps at 1.4 times and turns
    1.75 times as sharply, and the round ends after a minute, the survivors
    winning.
    About two minutes."""
    game, maps, assets = _needs_skate()
    host_address, join_address = _test_address(179), _test_address(180)
    edit = ("infection_minutes_spinner=1,infection_speed_spinner=140,infection_run_over_spinner=500,"
            "infection_damage_taken_spinner=25,infection_reach_spinner=40,infection_ratio_spinner=4,"
            "infection_turning_spinner=175,infection_out_of_reach_spinner=20")
    games = {"host": _start(tmp_path, "host", game, maps, {
        "HALO_NETWORK_TEST": "host:bloodgulch:infection", "HALO_NETWORK_TEST_START": "8", "HALO_SERVER_BOTS": "2",
        "HALO_BOT_DIFFICULTIES": "peaceful", "HALO_BOT_SEED": "4", "HALO_GAME_SEED": "4", "HALO_INFECTION_FIRST": "client",
        "HALO_GAMETYPE_EDIT": edit, "HALO_SKATE_ASSETS": assets, "HALO_NULL_RENDERER": "1", "HALO_NO_AUDIO": "1",
        "HALO_NET_ADDRESS": host_address, "HALO_NET_BROADCAST": join_address})}
    games["join"] = _start(tmp_path, "join", game, maps, {
        "HALO_NETWORK_TEST": "join", "HALO_NULL_RENDERER": "1", "HALO_NO_AUDIO": "1", "HALO_TEST_INPUT": "bot:7",
        "HALO_SKATE_ASSETS": assets, "HALO_NET_ADDRESS": join_address, "HALO_NET_BROADCAST": host_address})
    try:
        _wait(lambda: OVER.search(_log(tmp_path, "host")), 240, games, tmp_path, "the edited round did not end")
    finally:
        _stop(games, tmp_path, {"host": game, "join": game})
        for cache in tmp_path.rglob("cache*.map"):
            cache.unlink()
    host, join = _log(tmp_path, "host"), _log(tmp_path, "join")
    expected = ("60", "500", "140", "25", "40", "4", "175", "20")
    over = OVER.search(host)
    print(f"host {VARIANT.findall(host)[:1]}, client {VARIANT.findall(join)[:1]}, over {over.groups()}")
    assert VARIANT.findall(host)[:1] == [expected], VARIANT.findall(host)
    assert VARIANT.findall(join)[:1] == [expected], f"the client did not have the edited values: {VARIANT.findall(join)}"
    assert "rides at 1.40 times the speed" in join, "the client's infected board did not go faster"
    assert "turns 1.75 times as sharply" in join, "the client's infected board did not turn more sharply"
    assert over.group(2) == "survivors" and 55 * 30 <= int(over.group(1)) <= 70 * 30, over.groups()


OUT_OF_REACH = re.compile(r"infection: player (\d+) is out of reach at \((\S+) (\S+) (\S+)\)")
HURT = re.compile(r"infection: player (\d+) hurt out of reach \((\d+)\) at \(\S+ \S+ \S+\) at tick (\d+): shield (\S+) to "
                  r"([\d.]+), body ([\d.]+) to ([\d.]+)(, killed)?")
DIED_ALONE = re.compile(r"infection: player (\d+) dies a survivor \(killer -1\), infected at tick (\d+)")
PLACED = re.compile(r"infection: the test puts player (\d+) at")
TICK = re.compile(r"at tick (\d+)")
# where survivors stand in test_infection_out_of_reach, each map's (world
# units, on its walking graph's nodes): the spots no board reaches (the
# bases' roofs, a ledge high on a cliff, Hang 'Em High's tower top), then
# ground any board rides to (open ground, by the bases, Hang 'Em High's
# floor and its upper walkway)
CAMP_SPOTS = {
    "bloodgulch": [(95.42, -160.89, 1.73), (39.12, -78.09, 1.73), (69.42, -55.59, 6.78)],
    "hangemhigh": [(30.35, -13.97, -2.38)],
    "sidewinder": [(30.25, -38.78, 0.57), (-32.75, -35.78, 0.57)],
}
GROUND_SPOTS = {
    "bloodgulch": [(85.42, -149.49, 0.0), (65.42, -119.59, 0.12), (50.42, -89.59, 0.17)],
    "hangemhigh": [(20.35, -0.37, -7.95), (25.35, 9.63, -7.97), (33.35, -5.37, -3.9)],
    "sidewinder": [(25.25, -24.78, -3.58), (-24.75, -24.78, -3.83)],
}


def test_infection_out_of_reach(tmp_path):
    """Out of reach (the anti-camping rule) with the game type's own ten
    seconds, on Blood Gulch, Hang 'Em High and Sidewinder: peaceful
    survivors standing still (debug.bot_skate 'hold'), each put on a spot
    of the map (debug.infection_test_place) - where no board reaches (the
    bases' roofs, a cliff ledge, the tower top) and on ground any board rides
    to - and one infected standing still far off. Each on a roof or a ledge
    is told out of reach, hurt from ten seconds on, each second, and
    dies of it in a few (infected, nobody's kill); each on the ground is never out of reach nor
    hurt, and is a survivor still forty seconds in. Then a client's player on
    a roof is told out of reach on the client and hurt by the host. About
    four minutes."""
    _, maps, assets = _needs_skate()
    results = {}
    for index, name in enumerate(CAMP_SPOTS):
        spots = CAMP_SPOTS[name] + GROUND_SPOTS[name]
        camp = {str(1 + i) for i in range(len(CAMP_SPOTS[name]))}
        ground = {str(1 + len(camp) + i) for i in range(len(GROUND_SPOTS[name]))}
        place = ";".join(f"{1 + i}:{x},{y},{z + 0.05}" for i, (x, y, z) in enumerate(spots))

        def done(log, camp=camp):
            ticks = [int(t) for t in TICK.findall(log[-4000:])]
            return ({p for p, _ in DIED_ALONE.findall(log)} >= camp and ticks and max(ticks) >= 40 * 30) or _over(log)

        log = _bots_alone(tmp_path / name, "server", _server(), maps, assets, {
            "HALO_SERVER_BOTS": str(1 + len(spots)), "HALO_BOT_DIFFICULTIES": "peaceful", "HALO_BOT_SKATE": "*hold",
            "HALO_INFECTION_FIRST": "0", "HALO_INFECTION_TEST_PLACE": place, "HALO_BOT_SEED": "5",
            "HALO_GAME_SEED": "5", "HALO_SERVER_MAPS": f"{name}:infection", "HALO_SERVER_TIME_LIMIT": "2"},
            (_test_address(161 + 2 * index), _test_address(162 + 2 * index)), done=done)
        out = {p for p, *_ in OUT_OF_REACH.findall(log)}
        hurt = {p for p, *_ in HURT.findall(log)}
        died = {p for p, _ in DIED_ALONE.findall(log)}
        infected = {p for p, *_ in INFECTED.findall(log)}
        results[name] = (sorted(out), sorted(hurt), sorted(died))
        print(f"{name}: placed {PLACED.findall(log)}; out of reach {sorted(out)}, hurt {sorted(hurt)}, died of it "
              f"{sorted(died)}; camp {sorted(camp)}, ground {sorted(ground)}; first hurts "
              f"{[h for h in HURT.findall(log)][:4]}")
        assert set(PLACED.findall(log)) >= camp | ground, f"{name}: not everyone was put on their spot"
        assert camp <= out, f"{name}: on a roof or ledge and not out of reach: {sorted(camp - out)}"
        assert camp <= hurt, f"{name}: out of reach and not hurt: {sorted(camp - hurt)}"
        assert camp <= died, f"{name}: hurt out of reach and not dead of it: {sorted(camp - died)}"
        assert not (ground & (out | hurt | died | infected)), \
            f"{name}: on the ground, yet out of reach {sorted(ground & out)}, hurt {sorted(ground & hurt)}"
        # (hurt over a few seconds, not killed at once)
        for player in camp:
            hurts = [h for h in HURT.findall(log) if h[0] == player]
            assert len(hurts) >= 3 and not hurts[0][7], f"{name}: player {player} hurt {len(hurts)} times: {hurts}"
        # (the hurt starts at ten seconds out of reach, not before)
        for player, _, tick, *_ in HURT.findall(log):
            first = int(re.search(rf"infection: player {player} is out of reach at .* at tick (\d+)", log).group(1))
            assert int(tick) - first >= 10 * 30 - 1, f"{name}: player {player} hurt {int(tick) - first} ticks in"
    # a client's own player out of reach: told on the client (its own graph
    # of the map, its own count: what its warning shows), hurt by the host
    game = _needs_skate()[0]
    host_address, join_address = _test_address(167), _test_address(168)
    folder = tmp_path / "client"
    games = {"host": _start(folder, "host", game, maps, {
        "HALO_NETWORK_TEST": "host:bloodgulch:infection", "HALO_NETWORK_TEST_START": "8", "HALO_SERVER_BOTS": "1",
        "HALO_BOT_DIFFICULTIES": "peaceful", "HALO_BOT_SKATE": "*hold", "HALO_BOT_SEED": "5", "HALO_GAME_SEED": "5",
        "HALO_INFECTION_FIRST": "bots", "HALO_INFECTION_TEST_PLACE": "client:39.12,-78.09,1.78",
        "HALO_SKATE_ASSETS": assets, "HALO_NULL_RENDERER": "1", "HALO_NO_AUDIO": "1",
        "HALO_NET_ADDRESS": host_address, "HALO_NET_BROADCAST": join_address})}
    games["join"] = _start(folder, "join", game, maps, {
        "HALO_NETWORK_TEST": "join", "HALO_NULL_RENDERER": "1", "HALO_NO_AUDIO": "1",
        "HALO_SKATE_ASSETS": assets, "HALO_NET_ADDRESS": join_address, "HALO_NET_BROADCAST": host_address})

    def client_player():
        found = re.search(r"infection: the test puts player (\d+) at \(39\.12", _log(folder, "host"))
        return found.group(1) if found else None

    try:
        _wait(lambda: client_player() and re.search(rf"infection: this machine's player {client_player()} is infected",
                                                    _log(folder, "join")),
              240, games, folder, "the client's player out of reach did not die of it")
    finally:
        _stop(games, folder, {"host": game, "join": game})
        for cache in folder.rglob("cache*.map"):
            cache.unlink()
    host, join = _log(folder, "host"), _log(folder, "join")
    player = client_player()
    told = [o for o in OUT_OF_REACH.findall(join) if o[0] == player]
    hurts = [h for h in HURT.findall(host) if h[0] == player]
    print(f"client's player {player}: told out of reach on the client {told[:2]}, hurt by the host {hurts[:4]}")
    assert told, "the client did not tell its own player out of reach"
    assert len(hurts) >= 3, f"the host did not hurt the client's player out of reach: {hurts}"


STILL = re.compile(r"skate: the bots' skaters: (\d+) skating on average.*a riding skater stood still between its steps (\d+) times")
PERIODS = re.compile(r"skate: the bots' skaters: (\d+) near people step every (\d+) ticks?, (\d+) further away every (\d+)")


def test_infection_skaters_stepping_in_turn(tmp_path):
    """The run-over game of test_infection_a_run_over_infects with every
    bot's skater stepping only every other tick (debug.skate_bot_period 2),
    as a host short of processors has its skaters far from people do
    (skate.c, "many bots on boards"): between its steps each glides on, so
    no riding skater's biped ever stands still a tick, and the game plays
    on - the infected still rides at the survivors, the host decides its
    run-overs and they infect, and the infected win (a skater stepped every
    other tick rides half as fast: later). About three minutes."""
    _, maps, assets = _needs_skate()
    log = _bots_alone(tmp_path, "server", _server(), maps, assets, {
        "HALO_SERVER_BOTS": "5", "HALO_BOT_DIFFICULTIES": "hard,peaceful,peaceful,peaceful,peaceful",
        "HALO_BOT_SKATE": "ram,stand,stand,stand,stand", "HALO_INFECTION_FIRST": "0", "HALO_BOT_SEED": "1", "HALO_GAME_SEED": "1",
        "HALO_SKATE_BOT_PERIOD": "2", "HALO_SERVER_MAPS": "bloodgulch:infection", "HALO_SERVER_TIME_LIMIT": "3"},
        (_test_address(169), _test_address(170)), done=_over)
    infected = INFECTED.findall(log)
    over = OVER.search(log)
    periods = PERIODS.findall(log)
    still = STILL.findall(log)
    print(f"periods {periods}; still {still}; infected {infected}; run-over kills by {RUN_OVER_KILLED.findall(log)}; "
          f"over {over and over.groups()}")
    assert periods and all(p[1] == "2" and p[3] == "2" for p in periods), f"the skaters did not step every other tick: {periods}"
    assert still and any(int(s[0]) >= 1 for s in still), f"no report of the skaters riding: {still}"
    assert all(s[1] == "0" for s in still), f"a riding skater stood still between its steps: {still}"
    assert infected and infected[0][1] == "0", f"the infected infected nobody: {infected}"
    assert "0" in RUN_OVER_KILLED.findall(log), "the infected's kill was no run-over"
    assert over and over.group(2) == "infected", f"the infected did not win: {over and over.groups()}"
    assert not re.search(r"NOT FINITE|EXCEPTION|fatal error", log), "a pose not finite, or a fatal error"

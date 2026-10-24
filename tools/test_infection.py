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
    client joining it whose player starts infected (debug.infection_first 3:
    the bots are 0 and 1, the host's player 2): the client learns it is
    infected from the host's unit, its own skater gets on a board by itself
    and the host sees it ride; the client's scripted input (fire, the
    action button, moves: debug.test_input) does nothing it may not, and its
    player is never armed nor in a vehicle. About two minutes."""
    game, maps, assets = _needs_skate()
    host_address, join_address = _test_address(187), _test_address(188)
    games = {"host": _start(tmp_path, "host", game, maps, {
        "HALO_NETWORK_TEST": "host:bloodgulch:infection", "HALO_NETWORK_TEST_START": "8", "HALO_SERVER_BOTS": "2",
        "HALO_BOT_DIFFICULTIES": "peaceful", "HALO_BOT_SEED": "3", "HALO_GAME_SEED": "3", "HALO_INFECTION_FIRST": "3",
        "HALO_SKATE_ASSETS": assets, "HALO_NULL_RENDERER": "1", "HALO_NO_AUDIO": "1", "HALO_NETWORK_TEST_VEHICLES": "all",
        "HALO_NET_ADDRESS": host_address, "HALO_NET_BROADCAST": join_address})}
    games["join"] = _start(tmp_path, "join", game, maps, {
        "HALO_NETWORK_TEST": "join", "HALO_NULL_RENDERER": "1", "HALO_NO_AUDIO": "1", "HALO_TEST_INPUT": "bot:7",
        "HALO_SKATE_ASSETS": assets, "HALO_NET_ADDRESS": join_address, "HALO_NET_BROADCAST": host_address})

    def ridden():
        return len([s for s in SEEN.findall(_log(tmp_path, "host")) if s[0] == "3" and s[2] == "on a board"]) >= 4

    try:
        _wait(ridden, 240, games, tmp_path, "the host did not see the client's infected player ride a board")
    finally:
        _stop(games, tmp_path, {"host": game, "join": game})
        for cache in tmp_path.rglob("cache*.map"):
            cache.unlink()
    host, join = _log(tmp_path, "host"), _log(tmp_path, "join")
    seen = [s for s in SEEN.findall(host) if s[0] == "3"]
    print(f"host saw player 3 {seen[-6:]}; client: {re.findall(r'infection: this machine.*', join)[:3]}")
    assert STARTS.findall(host) == ["3"], f"not the client's player alone started infected: {STARTS.findall(host)}"
    assert re.search(r"infection: this machine's player 3 is infected", join), "the client never learnt it is infected"
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
    start) infect at least three survivors by running them over, and the
    survivors kill infected with their weapons (the round's outcome varies
    with the seed: tools/test_infection.py's INFECTION_TRIAL loop measured the
    share each side wins). About six minutes."""
    import os
    seed = int(os.environ.get("INFECTION_TRIAL_SEED") or "3")
    run_over = int(os.environ.get("INFECTION_TRIAL_RUN_OVER") or "0")
    minutes = int(os.environ.get("INFECTION_TRIAL_MINUTES") or "5")
    damage = int(os.environ.get("INFECTION_TRIAL_DAMAGE") or "0")
    winner, infected, final, log = _balance_game(tmp_path, seed, run_over, minutes, damage=damage)
    kills = sum(int(score) for _, side, score, _ in final if side == "a survivor")
    infections = len(infected)
    print(f"INFECTION TRIAL seed {seed} run-over {run_over or 'default'} damage taken {damage or 'default'}: "
          f"winner {winner}, {infections} infected by "

          f"run-overs of {len(RUN_OVER_KILLED.findall(log))}, deaths as survivors "
          f"{len(re.findall(r'dies a survivor', log))}, final {final}")
    if os.environ.get("INFECTION_TRIAL"):
        return
    assert winner in ("survivors", "infected"), "the round did not end"
    assert infections >= 3, f"the infected infected {infections} survivors: {infected}"

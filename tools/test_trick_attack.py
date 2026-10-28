"""Trick Attack (port/linux/game/trick_attack.c; port/linux/README.md's
"Trick Attack") in real games: bots alone on a dedicated server with Skate 3
mode and its data, or a listen host with a client - a --skate build
(HALO_TEST_SERVER, else HALO_TEST_GAME), the converted Skate 3 data
(HALO_SKATE_TEST_ASSETS) and the Halo maps (HALO_SKATE_TEST_HALO_MAPS); they
skip without them. Each game's random numbers are fixed (debug.bot_seed,
debug.game_seed) and so are the hot spots' and the bots' places where a
test needs them."""

import math
import re

from tools.test_bots import _log, _start, _stop, _wait
from tools.test_linux_port import _test_address
from tools.test_skating_bots import _bots_alone, _needs_skate, _server

VARIANT = re.compile(r"trick attack: the variant: round (\d+) seconds, bank delay (\d+) ticks, steal (\d+)%, hot spot "
                     r"x([\d.]+) for (\d+) seconds, ramming (on|off)")
LANDS = re.compile(r"trick attack: player (\d+) lands (\d+) tricks for (\d+) points( \(in the hot spot\))?: combo of "
                   r"(\d+) sequences x(\d+) worth (\d+) at tick (\d+)")
BANKS = re.compile(r"trick attack: player (\d+) banks a combo of (\d+) sequences \((\d+) tricks\) x(\d+) for (\d+) points, "
                   r"score (-?\d+), at tick (\d+)")
LOSES = re.compile(r"trick attack: player (\d+) (.+?) and loses a combo of (\d+) sequences \((\d+) tricks\) worth (\d+) "
                   r"at tick (\d+)")
RAMS = re.compile(r"trick attack: player (\d+) rams player (\d+) mid-combo at ([\d.]+) m/s and takes (\d+) of its (\d+) "
                  r"at tick (\d+)")
HOT = re.compile(r"trick attack: hot spot (\d+) at \((\S+) (\S+) (\S+)\), one of (\d+) a board reaches \(reaches (-?\d+)\), "
                 r"x([\d.]+) for (\d+) seconds, at tick (\d+)")
FINAL = re.compile(r"trick attack: player (\d+) scores (-?\d+): (\d+) combos banked \(best (\d+)\), (\d+) lost \((\d+) "
                   r"points\), (\d+) rams \((\d+) points taken\), (\d+) sequences in the hot spot")
OVER = re.compile(r"trick attack: the game is over at tick (\d+): player (-?\d+) wins with (-?\d+) points, (\d+) hot spots, "
                  r"(\d+) rules broken")
TALLY = re.compile(r"trick attack: at tick (\d+): (\d+) of (\d+) players on boards, (\d+) rules broken, (\d+) reports "
                   r"refused, presses refused (\d+) times")
HEADS = re.compile(r"bots: player (\d+) heads for the hot spot at")
HUNTS = re.compile(r"bots: player (\d+) hunts player (\d+)'s combo of (\d+)")
BANKING = re.compile(r"bots: player (\d+) rides on to bank a combo of (\d+)")


def _over(log):
    return OVER.search(log) and FINAL.findall(log)


def _scores(log):
    """{player: score} from the game's end"""
    return {int(p): int(s) for p, s, *_ in FINAL.findall(log)}


def test_trick_attack_bots_score_and_keep_the_rules(tmp_path):
    """Six bots on Blood Gulch with every vehicle it places, a three-minute
    round, the hot spot moving every 30 seconds, and debug.infection_test_press
    (every skater presses fire, melee, grenade, action, swap and reload each
    tick and asks off its board now and then): nobody fires, throws, picks
    up, boards a vehicle or leaves their board (the host checks every tick);
    the bots ride, chain tricks into combos (Skate 3's scoring) and bank them
    - their score is exactly what they banked and took by ramming - head for
    each hot spot, which is always where a board reaches and moves on; the
    highest score wins. Two legendary bots (they hunt big combos), two
    hard and two easy. About three and a half minutes."""
    _, maps, assets = _needs_skate()
    log = _bots_alone(tmp_path, "server", _server(), maps, assets, {
        "HALO_SERVER_BOTS": "6", "HALO_BOT_DIFFICULTIES": "legendary,legendary,hard,hard,easy,easy",
        "HALO_INFECTION_TEST_PRESS": "1", "HALO_NETWORK_TEST_VEHICLES": "all",
        "HALO_SERVER_TRICK_HOT_SPOT_SECONDS": "30", "HALO_SERVER_TRICK_MINUTES": "3",
        "HALO_BOT_SEED": "71", "HALO_GAME_SEED": "71",
        "HALO_SERVER_MAPS": "bloodgulch:trick_attack", "HALO_SERVER_TIME_LIMIT": "5"},
        (_test_address(201), _test_address(202)), done=_over)
    lands, banks, loses, rams = LANDS.findall(log), BANKS.findall(log), LOSES.findall(log), RAMS.findall(log)
    hots, tallies, over, scores = HOT.findall(log), TALLY.findall(log), OVER.search(log), _scores(log)
    print(f"variant {VARIANT.findall(log)[:1]}; {len(lands)} landings, {len(banks)} banks, {len(loses)} lost, rams {rams}")
    print(f"banks {banks[:12]}\nlost {loses[:8]}\nhot spots {hots}\ntallies {tallies[-3:]}")
    print(f"final {FINAL.findall(log)}\nover {over and over.groups()}; heading {sorted(set(HEADS.findall(log)))}, "
          f"hunting {HUNTS.findall(log)[:6]}, banking {BANKING.findall(log)[:6]}")
    # the rules: nothing they may not do happened, everyone on a board
    assert "RULE BROKEN" not in log, re.findall(r"trick attack: RULE BROKEN.*", log)[:5]
    assert tallies and int(tallies[-1][5]) > 1000, f"the presses were not refused: {tallies[-1:]}"
    assert all(int(t[1]) == int(t[2]) == 6 for t in tallies[2:]), f"not everyone on a board: {tallies}"
    assert re.search(r"skate: infected player \d+ asked off the board, stays on", log), "no dismount was refused"
    # tricks scored, chained, banked; a score is what was banked and taken
    assert len(lands) >= 20, f"only {len(lands)} sequences landed"
    assert len(banks) >= 8, f"only {len(banks)} combos banked"
    assert any(int(b[3]) >= 2 for b in banks), f"no combo of two or more sequences banked: {banks}"
    for player, score in scores.items():
        banked = sum(int(b[4]) for b in banks if int(b[0]) == player)
        taken = sum(int(r[3]) for r in rams if int(r[0]) == player)
        assert score == banked + taken, f"player {player}'s score {score} is not banked {banked} + taken {taken}"
    hard = [scores[p] for p in (0, 1, 2, 3)]
    easy = [scores[p] for p in (4, 5)]
    assert sum(hard) / 4 > sum(easy) / 2, f"the better bots did not outscore the easy ones: {scores}"
    assert over and int(over.group(3)) == max(scores.values()) and scores[int(over.group(2))] == max(scores.values())
    assert 170 * 30 <= int(over.group(1)) <= 190 * 30, f"the round did not last three minutes: {over.group(1)}"
    # the hot spot: where a board reaches, moving on every 30 seconds, sought
    assert len(hots) >= 5, f"the hot spot did not move on: {hots}"
    assert all(h[5] == "1" for h in hots), f"a hot spot no board reaches: {hots}"
    for before, after in zip(hots, hots[1:]):
        apart = math.hypot(float(after[1]) - float(before[1]), float(after[2]) - float(before[2]))
        assert apart >= 19.9, f"the hot spot moved only {apart:.1f} world units: {before} -> {after}"
        assert 29 * 30 <= int(after[8]) - int(before[8]) <= 32 * 30, f"the hot spot's time: {before} -> {after}"
    assert len(set(HEADS.findall(log))) >= 4, f"the bots did not head for the hot spots: {HEADS.findall(log)}"
    # every bank and loss worth what was landed since, none sooner than the
    # bank delay (two seconds) after its last landing; a bail loses a combo
    problems = _replay(log, 60)
    assert not problems, problems[:5]
    assert any(l[1] in ("bails", "comes off the board") for l in loses), f"no combo was lost to a bail: {loses}"


def _replay(log, bank_ticks):
    """each player's combos replayed from the log's landings, banks, losses
    and rams in order: every bank and loss must be worth what was landed
    since the last (times one for each trick, to ten), no bank sooner than
    bank_ticks after its last landing; the problems found"""
    events = []
    # (in the log's order, which is the host's)
    for line in log.splitlines():
        if found := LANDS.search(line):
            p, tricks, points, _, sequences, multiplier, worth, tick = found.groups()
            events.append((int(tick), int(p), ("land", int(tricks), int(points), int(sequences), int(multiplier),
                                               int(worth))))
        elif found := BANKS.search(line):
            p, sequences, tricks, multiplier, points, _, tick = found.groups()
            events.append((int(tick), int(p), ("bank", int(sequences), int(tricks), int(multiplier), int(points))))
        elif found := LOSES.search(line):
            p, why, sequences, tricks, worth, tick = found.groups()
            events.append((int(tick), int(p), ("lose", int(sequences), int(tricks), 0, int(worth))))
    problems = []
    combos = {}
    for tick, player, event in events:
        combo = combos.setdefault(player, {"points": 0, "sequences": 0, "tricks": 0, "last": None})
        if event[0] == "land":
            _, tricks, points, sequences, multiplier, worth = event
            combo["points"] += points
            combo["sequences"] += 1
            combo["tricks"] += tricks
            combo["last"] = tick
            expected = min(max(combo["tricks"], combo["sequences"]), 10)
            if sequences != combo["sequences"] or multiplier != expected or worth != combo["points"] * expected:
                problems.append(f"player {player} at {tick}: landing {event[1:]} but replayed {combo}")
        else:
            kind, sequences, tricks, multiplier, points = event
            expected = min(max(combo["tricks"], combo["sequences"]), 10)
            if sequences != combo["sequences"] or points != combo["points"] * expected:
                problems.append(f"player {player} at {tick}: {kind} {event[1:]} but replayed {combo}")
            if kind == "bank" and (combo["last"] is None or tick - combo["last"] < bank_ticks):
                problems.append(f"player {player} banked at {tick}, its last landing at {combo['last']}")
            combos[player] = {"points": 0, "sequences": 0, "tricks": 0, "last": None}
    return problems


def test_trick_attack_rams_steal(tmp_path):
    """Ramming on Prisoner: three peaceful bots chain tricks as hard bots do
    (debug.bot_tricks) with an eight-second bank delay, and two legendary
    bots that do no tricks of their own (but what their riding lands) hunt any
    combo of 300 or more (debug.trick_mark_points)
    and ride into its skater: a ram knocks the skater off their board (the
    host's verdict), loses them their combo and gives the rammer half of it
    (the variant's steal) at once; a skater who is never rammed keeps theirs.
    The scores are what was banked and taken. About three and a half
    minutes."""
    _, maps, assets = _needs_skate()
    log = _bots_alone(tmp_path, "server", _server(), maps, assets, {
        "HALO_SERVER_BOTS": "5", "HALO_BOT_DIFFICULTIES": "legendary,legendary,peaceful,peaceful,peaceful",
        "HALO_BOT_TRICKS": "off,off,hard,hard,hard", "HALO_TRICK_MARK_POINTS": "300",
        "HALO_SERVER_TRICK_BANK_DELAY": "80", "HALO_SERVER_TRICK_MINUTES": "3",
        "HALO_BOT_SEED": "72", "HALO_GAME_SEED": "72",
        "HALO_SERVER_MAPS": "prisoner:trick_attack", "HALO_SERVER_TIME_LIMIT": "5"},
        (_test_address(203), _test_address(204)), done=_over)
    rams, loses, banks, scores = RAMS.findall(log), LOSES.findall(log), BANKS.findall(log), _scores(log)
    print(f"variant {VARIANT.findall(log)[:1]}; hunts {HUNTS.findall(log)[:10]}\nrams {rams}\nlost {loses}\n"
          f"banks {len(banks)}; final {FINAL.findall(log)}")
    assert VARIANT.findall(log)[:1] == [("180", "240", "50", "2.0", "60", "on")], VARIANT.findall(log)
    assert HUNTS.findall(log), "no bot hunted a combo"
    assert rams, "nobody was rammed mid-combo"
    # (a hunter's ram; any skater knocking into a combo going takes too)
    assert any(r[0] in ("0", "1") and r[1] in ("2", "3", "4") for r in rams), f"no hunter rammed a combo: {rams}"
    for rammer, target, speed, taken, worth, tick in rams:
        assert int(taken) == int(worth) * 50 // 100, f"the take is not half the combo: {rams}"
        assert (target, "is rammed", worth, tick) in [(l[0], l[1], l[4], l[5]) for l in loses], \
            f"the rammed skater did not lose their combo then: {target} at {tick}: {loses}"
    for player, score in scores.items():
        banked = sum(int(b[4]) for b in banks if int(b[0]) == player)
        taken = sum(int(r[3]) for r in rams if int(r[0]) == player)
        assert score == banked + taken, f"player {player}'s score {score} is not banked {banked} + taken {taken}"
    assert all(scores[p] > 0 for p in (0, 1) if p in {int(r[0]) for r in rams}), scores
    problems = _replay(log, 240)
    assert not problems, problems[:5]


# (the gametype editor's spinners: menu_functions.c's options table)
EDIT = ("trick_minutes_spinner=2,trick_bank_delay_spinner=30,trick_steal_spinner=100,"
        "trick_hot_spot_multiplier_spinner=50,trick_hot_spot_seconds_spinner=45,trick_ramming_spinner=0")
HEARS_SPOT = re.compile(r"trick attack: this machine hears of hot spot (\d+) at \((\S+) (\S+) (\S+)\)")
HEARS_BANK = re.compile(r"trick attack: this machine hears player (\d+) banked (\d+) \(bank (\d+)\)")
CUT = re.compile(r"spectate: director cut to (.+?) \(player (-?\d+), (\w+)\) at tick (\d+) after ([\d.]+) s, (glide|hard cut)")
HUD = re.compile(r"trick attack hud: player (\d+) at tick (\d+): combo '([^']*)' live '([^']*)' hot '([^']*)' leaders "
                 r"'([^']*)' callout '([^']*)' marks '([^']*)'")


def test_trick_attack_a_client_scores_and_sees(tmp_path):
    """A listen host on Blood Gulch (its own player, three hard bots) whose
    Trick Attack gametype is edited through the gametype editor's own
    spinners (debug.gametype_edit: menu_functions.c's options table, as a
    person sets the Trick Attack Options screen) - a two-minute round, a
    three-second bank delay, a 100% steal, a x5.0 hot spot for 45 seconds,
    ramming off - and a client whose skater rides a scripted pad (pushes,
    carves and an ollie every four seconds: debug.network_test_skate). Both
    machines play by the edited values (the client has them from the host's
    game); the client's skater's tricks reach the host in its reports and
    the host scores them as anyone's (each ollie banked before the next);
    the client hears each hot spot and every bank
    from the host, and its HUD (debug.trick_hud_log) shows the leaders with
    the host's scores, the hot spot, its own combo while one goes, the marks
    over combos of 300 or more (debug.trick_mark_points) and a callout for
    one banked; a spectator's director (spectate_director.c) cuts to a
    combo going for its reason. The round ends after two minutes. About
    three minutes."""
    game, maps, assets = _needs_skate()
    host_address, join_address = _test_address(205), _test_address(206)
    common = {"HALO_SKATE_ASSETS": assets, "HALO_NULL_RENDERER": "1", "HALO_NO_AUDIO": "1", "HALO_TRICK_HUD_LOG": "1",
              "HALO_TRICK_MARK_POINTS": "300"}
    games = {"host": _start(tmp_path, "host", game, maps, {
        **common, "HALO_NETWORK_TEST": "host:bloodgulch:trick_attack", "HALO_NETWORK_TEST_START": "8",
        "HALO_SERVER_BOTS": "3", "HALO_BOT_DIFFICULTIES": "hard", "HALO_BOT_SEED": "73", "HALO_GAME_SEED": "73",
        "HALO_GAMETYPE_EDIT": EDIT, "HALO_NET_ADDRESS": host_address,
        "HALO_NET_BROADCAST": join_address})}
    games["join"] = _start(tmp_path, "join", game, maps, {
        **common, "HALO_NETWORK_TEST": "join", "HALO_NETWORK_TEST_SKATE": "3", "HALO_NET_ADDRESS": join_address,
        "HALO_NET_BROADCAST": host_address})
    games["watch"] = _start(tmp_path, "watch", game, maps, {
        **common, "HALO_NETWORK_TEST": "join", "HALO_SPECTATE": "1", "HALO_SPECTATE_CAMERA": "director",
        "HALO_NET_ADDRESS": _test_address(207), "HALO_NET_BROADCAST": host_address})
    try:
        _wait(lambda: OVER.search(_log(tmp_path, "host")), 300, games, tmp_path, "the round did not end")
    finally:
        _stop(games, tmp_path, {"host": game, "join": game, "watch": game})
        for cache in tmp_path.rglob("cache*.map"):
            cache.unlink()
    host, join, watch = _log(tmp_path, "host"), _log(tmp_path, "join"), _log(tmp_path, "watch")
    huds = HUD.findall(join)
    cuts = CUT.findall(watch)
    print(f"the spectator's director: {cuts}")
    assert any(c[2] == "combo" for c in cuts), f"the director never cut to a combo going: {cuts}"
    client = huds[0][0] if huds else None
    scores = _scores(host)
    print(f"client player {client}; host final {FINAL.findall(host)}")
    print(f"client lands {[l for l in LANDS.findall(host) if l[0] == client][:8]}")
    print(f"client banks {[b for b in BANKS.findall(host) if b[0] == client][:8]}")
    print(f"client heard spots {HEARS_SPOT.findall(join)[:4]}, banks {HEARS_BANK.findall(join)[:6]}")
    print("client HUD:\n" + "\n".join(str(h[1:]) for h in huds[:40]))
    expected = [("120", "90", "100", "5.0", "45", "off")]
    assert VARIANT.findall(host)[:1] == expected, f"the host's variant: {VARIANT.findall(host)}"
    assert VARIANT.findall(join)[:1] == expected, f"the client did not have the edited values: {VARIANT.findall(join)}"
    over = OVER.search(host)
    assert 115 * 30 <= int(over.group(1)) <= 125 * 30, f"the round did not last two minutes: {over.groups()}"
    assert not RAMS.findall(host), f"a ram with ramming off: {RAMS.findall(host)}"
    assert client is not None, "the client logged no HUD"
    # the client's tricks scored by the host
    assert [l for l in LANDS.findall(host) if l[0] == client], "the host scored none of the client's tricks"
    banked = sum(int(b[4]) for b in BANKS.findall(host) if b[0] == client)
    taken = sum(int(r[3]) for r in RAMS.findall(host) if r[0] == client)
    assert banked > 0 and scores[int(client)] == banked + taken, f"the client's score: {scores}, banked {banked}"
    # the client hears the host: hot spots where the host has them, banks
    spots = {h[0]: h[1:4] for h in HOT.findall(host)}
    assert HEARS_SPOT.findall(join) and all(spots.get(s[0]) == s[1:] for s in HEARS_SPOT.findall(join)), \
        f"the client's hot spots: {HEARS_SPOT.findall(join)} against {spots}"
    assert len(HEARS_BANK.findall(join)) >= 3, f"the client heard few banks: {HEARS_BANK.findall(join)}"
    # its HUD: the leaders (its own place among them), the hot spot, its combo, marks, a callout
    assert all(h[5].startswith("TRICK ATTACK / 1. ") for h in huds[2:]), [h[5] for h in huds]
    assert all(h[4].startswith("HOT SPOT x5.0 ") for h in huds[2:]), [h[4] for h in huds]
    assert len(HEARS_SPOT.findall(join)) >= 2, f"the client did not hear the hot spot move: {HEARS_SPOT.findall(join)}"
    assert any(re.fullmatch(r"COMBO [\d,]+  x\d+", h[2]) for h in huds), [h[2] for h in huds]
    assert any(h[7] for h in huds), "no mark over a big combo"
    assert any(re.fullmatch(r".+ landed a [\d,]+ combo", h[6]) for h in huds), [h[6] for h in huds]

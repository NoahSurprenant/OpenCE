//! Tricks against the real Skate 3 data (`common/mod.rs`): the right stick's
//! gestures read from the data (`halo_skate_gesture`), flicked through the pad
//! as a person (or the game's bots, port/linux/game/bots_tricks.c) flicks
//! them, and what the engine's own scoring made of them (`halo_skate_tricks`).
//! Skipped unless `HALO_SKATE_TEST_ASSETS` is set; run with `--release`.

mod common;

use common::*;
use halo_skate::*;
use std::ffi::{CStr, CString};

/// The gesture the engine knows `name` by, as pad positions (+Y up).
fn gesture(assets: &std::path::Path, name: &str) -> Vec<[f32; 2]> {
    let root = CString::new(assets.to_string_lossy().into_owned()).unwrap();
    let name = CString::new(name).unwrap();
    let mut points = [0.0f32; 32];
    let count = unsafe { halo_skate_gesture(root.as_ptr(), name.as_ptr(), points.as_mut_ptr(), 16) };
    assert!(count >= 0, "halo_skate_gesture failed");
    (0..count as usize).map(|i| [points[2 * i], points[2 * i + 1]]).collect()
}

fn stick(v: f32) -> i16 {
    (v.clamp(-1.0, 1.0) * FULL as f32) as i16
}

/// Flicks the gesture: its first point held `wind` game ticks (the crouch
/// before the pop), each other point one tick, then the stick let go.
fn flick(engine: &mut Engine, points: &[[f32; 2]], wind: usize, steer: Pad) {
    assert!(!points.is_empty(), "no such gesture");
    for (i, [x, y]) in points.iter().enumerate() {
        engine.hold(Pad { right: [stick(*x), stick(*y)], ..steer }, if i == 0 { wind } else { 1 });
    }
    engine.step(steer);
}

fn tricks() -> HaloSkateTricks {
    let mut out = HaloSkateTricks::default();
    assert_eq!(unsafe { halo_skate_tricks(&mut out) }, 0);
    out
}

fn name(t: &HaloSkateTricks) -> String {
    unsafe { CStr::from_ptr(t.last_trick.as_ptr()) }.to_string_lossy().into_owned()
}

fn show(what: &str, t: &HaloSkateTricks) {
    eprintln!(
        "{what}: {} tricks ({} landed, last {:?}), {} sequences, {} combos, {} bails, {:.0} points (best {:.0}, {} tricks)",
        t.tricks,
        t.landed,
        name(t),
        t.sequences,
        t.combos,
        t.bails,
        t.points,
        t.best_points,
        t.best_tricks
    );
}

/// Rolling at a good speed down the open lane from x.
fn roll(engine: &mut Engine, lane: f32, x: f32) {
    engine.activate([x, lane, 0.0], 0.0);
    for _ in 0..3 {
        push(engine, 15);
    }
}

/// 1. Flatground flips: an ollie, a kickflip, a heelflip, a pop shove-it and
/// a 360 flip flicked one after another while rolling down the open lane;
/// each is a trick the scoring names, landed and scored.
#[test]
fn flicked_gestures_are_flip_tricks() {
    let Some(assets) = assets("tricks_flips") else { return };
    let mut engine = Engine::start(&assets);
    let names = ["Ollie", "Kickflip", "Heelflip", "PopShuvit", "360Flip", "Nollie", "Hardflip", "VarialKickflip"];
    for n in names {
        eprintln!("gesture {n}: {:?}", gesture(&assets, n));
    }
    assert!(gesture(&assets, "NoSuchTrick").is_empty());
    roll(&mut engine, TestMap::OPEN, -40.0);
    // (the counts go on from the tests that had the engine before this one,
    // whichever ran first: this test's are what it adds to them)
    let start = tricks();
    let mut named = Vec::new();
    for n in names {
        let before = tricks();
        flick(&mut engine, &gesture(&assets, n), 8, Pad::NEUTRAL);
        engine.hold(Pad::NEUTRAL, 45);
        let after = tricks();
        show(n, &after);
        named.push((n, name(&after), after.tricks - before.tricks, after.landed - before.landed));
        push(&mut engine, 10);
    }
    eprintln!("flicked: {named:?}");
    let t = tricks();
    assert!(t.landed - start.landed >= 5, "most of the flicks were tricks landed: {named:?}");
    assert!(t.points > start.points && t.sequences - start.sequences >= 5, "they scored");
    assert_eq!(t.bails - start.bails, 0, "none bailed");
}

/// Rolls down the funbox lane at speed to just before `x` (metres).
fn to_funbox(engine: &mut Engine, x: f32) {
    roll(engine, TestMap::FUNBOX, -10.0);
    while engine.history.last().unwrap().at()[0] < x {
        engine.step(Pad::NEUTRAL);
        assert!(engine.history.len() < 900, "never reached x = {x}");
    }
}

fn states(frames: &[Snapshot]) -> Vec<String> {
    let mut seen: Vec<String> = Vec::new();
    for s in frames {
        if seen.last() != Some(&s.state) {
            seen.push(s.state.clone());
        }
    }
    seen
}

/// Off the end of the funbox's top (a metre's drop) with an ollie, into the
/// air (about two thirds of a second of it).
fn off_the_funbox(engine: &mut Engine, ollie: &[[f32; 2]]) -> usize {
    to_funbox(engine, 22.3);
    let from = engine.history.len();
    flick(engine, ollie, 6, Pad::NEUTRAL);
    let mut ticks = 0;
    while !engine.history.last().unwrap().airborne() {
        engine.step(Pad::NEUTRAL);
        ticks += 1;
        assert!(ticks < 20, "the ollie never left the ground: {:?}", states(engine.since(from)));
    }
    from
}

/// The game ticks from `from` until the board lands (or `limit`), holding
/// `pad`.
fn until_landed(engine: &mut Engine, pad: Pad, limit: usize) -> usize {
    let mut ticks = 0;
    while engine.history.last().unwrap().airborne() && ticks < limit {
        engine.step(pad);
        ticks += 1;
    }
    ticks
}

/// 2. In the air off the funbox: a grab (a trigger held to the landing) and a
/// late flip flicked as the board leaves the ground each make a combo with
/// the ollie; a late flip flicked halfway through the air is a bail, which
/// scores nothing (what an easy bot gets wrong now and then).
#[test]
fn grabs_and_late_flips_in_the_air() {
    let Some(assets) = assets("tricks_air") else { return };
    let mut engine = Engine::start(&assets);
    let ollie = gesture(&assets, "Ollie");
    let late = gesture(&assets, "L_F_Kickflip");
    assert!(!late.is_empty(), "no late flip in skater_air.pat");
    let before = tricks();
    let from = off_the_funbox(&mut engine, &ollie);
    let air = until_landed(&mut engine, Pad { triggers: [255, 0], ..Pad::NEUTRAL }, 60);
    engine.hold(Pad::NEUTRAL, 45);
    let grab = tricks();
    show("grab", &grab);
    eprintln!("  {air} ticks in the air, states {:?}", states(engine.since(from)));
    assert!(name(&grab).contains("GRAB"), "the last trick was a grab: {}", name(&grab));
    assert_eq!(grab.combos - before.combos, 1, "the ollie and the grab, one combo");
    assert_eq!(grab.bails, before.bails, "no bail");

    let from = off_the_funbox(&mut engine, &ollie);
    flick(&mut engine, &late, 1, Pad::NEUTRAL);
    until_landed(&mut engine, Pad::NEUTRAL, 60);
    engine.hold(Pad::NEUTRAL, 45);
    let flip = tricks();
    show("late flip", &flip);
    eprintln!("  states {:?}", states(engine.since(from)));
    assert!(name(&flip).contains("LATE_KICKFLIP"), "a late kickflip: {}", name(&flip));
    assert_eq!(flip.combos - grab.combos, 1, "the ollie and the late flip, one combo");
    assert_eq!(flip.bails, grab.bails, "no bail");

    let from = off_the_funbox(&mut engine, &ollie);
    engine.hold(Pad::NEUTRAL, 10);
    flick(&mut engine, &late, 1, Pad::NEUTRAL);
    engine.hold(Pad::NEUTRAL, 60);
    let sloppy = tricks();
    show("late flip too late", &sloppy);
    eprintln!("  states {:?}", states(engine.since(from)));
    assert_eq!(sloppy.bails - flip.bails, 1, "a bail: {:?}", states(engine.since(from)));
    assert_eq!(sloppy.points, flip.points, "a bailed sequence scores nothing");
}

/// 3. Manuals: the right stick held part of the way down while rolling is a
/// manual, part of the way up a nose manual.
#[test]
fn the_stick_part_way_is_a_manual() {
    let Some(assets) = assets("tricks_manual") else { return };
    let mut engine = Engine::start(&assets);
    for (y, expected) in [(-0.5f32, "MANUAL"), (0.5, "NOSE_MANU")] {
        roll(&mut engine, TestMap::OPEN, -40.0);
        let before = tricks();
        engine.hold(Pad::right(0, stick(y)), 30);
        // (rolling on until the sequence settles)
        let mut ticks = 0;
        while tricks().landed == before.landed && ticks < 120 {
            engine.step(Pad::NEUTRAL);
            ticks += 1;
        }
        let after = tricks();
        show(expected, &after);
        assert_eq!(after.landed - before.landed, 1, "one trick landed");
        assert!(name(&after).ends_with(expected), "{} is a {expected}", name(&after));
    }
}

/// 4. Onto the rail with an ollie, along it, and a kickflip off it near its
/// end: the ollie, the grind and the flip are one sequence (a combo of three
/// or more tricks), landed.
#[test]
fn a_grind_and_a_flip_off_it_are_a_combo() {
    let Some(assets) = assets("tricks_grind") else { return };
    let mut engine = Engine::start(&assets);
    let ollie = gesture(&assets, "Ollie");
    let before = tricks();
    engine.activate([0.0, TestMap::RAIL, 0.0], 0.0);
    push(&mut engine, 15);
    while engine.history.last().unwrap().at()[0] < TestMap::RAIL_START - 5.0 {
        engine.step(Pad::NEUTRAL);
        assert!(engine.history.len() < 600, "never reached the rail");
    }
    let from = engine.history.len();
    flick(&mut engine, &ollie, 8, Pad::NEUTRAL);
    let mut ticks = 0;
    while !engine.history.last().unwrap().grinding() {
        engine.step(Pad::NEUTRAL);
        ticks += 1;
        assert!(ticks < 60, "never grinded: {:?}", states(engine.since(from)));
    }
    while engine.history.last().unwrap().grinding() && engine.history.last().unwrap().at()[0] < TestMap::RAIL_END - 3.0 {
        engine.step(Pad::NEUTRAL);
        ticks += 1;
        assert!(ticks < 400, "the grind never neared the rail's end");
    }
    flick(&mut engine, &gesture(&assets, "Kickflip"), 4, Pad::NEUTRAL);
    engine.hold(Pad::NEUTRAL, 60);
    let after = tricks();
    show("grind and flip", &after);
    eprintln!("  states {:?}", states(engine.since(from)));
    assert!(after.best_tricks >= 3, "ollie, grind and flip in one sequence: best {}", after.best_tricks);
    assert!(after.combos > before.combos && after.bails == before.bails, "a combo landed");
    // (another session's tally: none made, nothing; no such session, -1)
    let mut other = HaloSkateTricks::default();
    assert_eq!(unsafe { halo_skate_tricks_in(7, &mut other) }, 0);
    assert_eq!(other.tricks, 0);
    assert_eq!(unsafe { halo_skate_tricks_in(100000, &mut other) }, -1);
}

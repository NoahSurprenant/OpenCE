//! The skater's sound events against the real Skate 3 data (`common/mod.rs`):
//! each one-shot is told of in the log (`halo-skate: sound: <name> ...`, on
//! by default), so the tests read the engine's log. Skipped unless
//! `HALO_SKATE_TEST_ASSETS` is set; run with `--release`.

mod common;

use common::*;
// (mixer.rs is not a public module: its C functions, by their symbols)
unsafe extern "C" {
    fn halo_skate_sound_load(assets: *const std::ffi::c_char) -> i32;
    fn halo_skate_sound_set_log(enabled: i32);
}

/// The one-shot sounds the log told of, in order.
fn sounds(log: &[String]) -> Vec<String> {
    log.iter()
        .filter_map(|l| l.strip_prefix("halo-skate: sound: "))
        .filter(|l| l.contains(" m/s)"))
        .map(|l| l.split(' ').next().unwrap_or_default().to_string())
        .collect()
}

fn position(events: &[String], name: &str) -> Option<usize> {
    events.iter().position(|e| e == name)
}

fn start(assets: &std::path::Path) -> Engine {
    let engine = Engine::start(assets);
    // the built-in sound set, as the game loads it with no sounds of its own
    let loaded = unsafe { halo_skate_sound_load(std::ptr::null()) };
    assert!(loaded > 0, "the built-in sound set: {loaded} sounds");
    unsafe { halo_skate_sound_set_log(1) };
    engine
}

/// 7a. Getting on makes no sound, and pushing off along the flat makes no
/// one-shot (the roll is a loop, not logged).
#[test]
fn getting_on_and_pushing_make_no_one_shots() {
    let Some(assets) = assets("sound_push") else { return };
    let mut engine = start(&assets);
    take_log();
    engine.activate([0.0, TestMap::OPEN, 0.0], 0.0);
    engine.hold(Pad::NEUTRAL, 15);
    let on = sounds(&take_log());
    for _ in 0..3 {
        push(&mut engine, 15);
    }
    engine.hold(Pad::NEUTRAL, 60);
    let pushing = sounds(&take_log());
    eprintln!("sound: getting on {on:?}, pushing {pushing:?}");
    assert!(on.is_empty(), "getting on is silent: {on:?}");
    assert!(pushing.is_empty(), "pushing along the flat: {pushing:?}");
}

/// 7b. An ollie: a pop, then a landing, once each.
#[test]
fn an_ollie_pops_then_lands() {
    let Some(assets) = assets("sound_ollie") else { return };
    let mut engine = start(&assets);
    engine.activate([0.0, TestMap::OPEN, 0.0], 0.0);
    push(&mut engine, 15);
    push(&mut engine, 15);
    take_log();
    ollie(&mut engine, Pad::NEUTRAL);
    engine.hold(Pad::NEUTRAL, 60);
    let events = sounds(&take_log());
    eprintln!("sound: ollie {events:?}");
    assert_eq!(events, ["pop", "land"], "an ollie on the flat");
}

/// 7c. Onto the rail: a pop, the grind starting, and at its end the grind
/// ending, then a landing.
#[test]
fn a_grind_starts_and_ends() {
    let Some(assets) = assets("sound_grind") else { return };
    let mut engine = start(&assets);
    engine.activate([0.0, TestMap::RAIL, 0.0], 0.0);
    push(&mut engine, 15);
    while engine.history.last().unwrap().at()[0] < TestMap::RAIL_START - 5.0 {
        engine.step(Pad::NEUTRAL);
        assert!(engine.history.len() < 600, "never reached the rail");
    }
    take_log();
    let from = engine.history.len();
    ollie(&mut engine, Pad::NEUTRAL);
    // along the rail to its end and off
    engine.hold(Pad::NEUTRAL, 240);
    let events = sounds(&take_log());
    let grinds: Vec<&Snapshot> = engine.since(from).iter().filter(|s| s.grinding()).collect();
    let states: Vec<&str> = {
        let mut seen: Vec<&str> = Vec::new();
        for s in engine.since(from) {
            if seen.last() != Some(&s.state.as_str()) {
                seen.push(&s.state);
            }
        }
        seen
    };
    eprintln!("sound: grind {events:?}, states {states:?}");
    assert!(!grinds.is_empty(), "the ollie landed on the rail and ground it: {states:?}");
    let start = position(&events, "grind_start").or(position(&events, "slide_start"));
    let end = position(&events, "grind_end").or(position(&events, "slide_end"));
    let pop = position(&events, "pop");
    assert!(pop.is_some(), "the ollie onto the rail popped: {events:?}");
    let start = start.unwrap_or_else(|| panic!("no grind start: {events:?}"));
    let end = end.unwrap_or_else(|| panic!("no grind end: {events:?}"));
    assert!(pop.unwrap() < start && start < end, "pop, then the grind starts, then ends: {events:?}");
    assert!(!events[pop.unwrap()..end].contains(&"land".to_string()), "no landing onto the rail: {events:?}");
    let starts = events.iter().filter(|e| e.ends_with("_start")).count();
    let ends = events.iter().filter(|e| e.ends_with("_end")).count();
    assert_eq!(starts, ends, "each grind that starts ends: {events:?}");
}

/// 7d. A bail into the box: the bail sounds once, after the pop; it is not a
/// step off, and being put back after it is not a landing.
#[test]
fn a_bail_sounds_once() {
    let Some(assets) = assets("sound_bail") else { return };
    let mut engine = start(&assets);
    engine.activate([0.0, TestMap::LEDGE, 0.0], 0.0);
    push(&mut engine, 12);
    push(&mut engine, 12);
    while engine.history.last().unwrap().at()[0] < TestMap::LEDGE_START - 6.0 {
        engine.step(Pad::NEUTRAL);
        assert!(engine.history.len() < 600, "never reached the box");
    }
    take_log();
    let from = engine.history.len();
    ollie(&mut engine, Pad::NEUTRAL);
    engine.hold(Pad::NEUTRAL, 240);
    let events = sounds(&take_log());
    let wiped = engine.since(from).iter().any(|s| s.state == "WipeoutGround");
    let mut states: Vec<(u32, &str)> = Vec::new();
    for s in engine.since(from) {
        if states.last().map(|x| x.1) != Some(s.state.as_str()) {
            states.push((s.tick, &s.state));
        }
    }
    eprintln!("sound: bail {events:?}, states {states:?}");
    assert!(wiped, "the skater bailed");
    let bails = events.iter().filter(|e| *e == "bail").count();
    assert_eq!(bails, 1, "one bail: {events:?}");
    let pop = position(&events, "pop").expect("the ollie popped");
    assert!(pop < position(&events, "bail").unwrap(), "pop, then the bail: {events:?}");
    assert!(!events.contains(&"step_off".to_string()), "a bail is not stepping off the board: {events:?}");
    let bail = position(&events, "bail").unwrap();
    assert!(!events[bail..].contains(&"land".to_string()), "being put back is not a landing: {events:?}");
}

/// 7e. Getting off with Y: a step off, once, and no bail.
#[test]
fn getting_off_with_y_steps_off() {
    let Some(assets) = assets("sound_step_off") else { return };
    let mut engine = start(&assets);
    engine.activate([0.0, TestMap::OPEN, 0.0], 0.0);
    engine.hold(Pad::NEUTRAL, 15);
    take_log();
    let from = engine.history.len();
    engine.hold(Pad::buttons(Y), 4);
    engine.hold(Pad::NEUTRAL, 90);
    let events = sounds(&take_log());
    let off = engine.since(from).iter().any(|s| s.off_board());
    eprintln!("sound: getting off {events:?}");
    assert!(off, "Y took the skater off the board");
    assert_eq!(events, ["step_off"], "getting off with Y");
}

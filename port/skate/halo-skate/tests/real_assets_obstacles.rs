//! Obstacles (`halo_skate_set_obstacles`, `obstacles.rs`) against the real
//! Skate 3 data (`common/mod.rs`): riding into a standing person (a capsule
//! Master Chief's size) and a vehicle (a box a Warthog's size) on the test
//! map's open floor, at a few speeds. Slow is a bump (the board stops at it,
//! the skater stays on), fast is a bail; nothing passes through, every frame
//! is finite, and after the bail the engine puts the skater back on the
//! board. Skipped unless `HALO_SKATE_TEST_ASSETS` is set; run with
//! `--release`.

mod common;

use common::*;
use halo_skate::obstacles::{KIND_BIPED, KIND_VEHICLE, SHAPE_BOX, SHAPE_CAPSULE};
use halo_skate::*;

/// The cyborg's collision pill (world units): 0.2 across, 0.7 tall.
const PERSON_RADIUS: f32 = 0.2;
const PERSON_HEIGHT: f32 = 0.7;
/// A Warthog-sized box (world units, half sizes): 1.6 long, 0.9 wide,
/// 0.7 high; standing across the lane.
const VEHICLE_HALF: [f32; 3] = [0.8, 0.45, 0.35];

/// A person standing at `x`, `y` (metres) on the floor.
fn person(x: f32, y: f32) -> HaloSkateObstacle {
    HaloSkateObstacle {
        kind: KIND_BIPED,
        shape: SHAPE_CAPSULE,
        center: [x / METRES, y / METRES, PERSON_HEIGHT / 2.0],
        forward: [1.0, 0.0, 0.0],
        left: [0.0, 1.0, 0.0],
        up: [0.0, 0.0, 1.0],
        half_extents: [PERSON_RADIUS, 0.0, PERSON_HEIGHT / 2.0],
        velocity: [0.0; 3],
    }
}

/// A vehicle parked across the lane, its middle at `x`, `y` (metres).
fn vehicle(x: f32, y: f32) -> HaloSkateObstacle {
    HaloSkateObstacle {
        kind: KIND_VEHICLE,
        shape: SHAPE_BOX,
        center: [x / METRES, y / METRES, VEHICLE_HALF[2]],
        // its forward across the lane (+Y), its left along it (-X)
        forward: [0.0, 1.0, 0.0],
        left: [-1.0, 0.0, 0.0],
        up: [0.0, 0.0, 1.0],
        half_extents: VEHICLE_HALF,
        velocity: [0.0; 3],
    }
}

fn set(obstacles: &[HaloSkateObstacle]) -> i32 {
    unsafe { halo_skate_set_obstacles(obstacles.as_ptr(), obstacles.len() as i32) }
}

/// What a ride into an obstacle came to.
#[derive(Debug)]
struct Ride {
    /// the board's speed along the ground as it came within 1.5 m, m/s
    speed: f32,
    /// a bail (the engine's wipeout, or a failed step put back on the board)
    bailed: bool,
    /// furthest the board got past the obstacle's middle before any bail, m
    past: f32,
    /// the board's speed along the ground just after, m/s
    after: f32,
    log: Vec<String>,
}

/// Rides along the open lane at `lane` (metres across), `pushes` pushes of
/// `push_ticks` game ticks, then rolls on into the obstacle `make(x, y)`
/// puts 3 m ahead of the board, the way it travels (`ahead` m further for
/// one driving at the board at `toward` m/s), sent every tick as the game
/// sends it, for four seconds.
fn ride_into(
    engine: &mut Engine,
    lane: f32,
    pushes: usize,
    push_ticks: usize,
    make: fn(f32, f32) -> HaloSkateObstacle,
    toward: f32,
) -> Ride {
    assert_eq!(set(&[]), 0);
    engine.activate([0.0, lane, 0.0], 0.0);
    take_log();
    for _ in 0..pushes {
        push(engine, push_ticks);
    }
    // (a push takes a moment to roll the board)
    for _ in 0..60 {
        if engine.history.last().unwrap().ground_speed() > 1.0 {
            break;
        }
        engine.step(Pad::NEUTRAL);
    }
    let start = engine.history.len();
    let last = engine.history.last().unwrap().clone();
    let at = last.at();
    let v = last.velocity;
    let along = (v[0] * v[0] + v[1] * v[1]).sqrt().max(1e-6);
    let direction = [v[0] / along, v[1] / along];
    let ahead = 3.0 + 1.5 * toward.max(0.0);
    let mut obstacle = make(at[0] + direction[0] * ahead, at[1] + direction[1] * ahead);
    obstacle.velocity = [-direction[0] * toward / METRES, -direction[1] * toward / METRES, 0.0];
    let mut met = None;
    for _ in 0..120 {
        assert_eq!(set(&[obstacle]), 1, "the obstacle is taken");
        let s = engine.step(Pad::NEUTRAL);
        for i in 0..3 {
            obstacle.center[i] += obstacle.velocity[i] * GAME_TICK;
        }
        let gap = (obstacle.center[0] * METRES - s.at()[0]) * direction[0]
            + (obstacle.center[1] * METRES - s.at()[1]) * direction[1];
        if met.is_none() && gap < 1.5 {
            met = Some(engine.history.len() - 1);
        }
    }
    assert_eq!(set(&[]), 0);
    let log = take_log();
    let frames = engine.since(start);
    engine.trace(start, 6);
    let met = met.unwrap_or(engine.history.len() - 1);
    let speed = engine.history[met.saturating_sub(1)].ground_speed();
    let after = engine.history[(met + 30).min(engine.history.len() - 1)].ground_speed();
    let bailed = frames.iter().any(|s| s.state.starts_with("Wipeout") || s.code == 2);
    // how far past the obstacle's middle the board got before any bail
    let end = obstacle.center.map(|v| v * METRES);
    let past = frames
        .iter()
        .take_while(|s| !s.state.starts_with("Wipeout"))
        .map(|s| (s.at()[0] - end[0]) * direction[0] + (s.at()[1] - end[1]) * direction[1])
        .fold(f32::MIN, f32::max);
    assert!(frames.iter().all(Snapshot::is_finite), "every frame is finite");
    Ride { speed, bailed, past, after, log }
}

/// After a bail the engine resets the skater onto the board: within a few
/// seconds it stands on it, not wiping out.
fn assert_back_on_the_board(engine: &mut Engine, what: &str) {
    engine.hold(Pad::NEUTRAL, 150);
    let last = engine.history.last().unwrap().clone();
    assert!(
        !last.off_board() && !last.state.starts_with("Wipeout"),
        "{what}: back on the board after the bail: {}",
        last.state
    );
    assert!(engine.history.iter().all(Snapshot::is_finite), "{what}: finite");
}

/// The closing speed (m/s) of the first hit on a `kind` the log tells of
/// (`obstacles.rs`: "hit a biped at 6.3 m/s (...)"), or None.
fn hit_at(log: &[String], kind: &str) -> Option<f32> {
    let prefix = format!("hit a {kind} at ");
    log.iter().find_map(|l| {
        let rest = &l[l.find(&prefix)? + prefix.len()..];
        rest.split(' ').next()?.parse().ok()
    })
}

fn report(what: &str, ride: &Ride, kind: &str) -> f32 {
    let closing = hit_at(&ride.log, kind).unwrap_or_else(|| panic!("{what}: the hit on the {kind} is logged"));
    eprintln!(
        "{what}: riding {:.2} m/s, closing {closing:.2} m/s: {}, {:.2} m past its middle, {:.2} m/s after",
        ride.speed,
        if ride.bailed { "a bail" } else { "a bump" },
        ride.past,
        ride.after
    );
    closing
}

/// The skater catches up with a person walking away at 3 m/s (closing under
/// 2 m/s): a bump, the board stopped behind them. Riding straight into one
/// standing, at full speed (8.5 m/s): a bail, then back on the board.
#[test]
fn riding_into_a_person_bumps_slow_and_bails_fast() {
    let Some(assets) = assets("riding into a person") else { return };
    let mut engine = Engine::start(&assets);
    let slow = ride_into(&mut engine, TestMap::OPEN, 1, 3, person, -3.0);
    let slow_closing = report("person, slow", &slow, "biped");
    let fast = ride_into(&mut engine, TestMap::OPEN, 4, 12, person, 0.0);
    let fast_closing = report("person, fast", &fast, "biped");
    assert!(slow_closing < 3.0, "slow: closing slowly: {slow_closing:.2} m/s");
    assert!(!slow.bailed, "slow ({slow_closing:.2} m/s): a bump, not a bail");
    assert!(slow.past < 0.0, "slow: the board stayed behind the person: {:.2} m past", slow.past);
    assert!(fast_closing > 6.0, "fast: at speed: {fast_closing:.2} m/s");
    assert!(fast.bailed, "fast ({fast_closing:.2} m/s): a bail");
    assert!(fast.past < 0.0, "fast: not through the person: {:.2} m past", fast.past);
    assert_back_on_the_board(&mut engine, "person, fast");
}

/// Riding into a parked vehicle after one push (4.8 m/s): a bump, the board
/// stopped at its side. At full speed: a bail, not through it, then back on
/// the board.
#[test]
fn riding_into_a_vehicle_bumps_slow_and_bails_fast() {
    let Some(assets) = assets("riding into a vehicle") else { return };
    let mut engine = Engine::start(&assets);
    let slow = ride_into(&mut engine, TestMap::OPEN, 1, 3, vehicle, 0.0);
    let slow_closing = report("vehicle, slow", &slow, "vehicle");
    let fast = ride_into(&mut engine, TestMap::OPEN, 4, 12, vehicle, 0.0);
    let fast_closing = report("vehicle, fast", &fast, "vehicle");
    assert!(slow_closing < 6.0, "slow: under the bail speed: {slow_closing:.2} m/s");
    assert!(!slow.bailed, "slow ({slow_closing:.2} m/s): a bump, not a bail");
    assert!(slow.past < -VEHICLE_HALF[1] * METRES, "slow: the board stopped at its side: {:.2} m past", slow.past);
    assert!(slow.after < 1.0, "slow: stopped: {:.2} m/s", slow.after);
    assert!(fast_closing > 6.0, "fast: at speed: {fast_closing:.2} m/s");
    assert!(fast.bailed, "fast ({fast_closing:.2} m/s): a bail");
    assert!(fast.past < 0.0, "fast: not through the vehicle: {:.2} m past its middle", fast.past);
    assert_back_on_the_board(&mut engine, "vehicle, fast");
}

/// The obstacle's velocity counts: rolling as slowly as the parked vehicle's
/// bump above, a vehicle driving into the skater at 5 m/s knocks them over.
#[test]
fn a_vehicle_driving_into_a_slow_skater_knocks_them_over() {
    let Some(assets) = assets("a vehicle driving into the skater") else { return };
    let mut engine = Engine::start(&assets);
    let hit = ride_into(&mut engine, TestMap::OPEN, 1, 3, vehicle, 5.0);
    let closing = report("vehicle driving at 5 m/s", &hit, "vehicle");
    assert!(closing > 8.0, "the vehicle's speed and the skater's: {closing:.2} m/s");
    assert!(hit.bailed, "a bail");
    assert_back_on_the_board(&mut engine, "vehicle driving");
}

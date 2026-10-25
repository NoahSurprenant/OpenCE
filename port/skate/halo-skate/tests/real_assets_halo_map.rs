//! Skate 3 mode on a real Halo level: Hang 'Em High's collision, read from
//! the owner's own Xbox map (`common/halo_map.rs`) as the game sends it, with
//! the real converted Skate 3 data, through the C interface the way the game
//! drives it. Build 28's `Nonfinite BipedAir launch packet` came on this map:
//! a jump from walking at (8.118, -11.901, -7.950) world units.
//!
//! Skipped unless both `HALO_SKATE_TEST_HALO_MAPS` names the folder of Halo
//! Xbox maps (`hangemhigh.map`) and `HALO_SKATE_TEST_ASSETS` the converted
//! `assets` folder (the collision test needs only the maps); run with
//! `--release`. `HALO_SKATE_TEST_SWEEP` sets how many spots the sweeps try
//! (8 by default, about 75 seconds in all; 0 for every spot found).

mod common;

use common::halo_map::{HaloMap, halo_maps};
use common::*;
use halo_skate::halo_skate_state;
use std::path::PathBuf;

const MAP: &str = "hangemhigh";
/// What the game logs for Hang 'Em High: the triangles it sends, those left
/// after deduplication, and the rails found in them.
const SENT: &str = "map 1 sent, 2874 triangles";
const DEDUPLICATED: &str = "2874 triangles deduplicated";
const RAILS: &str = "rails: 2096 walkable edges, 525 lips, 494 runs, 470 rails";
/// Build 28's failure: where the skater stood, world units, the way it faced
/// (the launch packet's forward, [0.973, 0, -0.229] in Skate's axes) and the
/// pad (X with the left stick at 23652, 30746).
const BUILD_28_SPOT: [f32; 3] = [8.118, -11.901, -7.950];
const BUILD_28_STICK: [i16; 2] = [23652, 30746];
const SPOTS: usize = 8;

fn build_28_yaw() -> f32 {
    0.229_f32.atan2(0.973)
}

/// The map's collision as the game sends it, or None to skip.
fn hangemhigh(test: &str) -> Option<HaloMap> {
    let path = halo_maps(test, MAP)?;
    Some(HaloMap::read(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display())))
}

/// The engine with Hang 'Em High loaded, or None to skip.
fn engine(test: &str) -> Option<(Engine, Vec<f32>)> {
    let map = hangemhigh(test)?;
    let assets: PathBuf = assets(test)?;
    let triangles = map.bsps[0].clone();
    let engine = Engine::start_with(&assets, || triangles.clone());
    Some((engine, triangles))
}

/// Gets on at `at` (world units) facing `yaw`.
fn activate(engine: &mut Engine, at: [f32; 3], yaw: f32) -> Result<Snapshot, String> {
    engine.try_activate(at.map(|v| v * METRES), yaw)
}

/// 1. The loader reads Hang 'Em High's one structure BSP and makes the
/// game's 2874 triangles from its collision, every number finite.
#[test]
fn hangemhigh_collision_is_the_triangles_the_game_sends() {
    let Some(map) = hangemhigh("hangemhigh_collision") else { return };
    assert_eq!(map.name, MAP);
    assert_eq!(map.bsps.len(), 1, "Hang 'Em High has one structure BSP");
    assert_eq!(map.triangle_count(0), 2874, "the game sends 2874 triangles for Hang 'Em High");
    assert!(map.bsps[0].iter().all(|v| v.is_finite()));
}

/// 2. Loaded, they make what the game logs: 2874 triangles kept, and its
/// walkable edges, lips, runs and rails.
#[test]
fn hangemhigh_loads_as_in_the_game() {
    let Some((engine, _)) = engine("hangemhigh_loads") else { return };
    let log = engine.map_log();
    for line in [SENT, DEDUPLICATED, RAILS] {
        assert!(log.iter().any(|l| l.contains(line)), "the load logs `{line}`:\n{}", log.join("\n"));
    }
}

/// What went wrong in a run, if anything: a frame that is not finite, a step
/// that failed (and was recovered from), or the session given up.
fn trouble(frames: &[Snapshot], log: &[String]) -> Option<String> {
    let failed: Vec<&String> = log.iter().filter(|l| l.contains("a step failed")).collect();
    if let Some(bad) = frames.iter().find(|s| !s.is_finite()) {
        return Some(format!("a frame not finite at engine tick {}: {bad:?}", bad.tick));
    }
    if !failed.is_empty() || frames.iter().any(|s| s.code == 2) {
        return Some(failed.iter().map(|l| l.as_str()).collect::<Vec<_>>().join("\n"));
    }
    None
}

/// Off the board at `at` facing `yaw`: on, `ride` game ticks of pushing, Y,
/// then `jumps` times: standing until 600 engine ticks after getting off (or
/// landing), then X with `stick` (pressed with it, from standing, as in
/// Build 28's log) and landing. (Build 28's NaN grew for some 180 engine
/// ticks after a landing before the next jump met it.)
fn stand_then_jump(
    engine: &mut Engine,
    at: [f32; 3],
    yaw: f32,
    ride: usize,
    stick: [i16; 2],
    jumps: usize,
) -> Result<(), String> {
    activate(engine, at, yaw)?;
    take_log();
    for _ in 0..ride / 30 {
        hold(engine, Pad::buttons(A), 15)?;
        hold(engine, Pad::NEUTRAL, 15)?;
    }
    hold(engine, Pad::NEUTRAL, 10)?;
    match get_off(engine)? {
        Some(since) => jumps_from(engine, since, stick, jumps),
        None => Ok(()),
    }
}

/// Y, and the engine tick the skater was off the board; None when it did not
/// get off (a spot at a drop, where getting on rolls the board off it, or
/// riding ended in a bail: Y does nothing while the skater is down), which
/// says so and leaves the rest of that run out.
fn get_off(engine: &mut Engine) -> Result<Option<u32>, String> {
    let mut states = Vec::new();
    // (pressed again each second: Y in the air, say just after a bail's
    // reset, is not taken)
    for i in 0..90 {
        let s = engine.try_step(Pad::buttons(if i % 30 < 4 { Y } else { 0 }))?;
        if s.off_board() {
            return Ok(Some(s.tick));
        }
        if states.last() != Some(&s.state) {
            states.push(s.state.clone());
        }
    }
    eprintln!("    did not get off the board (states {states:?}): the rest of this run left out");
    Ok(None)
}

/// `jumps` times: standing until 600 engine ticks after `since`, X with
/// `stick`, and up to 3 seconds to land.
fn jumps_from(engine: &mut Engine, mut since: u32, stick: [i16; 2], jumps: usize) -> Result<(), String> {
    for _ in 0..jumps {
        while engine.history.last().unwrap().tick < since + 600 {
            engine.try_step(Pad::NEUTRAL)?;
        }
        hold(engine, Pad { left: stick, ..Pad::NEUTRAL }.with_buttons(X), 6)?;
        let mut flying = false;
        since = engine.history.last().unwrap().tick;
        for _ in 0..90 {
            let s = engine.try_step(Pad::NEUTRAL)?;
            since = s.tick;
            flying |= s.state == "BipedAir";
            if flying && s.state == "BipedGround" {
                break;
            }
        }
    }
    Ok(())
}

/// 3. Build 28's failure where it came: on at the logged spot facing the
/// logged way, a little riding, off (Y), standing for 600 engine ticks, then
/// X with the logged stick. Every frame finite, no step failed: no
/// `Nonfinite BipedAir launch packet`.
#[test]
fn build_28_jump_from_walking_on_hangemhigh() {
    let Some((mut engine, _)) = engine("build_28_jump") else { return };
    for (ride, what) in [(0, "straight off"), (60, "after riding")] {
        let result = stand_then_jump(&mut engine, BUILD_28_SPOT, build_28_yaw(), ride, BUILD_28_STICK, 2);
        let log = take_log();
        engine.trace(0, 60);
        let frames = engine.history.clone();
        let where_ = frames.last().map(|s| s.at().map(|v| v / METRES));
        eprintln!("build 28 spot, {what}: ended at {where_:?} world units, {:?}", frames.last().map(|s| &s.state));
        assert!(
            frames.iter().any(|s| s.state == "BipedAir"),
            "{what}: X jumped (BipedAir): {:?}",
            frames.last()
        );
        if let Err(e) = result {
            panic!("{what}: the run stopped: {e}\n{}", log.join("\n"));
        }
        if let Some(trouble) = trouble(&frames, &log) {
            panic!("{what}: {trouble}");
        }
    }
}

/// Spots across the map's walkable floors, world units: the middle of the
/// largest flat-enough triangle (its normal within 25 degrees of up) in each
/// 1.5 world unit cell, with room above it for the skater (nothing within
/// 0.8 world units, 2.4 m, overhead), spread evenly through the map, `count`
/// of them (0: all).
fn floor_spots(triangles: &[f32], count: usize) -> Vec<[f32; 3]> {
    let tris: Vec<[[f32; 3]; 3]> = triangles
        .chunks_exact(9)
        .map(|t| [[t[0], t[1], t[2]], [t[3], t[4], t[5]], [t[6], t[7], t[8]]])
        .collect();
    let mut cells: std::collections::BTreeMap<(i32, i32, i32), ([f32; 3], f32)> = Default::default();
    for t in &tris {
        let (ab, ac) = (sub(t[1], t[0]), sub(t[2], t[0]));
        let n = cross(ab, ac);
        let area = length(n) / 2.0;
        if area < 0.02 || n[2] / length(n) < 0.9 {
            continue;
        }
        let c = [0, 1, 2].map(|k| (t[0][k] + t[1][k] + t[2][k]) / 3.0);
        if tris.iter().any(|u| overhead(c, u, 0.8)) {
            continue;
        }
        let key = ((c[0] / 1.5).floor() as i32, (c[1] / 1.5).floor() as i32, (c[2] / 1.5).floor() as i32);
        let cell = cells.entry(key).or_insert((c, 0.0));
        if area > cell.1 {
            *cell = (c, area);
        }
    }
    let all: Vec<[f32; 3]> = cells.into_values().map(|(c, _)| c).collect();
    if count == 0 || count >= all.len() {
        return all;
    }
    (0..count).map(|i| all[i * all.len() / count]).collect()
}

/// Whether triangle `t` lies straight above `p`, within `height`.
fn overhead(p: [f32; 3], t: &[[f32; 3]; 3], height: f32) -> bool {
    // the vertical line through p meets t's plane inside t (barycentric)
    let [a, b, c] = *t;
    let d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
    if d.abs() < 1e-9 {
        return false;
    }
    let u = ((b[1] - c[1]) * (p[0] - c[0]) + (c[0] - b[0]) * (p[1] - c[1])) / d;
    let v = ((c[1] - a[1]) * (p[0] - c[0]) + (a[0] - c[0]) * (p[1] - c[1])) / d;
    let w = 1.0 - u - v;
    if u < 0.0 || v < 0.0 || w < 0.0 {
        return false;
    }
    let z = u * a[2] + v * b[2] + w * c[2];
    z > p[2] + 0.01 && z < p[2] + height
}

fn sub(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}
fn cross(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
}

fn sweep_size() -> usize {
    std::env::var("HALO_SKATE_TEST_SWEEP").ok().and_then(|v| v.parse().ok()).unwrap_or(SPOTS)
}

/// Runs `f` at each spot and heading, gathering what went wrong (a session
/// given up is made again with `triangles`, as the game does on the next J),
/// and panics with all of it at the end.
fn sweep(
    engine: &mut Engine,
    triangles: &[f32],
    what: &str,
    spots: &[[f32; 3]],
    headings: &[f32],
    mut f: impl FnMut(&mut Engine, [f32; 3], f32) -> Result<(), String>,
) {
    let started = std::time::Instant::now();
    let mut troubles = Vec::new();
    let (mut runs, mut jumped) = (0, 0);
    for (i, &spot) in spots.iter().enumerate() {
        for (j, &yaw) in headings.iter().enumerate() {
            // each spot faces its own share of the headings
            if (i + j) % 2 == 1 && headings.len() > 2 {
                continue;
            }
            runs += 1;
            engine.history.clear();
            let result = f(engine, spot, yaw);
            jumped += usize::from(engine.history.iter().any(|s| s.state == "BipedAir"));
            let log = take_log();
            let place = format!("{what} at {spot:?} facing {yaw:?}");
            if let Some(trouble) = trouble(&engine.history, &log) {
                eprintln!("{place}: {trouble}");
                engine.trace(0, 30);
                troubles.push(format!("{place}: {trouble}"));
            }
            if let Err(e) = result {
                troubles.push(format!("{place}: the run stopped: {e}"));
                if halo_skate_state() == -1 {
                    // the session was given up
                    engine.reload(triangles);
                }
            }
        }
    }
    eprintln!(
        "{what}: {runs} runs at {} spots in {:.1} s, {jumped} jumped off the board, {} with trouble",
        spots.len(),
        started.elapsed().as_secs_f32(),
        troubles.len()
    );
    assert!(troubles.is_empty(), "{what}:\n{}", troubles.join("\n\n"));
    // (a run that never got off the board tested little)
    assert!(jumped * 4 >= runs * 3, "{what}: only {jumped} of {runs} runs jumped off the board");
}

const HEADINGS: [f32; 4] = [0.0, std::f32::consts::FRAC_PI_2, std::f32::consts::PI, -std::f32::consts::FRAC_PI_2];

/// 4. Standing then jumping across the map: at spots on its floors, a few
/// headings each, off the board, 600 engine ticks standing, then X with the
/// logged stick.
#[test]
fn standing_then_jumping_across_hangemhigh() {
    let Some((mut engine, triangles)) = engine("standing_then_jumping") else { return };
    let spots = floor_spots(&triangles, sweep_size());
    assert!(spots.len() >= SPOTS.min(sweep_size().max(1)), "found {} floor spots", spots.len());
    sweep(&mut engine, &triangles, "standing then jumping", &spots, &HEADINGS, |engine, spot, yaw| {
        stand_then_jump(engine, spot, yaw, 0, BUILD_28_STICK, 2)
    });
}

/// 5. Riding into walls and off edges: from spots on the floors, pushing
/// hard one way for 4 seconds, an ollie halfway, wherever that leads (walls,
/// ledges, drops: bails), then off the board and a jump. Every frame finite
/// and no step failed.
#[test]
fn riding_into_walls_and_edges_on_hangemhigh() {
    let Some((mut engine, triangles)) = engine("riding_into_walls") else { return };
    let spots = floor_spots(&triangles, sweep_size());
    sweep(&mut engine, &triangles, "riding into walls", &spots, &HEADINGS, |engine, spot, yaw| {
        activate(engine, spot, yaw)?;
        take_log();
        for k in 0..4 {
            engine.try_step(Pad::NEUTRAL)?;
            hold(engine, Pad::buttons(A), 14)?;
            hold(engine, Pad::NEUTRAL, 15)?;
            if k == 2 {
                hold(engine, Pad::right(0, -FULL), 8)?;
                hold(engine, Pad::right(0, FULL), 2)?;
            }
        }
        // 2 seconds, and up to 6 more for a bail's reset back onto the board
        hold(engine, Pad::NEUTRAL, 60)?;
        for _ in 0..180 {
            let s = engine.try_step(Pad::NEUTRAL)?;
            if !(s.state.starts_with("Wipeout") || s.state == "Teleporting" || s.airborne()) {
                break;
            }
        }
        // off, a jump 2 seconds later, and another 10 seconds after landing
        match get_off(engine)? {
            Some(off) => jumps_from(engine, off.saturating_sub(480), BUILD_28_STICK, 2),
            None => Ok(()),
        }
    });
}

/// 6. Build 28's way to its NaN: off the board, walking a second, then a jump
/// with the stick still held, into whatever is ahead (a wall restarts the
/// jump: its normal, a little off an axis on this level, gave the launch a w
/// lane, which the landing placed into the walk), landing, standing 10
/// seconds and jumping again.
#[test]
fn jumping_into_walls_from_walking_on_hangemhigh() {
    let Some((mut engine, triangles)) = engine("jumping_into_walls") else { return };
    let spots = floor_spots(&triangles, sweep_size());
    sweep(&mut engine, &triangles, "jumping into walls", &spots, &HEADINGS, |engine, spot, yaw| {
        activate(engine, spot, yaw)?;
        take_log();
        hold(engine, Pad::NEUTRAL, 10)?;
        if get_off(engine)?.is_none() {
            return Ok(());
        }
        hold(engine, Pad::NEUTRAL, 30)?;
        let walk = Pad::left(0, FULL);
        hold(engine, walk, 30)?;
        let tick = engine.history.last().unwrap().tick;
        jumps_from(engine, tick.saturating_sub(600), [0, FULL], 1)?;
        let landed = engine.history.last().unwrap().tick;
        jumps_from(engine, landed, BUILD_28_STICK, 1)
    });
}

/// `ticks` game ticks with `pad` held (`Engine::try_step`).
fn hold(engine: &mut Engine, pad: Pad, ticks: usize) -> Result<(), String> {
    for _ in 0..ticks {
        engine.try_step(pad)?;
    }
    Ok(())
}

/// 7. Skate 3's own reset never throws the skater back to where it rode
/// before it was put on the board somewhere else (a get-on, a fall put back,
/// a teleporter, a session kept from another map): its checkpoints are
/// forgotten when the skater is put down. Ridden about one spot for twelve
/// seconds, then put outside the level (where a bot's skater was thrown on a
/// test server: a checkpoint of Sidewinder, the map before, in a session kept
/// from it), it falls and the engine's reset teleports it: to where it was
/// put, never back to the first spot (it went straight there, 45 world units).
#[test]
fn a_bail_resets_about_where_the_skater_was_put_on_hangemhigh() {
    let Some((mut engine, _)) = engine("bail_resets_where_put") else { return };
    let first = [24.17, -21.08, -7.92];
    let second = [-18.89, -7.94, -3.0];
    let ride = |engine: &mut Engine, seconds: usize| -> Result<(), String> {
        for k in 0..seconds {
            hold(engine, Pad::buttons(A), 14)?;
            hold(engine, Pad::right(if k % 2 == 0 { FULL } else { -FULL }, 0), 16)?;
        }
        Ok(())
    };
    activate(&mut engine, first, 0.0).unwrap();
    ride(&mut engine, 12).unwrap();
    let rode = engine.history.len();
    // (activate clears the frames kept)
    activate(&mut engine, second, std::f32::consts::PI).unwrap();
    for _ in 0..4 {
        ride(&mut engine, 6).unwrap();
    }
    let states: std::collections::BTreeSet<&str> = engine.history.iter().map(|s| s.state.as_str()).collect();
    let farthest = engine.history.iter().map(|s| length(sub(s.position, second))).fold(0.0_f32, f32::max);
    let nearest_first = engine.history.iter().map(|s| length(sub(s.position, first))).fold(f32::MAX, f32::min);
    eprintln!("states {states:?}; farthest from the second spot {farthest:.1} world units, nearest the first \
        {nearest_first:.1} (rode {rode} frames about the first)");
    assert!(nearest_first > 8.0, "a bail threw the skater back near where it rode before: {nearest_first:.1} world units");
}

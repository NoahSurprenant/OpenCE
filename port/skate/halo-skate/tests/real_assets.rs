//! Skate 3 mode against the real converted Skate 3 data: the skater, its
//! animations and physics, on a small map made here (`common::TestMap`),
//! through the C interface the way the game drives it. Skipped unless
//! `HALO_SKATE_TEST_ASSETS` names the converted `assets` folder (see
//! `common/mod.rs`); run with `--release`.

mod common;

use common::*;
use halo_skate::*;
use std::ffi::c_char;

/// A biped skeleton like the cyborg's, by hand: the `bip01` nodes the
/// retarget follows (rig.rs) plus the head and hands, standing about 2.1 m
/// tall. Each node's name, parent, and position in metres (its default
/// rotation is the identity).
const BIPED: [(&str, i16, [f32; 3]); 21] = [
    ("bip01 pelvis", -1, [0.0, 0.0, 1.0]),
    ("bip01 spine", 0, [0.0, 0.0, 1.1]),
    ("bip01 spine1", 1, [0.0, 0.0, 1.35]),
    ("bip01 neck", 2, [0.0, 0.0, 1.7]),
    ("bip01 head", 3, [0.0, 0.0, 1.8]),
    ("bip01 l clavicle", 2, [0.0, 0.1, 1.6]),
    ("bip01 l upperarm", 5, [0.0, 0.25, 1.6]),
    ("bip01 l forearm", 6, [0.0, 0.25, 1.3]),
    ("bip01 l hand", 7, [0.0, 0.25, 1.05]),
    ("bip01 r clavicle", 2, [0.0, -0.1, 1.6]),
    ("bip01 r upperarm", 9, [0.0, -0.25, 1.6]),
    ("bip01 r forearm", 10, [0.0, -0.25, 1.3]),
    ("bip01 r hand", 11, [0.0, -0.25, 1.05]),
    ("bip01 l thigh", 0, [0.0, 0.12, 0.95]),
    ("bip01 l calf", 13, [0.0, 0.12, 0.52]),
    ("bip01 l foot", 14, [0.0, 0.12, 0.1]),
    ("bip01 r thigh", 0, [0.0, -0.12, 0.95]),
    ("bip01 r calf", 16, [0.0, -0.12, 0.52]),
    ("bip01 r foot", 17, [0.0, -0.12, 0.1]),
    ("bip01 l toe0", 15, [0.15, 0.12, 0.02]),
    ("bip01 r toe0", 18, [0.15, -0.12, 0.02]),
];
const L_FOOT: usize = 15;
const R_FOOT: usize = 18;

/// The biped above described to the engine; returns how many nodes follow a
/// skater bone.
fn describe_biped() -> i32 {
    let mut names = vec![0 as c_char; BIPED.len() * 32];
    let mut parents = Vec::new();
    let mut inverse = Vec::new();
    for (i, (name, parent, at)) in BIPED.iter().enumerate() {
        for (slot, byte) in names[i * 32..].iter_mut().zip(name.bytes()) {
            *slot = byte as c_char;
        }
        parents.push(*parent);
        let p = metres(*at);
        // scale, the identity's columns, and the position inverted
        inverse.extend([1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, -p[0], -p[1], -p[2]]);
    }
    unsafe { halo_skate_set_skeleton(BIPED.len() as i32, names.as_ptr(), parents.as_ptr(), inverse.as_ptr()) }
}

fn assert_all_finite(frames: &[Snapshot], what: &str) {
    if let Some(bad) = frames.iter().find(|s| !s.is_finite()) {
        panic!("{what}: the frame at engine tick {} ({}) is not finite: {bad:?}", bad.tick, bad.state);
    }
}

fn assert_no_failed_steps(frames: &[Snapshot], log: &[String], what: &str) {
    let failures: Vec<&String> = log.iter().filter(|l| l.contains("a step failed")).collect();
    let recovered: Vec<u32> = frames.iter().filter(|s| s.code == 2).map(|s| s.tick).collect();
    assert!(
        failures.is_empty() && recovered.is_empty(),
        "{what}: steps failed (recovered at engine ticks {recovered:?}):\n{}",
        failures.iter().map(|l| l.as_str()).collect::<Vec<_>>().join("\n")
    );
}

/// 1. The preload makes the session from the real data, and getting on
/// gives a finite pose: the skater standing on the board on the floor, with
/// a camera, and a biped posed from it with its feet at the floor.
#[test]
fn getting_on_gives_a_finite_pose_with_feet_on_the_board() {
    let Some(assets) = assets("getting_on") else { return };
    let mut engine = Engine::start(&assets);
    let on = engine.activate([0.0, TestMap::OPEN, 0.0], 0.0);
    let settled = engine.hold(Pad::NEUTRAL, 30);
    engine.trace(0, 10);
    for s in [&on, &settled] {
        assert!(s.is_finite(), "{s:?}");
        assert!(!s.off_board() && !s.airborne(), "standing on the board: {}", s.state);
        let [x, y, z] = s.at();
        assert!(x.abs() < 0.5 && (y - TestMap::OPEN).abs() < 0.5, "where it got on: ({x}, {y}, {z})");
        assert!((-0.1..0.6).contains(&z), "the skater's root above the floor: {z} m");
        assert!(s.up[2] > 0.9, "standing upright: up {:?}", s.up);
        assert!(s.forward[0] > 0.9, "facing +X as asked: forward {:?}", s.forward);
        assert!(s.has_camera, "the engine's camera");
        assert!(s.speed() < 0.5, "standing still: {} m/s", s.speed());
    }
    assert_all_finite(&engine.history, "standing on the board");

    // the biped, posed as the skater: every node, and its feet at the floor
    let mapped = describe_biped();
    assert_eq!(mapped, 16, "the bip01 nodes that follow a skater bone");
    let mut nodes = vec![f32::NAN; BIPED.len() * 13];
    let posed = unsafe { halo_skate_pose_nodes(nodes.as_mut_ptr(), BIPED.len() as i32) };
    assert_eq!(posed, BIPED.len() as i32, "nodes posed");
    assert!(nodes.iter().all(|v| v.is_finite()), "the posed nodes are finite");
    let node_at = |i: usize| [nodes[i * 13 + 10], nodes[i * 13 + 11], nodes[i * 13 + 12]].map(|v| v * METRES);
    let floor_to = |i: usize| node_at(i)[2];
    let (left, right, pelvis) = (floor_to(L_FOOT), floor_to(R_FOOT), floor_to(0));
    eprintln!("biped: left foot {:?} m, right foot {:?} m, pelvis {:?} m", node_at(L_FOOT), node_at(R_FOOT), node_at(0));
    for (foot, z) in [("left", left), ("right", right)] {
        // on the deck, the board's height above the floor (about 10 cm) plus
        // the ankle
        assert!((0.0..0.45).contains(&z), "the {foot} foot {z} m above the floor");
    }
    assert!(pelvis > left.max(right) + 0.5, "the pelvis {pelvis} m stands above the feet");
    let apart = {
        let (l, r) = (node_at(L_FOOT), node_at(R_FOOT));
        ((l[0] - r[0]).powi(2) + (l[1] - r[1]).powi(2)).sqrt()
    };
    assert!((0.15..1.0).contains(&apart), "the feet {apart} m apart, on the board");
}

/// 1b. The skater's own skeleton from the real data, through the engine's
/// Rust interface (`Session`, as the worker makes it): a sane number of
/// bones, all finite, the feet named and on the board just above the floor.
#[test]
fn the_skaters_skeleton_is_whole_and_its_feet_on_the_board() {
    let Some(assets) = assets("skeleton") else { return };
    // (the engine's turn, so that two sessions are not made at once)
    let _engine = Engine::start(&assets);
    let pose = std::thread::Builder::new()
        .stack_size(32 * 1024 * 1024)
        .spawn(move || {
            use skate_host::bridge::Session;
            Session::preload(&assets).expect("preload");
            let floor = vec![
                [[-10.0, 0.0, -10.0], [-10.0, 0.0, 10.0], [10.0, 0.0, 10.0]],
                [[-10.0, 0.0, -10.0], [10.0, 0.0, 10.0], [10.0, 0.0, -10.0]],
            ];
            let mut session = Session::new(&assets, floor, Vec::new(), [0.0; 3], 0.0).expect("session");
            session.activate([0.0; 3], 0.0).expect("activate")
        })
        .unwrap()
        .join()
        .expect("the session thread");
    let count = pose.bones.len();
    eprintln!("skeleton: {count} bones, root {:?}", pose.root.w_axis);
    assert_eq!(pose.names.len(), count, "a name for each bone");
    assert!((20..=400).contains(&count), "a skater's skeleton: {count} bones");
    assert!(pose.root.is_finite() && pose.bones.iter().all(|b| b.is_finite()), "finite");
    // Skate is Y up, in metres
    let bone = |name: &str| {
        let i = pose.names.iter().position(|n| n == name).unwrap_or_else(|| panic!("no {name} bone"));
        pose.bones[i].w_axis.truncate()
    };
    for (foot, toe) in [("LEFTFOOT", "LEFTTOEBASE"), ("RIGHTFOOT", "RIGHTTOEBASE")] {
        let (ankle, toe) = (bone(foot), bone(toe));
        eprintln!("skeleton: {foot} at {ankle:?}, {toe:?}");
        assert!((0.05..0.4).contains(&ankle.y), "{foot} {} m above the floor, on the deck", ankle.y);
        assert!(ankle.distance(toe) < 0.3, "{foot} near its toe: {} m", ankle.distance(toe));
    }
    let hips = bone("HIPS");
    assert!(hips.y > bone("LEFTFOOT").y + 0.4, "the hips above the feet: {hips:?}");
    let apart = bone("LEFTFOOT").distance(bone("RIGHTFOOT"));
    assert!((0.15..1.0).contains(&apart), "the feet {apart} m apart, on the board");
}

/// 2. Riding forward, pushing three times then rolling, for 10 s: the pose
/// stays finite and on the board, and the speed rises with the pushes, then
/// settles.
#[test]
fn riding_with_pushes_for_ten_seconds_stays_finite_and_speed_settles() {
    let Some(assets) = assets("riding") else { return };
    let mut engine = Engine::start(&assets);
    engine.activate([0.0, TestMap::OPEN, 0.0], 0.0);
    take_log();
    for _ in 0..3 {
        push(&mut engine, 15);
    }
    let pushed = engine.history.len() - 1;
    engine.hold(Pad::NEUTRAL, 300 - 90);
    let log = take_log();
    engine.trace(0, 15);
    let frames = &engine.history;
    assert_all_finite(frames, "riding");
    assert_no_failed_steps(frames, &log, "riding");
    assert!(frames.iter().all(|s| !s.off_board()), "stays on the board");
    let ticks = frames.last().unwrap().tick - frames[0].tick;
    assert!((590..=610).contains(&ticks), "10 s is about 600 engine ticks: {ticks}");
    let speeds: Vec<f32> = frames.iter().map(Snapshot::ground_speed).collect();
    let top = speeds.iter().cloned().fold(0.0, f32::max);
    let after_pushes = speeds[pushed];
    let at = |second: usize| speeds[(second * 30).min(speeds.len() - 1)];
    eprintln!(
        "speed: after the pushes {after_pushes:.2} m/s, top {top:.2}, at 4 s {:.2}, 7 s {:.2}, 10 s {:.2}",
        at(4),
        at(7),
        at(10)
    );
    assert!(after_pushes > 2.0, "the pushes got it going: {after_pushes} m/s");
    assert!(top < 15.0, "pushing on the flat is not that fast: {top} m/s");
    // settles: rolling on, neither speeding up nor stopping dead
    let (four, ten) = (at(4), at(10));
    assert!(ten <= four + 0.2, "it does not speed up without a push: {four} then {ten} m/s");
    assert!(ten > 0.3 * four, "it rolls on: {four} then {ten} m/s");
    let travelled = frames.last().unwrap().at()[0] - frames[0].at()[0];
    assert!(travelled > 10.0, "it went forward, along +X: {travelled} m");
}

/// 2b. Over the funbox: up the kicker, across, down the slope; finite all
/// the way.
#[test]
fn riding_over_the_funbox_stays_finite() {
    let Some(assets) = assets("funbox") else { return };
    let mut engine = Engine::start(&assets);
    engine.activate([0.0, TestMap::FUNBOX, 0.0], 0.0);
    take_log();
    for _ in 0..4 {
        push(&mut engine, 15);
    }
    engine.hold(Pad::NEUTRAL, 150);
    let log = take_log();
    engine.trace(0, 15);
    assert_all_finite(&engine.history, "over the funbox");
    assert_no_failed_steps(&engine.history, &log, "over the funbox");
    let highest = engine.history.iter().map(|s| s.at()[2]).fold(f32::MIN, f32::max);
    let furthest = engine.history.iter().map(|s| s.at()[0]).fold(f32::MIN, f32::max);
    eprintln!("funbox: highest {highest:.2} m, furthest {furthest:.2} m");
    assert!(highest > 0.7, "it went up onto the funbox: {highest} m");
    assert!(furthest > 27.0, "and past it: {furthest} m");
}

/// 3. An ollie from rolling: the right stick down, then flicked up. The board
/// leaves the ground and lands, and the pose stays finite.
#[test]
fn an_ollie_leaves_the_ground_and_lands() {
    let Some(assets) = assets("ollie") else { return };
    let mut engine = Engine::start(&assets);
    engine.activate([0.0, TestMap::OPEN, 0.0], 0.0);
    push(&mut engine, 15);
    push(&mut engine, 15);
    take_log();
    let from = engine.history.len();
    let ground = engine.history[from - 1].at()[2];
    ollie(&mut engine, Pad::NEUTRAL);
    engine.hold(Pad::NEUTRAL, 90);
    let log = take_log();
    engine.trace(from, 3);
    let frames = engine.since(from);
    assert_all_finite(frames, "the ollie");
    assert_no_failed_steps(frames, &log, "the ollie");
    let first_air = frames.iter().position(Snapshot::airborne);
    let peak = frames.iter().map(|s| s.at()[2]).fold(f32::MIN, f32::max) - ground;
    let air_ticks = frames.iter().filter(|s| s.airborne()).count();
    eprintln!("ollie: in the air for {air_ticks} game ticks, {peak:.2} m up");
    let first_air = first_air.expect("the ollie never left the ground");
    let landed = frames[first_air..].iter().position(|s| !s.airborne()).map(|i| i + first_air);
    let landed = landed.expect("the ollie never landed");
    assert!(peak > 0.15, "it popped: {peak} m");
    let after = &frames[landed..];
    assert!(after.iter().all(|s| !s.off_board()), "landed on the board: {:?}", after.last().unwrap().state);
    assert!(
        after.last().unwrap().state == "PhysicsGround",
        "rolling again after landing: {}",
        after.last().unwrap().state
    );
}

/// 4. Off the board (Y): standing still for 10 s (600 engine ticks) keeps
/// every number finite, and a jump from walking (X with the left stick held)
/// makes a launch packet the engine takes, with no `Nonfinite BipedAir launch
/// packet` (Build 28, on hangemhigh). Once on the open floor facing +X from a
/// standstill, once on the tilted pad rolling at an angle.
#[test]
fn off_the_board_standing_then_jumping_from_walking_stays_finite() {
    let Some(assets) = assets("off_the_board") else { return };
    let mut engine = Engine::start(&assets);
    off_the_board(&mut engine, "the open floor", [0.0, TestMap::OPEN, 0.0], 0.0, 0);
    let pad = [TestMap::TILT_START + 5.0, TestMap::TILT - 5.0, 0.4];
    off_the_board(&mut engine, "the tilted pad", pad, 0.6, 1);
}

fn off_the_board(engine: &mut Engine, place: &str, at: [f32; 3], yaw: f32, pushes: usize) {
    eprintln!("off the board on {place}:");
    engine.activate(at, yaw);
    engine.hold(Pad::NEUTRAL, 15);
    for _ in 0..pushes {
        push(engine, 15);
    }
    take_log();
    let from = engine.history.len();
    engine.hold(Pad::buttons(Y), 4);
    let mut off = None;
    for _ in 0..90 {
        let s = engine.step(Pad::NEUTRAL);
        if s.off_board() && off.is_none() {
            off = Some(engine.history.len() - 1);
        }
    }
    let off = off.unwrap_or_else(|| {
        engine.trace(from, 3);
        panic!("{place}: Y did not take the skater off the board")
    });
    let off_tick = engine.history[off].tick;
    eprintln!("{place}: off the board at engine tick {off_tick} ({})", engine.history[off].state);

    // standing: 600 engine ticks from getting off
    while engine.history.last().unwrap().tick < off_tick + 600 {
        engine.step(Pad::NEUTRAL);
    }
    let standing_log = take_log();
    engine.trace(off, 60);
    let standing = engine.since(off).to_vec();
    if let Some(bad) = standing.iter().find(|s| !s.is_finite() || s.code == 2) {
        eprintln!(
            "{place}: standing, the first bad frame came {} engine ticks ({:.2} s) after getting off: {bad:?}",
            bad.tick - off_tick,
            (bad.tick - off_tick) as f32 / 60.0
        );
    }
    assert_all_finite(&standing, &format!("{place}: standing off the board"));
    assert_no_failed_steps(&standing, &standing_log, &format!("{place}: standing off the board"));
    assert!(standing.iter().all(Snapshot::off_board), "{place}: stays off the board while standing");

    // walking, then jumping: X with the stick held (the stick of the Build 28
    // log, 23652 and 30746 of 32767)
    let walk = engine.history.len();
    let stick = Pad::left(23652, 30746);
    engine.hold(stick, 30);
    let walked = engine.history.last().unwrap().clone();
    engine.hold(stick.with_buttons(X), 6);
    engine.hold(stick, 24);
    engine.hold(Pad::NEUTRAL, 60);
    let jump_log = take_log();
    engine.trace(walk, 6);
    let jumping = engine.since(walk);
    let launch_failures: Vec<&String> =
        jump_log.iter().filter(|l| l.contains("Nonfinite BipedAir launch packet")).collect();
    assert!(launch_failures.is_empty(), "{place}: the jump from walking failed:\n{launch_failures:#?}");
    assert_all_finite(jumping, &format!("{place}: jumping from walking"));
    assert_no_failed_steps(jumping, &jump_log, &format!("{place}: jumping from walking"));
    let moved = {
        let (a, b) = (engine.history[walk].at(), walked.at());
        ((a[0] - b[0]).powi(2) + (a[1] - b[1]).powi(2)).sqrt()
    };
    eprintln!("{place}: walked {moved:.2} m in 1 s");
    assert!(moved > 0.5, "{place}: the left stick walks the skater: {moved} m in 1 s");
    assert!(jumping.iter().any(|s| s.state == "BipedAir"), "{place}: X jumped (BipedAir)");
    assert!(jumping.last().unwrap().state == "BipedGround", "{place}: and landed on its feet");
}

/// 6. A bail into the box ledge at speed: rolling at 8 m/s, an ollie a few
/// metres short of it hits the box's face in the air (the engine does not
/// bail riding straight into a wall: the board stops at it). Either no step
/// fails, or the one that does puts the skater back on the board, finite,
/// where it last was (recovery.rs); and the engine's own reset after the bail
/// leaves the skater riding again.
#[test]
fn a_bail_into_the_box_at_speed_recovers() {
    let Some(assets) = assets("bail") else { return };
    let mut engine = Engine::start(&assets);
    engine.activate([0.0, TestMap::LEDGE, 0.0], 0.0);
    take_log();
    push(&mut engine, 12);
    push(&mut engine, 12);
    while engine.history.last().unwrap().at()[0] < TestMap::LEDGE_START - 6.0 {
        engine.step(Pad::NEUTRAL);
        assert!(engine.history.len() < 600, "never reached the box");
    }
    ollie(&mut engine, Pad::NEUTRAL);
    engine.hold(Pad::NEUTRAL, 240);
    let log = take_log();
    engine.trace(0, 6);
    let frames = engine.history.clone();
    assert_all_finite(&frames, "the bail");
    let fastest = frames.iter().map(Snapshot::ground_speed).fold(0.0, f32::max);
    let states = transitions(&frames);
    eprintln!("bail: fastest {fastest:.2} m/s, states {states:?}");
    assert!(fastest > 6.0, "at speed: {fastest} m/s");
    let wipeout = frames.iter().position(|s| s.state == "WipeoutGround");
    let recoveries: Vec<usize> = (0..frames.len()).filter(|&i| frames[i].code == 2).collect();
    eprintln!(
        "bail: wipeout at {:?}, recoveries at engine ticks {:?}",
        wipeout.map(|i| frames[i].tick),
        recoveries.iter().map(|&i| frames[i].tick).collect::<Vec<_>>()
    );
    assert!(wipeout.is_some() || !recoveries.is_empty(), "hitting the box at {fastest} m/s is a bail");
    for &i in &recoveries {
        let (before, back) = (&frames[i - 1], &frames[i]);
        assert!(!back.off_board() && !back.airborne(), "put back on the board: {}", back.state);
        let (a, b) = (before.at(), back.at());
        let apart = ((a[0] - b[0]).powi(2) + (a[1] - b[1]).powi(2)).sqrt();
        assert!(apart < 2.0, "put back where it last was: {apart} m from the last good frame");
        assert!(log.iter().any(|l| l.contains("put back on the board")), "the recovery is logged");
    }
    assert!(!log.iter().any(|l| l.contains("making the session again")), "the session was given up");
    let last = frames.last().unwrap();
    assert!(
        !last.off_board() && last.state != "WipeoutGround" && last.speed() < 1.0,
        "after the bail the skater stands on the board again: {} at {:.2} m/s",
        last.state,
        last.speed()
    );
}

/// The states a run went through, each with the engine tick it began at.
fn transitions(frames: &[Snapshot]) -> Vec<(u32, &str)> {
    let mut seen: Vec<(u32, &str)> = Vec::new();
    for s in frames {
        if seen.last().map(|(_, state)| *state) != Some(s.state.as_str()) {
            seen.push((s.tick, &s.state));
        }
    }
    seen
}

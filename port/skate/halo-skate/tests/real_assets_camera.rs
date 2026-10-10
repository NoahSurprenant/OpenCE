//! The following camera off the board, against the real Skate 3 data
//! (`common/mod.rs`): with the frame's `off_board`, `skater_velocity` and
//! `facing`, `halo_skate_follow` follows the skater, not the board. Skipped
//! unless `HALO_SKATE_TEST_ASSETS` is set; run with `--release`.

mod common;

use common::*;
use halo_skate::*;
use std::f32::consts::{PI, TAU};

/// `struct halo_skate_follow` (halo_skate.h).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
struct Follow {
    heading: f32,
    still: f32,
}

/// What the camera test reads of the frame, past `Snapshot`.
#[derive(Clone, Copy, Debug)]
struct Subject {
    off_board: bool,
    /// world units a second
    board: [f32; 3],
    skater: [f32; 3],
    facing: [f32; 3],
    position: [f32; 3],
    heading: f32,
}

/// The game's camera settings (skate.c): 1 m/s, a fifth of the way a tick.
const MINIMUM_SPEED: f32 = 1.0 / METRES;
const TURN: f32 = 0.2;

fn heading_of(v: [f32; 3]) -> f32 {
    v[1].atan2(v[0]).rem_euclid(TAU)
}

/// The angle between two headings, 0 to pi.
fn apart(a: f32, b: f32) -> f32 {
    let d = (a - b).rem_euclid(TAU);
    if d > PI { TAU - d } else { d }
}

fn speed(v: [f32; 3]) -> f32 {
    (v[0] * v[0] + v[1] * v[1]).sqrt() * METRES
}

/// One game tick, then the camera's tick, as skate.c takes them.
fn tick(engine: &mut Engine, follow: &mut Follow, pad: Pad) -> Subject {
    let s = engine.step(pad);
    let heading = unsafe {
        halo_skate_follow(follow as *mut Follow as *mut _, &engine.frame, MINIMUM_SPEED, TURN, GAME_TICK)
    };
    let f = &engine.frame;
    let subject = Subject {
        off_board: f.off_board != 0,
        board: f.velocity,
        skater: f.skater_velocity,
        facing: f.facing,
        position: f.position,
        heading,
    };
    if std::env::var_os("CAMERA_TRACE").is_some() {
        eprintln!(
            "    tick {:5} {:16} at ({:6.2}, {:6.2}) m skater {:5.2} m/s {:5.2} board {:5.2} m/s {:5.2} facing {:5.2} camera {:5.2}",
            s.tick, s.state, s.at()[0], s.at()[1], speed(subject.skater), heading_of(subject.skater),
            speed(subject.board), heading_of(subject.board), heading_of(subject.facing), heading
        );
    }
    let finite = |v: [f32; 3]| v.iter().all(|x| x.is_finite());
    assert!(
        s.is_finite() && finite(subject.skater) && finite(subject.facing) && heading.is_finite(),
        "at engine tick {}: {subject:?}",
        s.tick
    );
    assert!((0.0..TAU).contains(&heading), "the heading is within 0 to 2 pi: {heading}");
    assert_eq!(subject.off_board, s.off_board(), "off_board agrees with the state {}", s.state);
    subject
}

/// `ticks` game ticks with `pad` held, each with the camera's tick.
fn hold(engine: &mut Engine, follow: &mut Follow, pad: Pad, ticks: usize) -> Vec<Subject> {
    (0..ticks).map(|_| tick(engine, follow, pad)).collect()
}

/// 5. Off the board the camera follows the skater, not the board: getting
/// off, then throwing the board (RT), the camera stays put while the board
/// rolls away; walking off another way, it turns to the way the skater
/// walks; stood still, it comes round behind the way the skater faces; and
/// it never snaps.
#[test]
fn off_the_board_the_camera_follows_the_skater_not_the_board() {
    let Some(assets) = assets("camera") else { return };
    let mut engine = Engine::start(&assets);
    engine.activate([0.0, TestMap::OPEN, 0.0], 0.0);
    let mut follow = Follow::default();
    let mut all = hold(&mut engine, &mut follow, Pad::NEUTRAL, 10);
    all.extend(hold(&mut engine, &mut follow, Pad::buttons(Y), 4));
    let off = hold(&mut engine, &mut follow, Pad::NEUTRAL, 45);
    assert!(off.last().unwrap().off_board, "Y took the skater off the board");
    all.extend(off);
    let before = all.last().unwrap().heading;

    // the board thrown: it rolls away, the skater stands
    let throw = Pad { triggers: [0, 255], ..Pad::NEUTRAL };
    let mut thrown = hold(&mut engine, &mut follow, throw, 10);
    thrown.extend(hold(&mut engine, &mut follow, Pad::NEUTRAL, 50));
    let fastest = thrown.iter().map(|s| speed(s.board)).fold(0.0, f32::max);
    let flying = thrown.iter().find(|s| speed(s.board) > 1.5).copied();
    eprintln!(
        "thrown: the board up to {fastest:.2} m/s, heading {:?}; the skater up to {:.2} m/s; the camera {:.3} to {:.3}",
        flying.map(|s| heading_of(s.board)),
        thrown.iter().map(|s| speed(s.skater)).fold(0.0, f32::max),
        before,
        thrown.last().unwrap().heading
    );
    assert!(fastest > 1.5, "RT threw the board: {fastest} m/s");
    for s in &thrown {
        assert!(s.off_board && speed(s.skater) < 1.0, "the skater stands while the board goes: {s:?}");
        // held, or lining up behind the facing; never after the board
        let behind = apart(s.heading, before) < 0.15 || apart(s.heading, heading_of(s.facing)) < 0.15;
        assert!(behind, "the camera stays behind the skater, not after the thrown board: {s:?}");
    }
    all.extend(thrown);

    // walking off: a quarter turn with the stick, then on along it
    let mut walk = hold(&mut engine, &mut follow, Pad::left(FULL, 0), 15);
    walk.extend(hold(&mut engine, &mut follow, Pad::left(0, FULL), 60));
    let walking = *walk.last().unwrap();
    let skater_heading = heading_of(walking.skater);
    let went = {
        let a = walk[walk.len() - 30].position;
        let b = walking.position;
        [b[0] - a[0], b[1] - a[1], 0.0]
    };
    eprintln!(
        "walking: the skater {:.2} m/s heading {skater_heading:.3} (went {:.3}), facing {:.3}; the board {:.2} m/s \
         heading {:.3}; the camera {:.3}",
        speed(walking.skater),
        heading_of(went),
        heading_of(walking.facing),
        speed(walking.board),
        heading_of(walking.board),
        walking.heading
    );
    assert!(walk.iter().all(|s| s.off_board), "off the board all the while");
    assert!(speed(walking.skater) > 1.0, "the skater walks: {} m/s", speed(walking.skater));
    assert!(
        apart(skater_heading, heading_of(went)) < 0.3,
        "skater_velocity is the skater's: heading {skater_heading}, the skater went {}",
        heading_of(went)
    );
    assert!(apart(walking.heading, skater_heading) < 0.2, "the camera follows the skater: {walking:?}");
    assert!(apart(walking.heading, before) > 0.5, "it turned with the skater, away from where it was");
    if speed(walking.board) > 1.0 && apart(heading_of(walking.board), skater_heading) > 0.5 {
        assert!(
            apart(walking.heading, heading_of(walking.board)) > 0.4,
            "the camera does not follow the board rolling its own way: {walking:?}"
        );
    }
    all.extend(walk);

    // stood still: after a second it comes round behind the facing
    let still = hold(&mut engine, &mut follow, Pad::NEUTRAL, 120);
    let stood = *still.last().unwrap();
    eprintln!(
        "stood: the camera {:.3}, facing {:.3}, the skater {:.2} m/s",
        stood.heading,
        heading_of(stood.facing),
        speed(stood.skater)
    );
    assert!(speed(stood.skater) < 1.0, "standing: {} m/s", speed(stood.skater));
    assert!(apart(stood.heading, heading_of(stood.facing)) < 0.2, "lined up behind the facing: {stood:?}");
    all.extend(still);

    // a fifth of the way a tick at most: no snap, no spin
    for w in all.windows(2) {
        assert!(apart(w[0].heading, w[1].heading) < 0.65, "the camera snapped: {:?} to {:?}", w[0], w[1]);
    }
}

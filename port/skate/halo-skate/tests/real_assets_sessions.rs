//! Several skaters in one process (lib.rs's sessions; the host's bots'
//! skaters, port/skate/README.md, "Several skaters"), with the real converted
//! Skate 3 data, through the C interface the way the game drives it: each
//! session made and loaded beside the others, what each costs in memory, and
//! that they keep apart: two sessions given the same ride from the same spot
//! ride it alike while a third rides something else, each keeps its own hits
//! and sounds, and session 0 (this machine's player's) is untouched.
//!
//! Skipped unless `HALO_SKATE_TEST_ASSETS` names the converted `assets`
//! folder; run with `--release`.

mod common;

use common::*;
use halo_skate::*;
use halo_skate::obstacles::{KIND_BIPED, SHAPE_CAPSULE};
use std::ffi::{CStr, CString};
use std::time::{Duration, Instant};

/// The sessions the test makes (session 0 is left alone).
const SESSIONS: [i32; 3] = [1, 2, 3];
/// The most an extra session may cost (MB of resident memory): one costs
/// about 13 on i686 (the animation banks and the board are shared).
const MOST_PER_SESSION_MB: u64 = 64;

/// This process's resident memory, MB (0 where /proc is not).
fn resident_mb() -> u64 {
    std::fs::read_to_string("/proc/self/status")
        .ok()
        .and_then(|text| {
            text.lines()
                .find(|l| l.starts_with("VmRSS:"))
                .and_then(|l| l.split_whitespace().nth(1)?.parse::<u64>().ok())
        })
        .map_or(0, |kb| kb / 1024)
}

fn session_error(session: i32) -> String {
    unsafe { CStr::from_ptr(halo_skate_error_in(session)) }.to_string_lossy().into_owned()
}

fn pad(buttons: u16, left: [i16; 2], right: [i16; 2]) -> HaloSkatePad {
    HaloSkatePad { buttons, triggers: [0, 0], left, right }
}

/// One game tick of `session` with `pad` held, waiting for its pose (the
/// tests do; the game goes on with the last).
fn step(session: i32, pad: &HaloSkatePad, frame: &mut HaloSkateFrame) -> Snapshot {
    let mut code = unsafe { halo_skate_step_in(session, pad, GAME_TICK, frame) };
    if code == 1 {
        code = unsafe { halo_skate_await_step_in(session, 30_000, frame) };
    }
    assert!(code == 0 || code == 2, "session {session}'s step: {code} ({})", session_error(session));
    Snapshot::of(code, frame)
}

/// The first ride: pushes, a carve and an ollie (the network test's
/// scripted pad, skate.c's skate_test_pad).
fn ride(tick: u32) -> HaloSkatePad {
    let carve = ((tick as f32) * 0.05).sin() * 8000.0;
    let buttons = if tick % 60 < 15 { A } else { 0 };
    let right = match tick % 120 {
        100..=107 => [0, -FULL],
        108..=109 => [0, FULL],
        _ => [0, 0],
    };
    pad(buttons, [carve as i16, 0], right)
}

/// The other ride: pushing hard, steering left.
fn other_ride(tick: u32) -> HaloSkatePad {
    pad(if tick % 30 < 15 { A } else { 0 }, [-20000, 0], [0, 0])
}

#[test]
fn sessions_ride_apart() {
    let Some(assets) = assets("sessions_ride_apart") else { return };
    let (_, log) = logged(|| {
        let root = CString::new(assets.to_string_lossy().into_owned()).unwrap();
        let triangles = TestMap::triangles();
        let count = (triangles.len() / 9) as i32;
        let before = resident_mb();
        // each session made and loaded in turn, to measure each
        let mut costs = Vec::new();
        for session in SESSIONS {
            let started = Instant::now();
            let was = resident_mb();
            assert_eq!(unsafe { halo_skate_preload_in(session, root.as_ptr()) }, 0, "session {session}'s preload");
            wait("a preload", Duration::from_secs(600), || halo_skate_preloading_in(session) == 0);
            assert_eq!(unsafe { halo_skate_load_in(session, root.as_ptr(), triangles.as_ptr(), count) }, 0);
            wait("a map", Duration::from_secs(300), || halo_skate_state_in(session) != 1);
            assert_eq!(halo_skate_state_in(session), 2, "session {session}: {}", session_error(session));
            let cost = resident_mb().saturating_sub(was);
            eprintln!(
                "session {session} made and loaded in {:.1} s, {cost} MB more resident ({} MB in all)",
                started.elapsed().as_secs_f32(),
                resident_mb()
            );
            costs.push(cost);
        }
        eprintln!("{} sessions: {} MB more resident than before", SESSIONS.len(), resident_mb().saturating_sub(before));
        // (the first pays for the animation banks and the board, once)
        for (session, cost) in SESSIONS.iter().zip(&costs).skip(1) {
            assert!(*cost <= MOST_PER_SESSION_MB, "session {session} cost {cost} MB");
        }
        // session 0 was not touched
        assert_eq!(halo_skate_state(), 0, "session 0 has a map: {}", error());

        // 1 and 2 on the open lane at the same spot, 3 beside them
        let mut frames = [zeroed_frame(), zeroed_frame(), zeroed_frame()];
        let spot = metres([0.0, TestMap::OPEN, 0.0]);
        let beside = metres([0.0, TestMap::OPEN + 10.0, 0.0]);
        for (i, session) in SESSIONS.iter().enumerate() {
            let at = if *session == 3 { beside } else { spot };
            assert_eq!(unsafe { halo_skate_activate_in(*session, at.as_ptr(), 0.0, &mut frames[i]) }, 0);
        }
        let mut rides: [Vec<Snapshot>; 3] = Default::default();
        // each session's sounds, taken each tick as the game takes them
        // (skate_network.c), and session 0's, which none of them is
        let mut sounds: [Vec<i32>; 4] = Default::default();
        let mut events = [HaloSkateSoundEvent::default(); 16];
        let mut loops = HaloSkateSoundLoops::default();
        let started = Instant::now();
        for tick in 0..300u32 {
            // (as the game steps its bots' skaters: each session's step
            // begun, then each one's pose taken, the workers side by side)
            let pads = [ride(tick), ride(tick), other_ride(tick)];
            for (i, session) in SESSIONS.iter().enumerate() {
                assert_eq!(unsafe { halo_skate_step_begin_in(*session, &pads[i], GAME_TICK) }, 0);
            }
            for (i, session) in SESSIONS.iter().enumerate() {
                let code = unsafe { halo_skate_await_step_in(*session, 30_000, &mut frames[i]) };
                assert!(code == 0 || code == 2, "session {session}'s step: {code} ({})", session_error(*session));
                rides[i].push(Snapshot::of(code, &frames[i]));
            }
            for (i, session) in [1, 2, 3, 0].into_iter().enumerate() {
                let count = unsafe { halo_skate_sound_take_in(session, events.as_mut_ptr(), 16, &mut loops) };
                sounds[i].extend(events[..count.max(0) as usize].iter().map(|e| e.sound));
            }
        }
        eprintln!(
            "300 game ticks of 3 sessions side by side in {:.1} s ({:.2} ms a game tick)",
            started.elapsed().as_secs_f32(),
            started.elapsed().as_secs_f64() * 1000.0 / 300.0
        );
        // 1 and 2 rode alike, all the way
        let mut furthest = 0.0f32;
        for (a, b) in rides[0].iter().zip(&rides[1]) {
            assert!(a.is_finite() && b.is_finite());
            let apart = length([a.position[0] - b.position[0], a.position[1] - b.position[1], a.position[2] - b.position[2]]);
            furthest = furthest.max(apart);
            assert_eq!(a.state, b.state, "sessions 1 and 2 parted at tick {}", a.tick);
        }
        assert!(furthest < 0.001, "sessions 1 and 2 rode {furthest} world units apart");
        let travelled = |ride: &[Snapshot]| {
            let (a, b) = (ride.first().unwrap().position, ride.last().unwrap().position);
            length([b[0] - a[0], b[1] - a[1], 0.0]) * METRES
        };
        eprintln!(
            "sessions 1 and 2 rode {:.1} m (at most {furthest} world units apart), session 3 {:.1} m",
            travelled(&rides[0]),
            travelled(&rides[2])
        );
        assert!(travelled(&rides[0]) > 10.0, "session 1 rode {:.1} m", travelled(&rides[0]));
        assert!(travelled(&rides[2]) > 10.0, "session 3 rode {:.1} m", travelled(&rides[2]));
        // (3 steered left, away from them)
        let last = |i: usize| rides[i].last().unwrap().position;
        assert!(length([last(2)[0] - last(0)[0], last(2)[1] - last(0)[1], 0.0]) > 1.0);

        // each kept its own sounds (1 and 2 the same); session 0 none
        eprintln!(
            "sounds kept: session 1 {:?}, session 2 {:?}, session 3 {:?}, session 0 {:?}",
            sounds[0], sounds[1], sounds[2], sounds[3]
        );
        assert!(!sounds[0].is_empty(), "session 1 kept no sounds");
        assert_eq!(sounds[0], sounds[1], "sessions 1 and 2 kept other sounds");
        assert!(sounds[3].is_empty(), "session 0 kept sounds it never made");

        // a held obstacle in session 3's way only: its hit is session 3's
        let ahead = rides[2].last().unwrap().clone();
        let velocity = ahead.velocity;
        let speed = length(velocity);
        let along = velocity.map(|v| v / speed.max(1e-6));
        let mut obstacle: HaloSkateObstacle = unsafe { std::mem::zeroed() };
        obstacle.kind = KIND_BIPED;
        obstacle.shape = SHAPE_CAPSULE;
        obstacle.center = [ahead.position[0] + along[0] * 0.6, ahead.position[1] + along[1] * 0.6, ahead.position[2] + 0.3];
        obstacle.up = [0.0, 0.0, 1.0];
        obstacle.half_extents = [0.2, 0.0, 0.35];
        obstacle.tag = 77;
        obstacle.held = 1;
        assert_eq!(unsafe { halo_skate_set_obstacles_in(3, &obstacle, 1, std::ptr::null(), 0) }, 1);
        let mut hit = HaloSkateHeldHit::default();
        let mut told = 0;
        let started = Instant::now();
        for tick in 300..360u32 {
            // (one after the other, as halo_skate_step does)
            step(1, &ride(tick), &mut frames[0]);
            step(3, &other_ride(tick), &mut frames[2]);
            if unsafe { halo_skate_take_held_hit_in(3, &mut hit) } == 1 && hit.touched != 0 {
                told += 1;
                assert_eq!(hit.tag, 77);
            }
            assert_ne!(unsafe { halo_skate_take_held_hit_in(1, &mut hit) }, 1, "session 1 told of session 3's hit");
        }
        eprintln!(
            "session 3 told of {told} ticks touching its obstacle; 60 game ticks of 2 sessions one after the other \
             in {:.2} s ({:.2} ms a session's game tick)",
            started.elapsed().as_secs_f32(),
            started.elapsed().as_secs_f64() * 1000.0 / 120.0
        );
        assert!(told > 0, "session 3 never told of its hit");
        for session in SESSIONS {
            halo_skate_close_in(session);
            assert_eq!(halo_skate_state_in(session), 0);
        }
    });
    assert!(log.iter().any(|l| l.starts_with("halo-skate (session 2): ")), "no line said it was session 2's");
}

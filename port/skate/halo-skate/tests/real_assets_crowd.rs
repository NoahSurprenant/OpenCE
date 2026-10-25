//! A crowd of skaters on a real Halo level - the host's bots' skaters
//! (port/skate/README.md, "Several skaters") - through the C interface the
//! way the game drives them: Blood Gulch's collision read from the owner's
//! own map (`common/halo_map.rs`), several sessions made from the real
//! converted Skate 3 data, riding fixed rides from fixed spots.
//!
//! 1. Stepped side by side on the engine's pool (each tick every session's
//!    step begun, then each one's pose taken, while the game looks at them
//!    in between as it does), they ride exactly as the same sessions
//!    stepped one at a time do, bit for bit: the pool changes when a step
//!    runs, never what it gives; and on a machine with several processors
//!    side by side is quicker.
//! 2. The engine's inline fused multiply-add (`skate_core::fused`, which
//!    replaced the library's `fmaf` call: most of a step's time on 32-bit
//!    x86) gives the library's results bit for bit.
//!
//! Skipped unless `HALO_SKATE_TEST_ASSETS` and `HALO_SKATE_TEST_HALO_MAPS`
//! are set (the fused test runs anyway); run with `--release`.
//! `HALO_SKATE_TEST_CROWD` sets how many sessions ride each way (4 by
//! default) and `HALO_SKATE_TEST_CROWD_TICKS` for how many ticks (300).

mod common;

use common::halo_map::{HaloMap, halo_maps};
use common::*;
use halo_skate::*;
use std::ffi::{CStr, CString};
use std::time::{Duration, Instant};

fn session_error(session: i32) -> String {
    unsafe { CStr::from_ptr(halo_skate_error_in(session)) }.to_string_lossy().into_owned()
}

fn setting(name: &str, default: usize) -> usize {
    std::env::var(name).ok().and_then(|v| v.parse().ok()).unwrap_or(default)
}

/// Flat open spots of the level, `count` of them, the same each run: the
/// middles of its largest floor triangles, one in each 6 world unit cell.
fn spots(triangles: &[f32], count: usize) -> Vec<[f32; 3]> {
    let mut cells: std::collections::BTreeMap<(i32, i32, i32), ([f32; 3], f32)> = Default::default();
    for t in triangles.chunks_exact(9) {
        let [a, b, c] = [[t[0], t[1], t[2]], [t[3], t[4], t[5]], [t[6], t[7], t[8]]];
        let (ab, ac) = ([b[0] - a[0], b[1] - a[1], b[2] - a[2]], [c[0] - a[0], c[1] - a[1], c[2] - a[2]]);
        let n = [ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0]];
        let area = length(n) / 2.0;
        if area < 0.5 || n[2] / length(n) < 0.97 {
            continue;
        }
        let middle = [0, 1, 2].map(|k| (a[k] + b[k] + c[k]) / 3.0);
        let key = ((middle[0] / 6.0).floor() as i32, (middle[1] / 6.0).floor() as i32, (middle[2] / 6.0).floor() as i32);
        let cell = cells.entry(key).or_insert((middle, 0.0));
        if area > cell.1 {
            *cell = (middle, area);
        }
    }
    let all: Vec<[f32; 3]> = cells.into_values().map(|(c, _)| c).collect();
    assert!(!all.is_empty(), "no open floor on the level");
    (0..count).map(|i| all[(i * 7919) % all.len()]).collect()
}

/// Rider `rider`'s pad at `tick`: pushes, carves and ollies, each rider its
/// own phase of them.
fn ride(tick: u32, rider: u32) -> HaloSkatePad {
    let t = tick + rider * 37;
    let carve = ((t as f32) * 0.05).sin() * 12000.0;
    let buttons = if t % 60 < 15 { A } else { 0 };
    let right = match t % 120 {
        100..=107 => [0, -FULL],
        108..=109 => [0, FULL],
        _ => [0, 0],
    };
    HaloSkatePad { buttons, triggers: [0, 0], left: [carve as i16, 0], right }
}

/// A frame as bits, to compare rides exactly.
fn bits(frame: &HaloSkateFrame) -> Vec<u32> {
    frame.position.iter().chain(frame.velocity.iter()).chain(frame.forward.iter()).map(|v| v.to_bits()).collect()
}

#[test]
fn a_crowd_rides_alike_side_by_side_and_one_at_a_time() {
    let Some(assets) = assets("a_crowd_rides_alike_side_by_side_and_one_at_a_time") else { return };
    let Some(path) = halo_maps("a_crowd_rides_alike_side_by_side_and_one_at_a_time", "bloodgulch") else { return };
    let map = HaloMap::read(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    let triangles = map.bsps[0].clone();
    let count = (triangles.len() / 9) as i32;
    let riders = setting("HALO_SKATE_TEST_CROWD", 4) as i32;
    let ticks = setting("HALO_SKATE_TEST_CROWD_TICKS", 300) as u32;
    // sessions 1..=riders ride side by side, riders+1..=2*riders the same
    // rides one at a time
    let together: Vec<i32> = (1..=riders).collect();
    let alone: Vec<i32> = (riders + 1..=2 * riders).collect();
    let root = CString::new(assets.to_string_lossy().into_owned()).unwrap();
    let started = Instant::now();
    for &session in together.iter().chain(&alone) {
        assert_eq!(unsafe { halo_skate_preload_in(session, root.as_ptr()) }, 0, "session {session}'s preload");
    }
    for &session in together.iter().chain(&alone) {
        wait("a preload", Duration::from_secs(600), || halo_skate_preloading_in(session) == 0);
        assert_eq!(unsafe { halo_skate_load_in(session, root.as_ptr(), triangles.as_ptr(), count) }, 0);
    }
    for &session in together.iter().chain(&alone) {
        wait("a map", Duration::from_secs(300), || halo_skate_state_in(session) != 1);
        assert_eq!(halo_skate_state_in(session), 2, "session {session}: {}", session_error(session));
    }
    eprintln!("{} sessions made and loaded in {:.1} s", 2 * riders, started.elapsed().as_secs_f32());
    let places = spots(&triangles, riders as usize);
    let mut frames: Vec<HaloSkateFrame> = (0..2 * riders).map(|_| zeroed_frame()).collect();
    for (i, &session) in together.iter().chain(&alone).enumerate() {
        let rider = i % riders as usize;
        let at = places[rider].map(|v| v * METRES);
        let code = unsafe { halo_skate_activate_in(session, at.as_ptr(), rider as f32, &mut frames[i]) };
        assert_eq!(code, 0, "session {session} on the board: {}", session_error(session));
    }
    let mut rides: Vec<Vec<Vec<u32>>> = (0..2 * riders).map(|_| Vec::new()).collect();
    let (mut side_by_side, mut one_at_a_time) = (Duration::ZERO, Duration::ZERO);
    let mut recovered = 0;
    for tick in 0..ticks {
        // side by side: every step begun, the game looking at the sessions
        // meanwhile (as its other calls do: a pose that comes in then is not
        // the skater's until taken), then every pose taken
        let begun = Instant::now();
        for (i, &session) in together.iter().enumerate() {
            assert_eq!(unsafe { halo_skate_step_begin_in(session, &ride(tick, i as u32), GAME_TICK) }, 0);
        }
        std::thread::sleep(Duration::from_micros(200));
        for &session in &together {
            assert_eq!(halo_skate_state_in(session), 2, "session {session}: {}", session_error(session));
        }
        for (i, &session) in together.iter().enumerate() {
            let code = unsafe { halo_skate_await_step_in(session, 30_000, &mut frames[i]) };
            assert!(code == 0 || code == 2, "session {session}'s step: {code} ({})", session_error(session));
            recovered += (code == 2) as u32;
            rides[i].push(bits(&frames[i]));
        }
        side_by_side += begun.elapsed();
        // one at a time
        let begun = Instant::now();
        for (i, &session) in alone.iter().enumerate() {
            let k = riders as usize + i;
            assert_eq!(unsafe { halo_skate_step_begin_in(session, &ride(tick, i as u32), GAME_TICK) }, 0);
            let code = unsafe { halo_skate_await_step_in(session, 30_000, &mut frames[k]) };
            assert!(code == 0 || code == 2, "session {session}'s step: {code} ({})", session_error(session));
            rides[k].push(bits(&frames[k]));
        }
        one_at_a_time += begun.elapsed();
    }
    let threads = unsafe { halo_skate_pool_load(std::ptr::null_mut(), std::ptr::null_mut()) };
    let per_tick = |d: Duration| d.as_secs_f64() * 1000.0 / f64::from(ticks);
    eprintln!(
        "{riders} riders, {ticks} ticks: side by side {:.2} ms a tick, one at a time {:.2} ms, on the pool's {threads} \
         threads; {recovered} steps recovered",
        per_tick(side_by_side),
        per_tick(one_at_a_time)
    );
    for i in 0..riders as usize {
        let k = riders as usize + i;
        let first = rides[i].iter().zip(&rides[k]).position(|(a, b)| a != b);
        assert!(
            first.is_none(),
            "rider {i} rode otherwise side by side (session {}) than alone (session {}) from tick {first:?}",
            together[i],
            alone[i]
        );
    }
    // (and they rode: rides that went nowhere would prove nothing)
    let ridden: Vec<f32> = (0..riders as usize)
        .map(|i| {
            rides[i].windows(2).map(|w| {
                let d = [0, 1, 2].map(|k| f32::from_bits(w[1][k]) - f32::from_bits(w[0][k]));
                length(d)
            }).sum()
        })
        .collect();
    eprintln!("ridden (world units): {ridden:?}");
    assert!(ridden.iter().all(|&d| d > 3.0), "a rider hardly rode: {ridden:?}");
    if threads >= 3 && riders >= 3 {
        assert!(
            side_by_side.as_secs_f64() < one_at_a_time.as_secs_f64() * 0.8,
            "side by side on {threads} threads was not quicker: {side_by_side:?} against {one_at_a_time:?}"
        );
    }
    for &session in together.iter().chain(&alone) {
        halo_skate_close_in(session);
    }
}

#[test]
fn the_fused_multiply_add_is_the_librarys() {
    let mut state = 0x9E37_79B9_7F4A_7C15u64;
    let mut next = move || {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        state
    };
    for i in 0..2_000_000u32 {
        let r = next();
        let (x, y) = if i % 2 == 0 {
            (f32::from_bits(r as u32), f32::from_bits((r >> 32) as u32))
        } else {
            // (the engine's sizes)
            ((r as u32 as f32 / u32::MAX as f32 - 0.5) * 20.0, ((r >> 32) as u32 as f32 / u32::MAX as f32 - 0.5) * 20.0)
        };
        let z = match i % 5 {
            0 => f32::from_bits(next() as u32),
            // (cancelling: z near -x*y)
            1 => -(x * y),
            2 => -(x * y) * (1.0 + f32::EPSILON),
            // (a small z beside a large product: ties in the low bits)
            3 => (x * y) * f32::EPSILON / 2.0,
            _ => (next() as u32 as f32 / u32::MAX as f32 - 0.5) * 400.0,
        };
        let want = x.mul_add(y, z);
        let got = skate_core::fused::fused_f32(x, y, z);
        assert!(
            want.to_bits() == got.to_bits() || (want.is_nan() && got.is_nan()),
            "{x:e} * {y:e} + {z:e}: {want:e} ({:#x}) but {got:e} ({:#x})",
            want.to_bits(),
            got.to_bits()
        );
    }
}

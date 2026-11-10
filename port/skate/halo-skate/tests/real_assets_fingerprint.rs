//! A crowd of the host's bots' skaters on a real Halo level, driven as the
//! game drives them (port/linux/game/skate.c, "many bots on boards"): each
//! tick every rider's obstacles sent (the others near it, their bodies and
//! boards, held for the host's verdict), every step begun on the engine's
//! pool, then each one taken with its biped's nodes posed and its held hits
//! told - and now and then a blast's push, a host's bail and Infection's
//! turning. Everything the engine gives back is hashed, bit for bit: the
//! frames, the posed nodes, the boards' placements and the held hits.
//!
//! The hash is the crowd's fingerprint. A change that only makes the engine
//! cheaper (the same arithmetic, less of it) leaves it as it was: run this
//! before and after such a change and compare the two lines it prints
//! ("fingerprint ..."). `HALO_SKATE_TEST_FINGERPRINT` (a hash, as printed)
//! makes the test check it. It also prints what a step cost, on the pool and
//! on one thread, for comparing the engine's speed.
//!
//! Skipped unless `HALO_SKATE_TEST_ASSETS` and `HALO_SKATE_TEST_HALO_MAPS`
//! are set; run with `--release`. `HALO_SKATE_TEST_FINGERPRINT_RIDERS` sets
//! how many ride (12 by default), `HALO_SKATE_TEST_FINGERPRINT_TICKS` for
//! how many ticks (600: 20 seconds of the game).

mod common;

use common::halo_map::{HaloMap, halo_maps};
use common::*;
use halo_skate::obstacles::{KIND_SKATER, SHAPE_CAPSULE};
use halo_skate::*;
use std::ffi::{CStr, CString, c_char};
use std::time::{Duration, Instant};

fn session_error(session: i32) -> String {
    unsafe { CStr::from_ptr(halo_skate_error_in(session)) }.to_string_lossy().into_owned()
}

fn setting(name: &str, default: usize) -> usize {
    std::env::var(name).ok().and_then(|v| v.parse().ok()).unwrap_or(default)
}

/// The processor time this process has had (all its threads), seconds, from
/// /proc (Linux; 0 elsewhere): what the steps cost, whatever else the
/// machine was doing meanwhile.
fn cpu_seconds() -> f64 {
    let Ok(stat) = std::fs::read_to_string("/proc/self/stat") else { return 0.0 };
    let Some(rest) = stat.rfind(')').map(|i| &stat[i + 2..]) else { return 0.0 };
    let fields: Vec<&str> = rest.split_whitespace().collect();
    let ticks = |i: usize| fields.get(i).and_then(|v| v.parse::<f64>().ok()).unwrap_or(0.0);
    // (utime and stime, in the kernel's 100ths of a second)
    (ticks(11) + ticks(12)) / 100.0
}

/// FNV-1a over bytes, as debug.skate_fingerprint hashes the game's.
struct Fingerprint(u64);

impl Fingerprint {
    fn add(&mut self, bytes: &[u8]) {
        for &b in bytes {
            self.0 = (self.0 ^ u64::from(b)).wrapping_mul(0x0100_0000_01b3);
        }
    }
    fn floats(&mut self, values: &[f32]) {
        for v in values {
            self.add(&v.to_bits().to_le_bytes());
        }
    }
    fn ints(&mut self, values: &[i32]) {
        for v in values {
            self.add(&v.to_le_bytes());
        }
    }
    fn frame(&mut self, f: &HaloSkateFrame) {
        for v in [f.position, f.forward, f.up, f.velocity, f.skater_velocity, f.facing] {
            self.floats(&v);
        }
        self.ints(&[f.off_board, f.has_camera, f.tick as i32]);
        for v in [f.camera_position, f.camera_forward, f.camera_up] {
            self.floats(&v);
        }
        self.floats(&[f.camera_field_of_view]);
        let state: Vec<u8> = f.state.iter().take_while(|&&c| c != 0).map(|&c| c as u8).collect();
        self.add(&state);
    }
}

/// The largest open floor of the level, as the crowd test finds them: the
/// middle of the largest flat triangle.
fn open_floor(triangles: &[f32]) -> [f32; 3] {
    let mut best = ([0.0; 3], 0.0);
    for t in triangles.chunks_exact(9) {
        let [a, b, c] = [[t[0], t[1], t[2]], [t[3], t[4], t[5]], [t[6], t[7], t[8]]];
        let (ab, ac) = ([b[0] - a[0], b[1] - a[1], b[2] - a[2]], [c[0] - a[0], c[1] - a[1], c[2] - a[2]]);
        let n = [ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0]];
        let area = length(n) / 2.0;
        if n[2] / length(n) >= 0.97 && area > best.1 {
            best = ([0, 1, 2].map(|k| (a[k] + b[k] + c[k]) / 3.0), area);
        }
    }
    assert!(best.1 > 0.0, "no open floor on the level");
    best.0
}

/// Rider `rider`'s pad at `tick`: pushes, carves, ollies and flip flicks,
/// now and then stepping off and back on, each rider its own phase.
fn ride(tick: u32, rider: u32) -> HaloSkatePad {
    let t = tick + rider * 53;
    let carve = ((t as f32) * (0.03 + rider as f32 * 0.004)).sin() * 20000.0;
    let mut buttons = if t % 50 < 12 { A } else { 0 };
    // (off the board and on again, once in a while)
    if t % 400 == 390 {
        buttons |= Y;
    }
    let right = match t % 90 {
        70..=75 => [0, -FULL],
        76..=77 => [if rider % 2 == 0 { FULL } else { -FULL }, FULL],
        _ => [0, 0],
    };
    HaloSkatePad { buttons, triggers: [0, 0], left: [carve as i16, 6000], right }
}

/// The rig the game describes for its biped, by hand: the `bip01` nodes
/// the retarget follows, each node's name, parent and position (world units).
const BIPED: [(&str, i16, [f32; 3]); 19] = [
    ("bip01 pelvis", -1, [0.0, 0.0, 0.33]),
    ("bip01 spine", 0, [0.0, 0.0, 0.36]),
    ("bip01 spine1", 1, [0.0, 0.0, 0.44]),
    ("bip01 neck", 2, [0.0, 0.0, 0.56]),
    ("bip01 head", 3, [0.0, 0.0, 0.59]),
    ("bip01 l clavicle", 2, [0.0, 0.03, 0.52]),
    ("bip01 l upperarm", 5, [0.0, 0.08, 0.52]),
    ("bip01 l forearm", 6, [0.0, 0.08, 0.43]),
    ("bip01 l hand", 7, [0.0, 0.08, 0.34]),
    ("bip01 r clavicle", 2, [0.0, -0.03, 0.52]),
    ("bip01 r upperarm", 9, [0.0, -0.08, 0.52]),
    ("bip01 r forearm", 10, [0.0, -0.08, 0.43]),
    ("bip01 r hand", 11, [0.0, -0.08, 0.34]),
    ("bip01 l thigh", 0, [0.0, 0.04, 0.31]),
    ("bip01 l calf", 13, [0.0, 0.04, 0.17]),
    ("bip01 l foot", 14, [0.0, 0.04, 0.03]),
    ("bip01 r thigh", 0, [0.0, -0.04, 0.31]),
    ("bip01 r calf", 16, [0.0, -0.04, 0.17]),
    ("bip01 r foot", 17, [0.0, -0.04, 0.03]),
];

fn describe_biped(session: i32) -> i32 {
    let mut names = vec![0 as c_char; BIPED.len() * 32];
    let mut parents = Vec::new();
    let mut inverse = Vec::new();
    for (i, (name, parent, at)) in BIPED.iter().enumerate() {
        for (slot, byte) in names[i * 32..].iter_mut().zip(name.bytes()) {
            *slot = byte as c_char;
        }
        parents.push(*parent);
        inverse.extend([1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, -at[0], -at[1], -at[2]]);
    }
    unsafe {
        halo_skate_set_skeleton_in(session, BIPED.len() as i32, names.as_ptr(), parents.as_ptr(), inverse.as_ptr())
    }
}

/// Rider `of`'s body as the others meet it (skate.c's skate_skater_obstacles:
/// a capsule from its feet to its head, held for the host's verdict), and its
/// board where its engine has it.
fn obstacles_of(of: usize, session: i32, frame: &HaloSkateFrame, out: &mut Vec<HaloSkateObstacle>) {
    let up = frame.up;
    let feet = frame.position;
    out.push(HaloSkateObstacle {
        kind: KIND_SKATER,
        shape: SHAPE_CAPSULE,
        center: [0, 1, 2].map(|k| feet[k] + up[k] * 0.3),
        forward: [1.0, 0.0, 0.0],
        left: [0.0, 1.0, 0.0],
        up,
        half_extents: [0.2, 0.0, 0.5],
        velocity: frame.velocity,
        tag: of as i32,
        held: 1,
        ..Default::default()
    });
    let mut placement = [0.0f32; 7];
    let mut board = HaloSkateObstacle::default();
    if unsafe { halo_skate_board_placement_in(session, placement.as_mut_ptr()) } == 0
        && unsafe { halo_skate_board_obstacle(placement.as_ptr(), &mut board) } == 0
    {
        board.tag = of as i32;
        board.held = 1;
        board.velocity = frame.velocity;
        out.push(board);
    }
}

#[test]
fn a_crowds_rides_fingerprint() {
    let Some(assets) = assets("a_crowds_rides_fingerprint") else { return };
    let Some(path) = halo_maps("a_crowds_rides_fingerprint", "bloodgulch") else { return };
    let map = HaloMap::read(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    let triangles = map.bsps[0].clone();
    let count = (triangles.len() / 9) as i32;
    let riders = setting("HALO_SKATE_TEST_FINGERPRINT_RIDERS", 12);
    let ticks = setting("HALO_SKATE_TEST_FINGERPRINT_TICKS", 600) as u32;
    let sessions: Vec<i32> = (1..=riders as i32).collect();
    let root = CString::new(assets.to_string_lossy().into_owned()).unwrap();
    for &session in &sessions {
        assert_eq!(unsafe { halo_skate_preload_in(session, root.as_ptr()) }, 0, "session {session}'s preload");
    }
    for &session in &sessions {
        wait("a preload", Duration::from_secs(600), || halo_skate_preloading_in(session) == 0);
        assert_eq!(unsafe { halo_skate_load_in(session, root.as_ptr(), triangles.as_ptr(), count) }, 0);
    }
    for &session in &sessions {
        wait("a map", Duration::from_secs(300), || halo_skate_state_in(session) != 1);
        assert_eq!(halo_skate_state_in(session), 2, "session {session}: {}", session_error(session));
        assert!(describe_biped(session) > 0, "session {session}'s biped");
    }
    // a ring around the open floor, each facing the middle: they meet
    let middle = open_floor(&triangles);
    let mut frames: Vec<HaloSkateFrame> = sessions.iter().map(|_| zeroed_frame()).collect();
    for (i, &session) in sessions.iter().enumerate() {
        let angle = i as f32 * std::f32::consts::TAU / riders as f32;
        let at = [middle[0] + 3.0 * angle.cos(), middle[1] + 3.0 * angle.sin(), middle[2] + 0.05];
        let yaw = angle + std::f32::consts::PI;
        let code = unsafe { halo_skate_activate_in(session, at.as_ptr(), yaw, &mut frames[i]) };
        assert_eq!(code, 0, "session {session} on the board: {}", session_error(session));
    }
    let mut fingerprint = Fingerprint(0xcbf2_9ce4_8422_2325);
    let mut nodes = vec![0.0f32; BIPED.len() * 13];
    let mut near = Vec::new();
    let (mut on_pool, mut one_at_a_time) = (Duration::ZERO, Duration::ZERO);
    let (mut held_hits, mut recovered, mut bails) = (0, 0, 0);
    let cpu_started = cpu_seconds();
    for tick in 0..ticks {
        // each rider's obstacles: the others within 4 world units, as the
        // game's skate_send_obstacles finds them
        for (i, &session) in sessions.iter().enumerate() {
            near.clear();
            for (j, &other) in sessions.iter().enumerate() {
                let d = [0, 1, 2].map(|k| frames[j].position[k] - frames[i].position[k]);
                if j != i && length(d) < 4.0 {
                    obstacles_of(j, other, &frames[j], &mut near);
                }
            }
            unsafe { halo_skate_set_obstacles_in(session, near.as_ptr(), near.len() as i32, std::ptr::null(), 0) };
            // (Infection's turning for some, a blast's push now and then)
            halo_skate_set_turn_scale_in(session, if i % 3 == 0 { 1.75 } else { 1.0 });
            if (tick + i as u32 * 31) % 240 == 120 {
                halo_skate_apply_impulse_in(session, 1.5, -0.5, 1.0);
            }
        }
        // every step begun on the pool, then taken, as the game takes them
        let begun = Instant::now();
        for (i, &session) in sessions.iter().enumerate() {
            assert_eq!(unsafe { halo_skate_step_begin_in(session, &ride(tick, i as u32), GAME_TICK) }, 0);
        }
        for (i, &session) in sessions.iter().enumerate() {
            let code = unsafe { halo_skate_await_step_in(session, 30_000, &mut frames[i]) };
            assert!(code == 0 || code == 2, "session {session}'s step: {code} ({})", session_error(session));
            recovered += (code == 2) as u32;
        }
        on_pool += begun.elapsed();
        for (i, &session) in sessions.iter().enumerate() {
            fingerprint.frame(&frames[i]);
            let written = unsafe { halo_skate_pose_nodes_in(session, nodes.as_mut_ptr(), BIPED.len() as i32) };
            fingerprint.ints(&[written]);
            fingerprint.floats(&nodes[..written.max(0) as usize * 13]);
            let mut placement = [0.0f32; 7];
            let placed = unsafe { halo_skate_board_placement_in(session, placement.as_mut_ptr()) };
            fingerprint.ints(&[placed]);
            fingerprint.floats(&placement);
            let mut held = HaloSkateHeldHit::default();
            if unsafe { halo_skate_take_held_hit_in(session, &mut held) } == 1 {
                held_hits += 1;
                fingerprint.ints(&[held.tag, held.kind, held.touched, held.bail_held, held.run_over]);
                fingerprint.floats(&[held.closing]);
                fingerprint.floats(&held.normal);
                // (the host's verdict: a hard hit bails)
                if held.bail_held != 0 || held.closing > 2.0 {
                    halo_skate_force_bail_in(session, held.closing);
                    bails += 1;
                }
            }
        }
    }
    let cpu = cpu_seconds() - cpu_started;
    // the same steps' cost on one thread: one rider a while longer alone
    let (session, rider) = (sessions[0], 0u32);
    for tick in ticks..ticks + 120 {
        let begun = Instant::now();
        assert_eq!(unsafe { halo_skate_step_begin_in(session, &ride(tick, rider), GAME_TICK) }, 0);
        let code = unsafe { halo_skate_await_step_in(session, 30_000, &mut frames[0]) };
        assert!(code == 0 || code == 2, "session {session}'s step: {code}");
        one_at_a_time += begun.elapsed();
    }
    let (mut steps, mut micros) = (0u64, 0u64);
    let threads = unsafe { halo_skate_pool_load(&mut steps, &mut micros) };
    let ridden: f32 = frames.iter().map(|f| length([0, 1, 2].map(|k| f.position[k] - middle[k]))).sum();
    eprintln!(
        "fingerprint {:016x}: {riders} riders, {ticks} ticks, {held_hits} held hits told, {bails} bails given, \
         {recovered} steps recovered",
        fingerprint.0
    );
    eprintln!(
        "a tick of {riders} steps on the pool's {threads} threads {:.3} ms; a step alone {:.3} ms; {steps} steps on \
         the pool, {:.3} ms each; {:.3} ms of processor time a step",
        on_pool.as_secs_f64() * 1000.0 / f64::from(ticks),
        one_at_a_time.as_secs_f64() * 1000.0 / 120.0,
        micros as f64 / 1000.0 / steps.max(1) as f64,
        cpu * 1000.0 / f64::from(ticks * riders as u32)
    );
    // (they rode, and met: a fingerprint of riders standing apart proves little)
    assert!(ridden > riders as f32 * 3.0, "the crowd hardly rode: {ridden} world units from the middle in all");
    assert!(held_hits > 0, "the riders never met");
    if let Ok(expected) = std::env::var("HALO_SKATE_TEST_FINGERPRINT") {
        assert_eq!(format!("{:016x}", fingerprint.0), expected.trim(), "the crowd rode otherwise");
    }
    for &session in &sessions {
        halo_skate_close_in(session);
    }
}

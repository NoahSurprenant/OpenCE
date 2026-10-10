//! Shared by the real-asset tests (`tests/real_assets*.rs`): the converted
//! Skate 3 data, a small map made of triangles here, and the C interface
//! driven the way the game drives it (`port/linux/game/skate.c`).
//!
//! The data is the owner's own, converted from Skate 3 (port/skate/README.md,
//! "What you need"), and is never part of this repository. Set
//! `HALO_SKATE_TEST_ASSETS` to its `assets` folder to run these tests; without
//! it each test says so and passes, as on GitHub's runners and in normal builds.
//!
//! The engine is slow unoptimised (a step can miss the game's 250 ms wait), so
//! run them with `--release`:
//!
//! ```text
//! HALO_SKATE_TEST_ASSETS=/path/to/skate-data/assets \
//!     cargo test --release -p halo-skate --test real_assets -- --nocapture
//! ```
#![allow(dead_code)]

use halo_skate::*;
use std::ffi::{CStr, CString, c_char};
use std::path::PathBuf;
use std::sync::{Mutex, MutexGuard};
use std::time::{Duration, Instant};

/// Metres in a world unit.
pub const METRES: f32 = 3.048;
/// One game tick: the game steps the engine at 30 Hz, two engine ticks each.
pub const GAME_TICK: f32 = 1.0 / 30.0;

pub const A: u16 = 0x1000;
pub const B: u16 = 0x2000;
pub const X: u16 = 0x4000;
pub const Y: u16 = 0x8000;
pub const FULL: i16 = 32767;

/// The tests share the one engine the C interface keeps: one at a time.
static ENGINE: Mutex<()> = Mutex::new(());
/// The engine's log lines since the last `take_log`.
static LOG: Mutex<Vec<String>> = Mutex::new(Vec::new());

unsafe extern "C" fn capture(line: *const c_char) {
    let line = unsafe { CStr::from_ptr(line) }.to_string_lossy().into_owned();
    eprintln!("    | {line}");
    LOG.lock().unwrap_or_else(|e| e.into_inner()).push(line);
}

/// The log lines since the last call.
pub fn take_log() -> Vec<String> {
    std::mem::take(&mut *LOG.lock().unwrap_or_else(|e| e.into_inner()))
}

/// The converted assets folder, or None (the test is skipped) when
/// `HALO_SKATE_TEST_ASSETS` is unset. A path that is set but missing fails
/// the test: that is a runner set up wrong, not a build without the data.
pub fn assets(test: &str) -> Option<PathBuf> {
    match std::env::var_os("HALO_SKATE_TEST_ASSETS") {
        Some(path) if !path.is_empty() => {
            let path = PathBuf::from(path);
            assert!(
                path.join("private").is_dir(),
                "HALO_SKATE_TEST_ASSETS={} is not a converted Skate 3 assets folder",
                path.display()
            );
            Some(path)
        }
        _ => {
            eprintln!("{test}: skipped, HALO_SKATE_TEST_ASSETS is not set");
            None
        }
    }
}

pub fn metres(p: [f32; 3]) -> [f32; 3] {
    p.map(|v| v / METRES)
}

/// The test map, in world units (Z up, a world unit 3.048 m), as the game
/// sends a level's collision: triangles wound counterclockwise seen from
/// outside. Everything stands on a flat floor at z = 0, and the features sit
/// in lanes along +X so that each test rides its own:
///
/// - y = 0 m: open floor, 200 m of it ahead (riding, the ollie, off the board)
/// - y = 30 m: a funbox: a 1 m kicker from x = 15 m to 19 m, a 4 m flat top
///   and a slope back down
/// - y = -30 m: a box ledge 1 m high and 10 m wide across the lane at
///   x = 20 m to 22 m (riding into it at speed is a bail)
/// - y = 60 m: a rail along the lane, 0.3 m high and 10 cm wide, from x = 10 m
///   to 30 m
/// - y = -50 m: a tilted pad 20 m square from x = 40 m, rising 4 cm a metre
///   along X and 1 cm along Y, in 2 m triangles (off the board on a slope)
pub struct TestMap;

impl TestMap {
    pub const OPEN: f32 = 0.0;
    pub const FUNBOX: f32 = 30.0;
    pub const LEDGE: f32 = -30.0;
    pub const LEDGE_START: f32 = 20.0;
    pub const RAIL: f32 = 60.0;
    pub const RAIL_START: f32 = 10.0;
    pub const RAIL_END: f32 = 30.0;
    pub const RAIL_HEIGHT: f32 = 0.3;
    pub const RAIL_WIDTH: f32 = 0.1;
    pub const TILT: f32 = -50.0;
    pub const TILT_START: f32 = 40.0;

    /// The triangles, nine floats each, world units.
    pub fn triangles() -> Vec<f32> {
        let mut t = Triangles::default();
        // the floor, in 10 m tiles: x from -50 to 200 m, y from -60 to 90 m
        let tile = 10.0;
        for i in -5..20 {
            for j in -6..9 {
                let (x0, y0) = (i as f32 * tile, j as f32 * tile);
                let (x1, y1) = (x0 + tile, y0 + tile);
                t.quad([x0, y0, 0.0], [x1, y0, 0.0], [x1, y1, 0.0], [x0, y1, 0.0]);
            }
        }
        // the funbox: kicker up, flat top, slope down, and its sides
        let (y0, y1) = (Self::FUNBOX - 5.0, Self::FUNBOX + 5.0);
        let profile = [(15.0, 0.0), (19.0, 1.0), (23.0, 1.0), (27.0, 0.0)];
        for w in profile.windows(2) {
            let ((xa, za), (xb, zb)) = (w[0], w[1]);
            t.quad([xa, y0, za], [xb, y0, zb], [xb, y1, zb], [xa, y1, za]);
        }
        // sides: the near (y0) side faces -Y, the far side +Y
        t.quad([19.0, y0, 0.0], [23.0, y0, 0.0], [23.0, y0, 1.0], [19.0, y0, 1.0]);
        t.tri([15.0, y0, 0.0], [19.0, y0, 0.0], [19.0, y0, 1.0]);
        t.tri([23.0, y0, 0.0], [27.0, y0, 0.0], [23.0, y0, 1.0]);
        t.quad([19.0, y1, 0.0], [19.0, y1, 1.0], [23.0, y1, 1.0], [23.0, y1, 0.0]);
        t.tri([15.0, y1, 0.0], [19.0, y1, 1.0], [19.0, y1, 0.0]);
        t.tri([23.0, y1, 0.0], [23.0, y1, 1.0], [27.0, y1, 0.0]);
        // the box ledge across its lane
        t.cuboid(
            [Self::LEDGE_START, Self::LEDGE - 5.0, 0.0],
            [Self::LEDGE_START + 2.0, Self::LEDGE + 5.0, 1.0],
        );
        // the rail along its lane
        let half = Self::RAIL_WIDTH / 2.0;
        t.cuboid(
            [Self::RAIL_START, Self::RAIL - half, 0.0],
            [Self::RAIL_END, Self::RAIL + half, Self::RAIL_HEIGHT],
        );
        // the tilted pad: a plane leaning a little both ways, 10 cm up at its
        // low corner, in small triangles, as a level's ground is
        let (x0, y0) = (Self::TILT_START, Self::TILT - 10.0);
        let z = |x: f32, y: f32| 0.1 + 0.04 * (x - x0) + 0.01 * (y - y0);
        for i in 0..10 {
            for j in 0..10 {
                let (xa, ya) = (x0 + i as f32 * 2.0, y0 + j as f32 * 2.0);
                let (xb, yb) = (xa + 2.0, ya + 2.0);
                t.quad([xa, ya, z(xa, ya)], [xb, ya, z(xb, ya)], [xb, yb, z(xb, yb)], [xa, yb, z(xa, yb)]);
            }
        }
        let (x1, y1) = (x0 + 20.0, y0 + 20.0);
        t.quad([x0, y0, 0.0], [x1, y0, 0.0], [x1, y0, z(x1, y0)], [x0, y0, z(x0, y0)]); // -Y
        t.quad([x1, y1, 0.0], [x0, y1, 0.0], [x0, y1, z(x0, y1)], [x1, y1, z(x1, y1)]); // +Y
        t.quad([x0, y1, 0.0], [x0, y0, 0.0], [x0, y0, z(x0, y0)], [x0, y1, z(x0, y1)]); // -X
        t.quad([x1, y0, 0.0], [x1, y1, 0.0], [x1, y1, z(x1, y1)], [x1, y0, z(x1, y0)]); // +X
        t.0
    }
}

/// Triangles in metres, Halo's axes, stored in world units.
#[derive(Default)]
struct Triangles(Vec<f32>);

impl Triangles {
    fn tri(&mut self, a: [f32; 3], b: [f32; 3], c: [f32; 3]) {
        for p in [a, b, c] {
            self.0.extend(metres(p));
        }
    }
    /// A quad a b c d, counterclockwise seen from its front.
    fn quad(&mut self, a: [f32; 3], b: [f32; 3], c: [f32; 3], d: [f32; 3]) {
        self.tri(a, b, c);
        self.tri(a, c, d);
    }
    /// A box standing on the floor: its top and four sides, facing out.
    fn cuboid(&mut self, min: [f32; 3], max: [f32; 3]) {
        let [x0, y0, z0] = min;
        let [x1, y1, z1] = max;
        self.quad([x0, y0, z1], [x1, y0, z1], [x1, y1, z1], [x0, y1, z1]); // top, +Z
        self.quad([x0, y0, z0], [x1, y0, z0], [x1, y0, z1], [x0, y0, z1]); // -Y
        self.quad([x1, y1, z0], [x0, y1, z0], [x0, y1, z1], [x1, y1, z1]); // +Y
        self.quad([x0, y1, z0], [x0, y0, z0], [x0, y0, z1], [x0, y1, z1]); // -X
        self.quad([x1, y0, z0], [x1, y1, z0], [x1, y1, z1], [x1, y0, z1]); // +X
    }
}

/// What the tests read of a frame, copied out of `HaloSkateFrame`.
#[derive(Clone, Debug)]
pub struct Snapshot {
    /// halo_skate_step's answer: 0 a new pose, 1 no answer in time, 2 a
    /// failed step recovered from, -1 failed.
    pub code: i32,
    pub tick: u32,
    pub state: String,
    pub position: [f32; 3],
    pub forward: [f32; 3],
    pub up: [f32; 3],
    /// world units a second
    pub velocity: [f32; 3],
    pub has_camera: bool,
    pub camera_position: [f32; 3],
    pub camera_forward: [f32; 3],
    pub camera_up: [f32; 3],
    pub camera_field_of_view: f32,
}

impl Snapshot {
    pub fn of(code: i32, f: &HaloSkateFrame) -> Self {
        let state = unsafe { CStr::from_ptr(f.state.as_ptr()) }.to_string_lossy().into_owned();
        Self {
            code,
            tick: f.tick,
            state,
            position: f.position,
            forward: f.forward,
            up: f.up,
            velocity: f.velocity,
            has_camera: f.has_camera != 0,
            camera_position: f.camera_position,
            camera_forward: f.camera_forward,
            camera_up: f.camera_up,
            camera_field_of_view: f.camera_field_of_view,
        }
    }

    /// Every number in the frame is finite.
    pub fn is_finite(&self) -> bool {
        let finite = |v: &[f32; 3]| v.iter().all(|x| x.is_finite());
        finite(&self.position)
            && finite(&self.forward)
            && finite(&self.up)
            && finite(&self.velocity)
            && (!self.has_camera
                || (finite(&self.camera_position)
                    && finite(&self.camera_forward)
                    && finite(&self.camera_up)
                    && self.camera_field_of_view.is_finite()))
    }

    /// The board's speed, m/s.
    pub fn speed(&self) -> f32 {
        length(self.velocity) * METRES
    }
    /// The board's speed along the ground, m/s.
    pub fn ground_speed(&self) -> f32 {
        length([self.velocity[0], self.velocity[1], 0.0]) * METRES
    }
    /// Where, in metres.
    pub fn at(&self) -> [f32; 3] {
        self.position.map(|v| v * METRES)
    }
    pub fn off_board(&self) -> bool {
        self.state.starts_with("Biped") || self.state == "OffBoardPushing"
    }
    pub fn airborne(&self) -> bool {
        self.state.contains("Air")
    }
    pub fn grinding(&self) -> bool {
        self.state.starts_with("Grind")
    }
}

pub fn length(v: [f32; 3]) -> f32 {
    (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]).sqrt()
}

pub fn zeroed_frame() -> HaloSkateFrame {
    // (all of its fields are numbers, for which zero is a value)
    unsafe { std::mem::zeroed() }
}

/// A pad held for a while.
#[derive(Clone, Copy, Default)]
pub struct Pad {
    pub buttons: u16,
    pub left: [i16; 2],
    pub right: [i16; 2],
    pub triggers: [u8; 2],
}

impl Pad {
    pub const NEUTRAL: Pad = Pad { buttons: 0, left: [0, 0], right: [0, 0], triggers: [0, 0] };
    pub fn buttons(buttons: u16) -> Pad {
        Pad { buttons, ..Pad::NEUTRAL }
    }
    pub fn left(x: i16, y: i16) -> Pad {
        Pad { left: [x, y], ..Pad::NEUTRAL }
    }
    pub fn right(x: i16, y: i16) -> Pad {
        Pad { right: [x, y], ..Pad::NEUTRAL }
    }
    pub fn with_buttons(self, buttons: u16) -> Pad {
        Pad { buttons, ..self }
    }
    fn c(&self) -> HaloSkatePad {
        HaloSkatePad { buttons: self.buttons, triggers: self.triggers, left: self.left, right: self.right }
    }
}

/// The engine, through the C interface, with the test map loaded. Holding
/// it keeps the other tests off the engine.
pub struct Engine {
    _turn: MutexGuard<'static, ()>,
    pub frame: HaloSkateFrame,
    /// Every frame stepped since `activate`, in order.
    pub history: Vec<Snapshot>,
}

impl Engine {
    /// Waits for its turn, then preloads the skater and loads the test map
    /// unless the engine has them already.
    pub fn start(assets: &std::path::Path) -> Engine {
        let turn = ENGINE.lock().unwrap_or_else(|e| e.into_inner());
        unsafe { halo_skate_set_log(Some(capture)) };
        if halo_skate_state() != 2 {
            let root = CString::new(assets.to_string_lossy().into_owned()).unwrap();
            let started = Instant::now();
            assert_eq!(unsafe { halo_skate_preload(root.as_ptr()) }, 0, "halo_skate_preload");
            wait("the preload", Duration::from_secs(600), || halo_skate_preloading() == 0);
            assert_ne!(halo_skate_state(), -1, "the preload failed: {}", error());
            let triangles = TestMap::triangles();
            let count = (triangles.len() / 9) as i32;
            assert_eq!(unsafe { halo_skate_load(root.as_ptr(), triangles.as_ptr(), count) }, 0, "halo_skate_load");
            wait("the map", Duration::from_secs(300), || halo_skate_state() != 1);
            assert_eq!(halo_skate_state(), 2, "the map did not load: {}", error());
            eprintln!("engine ready in {:.1} s", started.elapsed().as_secs_f32());
        }
        Engine { _turn: turn, frame: zeroed_frame(), history: Vec::new() }
    }

    /// Gets on the board at `at` (metres, on the floor) facing `yaw` from +X.
    pub fn activate(&mut self, at: [f32; 3], yaw: f32) -> Snapshot {
        let position = metres(at);
        let code = unsafe { halo_skate_activate(position.as_ptr(), yaw, &mut self.frame) };
        assert_eq!(code, 0, "halo_skate_activate: {}", error());
        self.history.clear();
        let snapshot = Snapshot::of(code, &self.frame);
        self.history.push(snapshot.clone());
        snapshot
    }

    /// One game tick (two engine ticks) with `pad` held.
    pub fn step(&mut self, pad: Pad) -> Snapshot {
        let code = unsafe { halo_skate_step(&pad.c(), GAME_TICK, &mut self.frame) };
        assert_ne!(code, -1, "halo_skate_step failed: {}", error());
        assert_ne!(code, 1, "the engine missed the game's 250 ms wait: run these tests with --release");
        let snapshot = Snapshot::of(code, &self.frame);
        self.history.push(snapshot.clone());
        snapshot
    }

    /// `ticks` game ticks with `pad` held; the last frame.
    pub fn hold(&mut self, pad: Pad, ticks: usize) -> Snapshot {
        let mut last = None;
        for _ in 0..ticks {
            last = Some(self.step(pad));
        }
        last.expect("at least one tick")
    }

    /// The frames since `from` (an index into `history`).
    pub fn since(&self, from: usize) -> &[Snapshot] {
        &self.history[from..]
    }

    /// Prints a line every `every` frames since `from`, for the test output.
    pub fn trace(&self, from: usize, every: usize) {
        for (i, s) in self.history[from..].iter().enumerate() {
            if i % every == 0 {
                let [x, y, z] = s.at();
                eprintln!(
                    "    tick {:5} code {} {:20} at ({x:7.2}, {y:7.2}, {z:5.2}) m, {:5.2} m/s{}",
                    s.tick,
                    s.code,
                    s.state,
                    s.speed(),
                    if s.is_finite() { "" } else { "  NOT FINITE" }
                );
            }
        }
    }
}

pub fn error() -> String {
    unsafe { CStr::from_ptr(halo_skate_error()) }.to_string_lossy().into_owned()
}

fn wait(what: &str, limit: Duration, done: impl Fn() -> bool) {
    let started = Instant::now();
    while !done() {
        assert!(started.elapsed() < limit, "{what} took longer than {limit:?}");
        std::thread::sleep(Duration::from_millis(50));
    }
}

/// The ollie: the right stick pulled down, then flicked up (`camera_tricks.rs`
/// in skate-host flicks the same way), at 30 game ticks a second.
pub fn ollie(engine: &mut Engine, steer: Pad) {
    engine.hold(Pad { right: [0, -FULL], ..steer }, 8);
    engine.hold(Pad { right: [0, FULL], ..steer }, 2);
}

/// Pushes: A held for `ticks` game ticks, then let go as long.
pub fn push(engine: &mut Engine, ticks: usize) {
    engine.hold(Pad::buttons(A), ticks);
    engine.hold(Pad::NEUTRAL, ticks);
}

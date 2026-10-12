//! Obstacles (`halo_skate_set_obstacles`, `obstacles.rs`) against the real
//! Skate 3 data (`common/mod.rs`): riding into a standing person (a capsule
//! Master Chief's size), a vehicle (a box a Warthog's size) and another
//! skater on their board (as another machine's is sent) on the test map's
//! open floor, at a few speeds. Slow is a bump (the board stops at it, the
//! skater stays on), fast is a bail; nothing passes through, every frame is
//! finite, and after the bail the engine puts the skater back on the board.
//! Off the board, walking into a wall sent as an obstacle stops the skater.
//! A blast's push (`halo_skate_apply_impulse`) moves the skater riding or
//! walking, and a strong one bails.
//! Skipped unless `HALO_SKATE_TEST_ASSETS` is set; run with `--release`.

mod common;

use common::*;
use halo_skate::obstacles::{KIND_BIPED, KIND_SCENERY, KIND_SKATER, KIND_VEHICLE, SHAPE_BOX, SHAPE_CAPSULE, SHAPE_MESH};
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
        ..Default::default()
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
        ..Default::default()
    }
}

fn set(obstacles: &[HaloSkateObstacle]) -> i32 {
    set_with(obstacles, &[])
}

/// Sends `obstacles`, a mesh's triangles among `triangles` (9 floats each).
fn set_with(obstacles: &[HaloSkateObstacle], triangles: &[f32]) -> i32 {
    unsafe {
        halo_skate_set_obstacles(obstacles.as_ptr(), obstacles.len() as i32, triangles.as_ptr(), (triangles.len() / 9) as i32)
    }
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
/// `push_ticks` game ticks, then rolls on into the obstacles `make(x, y)`
/// puts 3 m ahead of the board (the first the one measured from), the way it
/// travels (`ahead` m further for ones driving at the board at `toward`
/// m/s), sent every tick as the game sends them, for four seconds.
fn ride_into(
    engine: &mut Engine,
    lane: f32,
    pushes: usize,
    push_ticks: usize,
    make: &dyn Fn(f32, f32) -> Vec<HaloSkateObstacle>,
    toward: f32,
) -> Ride {
    ride_into_meshes(engine, lane, pushes, push_ticks, &|x, y| (make(x, y), Vec::new()), toward)
}

/// As `ride_into`, with meshes: `make` gives the obstacles and the triangles
/// (world units) their meshes are among, carried on with them.
fn ride_into_meshes(
    engine: &mut Engine,
    lane: f32,
    pushes: usize,
    push_ticks: usize,
    make: &dyn Fn(f32, f32) -> (Vec<HaloSkateObstacle>, Vec<f32>),
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
    let (mut obstacles, mut triangles) = make(at[0] + direction[0] * ahead, at[1] + direction[1] * ahead);
    let velocity = [-direction[0] * toward / METRES, -direction[1] * toward / METRES, 0.0];
    for o in &mut obstacles {
        o.velocity = velocity;
    }
    let mut met = None;
    for _ in 0..120 {
        assert_eq!(set_with(&obstacles, &triangles), obstacles.len() as i32, "the obstacles are taken");
        let s = engine.step(Pad::NEUTRAL);
        for o in &mut obstacles {
            for i in 0..3 {
                o.center[i] += o.velocity[i] * GAME_TICK;
            }
        }
        for (k, p) in triangles.iter_mut().enumerate() {
            *p += velocity[k % 3] * GAME_TICK;
        }
        let obstacle = obstacles[0];
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
    let end = obstacles[0].center.map(|v| v * METRES);
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
    // (five seconds, ten at most: a ragdoll a vehicle carried on slides a
    // while, longer on some machines, before the engine puts it back)
    engine.hold(Pad::NEUTRAL, 150);
    for _ in 0..150 {
        let last = engine.history.last().unwrap();
        if !last.off_board() && !last.state.starts_with("Wipeout") {
            break;
        }
        engine.step(Pad::NEUTRAL);
    }
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
    let slow = ride_into(&mut engine, TestMap::OPEN, 1, 3, &|x, y| vec![person(x, y)], -3.0);
    let slow_closing = report("person, slow", &slow, "biped");
    let fast = ride_into(&mut engine, TestMap::OPEN, 4, 12, &|x, y| vec![person(x, y)], 0.0);
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
    let slow = ride_into(&mut engine, TestMap::OPEN, 1, 3, &|x, y| vec![vehicle(x, y)], 0.0);
    let slow_closing = report("vehicle, slow", &slow, "vehicle");
    let fast = ride_into(&mut engine, TestMap::OPEN, 4, 12, &|x, y| vec![vehicle(x, y)], 0.0);
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
    let hit = ride_into(&mut engine, TestMap::OPEN, 1, 3, &|x, y| vec![vehicle(x, y)], 5.0);
    let closing = report("vehicle driving at 5 m/s", &hit, "vehicle");
    assert!(closing > 8.0, "the vehicle's speed and the skater's: {closing:.2} m/s");
    assert!(hit.bailed, "a bail");
    assert_back_on_the_board(&mut engine, "vehicle driving");
}

/// Walks off the board (Y, then the left stick up) on the open floor: a
/// second to find the way the skater walks, then on for five, with a wall (a
/// box 0.4 m thick, 6 m wide, 1.5 m high: scenery, so no bail) `ahead` m on
/// along that way if `wall`, sent every tick as the game sends it. Returns
/// how far along that way the skater got from where it stood when the wall
/// came, metres, and every frame since.
fn walk_toward(engine: &mut Engine, wall: Option<f32>) -> (f32, Vec<Snapshot>) {
    assert_eq!(set(&[]), 0);
    engine.activate([0.0, TestMap::OPEN, 0.0], 0.0);
    engine.hold(Pad::NEUTRAL, 15);
    engine.hold(Pad::buttons(Y), 4);
    for _ in 0..90 {
        if engine.step(Pad::NEUTRAL).off_board() {
            break;
        }
    }
    engine.hold(Pad::NEUTRAL, 30);
    assert!(engine.history.last().unwrap().off_board(), "off the board");
    let walk = Pad::left(0, FULL);
    let from = engine.history.last().unwrap().at();
    engine.hold(walk, 30);
    let start = engine.history.last().unwrap().at();
    let (dx, dy) = (start[0] - from[0], start[1] - from[1]);
    let moved = (dx * dx + dy * dy).sqrt();
    assert!(moved > 0.5, "walking: {moved} m in a second");
    let way = [dx / moved, dy / moved];
    let obstacle = wall.map(|ahead| HaloSkateObstacle {
        kind: KIND_SCENERY,
        shape: SHAPE_BOX,
        center: [(start[0] + way[0] * ahead) / METRES, (start[1] + way[1] * ahead) / METRES, 0.75 / METRES],
        forward: [way[0], way[1], 0.0],
        left: [-way[1], way[0], 0.0],
        up: [0.0, 0.0, 1.0],
        half_extents: [0.2 / METRES, 3.0 / METRES, 0.75 / METRES],
        velocity: [0.0; 3],
        ..Default::default()
    });
    let first = engine.history.len();
    for _ in 0..150 {
        let sent = obstacle.map_or_else(|| set(&[]), |o| set(&[o]));
        assert_eq!(sent, obstacle.is_some() as i32);
        engine.step(walk);
    }
    assert_eq!(set(&[]), 0);
    let frames = engine.since(first).to_vec();
    assert!(frames.iter().all(Snapshot::is_finite), "every frame is finite");
    let furthest = frames
        .iter()
        .map(|s| (s.at()[0] - start[0]) * way[0] + (s.at()[1] - start[1]) * way[1])
        .fold(f32::MIN, f32::max);
    (furthest, frames)
}

/// Walking off the board into a wall the game sends as an obstacle (the
/// off-board controller sweeps the obstacles' triangles with the level's:
/// `BoardWorld::set_offboard_obstacles`): the skater stops at it, where with
/// no wall it walks on well past it.
#[test]
fn walking_off_the_board_stops_at_an_obstacle() {
    let Some(assets) = assets("walking into an obstacle") else { return };
    let mut engine = Engine::start(&assets);
    const AHEAD: f32 = 2.5;
    let (open, _) = walk_toward(&mut engine, None);
    let (walled, frames) = walk_toward(&mut engine, Some(AHEAD));
    eprintln!("walking off the board: {open:.2} m with no wall, {walled:.2} m toward a wall {AHEAD} m ahead");
    engine.trace(engine.history.len() - frames.len(), 15);
    assert!(open > AHEAD + 1.0, "with no wall the skater walks past where it would be: {open:.2} m");
    // (the wall's near face is AHEAD - 0.2 m on; the skater's middle stops
    // its body's round short of it)
    assert!(walled < AHEAD - 0.2, "the skater walked {walled:.2} m, into the wall {AHEAD} m ahead");
    assert!(frames.iter().all(Snapshot::off_board), "still walking: {:?}", frames.last().unwrap().state);
}

/// Another skater standing on their board at `x`, `y` (metres), as the game
/// sends another machine's skater (skate.c's `skate_skater_obstacles`): their
/// body a capsule from their feet to their head, 0.09 world units round, and
/// their board where `placement` (the rider's own, `halo_skate_board_placement`)
/// would have it there, sized by `halo_skate_board_obstacle`.
fn skater(x: f32, y: f32, placement: [f32; 7]) -> Vec<HaloSkateObstacle> {
    let (feet, head) = (0.25 / METRES, 1.75 / METRES);
    let body = HaloSkateObstacle {
        kind: KIND_SKATER,
        shape: SHAPE_CAPSULE,
        center: [x / METRES, y / METRES, (feet + head) / 2.0],
        forward: [1.0, 0.0, 0.0],
        left: [0.0, 1.0, 0.0],
        up: [0.0, 0.0, 1.0],
        half_extents: [0.09, 0.0, (head - feet) / 2.0 + 0.09],
        velocity: [0.0; 3],
        ..Default::default()
    };
    let mut at = placement;
    at[4] = x / METRES;
    at[5] = y / METRES;
    let mut board = HaloSkateObstacle::default();
    assert_eq!(unsafe { halo_skate_board_obstacle(at.as_ptr(), &mut board) }, 0, "the board as an obstacle");
    vec![body, board]
}

/// Riding into another skater standing on their board, slowly and at full
/// speed (a bump or a bail slow, as into a person): the hit is on a skater,
/// the board does not pass through theirs,
/// and fast is a bail. Their board's box is the real board's size.
#[test]
fn riding_into_another_skater_bumps_slow_and_bails_fast() {
    let Some(assets) = assets("riding into another skater") else { return };
    let mut engine = Engine::start(&assets);
    engine.activate([0.0, TestMap::OPEN, 0.0], 0.0);
    engine.hold(Pad::NEUTRAL, 15);
    let mut placement = [0.0f32; 7];
    assert_eq!(unsafe { halo_skate_board_placement(placement.as_mut_ptr()) }, 0, "the rider's board placed");
    let board = skater(0.0, 0.0, placement)[1];
    let size = board.half_extents.map(|h| 2.0 * h * METRES);
    eprintln!("another skater's board: {size:.3?} m");
    let longest = size.iter().copied().fold(0.0, f32::max);
    assert!(longest > 0.7 && longest < 1.0, "a skateboard's length: {size:?} m");
    let slow = ride_into(&mut engine, TestMap::OPEN, 1, 3, &|x, y| skater(x, y, placement), 0.0);
    report("skater, slow", &slow, "skater");
    let fast = ride_into(&mut engine, TestMap::OPEN, 4, 12, &|x, y| skater(x, y, placement), 0.0);
    let fast_closing = report("skater, fast", &fast, "skater");
    assert!(slow.past < 0.0, "slow: not through the skater: {:.2} m past", slow.past);
    assert!(fast_closing > 6.0, "fast: at speed: {fast_closing:.2} m/s");
    assert!(fast.bailed, "fast ({fast_closing:.2} m/s): a bail");
    assert!(fast.past < 0.0, "fast: not through the skater: {:.2} m past", fast.past);
    assert_back_on_the_board(&mut engine, "skater, fast");
}

/// Triangles in world units, nine floats each, as the game sends a mesh.
#[derive(Clone, Default)]
struct Mesh(Vec<f32>);

impl Mesh {
    /// A triangle of points in metres.
    fn tri(&mut self, a: [f32; 3], b: [f32; 3], c: [f32; 3]) {
        for p in [a, b, c] {
            self.0.extend(metres(p));
        }
    }
    /// A quad a b c d in metres, counterclockwise seen from its front.
    fn quad(&mut self, a: [f32; 3], b: [f32; 3], c: [f32; 3], d: [f32; 3]) {
        self.tri(a, b, c);
        self.tri(a, c, d);
    }
    /// A closed box from `min` to `max` (metres), facing out.
    fn cuboid(min: [f32; 3], max: [f32; 3]) -> Mesh {
        let mut m = Mesh::default();
        let [x0, y0, z0] = min;
        let [x1, y1, z1] = max;
        m.quad([x0, y0, z1], [x1, y0, z1], [x1, y1, z1], [x0, y1, z1]); // +Z
        m.quad([x0, y1, z0], [x1, y1, z0], [x1, y0, z0], [x0, y0, z0]); // -Z
        m.quad([x0, y0, z0], [x1, y0, z0], [x1, y0, z1], [x0, y0, z1]); // -Y
        m.quad([x1, y1, z0], [x0, y1, z0], [x0, y1, z1], [x1, y1, z1]); // +Y
        m.quad([x0, y1, z0], [x0, y0, z0], [x0, y0, z1], [x0, y1, z1]); // -X
        m.quad([x1, y0, z0], [x1, y1, z0], [x1, y1, z1], [x1, y0, z1]); // +X
        m
    }
    /// Turned `yaw` about Z, then moved `by` (metres).
    fn placed(&self, yaw: f32, by: [f32; 3]) -> Mesh {
        let (s, c) = yaw.sin_cos();
        Mesh(
            self.0
                .chunks_exact(3)
                .flat_map(|p| [c * p[0] - s * p[1] + by[0] / METRES, s * p[0] + c * p[1] + by[1] / METRES, p[2] + by[2] / METRES])
                .collect(),
        )
    }
    /// The middle of its bounds and its half sizes, world units.
    fn bounds(&self) -> ([f32; 3], [f32; 3]) {
        let mut lo = [f32::MAX; 3];
        let mut hi = [f32::MIN; 3];
        for p in self.0.chunks_exact(3) {
            for i in 0..3 {
                lo[i] = lo[i].min(p[i]);
                hi[i] = hi[i].max(p[i]);
            }
        }
        (std::array::from_fn(|i| (lo[i] + hi[i]) / 2.0), std::array::from_fn(|i| (hi[i] - lo[i]) / 2.0))
    }
    /// As a mesh obstacle of `kind`, its triangles the first sent.
    fn obstacle(&self, kind: i32) -> HaloSkateObstacle {
        HaloSkateObstacle {
            kind,
            shape: SHAPE_MESH,
            center: self.bounds().0,
            first_triangle: 0,
            triangle_count: (self.0.len() / 9) as i32,
            ..Default::default()
        }
    }
}

/// Riding into a real Warthog's side, its hull read from Blood Gulch
/// (`common/halo_map.rs`) and sent as the game sends a vehicle now: its own
/// collision as triangles, not a box. Across the lane on the floor, after one
/// push it stops the board at its real side and the skater stays on; at full
/// speed it is a bail, not through it.
#[test]
fn riding_into_a_warthog_stops_at_its_real_side() {
    let Some(assets) = assets("riding into a Warthog") else { return };
    let Some(map) = common::halo_map::halo_maps("riding into a Warthog", "bloodgulch") else { return };
    let (name, hull) = common::halo_map::read_collision_model(&map, "warthog").expect("the Warthog's collision");
    let hull = Mesh(hull);
    let (middle, half) = hull.bounds();
    eprintln!("{name}: {} triangles, {:.2?} m long, wide and high", hull.0.len() / 9, half.map(|h| 2.0 * h * METRES));
    assert!(hull.0.len() / 9 >= 12, "a hull, not a box");
    // (its length across the lane: turned a quarter, its bottom on the floor)
    let at = move |x: f32, y: f32| {
        let m = hull.placed(std::f32::consts::FRAC_PI_2, [0.0; 3]);
        let (c, h) = m.bounds();
        m.placed(0.0, [x - c[0] * METRES, y - c[1] * METRES, (h[2] - c[2]) * METRES])
    };
    let make = move |x: f32, y: f32| {
        let m = at(x, y);
        (vec![m.obstacle(KIND_VEHICLE)], m.0)
    };
    let side = half[1] * METRES;
    let _ = middle;
    let mut engine = Engine::start(&assets);
    let slow = ride_into_meshes(&mut engine, TestMap::OPEN, 1, 3, &make, 0.0);
    let slow_closing = report("Warthog, slow", &slow, "vehicle");
    let fast = ride_into_meshes(&mut engine, TestMap::OPEN, 4, 12, &make, 0.0);
    let fast_closing = report("Warthog, fast", &fast, "vehicle");
    assert!(!slow.bailed, "slow ({slow_closing:.2} m/s): a bump, not a bail");
    assert!(slow.past < -side + 0.2, "slow: the board stopped at its side, {side:.2} m from its middle: {:.2} m past", slow.past);
    assert!(slow.after < 1.0, "slow: stopped: {:.2} m/s", slow.after);
    assert!(fast_closing > 6.0, "fast: at speed: {fast_closing:.2} m/s");
    assert!(fast.bailed, "fast ({fast_closing:.2} m/s): a bail");
    assert!(fast.past < 0.0, "fast: not through the Warthog: {:.2} m past its middle", fast.past);
    assert_back_on_the_board(&mut engine, "Warthog, fast");
}

/// A vehicle shaped as a wedge: a right triangle 3 m a leg, 1 m high, its
/// long side along the lane 1.3 m to one side of the board's path (past
/// the skater's arms). Sent as
/// its triangles the board rides past it untouched; sent as the box around
/// it (in its own axes, as each node was sent before), the box's corner
/// sticks out across the path and the board hits it.
#[test]
fn riding_past_a_wedge_misses_it_where_its_box_would_not() {
    let Some(assets) = assets("riding past a wedge") else { return };
    let leg = 3.0f32;
    let mut wedge = Mesh::default();
    // (in its own axes: the right angle at the origin, legs along +X and +Y)
    let (o, a, b) = ([0.0, 0.0], [leg, 0.0], [0.0, leg]);
    let p = |q: [f32; 2], z: f32| [q[0], q[1], z];
    wedge.tri(p(o, 1.0), p(a, 1.0), p(b, 1.0));
    wedge.tri(p(o, 0.0), p(b, 0.0), p(a, 0.0));
    wedge.quad(p(o, 0.0), p(a, 0.0), p(a, 1.0), p(o, 1.0));
    wedge.quad(p(a, 0.0), p(b, 0.0), p(b, 1.0), p(a, 1.0));
    wedge.quad(p(b, 0.0), p(o, 0.0), p(o, 1.0), p(b, 1.0));
    // (turned so that its long side, from a to b, runs along +X with the
    // right angle to its +Y side, away from the path: its long side then
    // lies leg/sqrt(2) below the right angle, moved to sit 1.3 m to the +Y
    // side of the path)
    let yaw = 1.25 * std::f32::consts::PI;
    let gap = 1.3;
    let reach = leg / 2.0f32.sqrt();
    // (its middle 4 m further on than ride_into puts obstacles, so that it
    // is all ahead of the board when it comes)
    const ON: f32 = 4.0;
    let place = move |x: f32, y: f32| wedge.placed(yaw, [x + ON, y + gap + reach, 0.0]);
    let make_mesh = move |x: f32, y: f32| {
        let m = place(x, y);
        let mut o = m.obstacle(KIND_VEHICLE);
        // (its middle on the path, for how far past it the board got)
        o.center = metres([x + ON, y, 0.5]);
        (vec![o], m.0)
    };
    let make_box = move |x: f32, y: f32| {
        // the box around it in its own axes: 3 m square, turned with it
        let (s, c) = yaw.sin_cos();
        let local_middle = [leg / 2.0, leg / 2.0];
        let middle = [
            x + ON + c * local_middle[0] - s * local_middle[1],
            y + gap + reach + s * local_middle[0] + c * local_middle[1],
        ];
        vec![HaloSkateObstacle {
            kind: KIND_VEHICLE,
            shape: SHAPE_BOX,
            center: metres([middle[0], middle[1], 0.5]),
            forward: [c, s, 0.0],
            left: [-s, c, 0.0],
            up: [0.0, 0.0, 1.0],
            half_extents: metres([leg / 2.0, leg / 2.0, 0.5]),
            ..Default::default()
        }]
    };
    let mut engine = Engine::start(&assets);
    let mesh = ride_into_meshes(&mut engine, TestMap::OPEN, 1, 3, &make_mesh, 0.0);
    let boxed = ride_into(&mut engine, TestMap::OPEN, 1, 3, &make_box, 0.0);
    eprintln!(
        "past a wedge: as its triangles {:.2} m past its middle at {:.2} m/s ({} hit); as its box {:.2} m past ({} hit)",
        mesh.past,
        mesh.after,
        if hit_at(&mesh.log, "vehicle").is_some() { "a" } else { "no" },
        boxed.past,
        if hit_at(&boxed.log, "vehicle").is_some() { "a" } else { "no" }
    );
    // (as its triangles at most a brush of the skater's arm on its long side)
    let brushed = hit_at(&mesh.log, "vehicle").unwrap_or(0.0);
    assert!(brushed < 0.5, "its triangles leave the path clear: {:?}", mesh.log);
    assert!(!mesh.bailed && mesh.past > 4.0 && mesh.after > 4.0, "the board rode on past it at speed: {:.2} m", mesh.past);
    let struck = hit_at(&boxed.log, "vehicle").expect("the box's corner is in the path");
    assert!(struck > 2.0, "the box's corner struck: {struck:.2} m/s");
    assert!(boxed.past < 3.0 || boxed.after < 3.0, "and stopped or turned the board: {:.2} m past", boxed.past);
}

/// Off the board, standing still on a lift: a platform 3 m square (its own
/// collision, as a device's) rising from under the floor at 0.5 m/s to 1.1 m
/// up, sent every tick with its velocity. The off-board controller's ground
/// probes see it (`ground_query/world.rs`), so the skater rises with it and
/// stands on it once it stops.
#[test]
fn standing_on_a_rising_lift_rides_up_with_it() {
    let Some(assets) = assets("standing on a lift") else { return };
    let mut engine = Engine::start(&assets);
    assert_eq!(set(&[]), 0);
    engine.activate([0.0, TestMap::OPEN, 0.0], 0.0);
    engine.hold(Pad::NEUTRAL, 15);
    engine.hold(Pad::buttons(Y), 4);
    for _ in 0..90 {
        if engine.step(Pad::NEUTRAL).off_board() {
            break;
        }
    }
    engine.hold(Pad::NEUTRAL, 45);
    let stood = engine.history.last().unwrap().clone();
    assert!(stood.off_board(), "off the board: {}", stood.state);
    let feet = stood.at();
    eprintln!("standing at ({:.2}, {:.2}, {:.2}) m", feet[0], feet[1], feet[2]);
    const SPEED: f32 = 0.5;
    const TOP: f32 = 1.1;
    let mut top = -0.1f32;
    let first = engine.history.len();
    for tick in 0..(30.0 * ((TOP - top) / SPEED + 1.5)) as usize {
        let rising = top < TOP;
        let lift = Mesh::cuboid([feet[0] - 1.5, feet[1] - 1.5, top - 0.3], [feet[0] + 1.5, feet[1] + 1.5, top]);
        let mut o = lift.obstacle(KIND_SCENERY);
        o.velocity = [0.0, 0.0, if rising { SPEED / METRES } else { 0.0 }];
        assert_eq!(set_with(&[o], &lift.0), 1);
        let s = engine.step(Pad::NEUTRAL);
        if tick % 15 == 0 {
            eprintln!("    lift top {top:.2} m: the skater at {:.2} m, {}", s.at()[2], s.state);
        }
        if rising {
            top = (top + SPEED * GAME_TICK).min(TOP);
        }
    }
    assert_eq!(set(&[]), 0);
    let frames = engine.since(first).to_vec();
    assert!(frames.iter().all(Snapshot::is_finite), "every frame is finite");
    let last = frames.last().unwrap();
    eprintln!("on the lift: the skater at {:.2} m, the lift's top at {TOP} m, {}", last.at()[2], last.state);
    assert!((last.at()[2] - TOP).abs() < 0.25, "standing on the lift's top: {:.2} m", last.at()[2]);
    let horizontal = ((last.at()[0] - feet[0]).powi(2) + (last.at()[1] - feet[1]).powi(2)).sqrt();
    assert!(horizontal < 1.0, "still on it: {horizontal:.2} m from where it stood");
    assert!(last.off_board(), "still walking: {}", last.state);
    engine.hold(Pad::NEUTRAL, 30);
}

/// Scenery made and then gone mid-level: the level's collision built again
/// (`halo_skate_reload`, as skate.c's `skate_watch_scenery` sends it) with a
/// wall 1 m high across the open lane 25 m on, while the skater rides; the
/// state stays ready and no step fails meanwhile. Ridden into, it stops the
/// board (or bails it), not through; built again without it, the skater
/// rides past where it was.
#[test]
fn scenery_made_and_gone_is_built_into_the_level_and_out() {
    let Some(assets) = assets("scenery made and gone") else { return };
    let mut engine = Engine::start(&assets);
    let level = TestMap::triangles();
    let mut walled = level.clone();
    walled.extend(Mesh::cuboid([25.0, TestMap::OPEN - 5.0, -0.1], [26.0, TestMap::OPEN + 5.0, 1.0]).0);
    let reload = |triangles: &[f32]| {
        let root = std::ffi::CString::new(assets.to_string_lossy().into_owned()).unwrap();
        assert_eq!(unsafe { halo_skate_reload(root.as_ptr(), triangles.as_ptr(), (triangles.len() / 9) as i32) }, 0);
    };
    // (rides along the open lane at full speed for `ticks`, the level sent
    // again with `triangles` once rolling; the furthest it got, and whether
    // it bailed)
    let ride = |engine: &mut Engine, triangles: &[f32], ticks: usize| -> (f32, bool, Vec<String>) {
        assert_eq!(set(&[]), 0);
        engine.activate([0.0, TestMap::OPEN, 0.0], 0.0);
        take_log();
        push(engine, 12);
        reload(triangles);
        let mut rebuilt = None;
        for tick in 0..ticks {
            if tick < 3 * 12 {
                engine.step(if tick % 24 < 12 { Pad::buttons(A) } else { Pad::NEUTRAL });
            } else {
                engine.step(Pad::NEUTRAL);
            }
            assert_eq!(halo_skate_state(), 2, "ready throughout the rebuild");
            if rebuilt.is_none() && take_log().iter().any(|l| l.contains("map rebuilt,")) {
                rebuilt = Some(tick);
            }
        }
        let frames = engine.history.clone();
        assert!(frames.iter().all(Snapshot::is_finite), "every frame is finite");
        assert!(frames.iter().all(|s| s.code != -1), "no step failed");
        let furthest = frames
            .iter()
            .take_while(|s| !s.state.starts_with("Wipeout"))
            .map(|s| s.at()[0])
            .fold(f32::MIN, f32::max);
        let bailed = frames.iter().any(|s| s.state.starts_with("Wipeout") || s.code == 2);
        let rebuilt = rebuilt.expect("the level was built again");
        (furthest, bailed, vec![format!("rebuilt {rebuilt} game ticks after it was sent")])
    };
    let (walled_furthest, walled_bailed, walled_log) = ride(&mut engine, &walled, 150);
    assert_back_on_the_board(&mut engine, "into the wall");
    let (open_furthest, _, open_log) = ride(&mut engine, &level, 150);
    eprintln!(
        "with the wall: {walled_furthest:.2} m on ({}; {walled_log:?}); without it again: {open_furthest:.2} m on ({open_log:?})",
        if walled_bailed { "a bail" } else { "no bail" }
    );
    assert!(walled_furthest < 25.0, "not through the wall made: {walled_furthest:.2} m");
    assert!(walled_furthest > 20.0, "the skater got to it: {walled_furthest:.2} m");
    assert!(open_furthest > 27.0, "past where the wall was, once it is gone: {open_furthest:.2} m");
    engine.hold(Pad::NEUTRAL, 30);
}

/// A teleporter takes the skater riding at speed along +X: put on the board
/// at its exit 40 m on (skate.c's `skate_follow_teleport` sends
/// `halo_skate_teleport`), turned a quarter left as the exit faces, moving as
/// fast as before, now along +Y. Half a second on they are still riding,
/// that way, near that fast.
#[test]
fn a_teleporter_keeps_the_skaters_speed_turned_with_the_exit() {
    let Some(assets) = assets("a teleporter") else { return };
    let mut engine = Engine::start(&assets);
    assert_eq!(set(&[]), 0);
    engine.activate([0.0, TestMap::OPEN, 0.0], 0.0);
    for _ in 0..3 {
        push(&mut engine, 12);
    }
    let before = engine.history.last().unwrap().clone();
    let v = before.velocity;
    let speed = before.ground_speed();
    assert!(speed > 5.0, "riding fast: {speed:.2} m/s");
    let yaw = before.forward[1].atan2(before.forward[0]) + std::f32::consts::FRAC_PI_2;
    // (the velocity turned a quarter left, as the unit was)
    let turned = [-v[1], v[0], v[2]];
    let exit = metres([before.at()[0] + 40.0, TestMap::OPEN, 0.0]);
    let code = unsafe { halo_skate_teleport(exit.as_ptr(), yaw, turned.as_ptr(), &mut engine.frame) };
    assert_eq!(code, 0, "halo_skate_teleport: {}", error());
    let at = Snapshot::of(code, &engine.frame);
    let first = engine.history.len();
    engine.hold(Pad::NEUTRAL, 15);
    let after = engine.history.last().unwrap().clone();
    engine.trace(first, 3);
    let moved = [after.at()[0] - at.at()[0], after.at()[1] - at.at()[1]];
    eprintln!(
        "teleported at {speed:.2} m/s: put at ({:.2}, {:.2}) m, half a second on {:.2} m/s, moved ({:.2}, {:.2}) m, {}",
        at.at()[0],
        at.at()[1],
        after.ground_speed(),
        moved[0],
        moved[1],
        after.state
    );
    assert!((at.at()[0] - exit[0] * METRES).abs() < 1.0 && (at.at()[1] - exit[1] * METRES).abs() < 1.0, "at the exit");
    assert!(engine.since(first).iter().all(Snapshot::is_finite), "every frame is finite");
    assert!(!after.state.starts_with("Wipeout") && !after.off_board(), "still riding: {}", after.state);
    assert!(after.ground_speed() > 0.6 * speed, "near as fast: {:.2} of {speed:.2} m/s", after.ground_speed());
    assert!(moved[1] > 2.0 && moved[1] > 3.0 * moved[0].abs(), "going the exit's way, +Y: {moved:?}");
}

/// Rides along the open lane at speed, then a blast pushes the skater
/// (`halo_skate_apply_impulse`, as skate.c sends Halo's push) by `blast`
/// (m/s, Halo's way: from in front of them, back and a little up), or none.
/// Returns the board's velocity (m/s, along the ground) just before, half a
/// second on, how far it went along its way in that half second (m), and
/// whether the skater bailed then.
fn blasted(engine: &mut Engine, blast: f32) -> ([f32; 2], [f32; 2], f32, bool) {
    assert_eq!(set(&[]), 0);
    engine.activate([0.0, TestMap::OPEN, 0.0], 0.0);
    for _ in 0..3 {
        push(engine, 12);
    }
    let before = engine.history.last().unwrap().clone();
    let v = [before.velocity[0] * METRES, before.velocity[1] * METRES];
    let along = (v[0] * v[0] + v[1] * v[1]).sqrt().max(1e-6);
    let way = [v[0] / along, v[1] / along];
    if blast > 0.0 {
        // (Halo's: the way from the blast, raised by 0.45 and made unit)
        let lift = 0.45f32;
        let n = (1.0 + lift * lift).sqrt();
        let p = [-way[0] / n * blast / METRES, -way[1] / n * blast / METRES, lift / n * blast / METRES];
        assert_eq!(halo_skate_apply_impulse(p[0], p[1], p[2]), 0, "halo_skate_apply_impulse: {}", error());
    }
    let first = engine.history.len();
    engine.hold(Pad::NEUTRAL, 15);
    let after = engine.history.last().unwrap().clone();
    let bailed = engine.since(first).iter().any(|s| s.state.starts_with("Wipeout") || s.code == 2);
    let moved = (after.at()[0] - before.at()[0]) * way[0] + (after.at()[1] - before.at()[1]) * way[1];
    assert!(engine.since(first).iter().all(Snapshot::is_finite), "every frame is finite");
    (v, [after.velocity[0] * METRES, after.velocity[1] * METRES], moved, bailed)
}


/// A blast pushes the skater riding along as Halo's pushes a player on
/// foot: one of 3 m/s from in front slows the board by about its level part
/// (2.7 m/s), so that it goes about that much less far in half a second
/// than with none, and they ride on; one of 9 m/s, faster than the obstacle
/// bail speed (6 m/s), knocks them over, and the engine puts them back on
/// the board.
#[test]
fn a_blast_pushes_the_riding_skater_and_a_strong_one_bails() {
    let Some(assets) = assets("a blast") else { return };
    let mut engine = Engine::start(&assets);
    let (v0, after0, moved0, bailed0) = blasted(&mut engine, 0.0);
    let (v1, after1, moved1, bailed1) = blasted(&mut engine, 3.0);
    let speed = |v: [f32; 2]| (v[0] * v[0] + v[1] * v[1]).sqrt();
    let slowed0 = speed(v0) - speed(after0);
    let slowed1 = speed(v1) - speed(after1);
    eprintln!(
        "no blast: {:.2} to {:.2} m/s, {moved0:.2} m on; a 3 m/s blast: {:.2} to {:.2} m/s, {moved1:.2} m on",
        speed(v0),
        speed(after0),
        speed(v1),
        speed(after1)
    );
    assert!(!bailed0 && !bailed1, "a small blast is no bail");
    assert!(
        (slowed1 - slowed0 - 2.7).abs() < 1.0,
        "slowed by the blast's level part, 2.7 m/s: {:.2} m/s",
        slowed1 - slowed0
    );
    assert!(moved0 - moved1 > 0.6, "went less far: {moved1:.2} m, not {moved0:.2} m");
    let (_, _, _, bailed) = blasted(&mut engine, 9.0);
    take_log();
    assert!(bailed, "a 9 m/s blast knocks the skater over");
    assert_back_on_the_board(&mut engine, "a blast");
}

/// Off the board, standing, a blast of 3 m/s along +X pushes the walking
/// skater that way, a third of a metre in half a second (the off-board
/// controller slows them back to a stand at once).
#[test]
fn a_blast_pushes_the_skater_off_the_board() {
    let Some(assets) = assets("a blast off the board") else { return };
    let mut engine = Engine::start(&assets);
    assert_eq!(set(&[]), 0);
    engine.activate([0.0, TestMap::OPEN, 0.0], 0.0);
    engine.hold(Pad::NEUTRAL, 15);
    engine.hold(Pad::buttons(Y), 4);
    for _ in 0..90 {
        if engine.step(Pad::NEUTRAL).off_board() {
            break;
        }
    }
    engine.hold(Pad::NEUTRAL, 45);
    let stood = engine.history.last().unwrap().clone();
    assert!(stood.off_board(), "off the board: {}", stood.state);
    engine.hold(Pad::NEUTRAL, 15);
    let still = engine.history.last().unwrap().clone();
    let drift = [still.at()[0] - stood.at()[0], still.at()[1] - stood.at()[1]];
    assert_eq!(halo_skate_apply_impulse(3.0 / METRES, 0.0, 0.0), 0, "halo_skate_apply_impulse: {}", error());
    engine.hold(Pad::NEUTRAL, 15);
    let pushed = engine.history.last().unwrap().clone();
    let moved = [pushed.at()[0] - still.at()[0], pushed.at()[1] - still.at()[1]];
    eprintln!("standing off the board: {drift:?} m in half a second; blasted, {moved:?} m, {}", pushed.state);
    assert!(pushed.off_board(), "still off the board: {}", pushed.state);
    assert!(moved[0] > 0.2 && moved[0] > 2.0 * moved[1].abs(), "pushed along +X: {moved:?} m");
}

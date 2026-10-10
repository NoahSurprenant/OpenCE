//! Skate 3 skating for the game, over a C ABI (port/skate/include/halo_skate.h).
//!
//! The skate engine runs on a worker thread of its own, with a large stack, and
//! owns one session for the map's collision. The game's thread sends it a
//! preload, a map, an activation or a tick's input, and waits only for the pose
//! that comes back: loading is never waited for.
//!
//! The session (input config, state graphs, physics, the skater and its
//! animation banks) is the slow part to make, so `halo_skate_preload` makes it
//! at startup on a flat placeholder floor; a map then only builds and swaps in
//! its collision.
//!
//! Halo space is Z up in world units of 10 feet; Skate space is Y up in metres.
//! `to_skate` is a proper rotation, so a triangle's winding survives it.

/// The crate's log lines go where the engine's go (`skate_host::log`): to the
/// game's log once `halo_skate_set_log` is called, else to stderr.
macro_rules! eprintln {
    ($($arg:tt)*) => {
        skate_host::log::write(&format!($($arg)*))
    };
}

mod board;
mod rails;
mod rig;

use bevy_math::{Mat3, Vec3};
use skate_host::bridge::{InputFrame, Pose, Session};
use std::ffi::{CStr, CString, c_char};
use std::path::{Path, PathBuf};
use std::sync::mpsc::{self, Receiver, RecvTimeoutError, Sender};
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

/// Metres in a world unit.
const METRES: f32 = 3.048;
/// Inches in a world unit, the unit the rail finder works in.
const INCHES: f32 = 120.0;
/// The skate engine's call stack: its animation and physics keep large arrays
/// on it.
const WORKER_STACK: usize = 32 * 1024 * 1024;
/// Longest the game waits for a tick's pose before it goes on without one.
const STEP_WAIT: Duration = Duration::from_millis(250);
const ACTIVATE_WAIT: Duration = Duration::from_secs(5);
/// Most simulated time one step may catch up on.
const MAX_CATCH_UP: f32 = 0.15;

pub(crate) fn to_skate(p: Vec3) -> Vec3 {
    Vec3::new(p.x, p.z, -p.y) * METRES
}
pub(crate) fn from_skate(p: Vec3) -> Vec3 {
    Vec3::new(p.x, -p.z, p.y) / METRES
}
pub(crate) fn direction_from_skate(v: Vec3) -> Vec3 {
    Vec3::new(v.x, -v.z, v.y)
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct HaloSkatePad {
    pub buttons: u16,
    pub triggers: [u8; 2],
    pub left: [i16; 2],
    pub right: [i16; 2],
}

#[repr(C)]
pub struct HaloSkateFrame {
    pub position: [f32; 3],
    pub forward: [f32; 3],
    pub up: [f32; 3],
    /// World units per second.
    pub velocity: [f32; 3],
    pub camera_position: [f32; 3],
    pub camera_forward: [f32; 3],
    pub camera_up: [f32; 3],
    pub camera_field_of_view: f32,
    pub has_camera: i32,
    pub tick: u32,
    pub state: [c_char; 32],
}

enum Job {
    /// Makes the session, on a placeholder floor, before any map.
    Preload { root: PathBuf },
    Load { root: PathBuf, triangles: Vec<[Vec3; 3]>, generation: u64 },
    Activate { sequence: u64, spawn: Vec3, yaw: f32 },
    Step { sequence: u64, pad: HaloSkatePad, dt: f32 },
    Suspend,
}

enum Reply {
    Preloaded,
    Loaded { triangles: usize, rails: usize, generation: u64 },
    /// The board of the skater model the session was made from (`board.rs`),
    /// or None when it has none to draw.
    Board(Option<Arc<board::Board>>),
    Pose(u64, Pose),
    Error(String),
}

struct Host {
    jobs: Sender<Job>,
    replies: Receiver<Reply>,
    /// The last map sent, shared with the worker so that it skips a map the
    /// game has already replaced (a BSP switched again before it was built).
    generation: Arc<AtomicU64>,
    /// The session is being made (`halo_skate_preload`).
    preloading: bool,
    /// The last map sent is being built; `ready` once it is installed.
    loading: bool,
    ready: bool,
    error: Option<CString>,
    sequence: u64,
    pose: Option<Pose>,
    rig: Option<rig::Rig>,
    board: Option<Arc<board::Board>>,
    /// Changes whenever another board is loaded, so that the game takes its
    /// mesh and textures again.
    board_generation: u32,
    /// Whether a failure to skin the board was reported (once).
    board_warned: bool,
}

static HOST: Mutex<Option<Host>> = Mutex::new(None);
/// Boards loaded so far, across engine restarts: each one's generation.
static BOARD_GENERATIONS: AtomicU32 = AtomicU32::new(0);

fn with_host<T>(f: impl FnOnce(&mut Host) -> T) -> Option<T> {
    let mut guard = HOST.lock().unwrap_or_else(|e| e.into_inner());
    guard.as_mut().map(f)
}

impl Host {
    fn start() -> Result<Self, String> {
        let (jobs, job_receiver) = mpsc::channel();
        let (reply_sender, replies) = mpsc::channel();
        let generation = Arc::new(AtomicU64::new(0));
        let latest = Arc::clone(&generation);
        std::thread::Builder::new()
            .name("skate".into())
            .stack_size(WORKER_STACK)
            .spawn(move || worker(job_receiver, reply_sender, latest))
            .map_err(|e| format!("skate worker: {e}"))?;
        Ok(Self {
            jobs,
            replies,
            generation,
            preloading: false,
            loading: false,
            ready: false,
            error: None,
            sequence: 0,
            pose: None,
            rig: None,
            board: None,
            board_generation: 0,
            board_warned: false,
        })
    }

    fn fail(&mut self, message: String) {
        eprintln!("halo-skate: {message}");
        self.ready = false;
        self.loading = false;
        self.preloading = false;
        self.error = Some(CString::new(message.replace('\0', " ")).unwrap_or_default());
    }

    fn take(&mut self, reply: Reply) {
        match reply {
            Reply::Preloaded => self.preloading = false,
            Reply::Loaded {
                triangles,
                rails,
                generation,
            } => {
                // (a map sent since is still on its way)
                if generation == self.generation.load(Ordering::SeqCst) {
                    eprintln!("halo-skate: map ready, {triangles} triangles, {rails} rails");
                    self.loading = false;
                    self.ready = true;
                }
            }
            Reply::Board(board) => {
                self.board = board;
                self.board_generation = BOARD_GENERATIONS.fetch_add(1, Ordering::Relaxed).wrapping_add(1).max(1);
                self.board_warned = false;
            }
            Reply::Pose(_, pose) => self.pose = Some(pose),
            Reply::Error(message) => self.fail(message),
        }
    }

    fn drain(&mut self) {
        loop {
            match self.replies.try_recv() {
                Ok(reply) => self.take(reply),
                Err(mpsc::TryRecvError::Empty) => break,
                Err(mpsc::TryRecvError::Disconnected) => {
                    if self.error.is_none() {
                        self.fail("the skate engine stopped".into());
                    }
                    break;
                }
            }
        }
    }

    /// Sends `job` and waits for the pose answering `sequence`.
    fn request(&mut self, job: Job, sequence: u64, wait: Duration) -> bool {
        if self.jobs.send(job).is_err() {
            self.fail("the skate engine stopped".into());
            return false;
        }
        let deadline = std::time::Instant::now() + wait;
        loop {
            let left = deadline.saturating_duration_since(std::time::Instant::now());
            match self.replies.recv_timeout(left) {
                Ok(Reply::Pose(answer, pose)) => {
                    self.pose = Some(pose);
                    if answer == sequence {
                        return true;
                    }
                }
                Ok(reply) => {
                    self.take(reply);
                    if self.error.is_some() {
                        return false;
                    }
                }
                Err(RecvTimeoutError::Timeout) => return false,
                Err(RecvTimeoutError::Disconnected) => {
                    self.fail("the skate engine stopped".into());
                    return false;
                }
            }
        }
    }
}

/// Starts the worker unless one is running (one that failed is replaced).
fn host_started(guard: &mut Option<Host>) -> Option<&mut Host> {
    if guard.as_ref().is_none_or(|h| h.error.is_some()) {
        match Host::start() {
            Ok(host) => *guard = Some(host),
            Err(e) => {
                eprintln!("halo-skate: {e}");
                return None;
            }
        }
    }
    guard.as_mut()
}

fn worker(jobs: Receiver<Job>, replies: Sender<Reply>, latest: Arc<AtomicU64>) {
    let mut session: Option<(PathBuf, Session)> = None;
    // the assets the board was last read from
    let mut board_root: Option<PathBuf> = None;
    let mut accumulated = 0.0f32;
    let mut packet = 0u32;
    let current = |generation: u64| generation == latest.load(Ordering::SeqCst);
    for job in jobs {
        let reply = match job {
            Job::Preload { root } => preload(&mut session, root),
            Job::Load {
                root,
                triangles,
                generation,
            } => {
                // (a map replaced before its turn came is not built at all)
                if !current(generation) {
                    continue;
                }
                match load(&mut session, root, triangles, || current(generation)) {
                    Some(Ok((triangles, rails))) => Reply::Loaded {
                        triangles,
                        rails,
                        generation,
                    },
                    Some(Err(e)) => Reply::Error(e),
                    None => continue,
                }
            }
            Job::Activate {
                sequence,
                spawn,
                yaw,
            } => match session.as_mut() {
                Some((_, s)) => {
                    accumulated = 0.0;
                    // Skate's board faces its local +Z; a yaw of zero faces +X.
                    s.activate(to_skate(spawn).to_array(), yaw + std::f32::consts::FRAC_PI_2)
                        .map(|pose| Reply::Pose(sequence, pose))
                        .unwrap_or_else(Reply::Error)
                }
                None => Reply::Error("no map is loaded for skating".into()),
            },
            Job::Step { sequence, pad, dt } => match session.as_mut() {
                Some((_, s)) => {
                    let period = s.period();
                    accumulated = (accumulated + dt).min(MAX_CATCH_UP);
                    let mut result = Ok(());
                    // One input sample a simulated tick: the flick and gesture
                    // recognisers read the stick a sample at a time.
                    while accumulated >= period && result.is_ok() {
                        packet = packet.wrapping_add(1);
                        s.collect(
                            InputFrame::from_pad(pad.buttons, pad.triggers, pad.left, pad.right, packet),
                            period,
                        );
                        result = s.advance();
                        accumulated -= period;
                    }
                    match result {
                        Ok(()) => {
                            let pose = s.pose();
                            if pose.root.is_finite() && pose.bones.iter().all(|b| b.is_finite()) {
                                Reply::Pose(sequence, pose)
                            } else {
                                Reply::Error("the skater's pose is not finite".into())
                            }
                        }
                        Err(e) => Reply::Error(e),
                    }
                }
                None => Reply::Error("no map is loaded for skating".into()),
            },
            Job::Suspend => {
                if let Some((_, s)) = session.as_mut() {
                    s.suspend_input();
                }
                continue;
            }
        };
        // The board goes with the skater: read once the session is made from
        // another assets folder (at the preload, or with a map when none was
        // preloaded), before the reply that says it is ready.
        if let Some((root, _)) = session.as_ref()
            && board_root.as_ref() != Some(root)
        {
            board_root = Some(root.clone());
            if replies.send(Reply::Board(load_board(root))).is_err() {
                return;
            }
        }
        let failed = matches!(reply, Reply::Error(_));
        if replies.send(reply).is_err() || failed {
            return;
        }
    }
}

/// A flat floor, 20 m square at the origin, for the session to be made on
/// before a map is known: `Session::new` wants a collision.
fn placeholder_floor() -> Vec<[[f32; 3]; 3]> {
    let [a, b, c, d] = [
        [-10.0, 0.0, -10.0],
        [-10.0, 0.0, 10.0],
        [10.0, 0.0, 10.0],
        [10.0, 0.0, -10.0],
    ];
    // (wound counterclockwise seen from above, +Y)
    vec![[a, b, c], [a, c, d]]
}

fn preload(session: &mut Option<(PathBuf, Session)>, root: PathBuf) -> Reply {
    if session.as_ref().is_some_and(|(loaded, _)| *loaded == root) {
        return Reply::Preloaded;
    }
    let started = Instant::now();
    eprintln!("halo-skate: preloading the skater from {}", root.display());
    if let Err(e) = Session::preload(&root) {
        return Reply::Error(e);
    }
    eprintln!("halo-skate: preload: animation banks in {}ms", started.elapsed().as_millis());
    let made = Instant::now();
    match Session::new(&root, placeholder_floor(), Vec::new(), [0.0; 3], 0.0) {
        Ok(s) => {
            eprintln!(
                "halo-skate: preload: session in {}ms, {}ms in all",
                made.elapsed().as_millis(),
                started.elapsed().as_millis()
            );
            *session = Some((root, s));
            Reply::Preloaded
        }
        Err(e) => Reply::Error(e),
    }
}

/// Builds the map's collision and puts it in the session (making the session
/// now, the slow way, when no preload made one for `root`). Returns the counts
/// of triangles and rails, or None when `current` says the game sent another
/// map while this one was built.
fn load(
    session: &mut Option<(PathBuf, Session)>,
    root: PathBuf,
    triangles: Vec<[Vec3; 3]>,
    current: impl Fn() -> bool,
) -> Option<Result<(usize, usize), String>> {
    let started = Instant::now();
    let (triangles, rails) = prepare(triangles);
    let counts = (triangles.len(), rails.len());
    eprintln!(
        "halo-skate: {} triangles, {} rails in {}ms",
        counts.0,
        counts.1,
        started.elapsed().as_millis()
    );
    let result = match session {
        Some((loaded, s)) if *loaded == root => {
            let built = Instant::now();
            let prepared = s.collision_builder().build(triangles, rails);
            eprintln!("halo-skate: collision built in {}ms", built.elapsed().as_millis());
            if !current() {
                return None;
            }
            let installed = Instant::now();
            let result = prepared.and_then(|prepared| s.install_collision(prepared));
            eprintln!("halo-skate: collision installed in {}ms", installed.elapsed().as_millis());
            result
        }
        _ => {
            let made = Instant::now();
            Session::new(&root, triangles, rails, [0.0; 3], 0.0).map(|s| {
                eprintln!("halo-skate: session made with the map in {}ms", made.elapsed().as_millis());
                *session = Some((root, s));
            })
        }
    };
    eprintln!("halo-skate: map loaded in {}ms in all", started.elapsed().as_millis());
    Some(result.map(|()| counts))
}

/// The board of the skater model in `root`, or None (reported) when it has
/// none to draw.
fn load_board(root: &Path) -> Option<Arc<board::Board>> {
    let started = Instant::now();
    match board::Board::load(root) {
        Ok(board) => {
            eprintln!(
                "halo-skate: board: {} vertices, {} triangles, {} textures in {}ms",
                board.vertices.len(),
                board.indices.len() / 3,
                board.textures.len(),
                started.elapsed().as_millis()
            );
            Some(Arc::new(board))
        }
        Err(e) => {
            eprintln!("halo-skate: the board is not drawn: {e}");
            None
        }
    }
}

/// The map's triangles and grind rails in Skate space. Triangles are sorted
/// along a Morton curve: the engine bounds them in runs of 64 in the order
/// given, so neighbours in the list should be neighbours on the map.
fn prepare(triangles: Vec<[Vec3; 3]>) -> (Vec<[[f32; 3]; 3]>, Vec<Vec<[f32; 3]>>) {
    let started = Instant::now();
    let mut seen = std::collections::HashSet::new();
    let mut inches: Vec<[Vec3; 3]> = triangles
        .into_iter()
        .map(|t| t.map(|p| p * INCHES))
        .filter(|t| {
            t.iter().all(|p| p.is_finite())
                && (t[1] - t[0]).cross(t[2] - t[0]).length_squared() > 0.001
        })
        .filter(|t| {
            let mut key = t.map(|p| p.to_array().map(|x| (x * 8.0).round() as i32));
            key.sort();
            seen.insert(key)
        })
        .collect();
    inches.sort_by_cached_key(|t| morton((t[0] + t[1] + t[2]) / (3.0 * INCHES)));
    eprintln!(
        "halo-skate: {} triangles deduplicated and sorted in {}ms",
        inches.len(),
        started.elapsed().as_millis()
    );
    let finding = Instant::now();
    let (found, census) = rails::find(&inches);
    eprintln!(
        "halo-skate: rails: {} walkable edges, {} lips, {} runs, {} rails in {}ms",
        census.candidates,
        census.lips,
        census.runs,
        census.rails,
        finding.elapsed().as_millis()
    );
    let skate = |p: Vec3| to_skate(p / INCHES).to_array();
    (
        inches.iter().map(|t| t.map(skate)).collect(),
        found
            .into_iter()
            .map(|rail| rail.into_iter().map(skate).collect())
            .collect(),
    )
}

fn morton(p: Vec3) -> u64 {
    let spread = |v: f32| {
        let mut x = ((v + 1024.0).clamp(0.0, 2047.0) * 16.0) as u64 & 0x1f_ffff;
        x = (x | x << 32) & 0x1f_0000_0000_ffff;
        x = (x | x << 16) & 0x1f_0000_ff00_00ff;
        x = (x | x << 8) & 0x100f_00f0_0f00_f00f;
        x = (x | x << 4) & 0x10c3_0c30_c30c_30c3;
        (x | x << 2) & 0x1249_2492_4924_9249
    };
    spread(p.x) | spread(p.y) << 1 | spread(p.z) << 2
}

fn write_frame(pose: &Pose, out: &mut HaloSkateFrame) {
    let root = pose.root;
    out.position = from_skate(root.w_axis.truncate()).to_array();
    out.forward = direction_from_skate(root.z_axis.truncate()).normalize_or_zero().to_array();
    out.up = direction_from_skate(root.y_axis.truncate()).normalize_or_zero().to_array();
    out.velocity = (direction_from_skate(pose.velocity) / METRES).to_array();
    match pose.camera {
        Some((position, basis, fov)) => {
            let basis: Mat3 = basis;
            out.camera_position = from_skate(position).to_array();
            // The camera looks down its -Z, with +Y up.
            out.camera_forward = direction_from_skate(-basis.z_axis).normalize_or_zero().to_array();
            out.camera_up = direction_from_skate(basis.y_axis).normalize_or_zero().to_array();
            out.camera_field_of_view = fov;
            out.has_camera = 1;
        }
        None => out.has_camera = 0,
    }
    out.tick = pose.tick as u32;
    out.state = [0; 32];
    for (slot, byte) in out.state.iter_mut().zip(pose.state.bytes().take(31)) {
        *slot = byte as c_char;
    }
}

/// The game's log function, `halo_skate_set_log`'s.
static LOG: Mutex<Option<unsafe extern "C" fn(*const c_char)>> = Mutex::new(None);

fn log_to_game(line: &str) {
    let log = *LOG.lock().unwrap_or_else(|e| e.into_inner());
    match (log, CString::new(line.replace('\0', " "))) {
        (Some(log), Ok(line)) => unsafe { log(line.as_ptr()) },
        _ => std::eprintln!("{line}"),
    }
}

/// Sends the engine's log lines (load timings, failures) to `log`, one line
/// per call without its line end, from any thread; NULL sends them back to
/// stderr. A Windows release build has no console, so stderr is lost there.
///
/// # Safety
/// `log` is callable from any thread for as long as it is set.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_set_log(log: Option<unsafe extern "C" fn(*const c_char)>) {
    *LOG.lock().unwrap_or_else(|e| e.into_inner()) = log;
    skate_host::log::set_sink(log.map(|_| log_to_game as fn(&str)));
}

/// Makes the skate session in the background, before any map, so that a map
/// later only has its collision to build. `assets` is the folder the Skate 3
/// converter wrote (`.../assets`). Returns 0 when the session is on its way,
/// or -1 when `assets` is not a folder or the engine could not start. Never
/// waits for the load; `halo_skate_state` tells of its failure.
///
/// # Safety
/// `assets` is a C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_preload(assets: *const c_char) -> i32 {
    if assets.is_null() {
        return -1;
    }
    let root = PathBuf::from(unsafe { CStr::from_ptr(assets) }.to_string_lossy().into_owned());
    if !root.is_dir() {
        eprintln!("halo-skate: no Skate 3 data at {}, nothing preloaded", root.display());
        return -1;
    }
    let mut guard = HOST.lock().unwrap_or_else(|e| e.into_inner());
    let Some(host) = host_started(&mut guard) else {
        return -1;
    };
    host.preloading = true;
    if host.jobs.send(Job::Preload { root }).is_err() {
        host.fail("the skate engine stopped".into());
        return -1;
    }
    0
}

/// Loads, or reloads, the collision the skater rides: `count` triangles of
/// nine floats each, in world units, wound counterclockwise seen from outside.
/// `assets` is the folder the Skate 3 converter wrote (`.../assets`). The
/// collision is built in the background, after any preload still under way;
/// a map sent before the last one was built replaces it. Returns 0, or -1
/// when the engine could not be started.
///
/// # Safety
/// `assets` is a C string and `triangles` holds `count * 9` floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_load(assets: *const c_char, triangles: *const f32, count: i32) -> i32 {
    if assets.is_null() || (triangles.is_null() && count > 0) {
        return -1;
    }
    let root = PathBuf::from(unsafe { CStr::from_ptr(assets) }.to_string_lossy().into_owned());
    let floats = unsafe { std::slice::from_raw_parts(triangles, count.max(0) as usize * 9) };
    let triangles: Vec<[Vec3; 3]> = floats
        .chunks_exact(9)
        .map(|t| {
            [
                Vec3::new(t[0], t[1], t[2]),
                Vec3::new(t[3], t[4], t[5]),
                Vec3::new(t[6], t[7], t[8]),
            ]
        })
        .collect();
    let mut guard = HOST.lock().unwrap_or_else(|e| e.into_inner());
    let Some(host) = host_started(&mut guard) else {
        return -1;
    };
    host.ready = false;
    host.loading = true;
    let generation = host.generation.fetch_add(1, Ordering::SeqCst) + 1;
    eprintln!(
        "halo-skate: map {generation} sent, {} triangles{}",
        triangles.len(),
        if host.preloading { " (after the preload)" } else { "" }
    );
    if host.jobs.send(Job::Load { root, triangles, generation }).is_err() {
        host.fail("the skate engine stopped".into());
        return -1;
    }
    0
}

/// The map's collision: 0: nothing loaded, 1: loading, 2: ready, -1: failed
/// (`halo_skate_error`; a failed preload too).
#[unsafe(no_mangle)]
pub extern "C" fn halo_skate_state() -> i32 {
    with_host(|host| {
        host.drain();
        if host.error.is_some() {
            -1
        } else if host.ready {
            2
        } else if host.loading {
            1
        } else {
            0
        }
    })
    .unwrap_or(0)
}

/// 1 while the session is still being made (`halo_skate_preload`), else 0.
#[unsafe(no_mangle)]
pub extern "C" fn halo_skate_preloading() -> i32 {
    with_host(|host| {
        host.drain();
        host.preloading as i32
    })
    .unwrap_or(0)
}

/// The last failure, or an empty string. Valid until the next call.
#[unsafe(no_mangle)]
pub extern "C" fn halo_skate_error() -> *const c_char {
    static EMPTY: &CStr = c"";
    with_host(|host| host.error.as_ref().map(|e| e.as_ptr()))
        .flatten()
        .unwrap_or(EMPTY.as_ptr())
}

/// Puts the skater on the board at `position` (world units, on the ground)
/// facing `yaw` radians from +X. Returns 0 with `out` filled, or -1.
///
/// # Safety
/// `position` holds 3 floats; `out` is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_activate(position: *const f32, yaw: f32, out: *mut HaloSkateFrame) -> i32 {
    if position.is_null() || out.is_null() {
        return -1;
    }
    let p = unsafe { std::slice::from_raw_parts(position, 3) };
    let spawn = Vec3::new(p[0], p[1], p[2]);
    with_host(|host| {
        host.drain();
        if !host.ready {
            return -1;
        }
        host.sequence += 1;
        let sequence = host.sequence;
        if !host.request(Job::Activate { sequence, spawn, yaw }, sequence, ACTIVATE_WAIT) {
            return -1;
        }
        match host.pose.as_ref() {
            Some(pose) => {
                write_frame(pose, unsafe { &mut *out });
                0
            }
            None => -1,
        }
    })
    .unwrap_or(-1)
}

/// Runs `dt` seconds of skating with `pad` held. Returns 0 with `out` filled
/// from the new pose, 1 when the engine did not answer in time (`out` holds the
/// last pose), or -1 on failure.
///
/// # Safety
/// `pad` is readable and `out` is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_step(pad: *const HaloSkatePad, dt: f32, out: *mut HaloSkateFrame) -> i32 {
    if pad.is_null() || out.is_null() {
        return -1;
    }
    let pad = unsafe { *pad };
    with_host(|host| {
        host.drain();
        if !host.ready {
            return -1;
        }
        host.sequence += 1;
        let sequence = host.sequence;
        let answered = host.request(Job::Step { sequence, pad, dt }, sequence, STEP_WAIT);
        if host.error.is_some() {
            return -1;
        }
        match host.pose.as_ref() {
            Some(pose) => {
                write_frame(pose, unsafe { &mut *out });
                if answered { 0 } else { 1 }
            }
            None => -1,
        }
    })
    .unwrap_or(-1)
}

/// Drops the input the engine holds, as the skater leaves the board.
#[unsafe(no_mangle)]
pub extern "C" fn halo_skate_suspend() {
    with_host(|host| {
        let _ = host.jobs.send(Job::Suspend);
    });
}

/// Describes the biped's skeleton: `count` nodes, each with a 32-byte name,
/// its parent's index (-1 for none) and the 13 floats of its default inverse
/// matrix (scale, forward, left, up, position). Returns how many nodes follow
/// a skater bone.
///
/// # Safety
/// `names` holds `count * 32` bytes, `parents` `count` shorts and
/// `inverse_defaults` `count * 13` floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_set_skeleton(
    count: i32,
    names: *const c_char,
    parents: *const i16,
    inverse_defaults: *const f32,
) -> i32 {
    if count <= 0 || names.is_null() || parents.is_null() || inverse_defaults.is_null() {
        return 0;
    }
    let count = count as usize;
    let names = unsafe { std::slice::from_raw_parts(names.cast::<u8>(), count * 32) };
    let parents = unsafe { std::slice::from_raw_parts(parents, count) };
    let matrices = unsafe { std::slice::from_raw_parts(inverse_defaults, count * 13) };
    let nodes: Vec<rig::NodeDefault> = (0..count)
        .map(|i| {
            let raw = &names[i * 32..i * 32 + 32];
            let end = raw.iter().position(|&b| b == 0).unwrap_or(32);
            rig::NodeDefault {
                name: String::from_utf8_lossy(&raw[..end]).into_owned(),
                parent: usize::try_from(parents[i]).ok().filter(|&p| p < count),
                inverse: rig::Matrix::from_floats(&matrices[i * 13..i * 13 + 13]),
            }
        })
        .collect();
    let rig = rig::Rig::new(nodes);
    let mapped = rig.mapped() as i32;
    with_host(|host| host.rig = Some(rig));
    mapped
}

/// Writes the biped's node matrices (13 floats each, world space) posed as
/// the skater of the last frame. Returns the number written, or 0.
///
/// # Safety
/// `out` holds room for `capacity * 13` floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_pose_nodes(out: *mut f32, capacity: i32) -> i32 {
    if out.is_null() || capacity <= 0 {
        return 0;
    }
    let out = unsafe { std::slice::from_raw_parts_mut(out, capacity as usize * 13) };
    with_host(|host| match (host.rig.as_ref(), host.pose.as_ref()) {
        (Some(rig), Some(pose)) => rig.pose(pose, out) as i32,
        _ => 0,
    })
    .unwrap_or(0)
}

#[repr(C)]
pub struct HaloSkateBoardInfo {
    /// Changes whenever another board is loaded (never 0).
    pub generation: u32,
    pub vertex_count: i32,
    pub index_count: i32,
    pub surface_count: i32,
    pub texture_count: i32,
}

#[repr(C)]
pub struct HaloSkateBoardSurface {
    pub first_index: i32,
    pub index_count: i32,
    pub texture: i32,
}

/// Describes the board of the loaded assets. Returns 0 with `out` filled, or
/// -1 when there is none (the skater is not loaded yet, or its model has no board).
///
/// # Safety
/// `out` is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_board_info(out: *mut HaloSkateBoardInfo) -> i32 {
    if out.is_null() {
        return -1;
    }
    with_host(|host| {
        host.drain();
        let board = host.board.as_ref()?;
        Some(HaloSkateBoardInfo {
            generation: host.board_generation,
            vertex_count: board.vertices.len() as i32,
            index_count: board.indices.len() as i32,
            surface_count: board.surfaces.len() as i32,
            texture_count: board.textures.len() as i32,
        })
    })
    .flatten()
    .map(|info| {
        unsafe { *out = info };
        0
    })
    .unwrap_or(-1)
}

/// Writes the board's triangles (three vertex indices each) and its surfaces,
/// the runs of indices drawn with each texture. Returns 0, or -1 when there
/// is no board or the room given is short.
///
/// # Safety
/// `indices` holds room for `index_capacity` and `surfaces` for
/// `surface_capacity` elements.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_board_mesh(
    indices: *mut u32,
    index_capacity: i32,
    surfaces: *mut HaloSkateBoardSurface,
    surface_capacity: i32,
) -> i32 {
    if indices.is_null() || surfaces.is_null() {
        return -1;
    }
    with_host(|host| {
        let board = host.board.as_ref()?;
        if board.indices.len() > index_capacity.max(0) as usize || board.surfaces.len() > surface_capacity.max(0) as usize {
            return None;
        }
        let out = unsafe { std::slice::from_raw_parts_mut(indices, board.indices.len()) };
        out.copy_from_slice(&board.indices);
        let out = unsafe { std::slice::from_raw_parts_mut(surfaces, board.surfaces.len()) };
        for (out, surface) in out.iter_mut().zip(&board.surfaces) {
            *out = HaloSkateBoardSurface {
                first_index: surface.first_index as i32,
                index_count: surface.index_count as i32,
                texture: surface.texture as i32,
            };
        }
        Some(0)
    })
    .flatten()
    .unwrap_or(-1)
}

/// Gives the board's texture `index`, RGBA rows from the top: its size, and
/// with `rgba` (else null) its pixels. Returns 0, or -1 when there is no such
/// texture or `rgba` is short.
///
/// # Safety
/// `width` and `height` are writable; `rgba` is null or holds `capacity`
/// bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_board_texture(
    index: i32,
    width: *mut i32,
    height: *mut i32,
    rgba: *mut u8,
    capacity: i32,
) -> i32 {
    if width.is_null() || height.is_null() {
        return -1;
    }
    with_host(|host| {
        let texture = host.board.as_ref()?.textures.get(usize::try_from(index).ok()?)?;
        unsafe {
            *width = texture.width as i32;
            *height = texture.height as i32;
        }
        if !rgba.is_null() {
            if texture.rgba.len() > capacity.max(0) as usize {
                return None;
            }
            unsafe { std::slice::from_raw_parts_mut(rgba, texture.rgba.len()) }.copy_from_slice(&texture.rgba);
        }
        Some(0)
    })
    .flatten()
    .unwrap_or(-1)
}

/// Writes the board's vertices skinned by the skater's last pose, 8 floats
/// each: the position in world units, the unit normal and the texture
/// coordinate. Returns how many were written, or 0 (no board, no pose, or the
/// skater lacks a bone the board follows, which is reported once).
///
/// # Safety
/// `out` holds room for `capacity * 8` floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_board_vertices(out: *mut f32, capacity: i32) -> i32 {
    if out.is_null() || capacity <= 0 {
        return 0;
    }
    let out = unsafe { std::slice::from_raw_parts_mut(out, capacity as usize * board::VERTEX_FLOATS) };
    with_host(|host| {
        let (Some(board), Some(pose)) = (host.board.as_ref(), host.pose.as_ref()) else {
            return 0;
        };
        match board.skin(pose, out) {
            Ok(count) => count as i32,
            Err(e) => {
                if !host.board_warned {
                    eprintln!("halo-skate: the board is not drawn: {e}");
                    host.board_warned = true;
                }
                0
            }
        }
    })
    .unwrap_or(0)
}

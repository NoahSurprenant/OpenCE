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
//!
//! Several skaters: up to `MAXIMUM_SESSIONS` sessions, each with its own
//! worker, session, collision and pose. Session 0 is this machine's player's,
//! which the calls without a session number drive; the others (the host's
//! bots) are driven by the `_in` calls, which take the session first. They
//! share what never changes: the animation banks (the engine's own cache),
//! the board's mesh (`load_board`) and the game's settings. Only session 0
//! plays its sounds here; the others' are kept for the game to send
//! (`sound_remote.rs`).

/// The crate's log lines go where the engine's go (`skate_host::log`): to the
/// game's log once `halo_skate_set_log` is called, else to stderr; another
/// session's than 0 say whose (`log_line`).
macro_rules! eprintln {
    ($($arg:tt)*) => {
        $crate::log_line(&format!($($arg)*))
    };
}

mod board;
mod camera;
mod mixer;
pub mod obstacles;
mod pool;
pub use obstacles::HaloSkateObstacle;
pub use sound_remote::{HaloSkateSoundEvent, HaloSkateSoundLoops, halo_skate_sound_take, halo_skate_sound_take_in};
mod rails;
mod recovery;
mod rig;
mod sound;
mod sound_defaults;
mod sound_remote;
mod sound_set;

use bevy_math::{Mat3, Mat4, Vec3};
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
pub(crate) const WORKER_STACK: usize = 32 * 1024 * 1024;
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
    /// The board's, world units per second.
    pub velocity: [f32; 3],
    /// The skater's own off the board, world units per second.
    pub skater_velocity: [f32; 3],
    /// The way the skater faces (`Pose::facing`), a unit vector.
    pub facing: [f32; 3],
    /// 1 while the skater is off the board (walking, or jumping off it).
    pub off_board: i32,
    pub camera_position: [f32; 3],
    pub camera_forward: [f32; 3],
    pub camera_up: [f32; 3],
    pub camera_field_of_view: f32,
    pub has_camera: i32,
    pub tick: u32,
    pub state: [c_char; 32],
}

pub(crate) enum Job {
    /// Makes the session, on a placeholder floor, before any map.
    Preload { root: PathBuf },
    Load { root: PathBuf, triangles: Vec<[Vec3; 3]>, generation: u64 },
    /// The same level's collision again (scenery made or gone): built aside,
    /// the skater riding the old meanwhile (`halo_skate_reload`).
    Reload { root: PathBuf, triangles: Vec<[Vec3; 3]>, generation: u64 },
    /// On the board at `spawn`; with `velocity` (world units a second) moving
    /// so from the start, as a teleporter leaves a skater.
    Activate { sequence: u64, spawn: Vec3, yaw: f32, velocity: Option<Vec3> },
    /// The obstacles the next steps collide with (`obstacles.rs`).
    Obstacles { obstacles: Vec<skate_host::bridge::KinematicObstacle>, bail_speed: f32, run_over: (f32, f32) },
    /// A blast's push (metres a second) on the skater, before the next step
    /// (`halo_skate_apply_impulse`).
    Impulse { velocity: Vec3 },
    /// A bail the game's host decided (`halo_skate_force_bail`): its hit's
    /// closing speed, metres a second.
    ForceBail { closing: f32 },
    Step { sequence: u64, pad: HaloSkatePad, dt: f32 },
    Suspend,
}

pub(crate) enum Reply {
    Preloaded,
    Loaded { triangles: usize, rails: usize, generation: u64 },
    /// A reload installed (`Job::Reload`).
    Reloaded { triangles: usize, rails: usize, generation: u64 },
    /// The board of the skater model the session was made from (`board.rs`),
    /// or None when it has none to draw.
    Board(Option<Arc<board::Board>>),
    /// The pose answering a request; `recovered` when a step failed and the
    /// skater was put back on the board in its place (`recovery.rs`).
    Pose {
        sequence: u64,
        pose: Pose,
        recovered: bool,
        /// hits on held obstacles since the last pose (`Session::take_held`)
        held: skate_host::bridge::HeldReport,
    },
    Error(String),
}

struct Host {
    jobs: Jobs,
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
    /// A pose since the last step's came from putting the skater back on the
    /// board after a failed step.
    recovered: bool,
    /// The biped's skeleton (`halo_skate_set_skeleton`), kept across engine
    /// restarts (`host_started`): the game describes it once per biped.
    rig: Option<rig::Rig>,
    board: Option<Arc<board::Board>>,
    /// Changes whenever another board is loaded, so that the game takes its
    /// mesh and textures again.
    board_generation: u32,
    /// Whether a failure to skin the board was reported (once).
    board_warned: bool,
    /// The board skinned for posing the biped on its deck, kept for its room.
    deck: Vec<f32>,
    /// Hits on held obstacles the game has not taken yet
    /// (`halo_skate_take_held_hit`): the fastest, and whether a bail was held.
    held: skate_host::bridge::HeldReport,
}

/// The most skate sessions at once: this machine's player's (0) and the
/// host's bots' (`halo_skate_sessions`).
pub const MAXIMUM_SESSIONS: usize = 129;

/// Each session's engine, by its number.
static HOSTS: [Mutex<Option<Host>>; MAXIMUM_SESSIONS] = [const { Mutex::new(None) }; MAXIMUM_SESSIONS];

thread_local! {
    /// The session this thread's calls are for: 0, or the one an `_in` call
    /// gives (`in_session`); a worker's own.
    pub(crate) static SESSION: std::cell::Cell<usize> = const { std::cell::Cell::new(0) };
}

/// Where a session's jobs go: session 0's (this machine's player's) to a
/// thread of its own, the others' to the pool (`pool.rs`).
enum Jobs {
    Thread(Sender<Job>),
    Pool(Arc<pool::Pooled>),
}

impl Jobs {
    fn send(&self, job: Job) -> Result<(), ()> {
        match self {
            Jobs::Thread(sender) => sender.send(job).map_err(|_| ()),
            Jobs::Pool(pooled) => pooled.send(job),
        }
    }
}

impl Drop for Jobs {
    /// (a pooled session ends with its host: its memory goes)
    fn drop(&mut self) {
        if let Jobs::Pool(pooled) = self {
            pooled.close();
        }
    }
}

/// The session this thread's calls are for.
pub(crate) fn current_session() -> usize {
    SESSION.with(|s| s.get())
}

/// Runs `f` for `session` (the `_in` calls): `fallback` for a session
/// that is not one.
pub(crate) fn in_session<T>(session: i32, fallback: T, f: impl FnOnce() -> T) -> T {
    let Some(session) = usize::try_from(session).ok().filter(|&s| s < MAXIMUM_SESSIONS) else {
        return fallback;
    };
    let previous = SESSION.with(|s| s.replace(session));
    let result = f();
    SESSION.with(|s| s.set(previous));
    result
}

/// A log line, told as another session's than 0 when it is (`eprintln!`).
pub(crate) fn log_line(line: &str) {
    let session = current_session();
    match line.strip_prefix("halo-skate: ") {
        Some(rest) if session != 0 => skate_host::log::write(&format!("halo-skate (session {session}): {rest}")),
        _ => skate_host::log::write(line),
    }
}

/// The current session's engine, locked.
fn host_guard() -> std::sync::MutexGuard<'static, Option<Host>> {
    HOSTS[current_session()].lock().unwrap_or_else(|e| e.into_inner())
}
/// Boards loaded so far, across engine restarts: each one's generation.
static BOARD_GENERATIONS: AtomicU32 = AtomicU32::new(0);

/// A float the game sets, kept across engine restarts.
struct Setting(AtomicU32);

impl Setting {
    const fn new(value: f32) -> Self {
        Self(AtomicU32::new(value.to_bits()))
    }
    fn get(&self) -> f32 {
        f32::from_bits(self.0.load(Ordering::Relaxed))
    }
    fn set(&self, value: f32) {
        self.0.store(value.to_bits(), Ordering::Relaxed);
    }
}

/// The board grown about its middle: Master Chief is larger than the skater
/// it was made for (`halo_skate_set_board_scale`).
static BOARD_SCALE: Setting = Setting::new(1.05);
/// The biped's ankle node above its soles, world units; 0 until the game
/// says (`halo_skate_set_feet`).
static ANKLE_HEIGHT: Setting = Setting::new(0.0);
/// Raises the riding biped along the board's up, world units.
static FEET_OFFSET: Setting = Setting::new(0.0);
/// How fast (world units a second) running into a person or a vehicle
/// knocks the skater over (`halo_skate_set_obstacle_bail_speed`): 6 m/s.
static OBSTACLE_BAIL_SPEED: Setting = Setting::new(6.0 / METRES);
/// Running over people on foot (`halo_skate_set_run_over`): the least
/// closing speed (world units a second; below 0, nobody is run over, as at
/// first) and the share of their speed the board and the skater keep.
static RUN_OVER_SPEED: Setting = Setting::new(-1.0);
static RUN_OVER_KEEP: Setting = Setting::new(1.0);

fn with_host<T>(f: impl FnOnce(&mut Host) -> T) -> Option<T> {
    let mut guard = host_guard();
    guard.as_mut().map(f)
}

impl Host {
    fn start() -> Result<Self, String> {
        let (reply_sender, replies) = mpsc::channel();
        let generation = Arc::new(AtomicU64::new(0));
        let latest = Arc::clone(&generation);
        let session = current_session();
        let jobs = if session == 0 {
            let (jobs, job_receiver) = mpsc::channel();
            std::thread::Builder::new()
                .name("skate".into())
                .stack_size(WORKER_STACK)
                .spawn(move || {
                    SESSION.with(|s| s.set(session));
                    worker(job_receiver, reply_sender, latest)
                })
                .map_err(|e| format!("skate worker: {e}"))?;
            Jobs::Thread(jobs)
        } else {
            Jobs::Pool(pool::Pooled::new(session, Worker::new(reply_sender, latest)))
        };
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
            recovered: false,
            rig: None,
            board: None,
            board_generation: 0,
            board_warned: false,
            deck: Vec::new(),
            held: Default::default(),
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
            Reply::Reloaded {
                triangles,
                rails,
                generation,
            } => {
                if generation == self.generation.load(Ordering::SeqCst) {
                    eprintln!("halo-skate: map rebuilt, {triangles} triangles, {rails} rails");
                }
            }
            Reply::Board(board) => {
                self.board = board;
                self.board_generation = BOARD_GENERATIONS.fetch_add(1, Ordering::Relaxed).wrapping_add(1).max(1);
                self.board_warned = false;
            }
            Reply::Pose { pose, recovered, held, .. } => {
                self.pose = Some(pose);
                self.recovered |= recovered;
                self.note_held(held);
            }
            Reply::Error(message) => self.fail(message),
        }
    }

    /// Adds a pose's hits on held obstacles to those not yet taken: the
    /// fastest hit, and any bail held.
    fn note_held(&mut self, held: skate_host::bridge::HeldReport) {
        if let Some(hit) = held.hit
            && self.held.hit.is_none_or(|h| (hit.run_over && !h.run_over) || (hit.run_over == h.run_over && hit.closing > h.closing))
        {
            self.held.hit = Some(hit);
        }
        self.held.bail_held |= held.bail_held;
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
        self.await_pose(sequence, wait)
    }

    /// Waits up to `wait` for the pose answering `sequence`, taking the
    /// older answers and other replies on the way.
    fn await_pose(&mut self, sequence: u64, wait: Duration) -> bool {
        let deadline = std::time::Instant::now() + wait;
        loop {
            let left = deadline.saturating_duration_since(std::time::Instant::now());
            match self.replies.recv_timeout(left) {
                Ok(Reply::Pose {
                    sequence: answer,
                    pose,
                    recovered,
                    held,
                }) => {
                    self.pose = Some(pose);
                    self.recovered |= recovered;
                    self.note_held(held);
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
/// A replaced worker's skeleton is kept: the game describes the biped's
/// skeleton once (`skate_describe_skeleton`), not on each restart, and a
/// worker without it never poses the biped, so never holsters its weapon.
fn host_started(guard: &mut Option<Host>) -> Option<&mut Host> {
    if guard.as_ref().is_none_or(|h| h.error.is_some()) {
        match Host::start() {
            Ok(mut host) => {
                host.rig = guard.take().and_then(|failed| failed.rig);
                *guard = Some(host);
            }
            Err(e) => {
                eprintln!("halo-skate: {e}");
                return None;
            }
        }
    }
    guard.as_mut()
}

/// A session's worker: its session, and what it keeps between jobs. Session
/// 0's runs on a thread of its own (`worker`), the others' on the pool
/// (`pool.rs`), a job at a time, each session's in order.
pub(crate) struct Worker {
    replies: Sender<Reply>,
    latest: Arc<AtomicU64>,
    session: Option<(PathBuf, Session)>,
    // the assets the board was last read from
    board_root: Option<PathBuf>,
    accumulated: f32,
    packet: u32,
    // the last good pose, to put the skater back on the board from and to
    // tell of a failure by; and the failures recovered from lately
    last: Option<Last>,
    recovery: recovery::Recovery,
    hits: obstacles::HitLog,
    // the skater's sounds between ticks (sound.rs)
    detector: sound::Detector,
    // a level's collision built again aside (Job::Reload), and its generation
    rebuilding: Option<(u64, Rebuild)>,
}

impl Worker {
    fn new(replies: Sender<Reply>, latest: Arc<AtomicU64>) -> Self {
        Self {
            replies,
            latest,
            session: None,
            board_root: None,
            accumulated: 0.0,
            packet: 0,
            last: None,
            recovery: recovery::Recovery::default(),
            hits: obstacles::HitLog::default(),
            detector: sound::Detector::default(),
            rebuilding: None,
        }
    }

    /// Does a job; false when the worker is done (its game gone, or it failed).
    pub(crate) fn handle(&mut self, job: Job) -> bool {
        let Worker { replies, latest, session, board_root, accumulated, packet, last, recovery, hits, detector, rebuilding } =
            self;
        let current = |generation: u64| generation == latest.load(Ordering::SeqCst);
        // (installed between steps once built, when the skater is not on a
        // rail nor in the air, which hold the old grind world's rails)
        if let Some((generation, rebuild)) = rebuilding.take() {
            let ready = rebuild.is_finished() && session.as_ref().is_none_or(|(_, s)| installable(&s.state_name()));
            if !ready {
                *rebuilding = Some((generation, rebuild));
            } else if current(generation) {
                let reply = match (rebuild.join(), session.as_mut()) {
                    (Ok((Ok(prepared), triangles, rails)), Some((_, s))) => match s.install_collision(prepared) {
                        Ok(()) => Reply::Reloaded {
                            triangles,
                            rails,
                            generation,
                        },
                        Err(e) => Reply::Error(e),
                    },
                    (Ok((Err(e), ..)), _) => Reply::Error(e),
                    (Ok(_), None) => Reply::Error("no session to rebuild the map in".into()),
                    (Err(_), _) => Reply::Error("the map's rebuild failed".into()),
                };
                let failed = matches!(reply, Reply::Error(_));
                if replies.send(reply).is_err() || failed {
                    return false;
                }
            }
        }
        let reply = match job {
            Job::Preload { root } => preload(session, root),
            Job::Reload {
                root,
                triangles,
                generation,
            } => {
                if !current(generation) {
                    return true;
                }
                match session.as_ref() {
                    Some((loaded, s)) if *loaded == root => {
                        let builder = s.collision_builder();
                        let session = current_session();
                        let spawned = std::thread::Builder::new()
                            .name("skate-rebuild".into())
                            .stack_size(WORKER_STACK)
                            .spawn(move || {
                                // (its lines and rails are the worker's session's)
                                SESSION.with(|s| s.set(session));
                                let started = Instant::now();
                                let (triangles, rails) = prepare(triangles);
                                let counts = (triangles.len(), rails.len());
                                let built = builder.build(triangles, rails);
                                eprintln!("halo-skate: map rebuilt aside in {}ms", started.elapsed().as_millis());
                                (built, counts.0, counts.1)
                            });
                        match spawned {
                            Ok(handle) => {
                                *rebuilding = Some((generation, handle));
                                return true;
                            }
                            Err(e) => Reply::Error(format!("skate rebuild: {e}")),
                        }
                    }
                    // (no session for these assets yet: loaded as any map)
                    _ => match load(session, root, triangles, || current(generation)) {
                        Some(Ok((triangles, rails))) => Reply::Reloaded {
                            triangles,
                            rails,
                            generation,
                        },
                        Some(Err(e)) => Reply::Error(e),
                        None => return true,
                    },
                }
            }
            Job::Load {
                root,
                triangles,
                generation,
            } => {
                // (a map replaced before its turn came is not built at all)
                if !current(generation) {
                    return true;
                }
                match load(session, root, triangles, || current(generation)) {
                    Some(Ok((triangles, rails))) => Reply::Loaded {
                        triangles,
                        rails,
                        generation,
                    },
                    Some(Err(e)) => Reply::Error(e),
                    None => return true,
                }
            }
            Job::Activate {
                sequence,
                spawn,
                yaw,
                velocity,
            } => match session.as_mut() {
                Some((_, s)) => {
                    *accumulated = 0.0;
                    recovery.reset();
                    hits.reset();
                    // Skate's board faces its local +Z; a yaw of zero faces +X.
                    let heading = yaw + std::f32::consts::FRAC_PI_2;
                    let placed = match velocity {
                        Some(v) => s.teleport(to_skate(spawn).to_array(), heading, to_skate(v).to_array()),
                        None => s.activate(to_skate(spawn).to_array(), heading),
                    };
                    match placed {
                        Ok(pose) => {
                            *last = Some(Last::of(&pose));
                            sound::reset(s, detector);
                            Reply::Pose {
                                sequence,
                                pose,
                                recovered: false,
                                held: Default::default(),
                            }
                        }
                        Err(e) => Reply::Error(e),
                    }
                }
                None => Reply::Error("no map is loaded for skating".into()),
            },
            Job::Step { sequence, pad, dt } => match session.as_mut() {
                Some((_, s)) => {
                    let period = s.period();
                    *accumulated = (*accumulated + dt).min(MAX_CATCH_UP);
                    let mut result = Ok(());
                    // One input sample a simulated tick: the flick and gesture
                    // recognisers read the stick a sample at a time.
                    while *accumulated >= period && result.is_ok() {
                        *packet = packet.wrapping_add(1);
                        s.collect(
                            InputFrame::from_pad(pad.buttons, pad.triggers, pad.left, pad.right, *packet),
                            period,
                        );
                        result = s.advance();
                        if result.is_ok() {
                            sound::observe(s, detector);
                            let (contacts, hit) = s.obstacle_contacts();
                            if contacts > 0 || hits.listening() {
                                let pose = s.pose();
                                for line in hits.observe(contacts, hit, &pose.state, pose.tick) {
                                    eprintln!("{line}");
                                }
                            }
                        }
                        *accumulated -= period;
                    }
                    let held = take_held(s);
                    let failure = match result {
                        Ok(()) => {
                            let pose = s.pose();
                            if pose_is_finite(&pose) {
                                recovery.stepped();
                                *last = Some(Last::of(&pose));
                                Ok(pose)
                            } else {
                                Err("the skater's pose is not finite".to_string())
                            }
                        }
                        Err(e) => Err(e),
                    };
                    match failure {
                        Ok(pose) => Reply::Pose {
                            sequence,
                            pose,
                            recovered: false,
                            held,
                        },
                        Err(e) => {
                            *accumulated = 0.0;
                            match recover(s, &e, last.as_ref(), &pad, recovery) {
                                Some(pose) => {
                                    *last = Some(Last::of(&pose));
                                    sound::reset(s, detector);
                                    Reply::Pose {
                                        sequence,
                                        pose,
                                        recovered: true,
                                        held,
                                    }
                                }
                                None => Reply::Error(e),
                            }
                        }
                    }
                }
                None => Reply::Error("no map is loaded for skating".into()),
            },
            Job::Obstacles { obstacles, bail_speed, run_over } => {
                if let Some((_, s)) = session.as_mut() {
                    s.set_obstacles(&obstacles, bail_speed);
                    s.set_run_over(run_over.0, run_over.1);
                }
                return true;
            }
            Job::Impulse { velocity } => {
                if let Some((_, s)) = session.as_mut() {
                    s.apply_impulse(velocity.to_array());
                }
                return true;
            }
            Job::ForceBail { closing } => {
                if let Some((_, s)) = session.as_mut() {
                    s.force_bail(closing);
                }
                return true;
            }
            Job::Suspend => {
                if let Some((_, s)) = session.as_mut() {
                    s.suspend_input();
                }
                return true;
            }
        };
        // The board goes with the skater: read once the session is made from
        // another assets folder (at the preload, or with a map when none was
        // preloaded), before the reply that says it is ready.
        if let Some((root, _)) = session.as_ref()
            && board_root.as_ref() != Some(root)
        {
            *board_root = Some(root.clone());
            if replies.send(Reply::Board(load_board(root))).is_err() {
                return false;
            }
        }
        let failed = matches!(reply, Reply::Error(_));
        if replies.send(reply).is_err() || failed {
            return false;
        }
        true
    }
}

fn worker(jobs: Receiver<Job>, replies: Sender<Reply>, latest: Arc<AtomicU64>) {
    let mut worker = Worker::new(replies, latest);
    for job in jobs {
        if !worker.handle(job) {
            return;
        }
    }
}

/// A level's collision being built again aside: its build, triangles and rails.
type Rebuild = std::thread::JoinHandle<(Result<skate_host::bridge::PreparedCollision, String>, usize, usize)>;

/// What a step had to tell of hits on held obstacles (`obstacles.rs`), for
/// the game's host to decide (`halo_skate_take_held_hit`); and the log of a
/// bail held, and of the host's bail requested or given up.
fn take_held(s: &mut Session) -> skate_host::bridge::HeldReport {
    let held = s.take_held();
    if held.bail_held {
        let hit = held.hit.map_or(String::new(), |h| {
            format!(" (hit a {} at {:.1} m/s)", obstacles::kind_name(h.kind), h.closing)
        });
        eprintln!("halo-skate: a bail held for the host's verdict{hit}, engine tick {}", held.bail_held_tick);
    }
    if let Some((closing, tick)) = held.run_over {
        eprintln!(
            "halo-skate: ran over a person at {closing:.1} m/s, keeping {:.0}% of the speed, engine tick {tick}",
            RUN_OVER_KEEP.get() * 100.0
        );
    }
    if let Some(tick) = held.forced {
        eprintln!("halo-skate: bailed by the host's verdict, engine tick {tick}");
    }
    if let Some(state) = &held.dropped {
        eprintln!("halo-skate: the host's bail given up, the skater did not ride in time ({state})");
    }
    held
}

/// Whether a rebuilt collision may be swapped in with the skater in `state`:
/// not on a rail, nor in the air (where the grind selector holds the old
/// world's rails), nor being put somewhere.
fn installable(state: &str) -> bool {
    !(state.starts_with("Grind") || state.contains("Air") || state == "Teleporting" || state == "LandingOnDeck")
}

fn pose_is_finite(pose: &Pose) -> bool {
    pose.root.is_finite() && pose.bones.iter().all(|b| b.is_finite())
}

/// What a failure is told by, and the skater put back from: the last good
/// pose's root, state, velocity and tick.
struct Last {
    root: Mat4,
    state: String,
    velocity: Vec3,
    tick: u64,
}

impl Last {
    fn of(pose: &Pose) -> Self {
        Self {
            root: pose.root,
            state: pose.state.clone(),
            // off the board, the skater's own (the board may lie still or fly)
            velocity: if pose.off_board { pose.skater_velocity } else { pose.velocity },
            tick: pose.tick,
        }
    }
}

/// A step failed with `error`: tells of it in the log, with where the skater
/// was and what it did, and puts it back on the board where the last good
/// pose had it (`recovery.rs`). Returns the new pose, or None when that is
/// not to be (a spot that keeps failing, no good pose yet, or the engine
/// fails again), and the session is given up.
fn recover(
    s: &mut Session,
    error: &str,
    last: Option<&Last>,
    pad: &HaloSkatePad,
    recovery: &mut recovery::Recovery,
) -> Option<Pose> {
    let after = s.pose();
    let after_finite = pose_is_finite(&after);
    match last {
        Some(last) => {
            let position = from_skate(last.root.w_axis.truncate());
            let velocity = direction_from_skate(last.velocity);
            eprintln!(
                "halo-skate: a step failed: {error}; at tick {}, state {}, the skater was at ({:.3}, {:.3}, {:.3}) \
                 world units moving ({:.2}, {:.2}, {:.2}) m/s, {:.2} m/s; now at tick {}, state {}, its pose {}; \
                 the pad: buttons {:04x}, left {:?}, right {:?}, triggers {:?}",
                last.tick,
                last.state,
                position.x,
                position.y,
                position.z,
                velocity.x,
                velocity.y,
                velocity.z,
                velocity.length(),
                after.tick,
                after.state,
                if after_finite { "finite" } else { "not finite" },
                pad.buttons,
                pad.left,
                pad.right,
                pad.triggers,
            );
        }
        None => eprintln!(
            "halo-skate: a step failed before any good pose: {error}; now at tick {}, state {}, its pose {}",
            after.tick,
            after.state,
            if after_finite { "finite" } else { "not finite" }
        ),
    }
    let (spawn, heading) = recovery::respawn(&last?.root)?;
    if !recovery.allow() {
        eprintln!(
            "halo-skate: it failed {} times in a row here: making the session again",
            recovery::MAXIMUM + 1
        );
        return None;
    }
    let started = Instant::now();
    match s.activate(spawn, heading) {
        Ok(pose) if pose_is_finite(&pose) => {
            let at = from_skate(Vec3::from_array(spawn));
            eprintln!(
                "halo-skate: the skater was put back on the board at ({:.3}, {:.3}, {:.3}) in {}ms, state {} \
                 ({} of {} recoveries in a row)",
                at.x,
                at.y,
                at.z,
                started.elapsed().as_millis(),
                pose.state,
                recovery.recent(),
                recovery::MAXIMUM
            );
            Some(pose)
        }
        Ok(_) => {
            eprintln!("halo-skate: the skater put back on the board has a pose that is not finite");
            None
        }
        Err(e) => {
            eprintln!("halo-skate: the skater could not be put back on the board: {e}");
            None
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
    match template(&root) {
        Ok((s, copied, waited, took)) => {
            // (the web test reads "preload: session in <n>ms")
            eprintln!(
                "halo-skate: preload: session in {}ms{}{}, {}ms in all",
                took.as_millis(),
                if copied { " (a copy of the first)" } else { "" },
                if waited.as_millis() > 0 { format!(" after {}ms waiting for the first", waited.as_millis()) } else { String::new() },
                started.elapsed().as_millis()
            );
            *session = Some((root, s));
            Reply::Preloaded
        }
        Err(e) => Reply::Error(e),
    }
}

/// A session for the assets in `root` on the placeholder floor: a copy of
/// the first one made from them (several seconds to make - the skater, its
/// graphs, its camera - against tens of milliseconds to copy, and a copy
/// rides exactly as a session made afresh: real_assets_sessions.rs), and
/// whether it was a copy, how long it waited for the first (sessions asked
/// for at once wait for it) and how long it took itself.
fn template(root: &Path) -> Result<(Session, bool, Duration, Duration), String> {
    static TEMPLATE: Mutex<Option<(PathBuf, Session)>> = Mutex::new(None);
    let asked = Instant::now();
    let mut template = TEMPLATE.lock().unwrap_or_else(|e| e.into_inner());
    let waited = asked.elapsed();
    let started = Instant::now();
    if let Some((from, s)) = template.as_ref()
        && from == root
    {
        return Ok((s.clone(), true, waited, started.elapsed()));
    }
    let s = Session::new(root, placeholder_floor(), Vec::new(), [0.0; 3], 0.0)?;
    *template = Some((root.to_path_buf(), s.clone()));
    Ok((s, false, waited, started.elapsed()))
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
/// none to draw: read once for every session made from `root` (it never
/// changes).
fn load_board(root: &Path) -> Option<Arc<board::Board>> {
    static LOADED: Mutex<Option<(PathBuf, Arc<board::Board>)>> = Mutex::new(None);
    let mut loaded = LOADED.lock().unwrap_or_else(|e| e.into_inner());
    if let Some((from, board)) = loaded.as_ref()
        && from == root
    {
        return Some(Arc::clone(board));
    }
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
            let board = Arc::new(board);
            *loaded = Some((root.to_path_buf(), Arc::clone(&board)));
            Some(board)
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
    // (every session gets the same map: deduplicated, sorted and its rails
    // found once, for all of them)
    type Prepared = (Vec<[[f32; 3]; 3]>, Vec<Vec<[f32; 3]>>, Vec<Vec<Vec3>>);
    static PREPARED: Mutex<Option<(u64, Arc<Prepared>)>> = Mutex::new(None);
    let key = {
        use std::hash::{Hash, Hasher};
        let mut hasher = std::collections::hash_map::DefaultHasher::new();
        triangles.len().hash(&mut hasher);
        for t in &triangles {
            for p in t {
                p.to_array().map(f32::to_bits).hash(&mut hasher);
            }
        }
        hasher.finish()
    };
    let mut cached = PREPARED.lock().unwrap_or_else(|e| e.into_inner());
    if let Some((k, prepared)) = cached.as_ref()
        && *k == key
    {
        keep_rails(prepared.2.clone());
        eprintln!("halo-skate: the map's {} triangles and {} rails as prepared before", prepared.0.len(), prepared.1.len());
        return (prepared.0.clone(), prepared.1.clone());
    }
    let (skate, rails, inches) = prepare_afresh(triangles);
    keep_rails(inches.clone());
    *cached = Some((key, Arc::new((skate.clone(), rails.clone(), inches))));
    (skate, rails)
}

/// `prepare`'s work, and the rails in world units (for the bots).
fn prepare_afresh(triangles: Vec<[Vec3; 3]>) -> (Vec<[[f32; 3]; 3]>, Vec<Vec<[f32; 3]>>, Vec<Vec<Vec3>>) {
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
    let world: Vec<Vec<Vec3>> = found.iter().map(|rail| rail.iter().map(|p| *p / INCHES).collect()).collect();
    let skate = |p: Vec3| to_skate(p / INCHES).to_array();
    (
        inches.iter().map(|t| t.map(skate)).collect(),
        found
            .into_iter()
            .map(|rail| rail.into_iter().map(skate).collect())
            .collect(),
        world,
    )
}

/// Each session's grind rails of its last map, world units (`halo_skate_rails_in`).
static RAILS: [Mutex<Vec<Vec<Vec3>>>; MAXIMUM_SESSIONS] = [const { Mutex::new(Vec::new()) }; MAXIMUM_SESSIONS];

/// (the worker, or its rebuild) the rails found in the current session's map.
fn keep_rails(rails: Vec<Vec<Vec3>>) {
    *RAILS[current_session()].lock().unwrap_or_else(|e| e.into_inner()) = rails;
}

/// The grind rails the engine found in the session's map (rails.rs: edges
/// where the ground drops away and nothing rises, chained), for the game's
/// bots to ride toward: each rail's points, world units, 3 floats each, into
/// `points` (room for `point_capacity` points), and the count of points
/// before each rail's end into `ends` (room for `end_capacity` rails).
/// Returns how many rails were written (those past the room are left out),
/// or -1 for a session that is not one or a null pointer.
///
/// # Safety
/// `points` holds `point_capacity * 3` floats and `ends` `end_capacity` ints.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_rails_in(
    session: i32,
    points: *mut f32,
    point_capacity: i32,
    ends: *mut i32,
    end_capacity: i32,
) -> i32 {
    let Some(session) = usize::try_from(session).ok().filter(|&s| s < MAXIMUM_SESSIONS) else {
        return -1;
    };
    if points.is_null() || ends.is_null() || point_capacity < 0 || end_capacity < 0 {
        return -1;
    }
    let rails = RAILS[session].lock().unwrap_or_else(|e| e.into_inner());
    let points = unsafe { std::slice::from_raw_parts_mut(points, point_capacity as usize * 3) };
    let ends = unsafe { std::slice::from_raw_parts_mut(ends, end_capacity as usize) };
    let mut used = 0usize;
    let mut written = 0usize;
    for rail in rails.iter() {
        if written >= ends.len() || used + rail.len() > point_capacity as usize {
            break;
        }
        for p in rail {
            points[used * 3..used * 3 + 3].copy_from_slice(&p.to_array());
            used += 1;
        }
        ends[written] = used as i32;
        written += 1;
    }
    written as i32
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
    out.skater_velocity = (direction_from_skate(pose.skater_velocity) / METRES).to_array();
    out.facing = direction_from_skate(pose.facing).normalize_or_zero().to_array();
    out.off_board = i32::from(pose.off_board);
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
    let mut guard = host_guard();
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
    // (no triangles may come as NULL, which from_raw_parts does not take)
    let floats: &[f32] = if count > 0 {
        unsafe { std::slice::from_raw_parts(triangles, count as usize * 9) }
    } else {
        &[]
    };
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
    let mut guard = host_guard();
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

/// Builds the loaded level's collision again (`count` triangles as
/// `halo_skate_load` takes them: the same level with scenery made or gone)
/// aside, in the background, while the skater rides the old one, and swaps it
/// in between steps once built, never with the skater on a rail or in the
/// air. The state stays ready throughout (2); a map sent later replaces it.
/// With no map loaded from `assets` yet it loads as `halo_skate_load` does.
/// Returns 0, or -1 when the engine could not be started.
///
/// # Safety
/// `assets` is a C string and `triangles` holds `count * 9` floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_reload(assets: *const c_char, triangles: *const f32, count: i32) -> i32 {
    if assets.is_null() || (triangles.is_null() && count > 0) {
        return -1;
    }
    let mut guard = host_guard();
    let ready = guard.as_ref().is_some_and(|h| h.ready && h.error.is_none());
    drop(guard);
    if !ready {
        return unsafe { halo_skate_load(assets, triangles, count) };
    }
    let root = PathBuf::from(unsafe { CStr::from_ptr(assets) }.to_string_lossy().into_owned());
    let floats: &[f32] = if count > 0 {
        unsafe { std::slice::from_raw_parts(triangles, count as usize * 9) }
    } else {
        &[]
    };
    let triangles = floats
        .chunks_exact(9)
        .map(|t| [Vec3::new(t[0], t[1], t[2]), Vec3::new(t[3], t[4], t[5]), Vec3::new(t[6], t[7], t[8])])
        .collect::<Vec<_>>();
    guard = host_guard();
    let Some(host) = guard.as_mut() else {
        return -1;
    };
    let generation = host.generation.fetch_add(1, Ordering::SeqCst) + 1;
    eprintln!("halo-skate: map {generation} sent to rebuild, {} triangles", triangles.len());
    if host.jobs.send(Job::Reload { root, triangles, generation }).is_err() {
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
    unsafe { activate(position, yaw, None, out) }
}

/// A teleporter took the skater: on the board at `position` (world units, on
/// the ground) facing `yaw` radians from +X, as `halo_skate_activate`, and
/// moving at `velocity` (3 floats, world units a second) from the start.
/// Returns 0 with `out` filled, or -1.
///
/// # Safety
/// `position` and `velocity` hold 3 floats; `out` is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_teleport(
    position: *const f32,
    yaw: f32,
    velocity: *const f32,
    out: *mut HaloSkateFrame,
) -> i32 {
    if velocity.is_null() {
        return -1;
    }
    let v = unsafe { std::slice::from_raw_parts(velocity, 3) };
    let v = Vec3::new(v[0], v[1], v[2]);
    if !v.is_finite() {
        return -1;
    }
    unsafe { activate(position, yaw, Some(v), out) }
}

unsafe fn activate(position: *const f32, yaw: f32, velocity: Option<Vec3>, out: *mut HaloSkateFrame) -> i32 {
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
        host.recovered = false;
        host.held = Default::default();
        let sequence = host.sequence;
        if !host.request(Job::Activate { sequence, spawn, yaw, velocity }, sequence, ACTIVATE_WAIT) {
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
/// last pose), 2 when a step failed and the skater was put back on the board
/// where it last was (`recovery.rs`; `out` holds where), or -1 on failure.
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
                if std::mem::take(&mut host.recovered) {
                    2
                } else if answered {
                    0
                } else {
                    1
                }
            }
            None => -1,
        }
    })
    .unwrap_or(-1)
}

/// Starts `dt` seconds of skating with `pad` held, as `halo_skate_step`, and
/// returns at once: `halo_skate_await_step` takes the pose. The game starts
/// each bot's skater's step so, then takes their poses, so that the sessions'
/// workers step side by side. 0, or -1 (nothing loaded, or a failure).
///
/// # Safety
/// `pad` is readable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_step_begin(pad: *const HaloSkatePad, dt: f32) -> i32 {
    if pad.is_null() {
        return -1;
    }
    let pad = unsafe { *pad };
    with_host(|host| {
        host.drain();
        if !host.ready || host.error.is_some() {
            return -1;
        }
        host.sequence += 1;
        let sequence = host.sequence;
        if host.jobs.send(Job::Step { sequence, pad, dt }).is_err() {
            host.fail("the skate engine stopped".into());
            return -1;
        }
        0
    })
    .unwrap_or(-1)
}

/// After `halo_skate_step` returned 1 (no answer in time; the worker still
/// runs that step, and its pose comes with the next call), waits up to
/// `wait_ms` for that pose. Returns 0 with `out` filled from it, 2 if the
/// step failed and was recovered from, 1 if it is still not there, or -1 on
/// failure. The game does not call this: it moves on with the last pose
/// rather than hold its frame. The tests do, so that under load each tick's
/// frame is that tick's.
///
/// # Safety
/// `out` is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_await_step(wait_ms: u32, out: *mut HaloSkateFrame) -> i32 {
    if out.is_null() {
        return -1;
    }
    with_host(|host| {
        let sequence = host.sequence;
        let answered = host.await_pose(sequence, Duration::from_millis(u64::from(wait_ms)));
        if host.error.is_some() {
            return -1;
        }
        match host.pose.as_ref() {
            Some(pose) => {
                write_frame(pose, unsafe { &mut *out });
                if std::mem::take(&mut host.recovered) {
                    2
                } else if answered {
                    0
                } else {
                    1
                }
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

/// The obstacles the board and the skater collide with from the next step
/// (`obstacles.rs`): `count` of them (0 for none), where they are now and how
/// they move; they carry on at their velocities until the next call. A mesh's
/// triangles are among `triangles` (`triangle_count` of 9 floats each, world
/// units, where the object is now). Those of a kind or shape not known, or not
/// valid, are left out, as are any past the engine's most
/// (HALO_SKATE_MAXIMUM_OBSTACLES) and meshes past its most triangles
/// (HALO_SKATE_MAXIMUM_OBSTACLE_TRIANGLES); the game sends the nearest first.
/// Returns how many were sent on, or -1.
///
/// # Safety
/// `obstacles` holds `count` obstacles (or is NULL with `count` 0), and
/// `triangles` `triangle_count * 9` floats (or is NULL with `triangle_count` 0).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_set_obstacles(
    obstacles: *const obstacles::HaloSkateObstacle,
    count: i32,
    triangles: *const f32,
    triangle_count: i32,
) -> i32 {
    if count < 0 || (count > 0 && obstacles.is_null()) || triangle_count < 0 || (triangle_count > 0 && triangles.is_null()) {
        return -1;
    }
    let given = if count == 0 {
        &[][..]
    } else {
        unsafe { std::slice::from_raw_parts(obstacles, count as usize) }
    };
    let floats = if triangle_count == 0 {
        &[][..]
    } else {
        unsafe { std::slice::from_raw_parts(triangles, triangle_count as usize * 9) }
    };
    let mut meshed = 0usize;
    let taken: Vec<_> = given
        .iter()
        .filter(|o| {
            // (meshes, nearest first, until the most triangles are taken)
            o.shape != obstacles::SHAPE_MESH || {
                meshed += o.triangle_count.max(0) as usize;
                meshed <= skate_host::bridge::MAXIMUM_MESH_TRIANGLES
            }
        })
        .filter_map(|o| o.to_engine(floats))
        .filter_map(|o| o.validated())
        .take(skate_host::bridge::MAXIMUM_OBSTACLES)
        .collect();
    let sent = taken.len() as i32;
    with_host(|host| {
        let bail_speed = OBSTACLE_BAIL_SPEED.get() * METRES;
        let run_over = RUN_OVER_SPEED.get();
        let run_over = (if run_over >= 0.0 { run_over * METRES } else { f32::INFINITY }, RUN_OVER_KEEP.get());
        if host.jobs.send(Job::Obstacles { obstacles: taken, bail_speed, run_over }).is_err() {
            return -1;
        }
        sent
    })
    .unwrap_or(-1)
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
/// the skater of the last frame, its soles on the deck while it rides.
/// Returns the number written, or 0.
///
/// # Safety
/// `out` holds room for `capacity * 13` floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_pose_nodes(out: *mut f32, capacity: i32) -> i32 {
    if out.is_null() || capacity <= 0 {
        return 0;
    }
    let out = unsafe { std::slice::from_raw_parts_mut(out, capacity as usize * 13) };
    with_host(|host| {
        let Host {
            rig, pose, board, deck, ..
        } = host;
        let (Some(rig), Some(pose)) = (rig.as_ref(), pose.as_ref()) else {
            return 0;
        };
        // the board as the game draws it (halo_skate_board_vertices), grown
        let scale = BOARD_SCALE.get();
        let mut on_deck = None;
        if let Some(board) = board.as_ref() {
            deck.resize(board.vertices.len() * board::VERTEX_FLOATS, 0.0);
            if board.skin(pose, scale, deck).is_ok() {
                on_deck = Some(rig::Deck {
                    vertices: deck.as_slice(),
                    indices: board.indices.as_slice(),
                    ankle: ANKLE_HEIGHT.get(),
                    offset: FEET_OFFSET.get(),
                    scale,
                });
            }
        }
        rig.pose(pose, on_deck.as_ref(), out) as i32
    })
    .unwrap_or(0)
}

/// Grows the drawn board `scale` times about its middle (1.05 at first),
/// for this session. Returns the scale kept: the last that made sense.
#[unsafe(no_mangle)]
pub extern "C" fn halo_skate_set_board_scale(scale: f32) -> f32 {
    if scale.is_finite() && scale > 0.0 {
        BOARD_SCALE.set(scale);
    }
    BOARD_SCALE.get()
}

/// How fast (world units a second, along the contact) running into a person
/// or a vehicle knocks the skater over (6 m/s at first; slower is a bump),
/// for this session. Returns the speed kept: the last that made sense.
#[unsafe(no_mangle)]
pub extern "C" fn halo_skate_set_obstacle_bail_speed(speed: f32) -> f32 {
    if speed.is_finite() && speed >= 0.0 {
        OBSTACLE_BAIL_SPEED.set(speed);
    }
    OBSTACLE_BAIL_SPEED.get()
}

/// Running over people on foot (`halo_skate_obstacle`'s run_over, a person on
/// foot the game marks while the skater rides): a contact with one closing at
/// `speed` world units a second or faster goes through them, the board and the
/// skater keeping `keep` (0 to 1) of their speed, and the hit is told as held
/// and run over (`halo_skate_take_held_hit`); slower, it is a held obstacle as
/// any. A speed below 0 or not finite runs nobody over (as at first). Kept
/// across engine restarts; returns the speed kept.
#[unsafe(no_mangle)]
pub extern "C" fn halo_skate_set_run_over(speed: f32, keep: f32) -> f32 {
    RUN_OVER_SPEED.set(if speed.is_finite() { speed } else { -1.0 });
    RUN_OVER_KEEP.set(if keep.is_finite() { keep.clamp(0.0, 1.0) } else { 1.0 });
    RUN_OVER_SPEED.get()
}

/// A blast's push on Halo's world units a second (`x`, `y`, `z`) in the skate
/// engine's metres a second; none for one not finite or nothing.
pub(crate) fn impulse_to_skate(velocity: Vec3) -> Option<Vec3> {
    (velocity.is_finite() && velocity != Vec3::ZERO).then(|| to_skate(velocity))
}

/// A blast pushes the skater as Halo's pushes a player on foot (skate.c takes
/// the velocity Halo's damage gives the biped, `biped_accelerate`'s): `x`,
/// `y`, `z` world units a second added to the skater's before the next step,
/// the board's too while riding, and faster than the obstacle bail speed
/// (`halo_skate_set_obstacle_bail_speed`) a bail. Returns 0, or -1 (nothing
/// skating, or a push not finite).
#[unsafe(no_mangle)]
pub extern "C" fn halo_skate_apply_impulse(x: f32, y: f32, z: f32) -> i32 {
    let Some(velocity) = impulse_to_skate(Vec3::new(x, y, z)) else {
        return -1;
    };
    with_host(|host| if host.jobs.send(Job::Impulse { velocity }).is_ok() { 0 } else { -1 }).unwrap_or(-1)
}

/// `halo_skate_held_hit` (include/halo_skate.h).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct HaloSkateHeldHit {
    pub tag: i32,
    pub kind: i32,
    pub closing: f32,
    pub normal: [f32; 3],
    pub touched: i32,
    pub bail_held: i32,
    pub run_over: i32,
}

/// What the steps since the last call had to tell of hits on held obstacles
/// (`HaloSkateObstacle::held`: in a network game another player, their
/// vehicle, another skater, whose hits the host decides): the fastest
/// contact with one (`touched`; its tag, kind, closing speed in world units
/// a second, and the normal from it toward the skater, Z up) and whether the
/// engine held a bail it would have had (`bail_held`). Returns 1 with `out`
/// filled, 0 with nothing to tell, -1 on failure.
///
/// # Safety
/// `out` is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_take_held_hit(out: *mut HaloSkateHeldHit) -> i32 {
    if out.is_null() {
        return -1;
    }
    with_host(|host| {
        host.drain();
        let held = std::mem::take(&mut host.held);
        if held.hit.is_none() && !held.bail_held {
            return 0;
        }
        let mut told = HaloSkateHeldHit {
            bail_held: i32::from(held.bail_held),
            ..Default::default()
        };
        if let Some(hit) = held.hit {
            told.tag = hit.tag as i32;
            told.kind = obstacles::kind_number(hit.kind);
            told.closing = hit.closing / METRES;
            told.normal = direction_from_skate(Vec3::new(hit.normal.x, hit.normal.y, hit.normal.z)).to_array();
            told.touched = 1;
            told.run_over = i32::from(hit.run_over);
        }
        unsafe { *out = told };
        1
    })
    .unwrap_or(-1)
}

/// The game's host decided the skater bails (a hit on a held obstacle, here
/// or on another machine): the engine knocks them over on the next step they
/// ride, as running into a person at `closing` world units a second does,
/// whether it wanted to or not (given up after half a second of not riding).
/// Returns 0, or -1.
#[unsafe(no_mangle)]
pub extern "C" fn halo_skate_force_bail(closing: f32) -> i32 {
    let closing = if closing.is_finite() { closing.max(0.0) * METRES } else { 0.0 };
    with_host(|host| if host.jobs.send(Job::ForceBail { closing }).is_ok() { 0 } else { -1 }).unwrap_or(-1)
}

/// How the riding biped stands on the deck: its ankle node `ankle_height`
/// world units above its soles (0 or less for unknown, which leaves its
/// ankles on the skater's), and raised `offset` world units more along the
/// board's up.
#[unsafe(no_mangle)]
pub extern "C" fn halo_skate_set_feet(ankle_height: f32, offset: f32) {
    ANKLE_HEIGHT.set(if ankle_height.is_finite() { ankle_height.max(0.0) } else { 0.0 });
    FEET_OFFSET.set(if offset.is_finite() { offset } else { 0.0 });
}

/// The following camera's next heading (radians, 0 to 2 pi), `dt` seconds
/// on from `follow`, for the skater of `frame` (camera.rs): on the board
/// `fraction` of the way toward the way it travels, off it toward the way
/// the skater walks, when faster than `minimum_speed` (world units a
/// second); off it and still a while, slowly round behind its facing; else
/// held. Updates `follow` and returns its heading; a null pointer leaves it
/// and returns 0.
///
/// # Safety
/// `follow` and `frame` are null or valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_follow(
    follow: *mut camera::Follow,
    frame: *const HaloSkateFrame,
    minimum_speed: f32,
    fraction: f32,
    dt: f32,
) -> f32 {
    let (Some(follow), Some(frame)) = (unsafe { follow.as_mut() }, unsafe { frame.as_ref() }) else {
        return 0.0;
    };
    let along = |v: [f32; 3]| [v[0], v[1]];
    let subject = camera::Subject {
        off_board: frame.off_board != 0,
        board_velocity: along(frame.velocity),
        skater_velocity: along(frame.skater_velocity),
        facing: along(frame.facing),
    };
    follow.step(&subject, minimum_speed, fraction, dt)
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
        match board.skin(pose, BOARD_SCALE.get(), out) {
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

/// Where the skater's last pose has the board (`board.rs`'s `placement`),
/// for another machine to draw it (multiplayer, `../linux/game/skate_network.c`):
/// 7 floats, a unit quaternion (x, y, z, w) and a position in world units.
/// Returns 0, or -1 without a board or a pose.
///
/// # Safety
/// `out` holds room for 7 floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_board_placement(out: *mut f32) -> i32 {
    if out.is_null() {
        return -1;
    }
    with_host(|host| {
        let (Some(board), Some(pose)) = (host.board.as_ref(), host.pose.as_ref()) else {
            return None;
        };
        board.placement(pose)
    })
    .flatten()
    .map(|(rotation, position)| {
        let out = unsafe { std::slice::from_raw_parts_mut(out, 7) };
        out[..4].copy_from_slice(&rotation.to_array());
        out[4..].copy_from_slice(&position.to_array());
        0
    })
    .unwrap_or(-1)
}

/// Writes the board's vertices placed whole at `placement` (7 floats, as
/// `halo_skate_board_placement` gives them: another machine's skater's
/// board), grown `halo_skate_set_board_scale` times, 8 floats each as
/// `halo_skate_board_vertices` writes them. Returns how many, or 0 (no board,
/// or a placement that is not one).
///
/// # Safety
/// `placement` holds 7 floats and `out` room for `capacity * 8`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_board_vertices_at(placement: *const f32, out: *mut f32, capacity: i32) -> i32 {
    if placement.is_null() || out.is_null() || capacity <= 0 {
        return 0;
    }
    let placement = unsafe { std::slice::from_raw_parts(placement, 7) };
    let rotation = bevy_math::Quat::from_xyzw(placement[0], placement[1], placement[2], placement[3]);
    let position = Vec3::new(placement[4], placement[5], placement[6]);
    if !rotation.is_finite() || !position.is_finite() || (rotation.length() - 1.0).abs() > 0.01 {
        return 0;
    }
    let out = unsafe { std::slice::from_raw_parts_mut(out, capacity as usize * board::VERTEX_FLOATS) };
    with_host(|host| {
        host.drain();
        host.board
            .as_ref()
            .map_or(0, |board| board.place(rotation.normalize(), position, BOARD_SCALE.get(), out) as i32)
    })
    .unwrap_or(0)
}

/// Another machine's skater's board at `placement` (7 floats, as
/// `halo_skate_board_placement` gives them) as an obstacle: a box of kind
/// skater, turned as the board is and around it as drawn
/// (`board.rs`'s `local_box`), at rest (the game sets the velocity).
/// Returns 0, or -1 with no board or a placement that is not one.
///
/// # Safety
/// `placement` holds 7 floats; `out` is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_board_obstacle(placement: *const f32, out: *mut obstacles::HaloSkateObstacle) -> i32 {
    if placement.is_null() || out.is_null() {
        return -1;
    }
    let placement = unsafe { std::slice::from_raw_parts(placement, 7) };
    let rotation = bevy_math::Quat::from_xyzw(placement[0], placement[1], placement[2], placement[3]);
    let position = Vec3::new(placement[4], placement[5], placement[6]);
    if !rotation.is_finite() || !position.is_finite() || (rotation.length() - 1.0).abs() > 0.01 {
        return -1;
    }
    let local = with_host(|host| {
        host.drain();
        host.board.as_ref().and_then(|board| board.local_box(BOARD_SCALE.get()))
    })
    .flatten();
    let Some((middle, half)) = local else {
        return -1;
    };
    unsafe { *out = obstacles::board_obstacle(rotation.normalize(), position, middle, half) };
    0
}

// ---------- other sessions than 0 (the host's bots' skaters)
//
// Each `_in` call is the call of the same name for `session` (1 to
// `halo_skate_sessions() - 1`; 0 is this machine's player's, as the calls
// without a number): each session has its own worker, collision, skater,
// pose and hits; what never changes is shared. A session number that is not
// one fails as the call does with nothing loaded.

/// Whether `assets` is a folder the converter wrote (it has `private/`):
/// 1, else 0. Nothing is loaded (a dedicated server, which makes no skater of
/// its own, looks before its bots ride).
///
/// # Safety
/// `assets` is NULL or a C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_assets_present(assets: *const c_char) -> i32 {
    if assets.is_null() {
        return 0;
    }
    let root = PathBuf::from(unsafe { CStr::from_ptr(assets) }.to_string_lossy().into_owned());
    i32::from(root.join("private").is_dir())
}

/// How many sessions there may be at once, this machine's player's (0) with
/// them.
#[unsafe(no_mangle)]
pub extern "C" fn halo_skate_sessions() -> i32 {
    MAXIMUM_SESSIONS as i32
}

/// Ends the session: its worker stops and its skater, collision and memory
/// go (a bot left). The next `halo_skate_preload_in` makes it again.
#[unsafe(no_mangle)]
pub extern "C" fn halo_skate_close_in(session: i32) {
    in_session(session, (), || {
        let closed = host_guard().take();
        if closed.is_some() {
            eprintln!("halo-skate: session closed");
        }
    });
}

/// # Safety
/// As `halo_skate_preload`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_preload_in(session: i32, assets: *const c_char) -> i32 {
    in_session(session, -1, || unsafe { halo_skate_preload(assets) })
}

#[unsafe(no_mangle)]
pub extern "C" fn halo_skate_preloading_in(session: i32) -> i32 {
    in_session(session, 0, || halo_skate_preloading())
}

/// # Safety
/// As `halo_skate_load`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_load_in(session: i32, assets: *const c_char, triangles: *const f32, count: i32) -> i32 {
    in_session(session, -1, || unsafe { halo_skate_load(assets, triangles, count) })
}

/// # Safety
/// As `halo_skate_reload`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_reload_in(
    session: i32,
    assets: *const c_char,
    triangles: *const f32,
    count: i32,
) -> i32 {
    in_session(session, -1, || unsafe { halo_skate_reload(assets, triangles, count) })
}

#[unsafe(no_mangle)]
pub extern "C" fn halo_skate_state_in(session: i32) -> i32 {
    in_session(session, 0, || halo_skate_state())
}

#[unsafe(no_mangle)]
pub extern "C" fn halo_skate_error_in(session: i32) -> *const c_char {
    static EMPTY: &CStr = c"";
    in_session(session, EMPTY.as_ptr(), || halo_skate_error())
}

/// # Safety
/// As `halo_skate_activate`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_activate_in(
    session: i32,
    position: *const f32,
    yaw: f32,
    out: *mut HaloSkateFrame,
) -> i32 {
    in_session(session, -1, || unsafe { halo_skate_activate(position, yaw, out) })
}

/// # Safety
/// As `halo_skate_teleport`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_teleport_in(
    session: i32,
    position: *const f32,
    yaw: f32,
    velocity: *const f32,
    out: *mut HaloSkateFrame,
) -> i32 {
    in_session(session, -1, || unsafe { halo_skate_teleport(position, yaw, velocity, out) })
}

#[unsafe(no_mangle)]
pub extern "C" fn halo_skate_apply_impulse_in(session: i32, x: f32, y: f32, z: f32) -> i32 {
    in_session(session, -1, || halo_skate_apply_impulse(x, y, z))
}

/// # Safety
/// As `halo_skate_step`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_step_in(
    session: i32,
    pad: *const HaloSkatePad,
    dt: f32,
    out: *mut HaloSkateFrame,
) -> i32 {
    in_session(session, -1, || unsafe { halo_skate_step(pad, dt, out) })
}

/// # Safety
/// As `halo_skate_step_begin`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_step_begin_in(session: i32, pad: *const HaloSkatePad, dt: f32) -> i32 {
    in_session(session, -1, || unsafe { halo_skate_step_begin(pad, dt) })
}

/// # Safety
/// As `halo_skate_await_step`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_await_step_in(session: i32, wait_ms: u32, out: *mut HaloSkateFrame) -> i32 {
    in_session(session, -1, || unsafe { halo_skate_await_step(wait_ms, out) })
}

#[unsafe(no_mangle)]
pub extern "C" fn halo_skate_suspend_in(session: i32) {
    in_session(session, (), || halo_skate_suspend())
}

/// # Safety
/// As `halo_skate_set_obstacles`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_set_obstacles_in(
    session: i32,
    obstacles: *const obstacles::HaloSkateObstacle,
    count: i32,
    triangles: *const f32,
    triangle_count: i32,
) -> i32 {
    in_session(session, -1, || unsafe { halo_skate_set_obstacles(obstacles, count, triangles, triangle_count) })
}

/// # Safety
/// As `halo_skate_set_skeleton`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_set_skeleton_in(
    session: i32,
    count: i32,
    names: *const c_char,
    parents: *const i16,
    inverse_defaults: *const f32,
) -> i32 {
    in_session(session, 0, || unsafe { halo_skate_set_skeleton(count, names, parents, inverse_defaults) })
}

/// # Safety
/// As `halo_skate_pose_nodes`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_pose_nodes_in(session: i32, out: *mut f32, capacity: i32) -> i32 {
    in_session(session, 0, || unsafe { halo_skate_pose_nodes(out, capacity) })
}

/// # Safety
/// As `halo_skate_take_held_hit`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_take_held_hit_in(session: i32, out: *mut HaloSkateHeldHit) -> i32 {
    in_session(session, -1, || unsafe { halo_skate_take_held_hit(out) })
}

#[unsafe(no_mangle)]
pub extern "C" fn halo_skate_force_bail_in(session: i32, closing: f32) -> i32 {
    in_session(session, -1, || halo_skate_force_bail(closing))
}

/// # Safety
/// As `halo_skate_board_placement`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_board_placement_in(session: i32, out: *mut f32) -> i32 {
    in_session(session, -1, || unsafe { halo_skate_board_placement(out) })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A blast's push in world units a second goes to the skate engine in
    /// metres a second, its axes as positions' are (Halo's up is the engine's
    /// +Y, Halo's +Y its -Z); nothing, or not finite, is no push.
    #[test]
    fn a_blasts_push_goes_to_the_engine_in_metres_a_second() {
        let close = |a: Vec3, b: Vec3| (a - b).length() < 1e-5;
        assert!(close(impulse_to_skate(Vec3::new(1.0, 0.0, 0.0)).unwrap(), Vec3::new(METRES, 0.0, 0.0)));
        assert!(close(impulse_to_skate(Vec3::new(0.0, 1.0, 0.0)).unwrap(), Vec3::new(0.0, 0.0, -METRES)));
        assert!(close(impulse_to_skate(Vec3::new(0.0, 0.0, 2.0)).unwrap(), Vec3::new(0.0, 2.0 * METRES, 0.0)));
        assert_eq!(impulse_to_skate(Vec3::ZERO), None);
        assert_eq!(impulse_to_skate(Vec3::new(f32::NAN, 0.0, 0.0)), None);
        assert_eq!(impulse_to_skate(Vec3::new(0.0, f32::INFINITY, 0.0)), None);
        assert_eq!(halo_skate_apply_impulse(f32::NAN, 0.0, 0.0), -1);
    }

    fn rig() -> rig::Rig {
        rig::Rig::new(vec![rig::NodeDefault {
            name: "bip01 pelvis".into(),
            parent: None,
            inverse: rig::Matrix::from_floats(&[1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0]),
        }])
    }

    /// A worker that failed (a step's error ends it) is replaced with its
    /// skeleton: the game describes the biped once, and without it the next
    /// worker would never pose the biped nor holster its weapon, leaving it
    /// on the board with its weapon in hand.
    #[test]
    fn a_replaced_worker_keeps_the_skeleton() {
        let mut guard = Some(Host::start().unwrap());
        let host = guard.as_mut().unwrap();
        host.rig = Some(rig());
        let mapped = host.rig.as_ref().unwrap().mapped();
        host.fail("a step failed".into());
        let replaced = host_started(&mut guard).unwrap();
        assert!(replaced.error.is_none());
        assert_eq!(replaced.rig.as_ref().map(|r| r.mapped()), Some(mapped));
        // and a working one is kept as it is
        let generation = Arc::clone(&guard.as_ref().unwrap().generation);
        let kept = host_started(&mut guard).unwrap();
        assert!(Arc::ptr_eq(&kept.generation, &generation));
        assert!(kept.rig.is_some());
    }

    #[test]
    fn a_first_worker_has_no_skeleton() {
        let mut guard = None;
        assert!(host_started(&mut guard).unwrap().rig.is_none());
    }

    /// The game's way back on after a failure, through the C interface: the
    /// failed engine reports -1, J sends the map again (`skate_load_map`),
    /// which restarts the engine, and the skeleton described before the
    /// failure is still there to pose the biped by.
    #[test]
    fn the_skeleton_survives_a_failure_and_the_next_load() {
        let mut names = [0 as c_char; 32];
        for (slot, byte) in names.iter_mut().zip(b"bip01 pelvis") {
            *slot = *byte as c_char;
        }
        let parents = [-1i16];
        let inverse = [1.0f32, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0];
        *host_guard() = Some(Host::start().unwrap());
        unsafe { halo_skate_set_skeleton(1, names.as_ptr(), parents.as_ptr(), inverse.as_ptr()) };
        assert_eq!(with_host(|h| h.rig.is_some()), Some(true));
        with_host(|h| h.fail("Nonfinite BipedAir launch packet".into()));
        assert_eq!(halo_skate_state(), -1);
        let assets = CString::new("no-skate-data-here").unwrap();
        assert_eq!(unsafe { halo_skate_load(assets.as_ptr(), std::ptr::null(), 0) }, 0);
        assert_eq!(with_host(|h| h.rig.is_some()), Some(true));
    }

    /// Sessions are apart: one started, its skeleton described and failed is
    /// none of another's business, and a number that is no session is refused.
    #[test]
    fn sessions_are_apart() {
        let mut names = [0 as c_char; 32];
        for (slot, byte) in names.iter_mut().zip(b"bip01 pelvis") {
            *slot = *byte as c_char;
        }
        let parents = [-1i16];
        let inverse = [1.0f32, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0];
        in_session(7, (), || *host_guard() = Some(Host::start().unwrap()));
        unsafe { halo_skate_set_skeleton_in(7, 1, names.as_ptr(), parents.as_ptr(), inverse.as_ptr()) };
        assert_eq!(in_session(7, None, || with_host(|h| h.rig.is_some())), Some(true));
        // (session 8 never started: nothing there, and nothing loaded)
        assert_eq!(in_session(8, Some(true), || with_host(|h| h.rig.is_some())), None);
        assert_eq!(halo_skate_state_in(8), 0);
        in_session(7, None, || with_host(|h| h.fail("a step failed".into())));
        assert_eq!(halo_skate_state_in(7), -1);
        assert_eq!(halo_skate_state_in(8), 0);
        // (no such sessions)
        assert_eq!(halo_skate_state_in(-1), 0);
        assert_eq!(halo_skate_state_in(MAXIMUM_SESSIONS as i32), 0);
        assert_eq!(halo_skate_force_bail_in(MAXIMUM_SESSIONS as i32, 1.0), -1);
        // (the calls come back to session 0)
        assert_eq!(current_session(), 0);
        halo_skate_close_in(7);
        assert_eq!(halo_skate_state_in(7), 0);
    }
}

//! Tricks: what the engine's own scoring (Skate 3's ScoreModule, in
//! skate-host's `scoring_runtime.rs`) made of a skater's riding, kept for the
//! game (`halo_skate_tricks_in`: the host's bots learn from it what they
//! landed, and the tests count it), and the right stick's gestures the
//! engine knows tricks by (`halo_skate_gesture`: a bot flicks a trick the way
//! a person does, through the same pad).
//!
//! Nothing here changes the simulation: after each simulated tick the worker
//! looks at the scoring (`Session::score_observation`) and adds up what
//! changed.

use crate::MAXIMUM_SESSIONS;
use skate_host::bridge::{ScoreObservation, Session, gesture_patterns};
use std::collections::HashMap;
use std::ffi::{CStr, c_char};
use std::path::{Path, PathBuf};
use std::sync::Mutex;

/// `halo_skate_tricks` (include/halo_skate.h).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct HaloSkateTricks {
    pub tricks: u32,
    pub landed: u32,
    pub sequences: u32,
    pub combos: u32,
    pub bails: u32,
    pub best_tricks: u32,
    pub sequence_tricks: u32,
    pub points: f32,
    pub best_points: f32,
    pub sequence_points: f32,
    pub multiplier: f32,
    pub last_trick: [c_char; 32],
}

impl Default for HaloSkateTricks {
    fn default() -> Self {
        Self {
            tricks: 0,
            landed: 0,
            sequences: 0,
            combos: 0,
            bails: 0,
            best_tricks: 0,
            sequence_tricks: 0,
            points: 0.0,
            best_points: 0.0,
            sequence_points: 0.0,
            multiplier: 1.0,
            last_trick: [0; 32],
        }
    }
}

impl HaloSkateTricks {
    /// Adds one tick's scoring.
    pub(crate) fn observe(&mut self, o: &ScoreObservation) {
        if o.new_trick {
            self.tricks += 1;
            self.sequence_tricks += 1;
        }
        if (o.new_trick || o.modified_trick) && !o.trick_name.is_empty() {
            self.last_trick = [0; 32];
            for (to, from) in self.last_trick.iter_mut().zip(o.trick_name.bytes().take(31)) {
                *to = from as c_char;
            }
        }
        self.sequence_points = o.sequence_score.max(0.0);
        self.multiplier = o.multiplier;
        if let Some((points, bailed)) = o.published {
            if bailed {
                self.bails += 1;
            } else if self.sequence_tricks > 0 || points > 0.0 {
                self.sequences += 1;
                self.landed += self.sequence_tricks;
                self.points += points.max(0.0);
                if self.sequence_tricks >= 2 {
                    self.combos += 1;
                }
                self.best_tricks = self.best_tricks.max(self.sequence_tricks);
                self.best_points = self.best_points.max(points);
            }
            self.sequence_tricks = 0;
            self.sequence_points = 0.0;
        }
    }
}

/// Each session's tricks since it was made.
static TRICKS: [Mutex<Option<HaloSkateTricks>>; MAXIMUM_SESSIONS] = [const { Mutex::new(None) }; MAXIMUM_SESSIONS];

/// (the worker, after each simulated tick) adds the tick's scoring to its
/// session's tricks.
pub(crate) fn observe(s: &Session) {
    let observation = s.score_observation();
    let mut guard = TRICKS[crate::current_session()].lock().unwrap_or_else(|e| e.into_inner());
    guard.get_or_insert_with(HaloSkateTricks::default).observe(&observation);
}

/// (a session made afresh, or closed) its tricks gone.
pub(crate) fn forget(session: usize) {
    if let Some(slot) = TRICKS.get(session) {
        *slot.lock().unwrap_or_else(|e| e.into_inner()) = None;
    }
}

/// The current session's tricks, or None before its first tick.
pub(crate) fn take() -> Option<HaloSkateTricks> {
    *TRICKS[crate::current_session()].lock().unwrap_or_else(|e| e.into_inner())
}

/// The joystick pattern files the gestures are read from, as the engine's
/// recognisers load them (skate-host's `input/gesture_input.rs`): the
/// right stick's on the ground and in the air.
const GESTURE_FILES: [&str; 3] = ["skater.pat", "skater_air.pat", "skater_fingerflip.pat"];

/// The gestures read from each assets folder: (name, points) in the files'
/// order.
static GESTURES: Mutex<Option<HashMap<PathBuf, Vec<(String, Vec<[f32; 2]>)>>>> = Mutex::new(None);

/// The gestures of the assets folder at `root`, read once.
fn gestures(root: &Path, f: impl FnOnce(&[(String, Vec<[f32; 2]>)]) -> i32) -> i32 {
    let mut guard = GESTURES.lock().unwrap_or_else(|e| e.into_inner());
    let map = guard.get_or_insert_with(HashMap::new);
    if !map.contains_key(root) {
        let mut read = Vec::new();
        for file in GESTURE_FILES {
            match gesture_patterns(root, file) {
                Ok(patterns) => read.extend(patterns),
                Err(e) => eprintln!("halo-skate: gestures: {e}"),
            }
        }
        map.insert(root.to_path_buf(), read);
    }
    f(&map[root])
}

/// The right stick's way through the gesture the engine knows `name` by
/// (the first of that name in the joystick files of the converted data at
/// `assets`: "Ollie", "Kickflip", "L_F_Kickflip" ...), as pad positions
/// from -1 to 1 with +Y up (the pad's: the files' Y is down), two floats
/// each, into `points` (room for `capacity` of them). How many, 0 for no
/// such gesture, -1 on a bad argument.
///
/// # Safety
/// `assets` and `name` are NUL-terminated strings; `points` has room for
/// `capacity` pairs of floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_gesture(
    assets: *const c_char,
    name: *const c_char,
    points: *mut f32,
    capacity: i32,
) -> i32 {
    if assets.is_null() || name.is_null() || points.is_null() || capacity <= 0 {
        return -1;
    }
    let root = PathBuf::from(unsafe { CStr::from_ptr(assets) }.to_string_lossy().into_owned());
    let name = unsafe { CStr::from_ptr(name) }.to_string_lossy().into_owned();
    gestures(&root, |all| {
        let Some((_, found)) = all.iter().find(|(n, _)| *n == name) else {
            return 0;
        };
        let count = found.len().min(capacity as usize);
        for (i, [x, y]) in found.iter().take(count).enumerate() {
            unsafe {
                *points.add(2 * i) = *x;
                *points.add(2 * i + 1) = -*y;
            }
        }
        count as i32
    })
}

/// What the engine's scoring made of the current session's riding since the
/// session was made (`halo_skate_tricks` in include/halo_skate.h). 0 with
/// `out` filled (all naught before the first step), -1 with no session.
fn tricks_into(out: *mut HaloSkateTricks) -> i32 {
    if out.is_null() {
        return -1;
    }
    let tricks = take().unwrap_or_default();
    unsafe { *out = tricks };
    0
}

/// # Safety
/// `out` points to a `HaloSkateTricks`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_tricks(out: *mut HaloSkateTricks) -> i32 {
    tricks_into(out)
}

/// # Safety
/// `out` points to a `HaloSkateTricks`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_tricks_in(session: i32, out: *mut HaloSkateTricks) -> i32 {
    crate::in_session(session, -1, || tricks_into(out))
}

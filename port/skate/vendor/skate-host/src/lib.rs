#![allow(dead_code, unused_imports)]

/// Where the engine's log lines go. halo-skate: a Windows release build of the
/// game has no console, so stderr is lost there; the game sets a sink that
/// writes to its own log. Without one, lines go to stderr.
pub mod log {
    use std::sync::RwLock;

    static SINK: RwLock<Option<fn(&str)>> = RwLock::new(None);

    /// Sends every later log line to `sink` (None: back to stderr).
    pub fn set_sink(sink: Option<fn(&str)>) {
        *SINK.write().unwrap_or_else(|e| e.into_inner()) = sink;
    }

    pub fn write(line: &str) {
        let sink = *SINK.read().unwrap_or_else(|e| e.into_inner());
        match sink {
            Some(sink) => sink(line),
            None => std::eprintln!("{line}"),
        }
    }
}

/// halo-skate: the crate's `eprintln!` goes to `log::write` (this shadows the
/// standard one in every module below), so that the engine's lines reach the
/// game's log without each one being edited.
macro_rules! eprintln {
    ($($arg:tt)*) => {
        $crate::log::write(&format!($($arg)*))
    };
}

mod physics;
mod graph_host;
mod graph_runtime;
mod skater_animation;
mod animation_pose;
mod camera;
mod difficulty;
mod grind_world;
mod input;
mod scoring_runtime;
mod skate_world;
mod animation;
mod crash_context;
mod tuning;

pub use physics::bridge;

mod session_marker;

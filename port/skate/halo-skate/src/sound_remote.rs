//! Every skater's sounds in a network game, not only this machine's
//! (port/skate/README.md, "Multiplayer" and "Sound").
//!
//! Each machine runs the engine for its own skater alone, so only its own
//! skater's ticks make sounds (`sound.rs`). For the others to hear them, this
//! machine's skater's one-shots (a pop, a landing, a grind starting, a bail,
//! ...) and its loops' state (wheels down, speed, a grind or slide, a
//! powerslide) are kept here between the game's ticks, and skate_network.c
//! takes them each tick (`halo_skate_sound_take`) and sends them, each
//! one-shot with a sequence number and a few ticks over, so that one lost
//! datagram loses none. On another machine they come back here
//! (`halo_skate_sound_remote_loops`, `halo_skate_sound_remote_event`) and
//! play as that player's own source in the mixer, at their board where that
//! machine draws it, panned and quieter with distance like this machine's
//! skater's: the loops from the latest word on them, each one-shot once.

use crate::mixer::{self, LOG, SOURCES};
use crate::sound::{Event, Loops};
use crate::sound_set::{Sound, Surface};
use bevy_math::Vec3;
use std::collections::VecDeque;
use std::sync::Mutex;
use std::sync::atomic::Ordering;
use std::time::{Duration, Instant};

/// A one-shot, as the C side has it (halo_skate_sound.h): which sound (its
/// place in `Sound::ALL`, HALO_SKATE_SOUND_...), how hard (0 to 1) and the
/// speed it was measured from (m/s).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct HaloSkateSoundEvent {
    pub sound: i32,
    pub strength: f32,
    pub speed: f32,
}

/// The loops' state, as the C side has it: wheels down (0 to 4), the board's
/// speed (m/s), the grind (0 none, 1 on the trucks, 2 on the deck) and
/// whether it powerslides.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct HaloSkateSoundLoops {
    pub wheels: i32,
    pub speed: f32,
    pub grind: i32,
    pub powerslide: i32,
}

/// One-shots kept between the game's takes at most (the newest), and for
/// how long: a take that comes later than that (the game paused, or not in
/// a network game) does not send old news.
const KEPT: usize = 16;
const KEPT_FOR: Duration = Duration::from_millis(500);

/// This machine's skater's one-shots since the last take, and its loops'
/// state after its last tick.
#[derive(Default)]
struct Kept {
    events: VecDeque<(Event, Instant)>,
    loops: Option<Loops>,
}

impl Kept {
    fn note(&mut self, events: &[Event], loops: &Loops, now: Instant) {
        for event in events {
            if self.events.len() >= KEPT {
                self.events.pop_front();
            }
            self.events.push_back((*event, now));
        }
        self.loops = Some(*loops);
    }

    /// The one-shots no older than KEPT_FOR, at most `capacity` (the newest),
    /// oldest first; the rest are dropped.
    fn take(&mut self, capacity: usize, now: Instant) -> (Vec<Event>, Loops) {
        let fresh: Vec<Event> =
            self.events.drain(..).filter(|(_, at)| now.duration_since(*at) <= KEPT_FOR).map(|(e, _)| e).collect();
        let skip = fresh.len().saturating_sub(capacity);
        (fresh.into_iter().skip(skip).collect(), self.loops.unwrap_or_default())
    }
}

static KEPT_STATE: Mutex<Option<Kept>> = Mutex::new(None);

/// (the worker, after each engine tick: mixer.rs's engine_tick) this
/// machine's skater's one-shots and loops, kept for the network.
pub(crate) fn note_local(events: &[Event], loops: &Loops) {
    let mut guard = KEPT_STATE.lock().unwrap_or_else(|e| e.into_inner());
    guard.get_or_insert_with(Kept::default).note(events, loops, Instant::now());
}

/// A sound's number on the wire: its place in `Sound::ALL`.
pub(crate) fn sound_index(sound: Sound) -> i32 {
    Sound::ALL.iter().position(|&s| s == sound).map_or(-1, |i| i as i32)
}

/// ... and back; None for a number this build does not know (a newer one's).
pub(crate) fn sound_from_index(index: i32) -> Option<Sound> {
    usize::try_from(index).ok().and_then(|i| Sound::ALL.get(i).copied())
}

fn loops_to_c(loops: &Loops) -> HaloSkateSoundLoops {
    HaloSkateSoundLoops {
        wheels: i32::from(loops.wheels.min(4)),
        speed: if loops.speed.is_finite() { loops.speed.max(0.0) } else { 0.0 },
        grind: match loops.grind {
            Some(Sound::Grind) => 1,
            Some(Sound::Slide) => 2,
            _ => 0,
        },
        powerslide: i32::from(loops.powerslide),
    }
}

fn loops_from_c(loops: &HaloSkateSoundLoops, position: Vec3) -> Loops {
    Loops {
        wheels: loops.wheels.clamp(0, 4) as u8,
        speed: if loops.speed.is_finite() { loops.speed.clamp(0.0, 100.0) } else { 0.0 },
        grind: match loops.grind {
            1 => Some(Sound::Grind),
            2 => Some(Sound::Slide),
            _ => None,
        },
        powerslide: loops.powerslide != 0,
        position,
    }
}

/// The mixer's source for a network player's index; None for none.
fn source_of(player: i32) -> Option<u8> {
    let source = usize::try_from(player).ok()? + 1;
    (source < SOURCES).then_some(source as u8)
}

/// A position from the C side (world units), None for none or not finite.
///
/// # Safety
/// `position` is NULL or holds 3 floats.
unsafe fn position_from(position: *const f32) -> Option<Vec3> {
    if position.is_null() {
        return None;
    }
    let p = Vec3::from_slice(unsafe { std::slice::from_raw_parts(position, 3) });
    p.is_finite().then_some(p)
}

/// (skate_network.c, each tick this machine's skater skates) this machine's
/// skater's one-shots since the last take, at most `capacity` (the newest),
/// oldest first, into `events`, and its loops' state into `loops`: how many.
/// One-shots older than half a second are dropped, not sent.
///
/// # Safety
/// `events` is NULL or holds `capacity` events; `loops` is NULL or one.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_sound_take(
    events: *mut HaloSkateSoundEvent,
    capacity: i32,
    loops: *mut HaloSkateSoundLoops,
) -> i32 {
    let capacity = if events.is_null() { 0 } else { usize::try_from(capacity).unwrap_or(0) };
    let (taken, state) = {
        let mut guard = KEPT_STATE.lock().unwrap_or_else(|e| e.into_inner());
        guard.get_or_insert_with(Kept::default).take(capacity, Instant::now())
    };
    if !loops.is_null() {
        unsafe { *loops = loops_to_c(&state) };
    }
    for (i, event) in taken.iter().enumerate() {
        unsafe {
            *events.add(i) = HaloSkateSoundEvent {
                sound: sound_index(event.sound),
                strength: event.strength,
                speed: event.speed,
            };
        }
    }
    taken.len() as i32
}

/// (skate_network.c, each tick) another machine's skater's loops as their
/// latest word on them says, at `position` (their board here, world units),
/// on `surface` (HALO_SKATE_SURFACE_...). `player` is their network player's
/// index.
///
/// # Safety
/// `loops` is NULL or one; `position` is NULL or holds 3 floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_sound_remote_loops(
    player: i32,
    loops: *const HaloSkateSoundLoops,
    position: *const f32,
    surface: i32,
) {
    let (Some(source), Some(position)) = (source_of(player), unsafe { position_from(position) }) else {
        return;
    };
    if loops.is_null() {
        return;
    }
    let loops = loops_from_c(unsafe { &*loops }, position);
    mixer::with_mixer(|m| m.set_loops_of(source, &loops, Surface::from_index(surface), Instant::now()));
}

/// (skate_network.c) another machine's skater's one-shot, once, at
/// `position` (their board here, world units), on `surface`; told of in the
/// log as "halo-skate: sound (remote player N): ..." (`skate_sound_log`).
/// Returns 1 when it had a sample, else 0 (none, or a sound this build does
/// not know).
///
/// # Safety
/// `event` is NULL or one; `position` is NULL or holds 3 floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_sound_remote_event(
    player: i32,
    event: *const HaloSkateSoundEvent,
    position: *const f32,
    surface: i32,
) -> i32 {
    let (Some(source), Some(position)) = (source_of(player), unsafe { position_from(position) }) else {
        return 0;
    };
    if event.is_null() {
        return 0;
    }
    let event = unsafe { *event };
    let Some(sound) = sound_from_index(event.sound) else {
        return 0;
    };
    let event = Event {
        sound,
        strength: if event.strength.is_finite() { event.strength.clamp(0.0, 1.0) } else { 0.0 },
        speed: if event.speed.is_finite() { event.speed.max(0.0) } else { 0.0 },
        position,
    };
    let sample = mixer::with_mixer(|m| m.play_of(source, &event, Surface::from_index(surface))).flatten();
    if LOG.load(Ordering::Relaxed) {
        eprintln!(
            "halo-skate: sound (remote player {player}): {} ({:.2}, {:.1} m/s) {}",
            sound.name(),
            event.strength,
            event.speed,
            sample.as_deref().unwrap_or("silent: no sample")
        );
    }
    i32::from(sample.is_some())
}

/// (skate_network.c) another machine's skater's sounds fade out: they stopped
/// skating, left, or their machine's word on them stopped coming.
#[unsafe(no_mangle)]
pub extern "C" fn halo_skate_sound_remote_stop(player: i32) {
    if let Some(source) = source_of(player) {
        mixer::with_mixer(|m| m.stop_of(source));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn event(sound: Sound) -> Event {
        Event { sound, strength: 0.5, speed: 3.0, position: Vec3::ZERO }
    }

    /// The numbers on the wire are halo_skate_sound.h's HALO_SKATE_SOUND_...,
    /// in this order: a change here is a change of the wire format.
    #[test]
    fn the_sounds_numbers_are_the_headers() {
        let header = include_str!("../../include/halo_skate_sound.h");
        for sound in Sound::ALL {
            let line = format!("HALO_SKATE_SOUND_{} = {}", sound.name().to_uppercase(), sound_index(sound));
            assert!(header.contains(&line), "{line} not in halo_skate_sound.h");
            assert_eq!(sound_from_index(sound_index(sound)), Some(sound));
        }
        assert_eq!(sound_from_index(-1), None);
        assert_eq!(sound_from_index(Sound::ALL.len() as i32), None);
    }

    #[test]
    fn the_loops_go_through_the_c_side_as_they_are() {
        let loops = Loops { wheels: 3, speed: 6.5, grind: Some(Sound::Slide), powerslide: true, position: Vec3::ONE };
        assert_eq!(loops_from_c(&loops_to_c(&loops), Vec3::ONE), loops);
        let grinding = Loops { grind: Some(Sound::Grind), ..loops };
        assert_eq!(loops_from_c(&loops_to_c(&grinding), Vec3::ONE), grinding);
        // and nothing odd from the network makes odd loops
        let odd = HaloSkateSoundLoops { wheels: 9, speed: f32::NAN, grind: 7, powerslide: 2 };
        let read = loops_from_c(&odd, Vec3::ZERO);
        assert_eq!((read.wheels, read.speed, read.grind, read.powerslide), (4, 0.0, None, true));
    }

    /// A take has what came since the last, the newest when there are more
    /// than it holds, and nothing older than half a second.
    #[test]
    fn a_take_has_the_fresh_one_shots() {
        let mut kept = Kept::default();
        let start = Instant::now();
        let rolling = Loops { wheels: 4, speed: 3.0, ..Default::default() };
        kept.note(&[event(Sound::Pop)], &rolling, start);
        kept.note(&[event(Sound::Land), event(Sound::GrindStart)], &rolling, start);
        let (events, loops) = kept.take(2, start);
        assert_eq!(events.iter().map(|e| e.sound).collect::<Vec<_>>(), [Sound::Land, Sound::GrindStart]);
        assert_eq!(loops, rolling);
        assert!(kept.take(4, start).0.is_empty(), "taken once");
        kept.note(&[event(Sound::Bail)], &rolling, start);
        assert!(kept.take(4, start + Duration::from_secs(1)).0.is_empty(), "too old to send");
        for _ in 0..40 {
            kept.note(&[event(Sound::Pop)], &rolling, start);
        }
        assert_eq!(kept.take(100, start).0.len(), KEPT);
    }

    #[test]
    fn the_c_side_is_safe_with_what_it_might_pass() {
        let mut loops = HaloSkateSoundLoops::default();
        assert_eq!(unsafe { halo_skate_sound_take(std::ptr::null_mut(), 4, &mut loops) }, 0);
        let event = HaloSkateSoundEvent { sound: 4, strength: 1.0, speed: 2.0 };
        let position = [1.0f32, 2.0, 3.0];
        let nan = [f32::NAN; 3];
        unsafe {
            assert_eq!(halo_skate_sound_remote_event(-1, &event, position.as_ptr(), 0), 0);
            assert_eq!(halo_skate_sound_remote_event(500, &event, position.as_ptr(), 0), 0);
            assert_eq!(halo_skate_sound_remote_event(1, std::ptr::null(), position.as_ptr(), 0), 0);
            assert_eq!(halo_skate_sound_remote_event(1, &event, nan.as_ptr(), 0), 0);
            assert_eq!(halo_skate_sound_remote_event(1, &event, std::ptr::null(), 0), 0);
            let unknown = HaloSkateSoundEvent { sound: 99, ..event };
            assert_eq!(halo_skate_sound_remote_event(1, &unknown, position.as_ptr(), 0), 0);
            halo_skate_sound_remote_loops(1, std::ptr::null(), position.as_ptr(), 0);
            halo_skate_sound_remote_loops(-3, &loops, position.as_ptr(), 9);
        }
        halo_skate_sound_remote_stop(-1);
        halo_skate_sound_remote_stop(1000);
    }
}

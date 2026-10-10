//! The skater's sounds, from what the engine knows each tick.
//!
//! After each simulated tick (60 a second) the worker looks at the engine's
//! state (`skate_host::bridge::SoundObservation`: the state, which of the
//! board's parts touch the world, how hard they hit, the last launch into the
//! air, the ragdoll) and `Detector` turns the changes into one-shot events
//! (a pop, a landing, a grind starting and ending, a bail, ...) and the
//! loops' state (the wheels rolling, a grind or slide going on), which go to
//! the mixer (`mixer.rs`). Each one-shot is told of in the log
//! (`skate_sound_log 0` quiets that).

use crate::sound_set::Sound;
use bevy_math::Vec3;
use skate_host::bridge::SoundObservation;

/// A one-shot event.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct Event {
    pub(crate) sound: Sound,
    /// How hard, 0 to 1: a landing's or impact's closing speed, a bail's
    /// speed.
    pub(crate) strength: f32,
    /// The speed it was measured from, m/s (for the log).
    pub(crate) speed: f32,
    /// Where, world units.
    pub(crate) position: Vec3,
}

/// What the loops play, after a tick.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(crate) struct Loops {
    /// Wheels on the ground, 0 to 4.
    pub(crate) wheels: u8,
    /// The board's speed, m/s.
    pub(crate) speed: f32,
    /// A grind (trucks) or slide (deck) going on: which loop.
    pub(crate) grind: Option<Sound>,
    pub(crate) powerslide: bool,
    /// The board, world units.
    pub(crate) position: Vec3,
}

/// The engine's states (`PhysicalStateId`) as the sounds group them.
fn is_grind(state: u32) -> bool {
    (400..=405).contains(&state)
}
/// Boardslide, tipslide (nose and tail slides) and darkslide slide on the deck;
/// the other grinds are on the trucks.
fn grind_sound(state: u32) -> Option<Sound> {
    match state {
        400 | 402 | 405 => Some(Sound::Slide),
        401 | 403 | 404 => Some(Sound::Grind),
        _ => None,
    }
}
fn is_off_board(state: u32) -> bool {
    (500..=502).contains(&state)
}
const WIPEOUT: u32 = 300;
const POWERSLIDE: u32 = 101;

/// Ticks in the air before touching down counts as a landing (0.1 s): less is
/// a bump.
const LANDING_AIR_TICKS: u32 = 6;
/// A launch counts as a pop only so soon after the board was on something
/// (wheels down or grinding).
const POP_SUPPORT_TICKS: u32 = 10;
/// Closing speed, m/s, of the deck or a truck hitting something that makes a
/// sound, and the least ticks between two such.
const IMPACT_SPEED: f32 = 1.0;
const IMPACT_TICKS: u32 = 8;
/// Closing speed (m/s) and speed of a bail that make the loudest sound.
const LOUDEST_LANDING: f32 = 5.0;
const LOUDEST_IMPACT: f32 = 4.0;
const LOUDEST_BAIL: f32 = 8.0;

fn speed_of(o: &SoundObservation) -> f32 {
    let s = Vec3::from_array(o.board_velocity).length();
    if s.is_finite() { s } else { 0.0 }
}

fn position_of(o: &SoundObservation) -> Vec3 {
    let p = crate::from_skate(Vec3::from_array(o.board_position));
    if p.is_finite() { p } else { Vec3::ZERO }
}

/// Turns the engine's ticks into events and loops.
#[derive(Default)]
pub(crate) struct Detector {
    last: Option<SoundObservation>,
    /// The launch last seen (its number), so that only a new one pops.
    launch: Option<u64>,
    /// Ticks since the wheels were down or the board grinding.
    unsupported: u32,
    /// Ticks the wheels have been off the ground.
    airborne: u32,
    /// Ticks before another impact may sound.
    impact_wait: u32,
    /// The fastest the board fell toward the ground in the air (m/s), for a
    /// landing whose contacts say less.
    falling: f32,
}

impl Detector {
    /// The skater was put on the board (J, or after a failed step): what it
    /// is doing now makes no sound.
    pub(crate) fn reset(&mut self, now: &SoundObservation) {
        *self = Self {
            last: Some(*now),
            launch: now.launch.map(|(id, _)| id),
            ..Self::default()
        };
    }

    /// One tick of the engine: the events it made, appended to `events`, and
    /// the loops' state.
    pub(crate) fn observe(&mut self, now: &SoundObservation, events: &mut Vec<Event>) -> Loops {
        let speed = speed_of(now);
        let position = position_of(now);
        let wheels = now.wheels.iter().filter(|&&w| w).count() as u8;
        let grinding = is_grind(now.state);
        let Some(last) = self.last.replace(*now) else {
            self.launch = now.launch.map(|(id, _)| id);
            return Loops { wheels, speed, grind: grind_sound(now.state), powerslide: now.state == POWERSLIDE, position };
        };
        let mut emit = |sound: Sound, strength: f32, speed: f32| {
            events.push(Event { sound, strength: strength.clamp(0.0, 1.0), speed, position });
        };
        let was_grinding = is_grind(last.state);
        let was_off = is_off_board(last.state);
        let off = is_off_board(now.state);
        let wiping = now.state == WIPEOUT || now.ragdoll;
        let was_wiping = last.state == WIPEOUT || last.ragdoll;
        let riding = !off && !wiping;
        let last_wheels = last.wheels.iter().filter(|&&w| w).count();
        self.impact_wait = self.impact_wait.saturating_sub(1);

        // a new launch into the air that the skater jumped into, from the
        // board on something: a pop (an ollie, nollie, or off a grind)
        let launch = now.launch.map(|(id, _)| id);
        if launch.is_some() && launch != self.launch {
            if now.launch.is_some_and(|(_, jumped)| jumped) && riding && self.unsupported <= POP_SUPPORT_TICKS {
                emit(Sound::Pop, 1.0, speed);
            }
            self.launch = launch;
        }

        // grinds and slides
        if grinding && !was_grinding {
            let start = if grind_sound(now.state) == Some(Sound::Slide) { Sound::SlideStart } else { Sound::GrindStart };
            emit(start, (now.closing_speed.max(self.falling) / LOUDEST_LANDING).max(0.4), speed);
        } else if was_grinding && !grinding {
            let end = if grind_sound(last.state) == Some(Sound::Slide) { Sound::SlideEnd } else { Sound::GrindEnd };
            emit(end, (speed / LOUDEST_IMPACT).max(0.3), speed);
        }

        // the wheels back down after a while in the air, riding: a landing,
        // as loud as the board hit (its parts' closing speed, or how fast it
        // was falling)
        let mut landed = false;
        if wheels > 0 && last_wheels == 0 && self.airborne >= LANDING_AIR_TICKS && !grinding {
            let hit = now.closing_speed.max(self.falling);
            if riding {
                emit(Sound::Land, hit / LOUDEST_LANDING, hit);
                landed = true;
            } else if hit >= IMPACT_SPEED {
                // the board landing on its own
                emit(Sound::BoardImpact, hit / LOUDEST_IMPACT, hit);
                self.impact_wait = IMPACT_TICKS;
                landed = true;
            }
        }

        // the deck or a truck newly hitting something hard, not grinding:
        // the board slapping a ledge, landing primo, or tumbling in a bail
        let parts_hit = (now.deck && !last.deck) || (now.trucks[0] && !last.trucks[0]) || (now.trucks[1] && !last.trucks[1]);
        if parts_hit && !landed && !grinding && now.closing_speed >= IMPACT_SPEED && self.impact_wait == 0 {
            emit(Sound::BoardImpact, now.closing_speed / LOUDEST_IMPACT, now.closing_speed);
            self.impact_wait = IMPACT_TICKS;
        }

        // a bail, getting off and back on
        if wiping && !was_wiping {
            emit(Sound::Bail, speed / LOUDEST_BAIL, speed);
        }
        if off && !was_off && !was_wiping && !wiping {
            emit(Sound::StepOff, 0.6, speed);
        } else if was_off && riding {
            emit(Sound::StepOn, 0.6, speed);
        }

        // what the next tick measures from
        if wheels > 0 || grinding {
            self.unsupported = 0;
            self.airborne = 0;
            self.falling = 0.0;
        } else {
            self.unsupported = self.unsupported.saturating_add(1);
            self.airborne = self.airborne.saturating_add(1);
            // (Skate is Y up)
            let down = -now.board_velocity[1];
            if down.is_finite() {
                self.falling = self.falling.max(down);
            }
        }
        Loops {
            wheels,
            speed,
            grind: grind_sound(now.state),
            powerslide: now.state == POWERSLIDE && wheels > 0,
            position,
        }
    }
}

/// The worker's detector, between its ticks.
static DETECTOR: std::sync::Mutex<Option<Detector>> = std::sync::Mutex::new(None);

/// (the worker, after each tick of the engine) the tick's events and loops to
/// the mixer.
pub(crate) fn observe(session: &skate_host::bridge::Session) {
    let now = session.sound_observation();
    let mut events = Vec::new();
    let loops = {
        let mut guard = DETECTOR.lock().unwrap_or_else(|e| e.into_inner());
        guard.get_or_insert_with(Detector::default).observe(&now, &mut events)
    };
    crate::mixer::engine_tick(&events, &loops);
}

/// (the worker) the skater was put on the board, by J or after a failed
/// step: its state as it is makes no sound.
pub(crate) fn reset(session: &skate_host::bridge::Session) {
    let now = session.sound_observation();
    DETECTOR.lock().unwrap_or_else(|e| e.into_inner()).get_or_insert_with(Detector::default).reset(&now);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rolling(speed: f32) -> SoundObservation {
        SoundObservation {
            state: 100,
            wheels: [true; 4],
            board_velocity: [0.0, 0.0, speed],
            launch: Some((1, false)),
            ..Default::default()
        }
    }

    fn run(detector: &mut Detector, ticks: &[SoundObservation]) -> Vec<Event> {
        let mut events = Vec::new();
        for t in ticks {
            detector.observe(t, &mut events);
        }
        events
    }

    fn sounds(events: &[Event]) -> Vec<Sound> {
        events.iter().map(|e| e.sound).collect()
    }

    /// An ollie: the engine launches a jumped trajectory, the wheels leave the
    /// ground, the skater is in the air a while, and the wheels come down
    /// with the board falling at 3 m/s.
    #[test]
    fn an_ollie_pops_and_lands() {
        let mut d = Detector::default();
        let ground = rolling(4.0);
        let mut popped = rolling(4.0);
        popped.launch = Some((2, true));
        let mut air = popped;
        air.state = 200;
        air.wheels = [false; 4];
        air.board_velocity = [0.0, 2.0, 4.0];
        let mut falling = air;
        falling.board_velocity = [0.0, -3.0, 4.0];
        let mut down = rolling(4.0);
        down.launch = Some((2, true));
        down.closing_speed = 2.5;
        let mut ticks = vec![ground, ground, popped];
        ticks.extend(std::iter::repeat_n(air, 10));
        ticks.extend(std::iter::repeat_n(falling, 10));
        ticks.extend([down, down]);
        let events = run(&mut d, &ticks);
        assert_eq!(sounds(&events), [Sound::Pop, Sound::Land]);
        // as loud as it fell (3 m/s, more than the contacts' 2.5)
        assert!((events[1].strength - 3.0 / LOUDEST_LANDING).abs() < 1e-5);
    }

    /// Rolling off an edge: a launch the skater did not jump into, so no pop;
    /// the landing still sounds.
    #[test]
    fn rolling_off_an_edge_does_not_pop() {
        let mut d = Detector::default();
        let mut off = rolling(5.0);
        off.state = 200;
        off.wheels = [false; 4];
        off.launch = Some((3, false));
        off.board_velocity = [0.0, -1.0, 5.0];
        let mut ticks = vec![rolling(5.0)];
        ticks.extend(std::iter::repeat_n(off, 12));
        ticks.push(rolling(5.0));
        assert_eq!(sounds(&run(&mut d, &ticks)), [Sound::Land]);
    }

    /// A bump: the wheels off the ground for less than LANDING_AIR_TICKS.
    #[test]
    fn a_bump_is_not_a_landing() {
        let mut d = Detector::default();
        let mut bump = rolling(5.0);
        bump.wheels = [false; 4];
        let mut ticks = vec![rolling(5.0)];
        ticks.extend(std::iter::repeat_n(bump, 3));
        ticks.push(rolling(5.0));
        assert!(run(&mut d, &ticks).is_empty());
    }

    /// A 50-50 then a boardslide: the grind starts and ends, and the loop is
    /// the trucks' then the deck's.
    #[test]
    fn grinds_and_slides_start_and_end() {
        let mut d = Detector::default();
        let mut grind = rolling(3.0);
        grind.state = 401;
        grind.wheels = [false; 4];
        grind.trucks = [true; 2];
        let mut slide = grind;
        slide.state = 400;
        let mut events = Vec::new();
        d.observe(&rolling(3.0), &mut events);
        assert_eq!(d.observe(&grind, &mut events).grind, Some(Sound::Grind));
        assert_eq!(d.observe(&grind, &mut events).grind, Some(Sound::Grind));
        assert_eq!(d.observe(&rolling(3.0), &mut events).grind, None);
        d.observe(&slide, &mut events);
        d.observe(&rolling(3.0), &mut events);
        assert_eq!(sounds(&events), [Sound::GrindStart, Sound::GrindEnd, Sound::SlideStart, Sound::SlideEnd]);
    }

    /// A bail: the wipeout state, the ragdoll, then the board tumbling (its
    /// deck hitting the ground hard), then back on.
    #[test]
    fn a_bail_and_the_board_tumbling() {
        let mut d = Detector::default();
        let mut bail = rolling(6.0);
        bail.state = 300;
        bail.ragdoll = true;
        let mut tumble = bail;
        tumble.wheels = [false; 4];
        let mut hit = tumble;
        hit.deck = true;
        hit.closing_speed = 2.0;
        let mut soft = tumble;
        soft.deck = true;
        soft.closing_speed = 0.2;
        let ticks = [rolling(6.0), bail, bail, tumble, hit, tumble, soft, tumble];
        let events = run(&mut d, &ticks);
        assert_eq!(sounds(&events), [Sound::Bail, Sound::BoardImpact]);
        assert!((events[0].strength - 6.0 / LOUDEST_BAIL).abs() < 1e-5);
    }

    #[test]
    fn stepping_off_and_on() {
        let mut d = Detector::default();
        let mut off = rolling(0.0);
        off.state = 500;
        let mut on = rolling(0.0);
        on.state = 503;
        let events = run(&mut d, &[rolling(1.0), off, off, on, rolling(0.0)]);
        assert_eq!(sounds(&events), [Sound::StepOff, Sound::StepOn]);
    }

    /// Getting on (J) in the air, grinding or bailing makes no sound of its
    /// own: reset takes the state as it is.
    #[test]
    fn a_reset_takes_the_state_as_it_is() {
        let mut d = Detector::default();
        let mut grind = rolling(3.0);
        grind.state = 401;
        grind.launch = Some((9, true));
        d.reset(&grind);
        assert!(run(&mut d, &[grind, grind]).is_empty());
    }

    #[test]
    fn the_loops_follow_the_wheels_and_the_speed() {
        let mut d = Detector::default();
        let mut events = Vec::new();
        let mut two = rolling(7.0);
        two.wheels = [true, true, false, false];
        let loops = d.observe(&two, &mut events);
        assert_eq!(loops.wheels, 2);
        assert!((loops.speed - 7.0).abs() < 1e-5);
        let mut slide = rolling(4.0);
        slide.state = POWERSLIDE;
        assert!(d.observe(&slide, &mut events).powerslide);
        let mut nan = rolling(f32::NAN);
        nan.board_position = [f32::NAN; 3];
        let loops = d.observe(&nan, &mut events);
        assert_eq!(loops.speed, 0.0);
        assert_eq!(loops.position, Vec3::ZERO);
    }
}

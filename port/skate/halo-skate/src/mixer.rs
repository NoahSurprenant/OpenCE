//! The skater's sounds played: a few voices of our own samples, mixed into
//! the game's output by the port's audio thread (`dsound_sdl.c` calls
//! `halo_skate_sound_mix`, after the game's voices and before its limiter).
//!
//! The worker hands each engine tick's events and loops over
//! (`engine_tick`). A loop (the wheels, a grind) is a voice whose volume and
//! pitch glide toward what the tick asks; a change of sample (a new surface)
//! fades the old voice out as the new one fades in. A one-shot plays once.
//! Every volume, pan and pitch change is smoothed over some milliseconds, so
//! nothing clicks, and loops fade out by themselves when the game stops
//! telling them what to do (paused, or off the board).
//!
//! Each voice is placed at the board: panned by where it is from the camera
//! (`halo_skate_sound_listener`) and quieter with distance past 1 m.

use crate::sound::{Event, Loops};
use crate::sound_set::{Bank, Clip, Sound, Surface};
use bevy_math::Vec3;
use std::ffi::{CStr, c_char};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

/// The most voices at once; a one-shot past that takes the oldest one-shot's.
const VOICES: usize = 24;
/// How long volume and pan changes take, and pitch changes, seconds.
const GAIN_GLIDE: f32 = 0.02;
const PITCH_GLIDE: f32 = 0.06;
const LOOPS: [Sound; 4] = [Sound::Roll, Sound::Grind, Sound::Slide, Sound::Powerslide];
/// Loops not told what to do for this long fade out (the game paused).
const SILENCE_AFTER: Duration = Duration::from_millis(250);
/// The distance (world units, about 1 m) within which a sound is as loud as
/// it is, and how far it pans at most (1: all the way).
const NEAR: f32 = 1.0 / 3.048;
const PAN_WIDTH: f32 = 0.6;
/// Loops' crossfade at their seam, seconds at most.
const SEAM: f32 = 0.01;

/// Where the sounds are heard from: the camera.
#[derive(Clone, Copy, Default)]
struct Listener {
    position: Vec3,
    right: Vec3,
    set: bool,
}

struct Voice {
    clip: Arc<Clip>,
    sound: Sound,
    looped: bool,
    /// Frames into the clip.
    position: f64,
    gain: f32,
    target_gain: f32,
    pitch: f32,
    target_pitch: f32,
    left: f32,
    right: f32,
    target_left: f32,
    target_right: f32,
    /// A loop being replaced, or anything stopped: done once silent.
    fading: bool,
    /// When it started, for taking the oldest one-shot's voice.
    serial: u64,
}

impl Voice {
    fn done(&self) -> bool {
        (self.fading || self.target_gain == 0.0) && self.gain < 1e-4 && self.target_gain == 0.0
            || (!self.looped && self.position >= self.clip.samples.len() as f64)
    }

    /// The clip's sample at frame `at` (between frames, blended); a loop's
    /// last `seam` frames are crossfaded into its start, so its seam is
    /// smooth whatever the file's ends are.
    fn sample(&self, at: f64) -> f32 {
        let samples = &self.clip.samples;
        let length = samples.len();
        if length == 0 {
            return 0.0;
        }
        let read = |i: usize| samples.get(i).copied().unwrap_or(0.0);
        let at_index = |at: f64| {
            let i = at.floor();
            let t = (at - i) as f32;
            let i = i as usize;
            (i, t)
        };
        if !self.looped {
            let (i, t) = at_index(at);
            return read(i) * (1.0 - t) + read(i + 1) * t;
        }
        let seam = seam_frames(&self.clip);
        let period = (length - seam).max(1);
        let wrapped = |i: usize| -> f32 {
            let i = i % period;
            if i < seam {
                let t = i as f32 / seam as f32;
                read(i) * t + read(i + period) * (1.0 - t)
            } else {
                read(i)
            }
        };
        let (i, t) = at_index(at);
        wrapped(i) * (1.0 - t) + wrapped(i + 1) * t
    }

    fn length(&self) -> f64 {
        if self.looped {
            (self.clip.samples.len() - seam_frames(&self.clip)).max(1) as f64
        } else {
            self.clip.samples.len() as f64
        }
    }
}

fn seam_frames(clip: &Clip) -> usize {
    ((clip.rate as f32 * SEAM) as usize).min(clip.samples.len() / 4)
}

/// How a loop plays for the tick's state: its volume (0 is silent) and pitch.
pub(crate) fn loop_level(sound: Sound, loops: &Loops) -> (f32, f32) {
    let fast = |top: f32| (loops.speed / top).clamp(0.0, 1.0);
    match sound {
        Sound::Roll if loops.wheels > 0 && loops.grind.is_none() && loops.speed > 0.15 => {
            let s = fast(8.0);
            (0.55 * s.sqrt() * (0.7 + 0.3 * f32::from(loops.wheels.min(4)) / 4.0), 0.8 + 0.45 * s)
        }
        Sound::Grind | Sound::Slide if loops.grind == Some(sound) => {
            (0.6 * (0.4 + loops.speed / 6.0).clamp(0.0, 1.0), 0.9 + 0.25 * fast(8.0))
        }
        Sound::Powerslide if loops.powerslide => (0.5 * fast(5.0), 0.9 + 0.2 * fast(8.0)),
        _ => (0.0, 1.0),
    }
}

/// A one-shot's volume and pitch for its strength.
pub(crate) fn one_shot_level(event: &Event) -> (f32, f32) {
    let s = event.strength.clamp(0.0, 1.0);
    match event.sound {
        Sound::Pop => (0.8, 1.0),
        Sound::Land => (0.3 + 0.7 * s, 1.06 - 0.12 * s),
        Sound::GrindStart | Sound::SlideStart => (0.4 + 0.5 * s, 1.0),
        Sound::GrindEnd | Sound::SlideEnd => (0.3 + 0.4 * s, 1.0),
        Sound::BoardImpact => (0.25 + 0.75 * s, 1.08 - 0.16 * s),
        Sound::Bail => (0.5 + 0.5 * s, 1.0),
        Sound::StepOff | Sound::StepOn => (0.35, 1.0),
        _ => (0.0, 1.0),
    }
}

pub(crate) struct Mixer {
    bank: Arc<Bank>,
    voices: Vec<Voice>,
    listener: Listener,
    surface: Surface,
    /// The skate sounds' own volume (audio.effects_volume times skate_volume).
    volume: f32,
    /// When the loops were last told what to do.
    updated: Option<Instant>,
    serial: u64,
    random: u32,
}

impl Mixer {
    pub(crate) fn new(bank: Arc<Bank>) -> Self {
        Self {
            bank,
            voices: Vec::with_capacity(VOICES),
            listener: Listener::default(),
            surface: Surface::Concrete,
            volume: 1.0,
            updated: None,
            serial: 0,
            random: 0x2545_f491,
        }
    }

    fn next_random(&mut self) -> u32 {
        // xorshift32
        let mut x = self.random;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        self.random = x;
        x
    }

    fn pick(&mut self, clips: &[Arc<Clip>]) -> Option<Arc<Clip>> {
        if clips.is_empty() {
            return None;
        }
        let i = self.next_random() as usize % clips.len();
        clips.get(i).cloned()
    }

    /// Left and right gains for a sound at `position`.
    fn place(&self, position: Vec3) -> (f32, f32) {
        if !self.listener.set {
            let side = std::f32::consts::FRAC_1_SQRT_2;
            return (side, side);
        }
        let to = position - self.listener.position;
        let distance = to.length();
        let near = if distance.is_finite() && distance > NEAR { NEAR / distance } else { 1.0 };
        let pan = if distance > 1e-4 { (to / distance).dot(self.listener.right) * PAN_WIDTH } else { 0.0 };
        let pan = if pan.is_finite() { pan.clamp(-1.0, 1.0) } else { 0.0 };
        let angle = (pan + 1.0) * std::f32::consts::FRAC_PI_4;
        (angle.cos() * near, angle.sin() * near)
    }

    /// The loops as the tick asks them to play, at `now`.
    pub(crate) fn set_loops(&mut self, loops: &Loops, now: Instant) {
        self.updated = Some(now);
        let (left, right) = self.place(loops.position);
        for sound in LOOPS {
            let (gain, pitch) = loop_level(sound, loops);
            let clips: Vec<Arc<Clip>> = self.bank.lookup(sound, self.surface).to_vec();
            let current = self.voices.iter().position(|v| v.sound == sound && v.looped && !v.fading);
            let keep = current.filter(|&i| clips.iter().any(|c| Arc::ptr_eq(c, &self.voices[i].clip)));
            if let Some(i) = current.filter(|_| keep.is_none()) {
                // another surface's sample, or none any more: faded out
                self.voices[i].fading = true;
                self.voices[i].target_gain = 0.0;
            }
            match keep {
                Some(i) => {
                    let v = &mut self.voices[i];
                    v.target_gain = gain;
                    v.target_pitch = pitch;
                    v.target_left = left;
                    v.target_right = right;
                }
                None if gain > 0.0 => {
                    let Some(clip) = self.pick(&clips) else {
                        continue;
                    };
                    // (from anywhere in it, so that it never starts the same)
                    let start = f64::from(self.next_random() % 1024) / 1024.0 * clip.samples.len() as f64 * 0.75;
                    self.start(Voice {
                        clip,
                        sound,
                        looped: true,
                        position: start,
                        gain: 0.0,
                        target_gain: gain,
                        pitch,
                        target_pitch: pitch,
                        left,
                        right,
                        target_left: left,
                        target_right: right,
                        fading: false,
                        serial: 0,
                    });
                }
                None => {}
            }
        }
    }

    /// Plays a one-shot; returns the sample's name, or None for silence.
    pub(crate) fn play(&mut self, event: &Event) -> Option<String> {
        let (gain, pitch) = one_shot_level(event);
        let clips: Vec<Arc<Clip>> = self.bank.lookup(event.sound, self.surface).to_vec();
        let clip = self.pick(&clips)?;
        let name = clip.name.clone();
        // a few percent higher or lower each time
        let pitch = pitch * (0.96 + 0.08 * (self.next_random() % 1000) as f32 / 1000.0);
        let (left, right) = self.place(event.position);
        self.start(Voice {
            clip,
            sound: event.sound,
            looped: false,
            position: 0.0,
            gain,
            target_gain: gain,
            pitch,
            target_pitch: pitch,
            left,
            right,
            target_left: left,
            target_right: right,
            fading: false,
            serial: 0,
        });
        Some(name)
    }

    fn start(&mut self, mut voice: Voice) {
        self.serial += 1;
        voice.serial = self.serial;
        if self.voices.len() >= VOICES {
            // the oldest one-shot gives its voice up, else the oldest fading one
            let oldest = self
                .voices
                .iter()
                .enumerate()
                .filter(|(_, v)| !v.looped || v.fading)
                .min_by_key(|(_, v)| v.serial)
                .map(|(i, _)| i);
            match oldest {
                Some(i) => {
                    self.voices.swap_remove(i);
                }
                None => return,
            }
        }
        self.voices.push(voice);
    }

    /// Everything fades out (off the board).
    pub(crate) fn stop(&mut self) {
        for v in &mut self.voices {
            v.fading = true;
            v.target_gain = 0.0;
        }
        self.updated = None;
    }

    /// Adds the voices to `output` (interleaved stereo, `rate` frames a
    /// second), times `gain`, at `now`.
    pub(crate) fn mix(&mut self, output: &mut [f32], rate: u32, gain: f32, now: Instant) {
        if self.voices.is_empty() || rate == 0 {
            return;
        }
        if self.updated.is_none_or(|at| now.duration_since(at) > SILENCE_AFTER) {
            for v in self.voices.iter_mut().filter(|v| v.looped) {
                v.target_gain = 0.0;
            }
        }
        let rate = rate as f32;
        let glide = 1.0 - (-1.0 / (GAIN_GLIDE * rate)).exp();
        let pitch_glide = 1.0 - (-1.0 / (PITCH_GLIDE * rate)).exp();
        let gain = if gain.is_finite() { gain.clamp(0.0, 4.0) } else { 0.0 } * self.volume;
        for v in &mut self.voices {
            let step = f64::from(v.clip.rate) / f64::from(rate);
            let length = v.length();
            for frame in output.chunks_exact_mut(2) {
                if !v.looped && v.position >= length {
                    break;
                }
                v.gain += (v.target_gain - v.gain) * glide;
                v.left += (v.target_left - v.left) * glide;
                v.right += (v.target_right - v.right) * glide;
                v.pitch += (v.target_pitch - v.pitch) * pitch_glide;
                let s = v.sample(v.position) * v.gain * gain;
                frame[0] += s * v.left;
                frame[1] += s * v.right;
                v.position += step * f64::from(v.pitch.clamp(0.25, 4.0));
                if v.looped && v.position >= length {
                    v.position %= length;
                }
            }
        }
        self.voices.retain(|v| !v.done());
    }

    #[cfg(test)]
    fn voices_of(&self, sound: Sound) -> usize {
        self.voices.iter().filter(|v| v.sound == sound).count()
    }
}

/// The mixer the audio thread plays; None until the sound set is loaded.
static MIXER: Mutex<Option<Mixer>> = Mutex::new(None);
/// Each one-shot told of in the log (`skate_sound_log`).
static LOG: AtomicBool = AtomicBool::new(true);

fn with_mixer<T>(f: impl FnOnce(&mut Mixer) -> T) -> Option<T> {
    let mut guard = MIXER.lock().unwrap_or_else(|e| e.into_inner());
    guard.as_mut().map(f)
}

/// (the worker, after each engine tick) the tick's events played, each told
/// of in the log, and the loops set.
pub(crate) fn engine_tick(events: &[Event], loops: &Loops) {
    let log = LOG.load(Ordering::Relaxed);
    let played: Vec<(Event, Option<String>)> = with_mixer(|m| {
        m.set_loops(loops, Instant::now());
        events.iter().map(|e| (*e, m.play(e))).collect()
    })
    .unwrap_or_else(|| events.iter().map(|e| (*e, None)).collect());
    if log {
        for (e, sample) in played {
            eprintln!(
                "halo-skate: sound: {} ({:.2}, {:.1} m/s) {}",
                e.sound.name(),
                e.strength,
                e.speed,
                sample.as_deref().unwrap_or("silent: no sample")
            );
        }
    }
}

/// Loads the sound set (`sound_set.rs`) for the converted data in
/// `assets` (`skate-data/assets`; NULL for none): `HALO_SKATE_SOUNDS`,
/// `skate-data/sounds`, then the built-in set. Returns how many of the sounds
/// have samples; each one without says so in the log, once.
///
/// # Safety
/// `assets` is NULL or a C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_sound_load(assets: *const c_char) -> i32 {
    let assets = (!assets.is_null()).then(|| PathBuf::from(unsafe { CStr::from_ptr(assets) }.to_string_lossy().into_owned()));
    let started = Instant::now();
    let folders = Bank::folders(assets.as_deref());
    let bank = Bank::load(&folders, crate::sound_defaults::DEFAULTS, &mut |line| eprintln!("{line}"));
    let mut found = 0;
    for sound in Sound::ALL {
        let has_any = Surface::ALL.iter().any(|&s| !bank.lookup(sound, s).is_empty());
        match (bank.source(sound), has_any) {
            (Some(source), _) => {
                found += 1;
                if source != "the built-in set" {
                    eprintln!("halo-skate: sound: {} from {source}", sound.name());
                }
            }
            (None, true) => found += 1,
            (None, false) => eprintln!("halo-skate: sound: no samples for {}: silent", sound.name()),
        }
    }
    eprintln!(
        "halo-skate: sound: {found} of {} sounds have samples (looked in {}), in {}ms",
        Sound::ALL.len(),
        folders.iter().map(|f| f.display().to_string()).collect::<Vec<_>>().join(", "),
        started.elapsed().as_millis()
    );
    let bank = Arc::new(bank);
    let mut guard = MIXER.lock().unwrap_or_else(|e| e.into_inner());
    match guard.as_mut() {
        Some(mixer) => {
            mixer.stop();
            mixer.voices.clear();
            mixer.bank = bank;
        }
        None => *guard = Some(Mixer::new(bank)),
    }
    found
}

/// The skate sounds' volume, 0 to 4 (audio.effects_volume times
/// skate_volume); audio.volume is applied by the mix.
#[unsafe(no_mangle)]
pub extern "C" fn halo_skate_sound_set_volume(volume: f32) {
    let volume = if volume.is_finite() { volume.clamp(0.0, 4.0) } else { 1.0 };
    with_mixer(|m| m.volume = volume);
}

/// Whether each one-shot is told of in the log (1, at first) or not (0).
#[unsafe(no_mangle)]
pub extern "C" fn halo_skate_sound_set_log(enabled: i32) {
    LOG.store(enabled != 0, Ordering::Relaxed);
}

/// Where the sounds are heard from (the camera, world units) and which way
/// it looks, and the surface under the board (0 concrete, 1 metal, 2 wood,
/// 3 rough), each tick.
///
/// # Safety
/// `position`, `forward` and `up` hold 3 floats each.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_sound_listener(position: *const f32, forward: *const f32, up: *const f32, surface: i32) {
    if position.is_null() || forward.is_null() || up.is_null() {
        return;
    }
    let read = |p: *const f32| Vec3::from_slice(unsafe { std::slice::from_raw_parts(p, 3) });
    let (position, forward, up) = (read(position), read(forward), read(up));
    // (Halo's forward, left, up: right is forward cross up)
    let right = forward.cross(up).normalize_or_zero();
    with_mixer(|m| {
        m.surface = Surface::from_index(surface);
        if position.is_finite() && right != Vec3::ZERO {
            m.listener = Listener { position, right, set: true };
        }
    });
}

/// Everything fades out: the skater is off the board.
#[unsafe(no_mangle)]
pub extern "C" fn halo_skate_sound_stop() {
    with_mixer(Mixer::stop);
}

/// (the audio thread) adds the skater's sounds to `output`, `frames` frames
/// of interleaved stereo float at `rate` Hz, times `gain` (audio.volume).
///
/// # Safety
/// `output` holds `frames * 2` floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn halo_skate_sound_mix(output: *mut f32, frames: u32, rate: i32, gain: f32) {
    if output.is_null() || frames == 0 || rate <= 0 {
        return;
    }
    let output = unsafe { std::slice::from_raw_parts_mut(output, frames as usize * 2) };
    with_mixer(|m| m.mix(output, rate as u32, gain, Instant::now()));
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::sound_set::tests::wav;

    fn bank() -> Arc<Bank> {
        // a steady tone each, so that a voice's level is easy to read
        let tone = wav(1, 48000, &[16384; 4800]);
        let metal = wav(1, 48000, &[8192; 4800]);
        let click = wav(1, 48000, &[32767; 480]);
        let defaults: Vec<(&str, &[u8])> = vec![
            ("roll.wav", &tone),
            ("roll_metal.wav", &metal),
            ("grind.wav", &tone),
            ("pop.wav", &click),
            ("land.wav", &click),
        ];
        Arc::new(Bank::load(&[], &defaults, &mut |_| {}))
    }

    fn rolling(speed: f32) -> Loops {
        Loops { wheels: 4, speed, ..Default::default() }
    }

    fn peak(output: &[f32]) -> f32 {
        output.iter().fold(0.0f32, |m, s| m.max(s.abs()))
    }

    /// The step from one output sample to the next, the largest: a click
    /// would be a jump.
    fn largest_step(output: &[f32]) -> f32 {
        output.chunks_exact(2).collect::<Vec<_>>().windows(2).map(|w| (w[1][0] - w[0][0]).abs()).fold(0.0, f32::max)
    }

    #[test]
    fn the_roll_follows_the_speed_and_fades_in_without_a_click() {
        let mut m = Mixer::new(bank());
        let now = Instant::now();
        m.set_loops(&rolling(2.0), now);
        let mut slow = vec![0.0; 9600];
        m.mix(&mut slow, 48000, 1.0, now);
        // fading in: no jump at the start
        assert!(largest_step(&slow) < 0.01, "{}", largest_step(&slow));
        m.set_loops(&rolling(8.0), now);
        let mut fast = vec![0.0; 9600];
        m.mix(&mut fast, 48000, 1.0, now);
        assert!(peak(&fast[4800..]) > peak(&slow[4800..]) * 1.5);
        // in the air: silent, faded
        m.set_loops(&Loops { wheels: 0, speed: 8.0, ..Default::default() }, now);
        let mut air = vec![0.0; 48000];
        m.mix(&mut air, 48000, 1.0, now);
        assert!(largest_step(&air) < 0.01);
        assert!(peak(&air[40000..]) < 1e-4);
        assert_eq!(m.voices_of(Sound::Roll), 0, "a silent loop gives its voice up");
    }

    #[test]
    fn another_surface_crossfades_to_its_sample() {
        let mut m = Mixer::new(bank());
        let now = Instant::now();
        m.set_loops(&rolling(6.0), now);
        let mut out = vec![0.0; 9600];
        m.mix(&mut out, 48000, 1.0, now);
        m.surface = Surface::Metal;
        m.set_loops(&rolling(6.0), now);
        assert_eq!(m.voices_of(Sound::Roll), 2, "the old fading out, the new fading in");
        let mut out = vec![0.0; 19200];
        m.mix(&mut out, 48000, 1.0, now);
        assert!(largest_step(&out) < 0.01);
        assert_eq!(m.voices_of(Sound::Roll), 1);
        assert!(m.voices[0].clip.name.contains("roll_metal"));
    }

    #[test]
    fn loops_fade_out_when_the_game_stops_telling_them() {
        let mut m = Mixer::new(bank());
        let then = Instant::now();
        m.set_loops(&rolling(6.0), then);
        let mut out = vec![0.0; 4800];
        m.mix(&mut out, 48000, 1.0, then);
        let mut out = vec![0.0; 48000];
        m.mix(&mut out, 48000, 1.0, then + Duration::from_secs(1));
        assert!(peak(&out[40000..]) < 1e-3);
        assert!(largest_step(&out) < 0.01);
    }

    #[test]
    fn one_shots_play_once_louder_for_harder_landings() {
        let mut m = Mixer::new(bank());
        let now = Instant::now();
        let land = |strength| Event { sound: Sound::Land, strength, speed: 0.0, position: Vec3::ZERO };
        assert!(m.play(&land(0.1)).is_some());
        let mut soft = vec![0.0; 2400];
        m.mix(&mut soft, 48000, 1.0, now);
        assert_eq!(m.voices.len(), 0, "played out");
        assert!(m.play(&land(1.0)).is_some());
        let mut hard = vec![0.0; 2400];
        m.mix(&mut hard, 48000, 1.0, now);
        assert!(peak(&hard) > peak(&soft) * 2.0);
        // a sound without samples is silence
        assert!(m.play(&Event { sound: Sound::Bail, strength: 1.0, speed: 0.0, position: Vec3::ZERO }).is_none());
        // and the volume and the mix's gain scale it all
        m.volume = 0.0;
        m.play(&land(1.0));
        let mut muted = vec![0.0; 2400];
        m.mix(&mut muted, 48000, 1.0, now);
        assert_eq!(peak(&muted), 0.0);
    }

    #[test]
    fn sounds_pan_toward_the_board_and_fade_with_distance() {
        let mut m = Mixer::new(bank());
        m.listener = Listener { position: Vec3::ZERO, right: Vec3::new(0.0, -1.0, 0.0), set: true };
        let (l, r) = m.place(Vec3::new(0.0, -0.2, 0.0));
        assert!(r > l * 2.0, "to the right: {l} {r}");
        let (l, r) = m.place(Vec3::new(0.2, 0.0, 0.0));
        assert!((l - r).abs() < 1e-5, "ahead: centred");
        let (near, _) = m.place(Vec3::new(0.2, 0.0, 0.0));
        let (far, _) = m.place(Vec3::new(4.0, 0.0, 0.0));
        assert!(far < near * 0.2);
    }

    #[test]
    fn voices_never_run_out() {
        let mut m = Mixer::new(bank());
        m.set_loops(&rolling(5.0), Instant::now());
        for _ in 0..100 {
            m.play(&Event { sound: Sound::Pop, strength: 1.0, speed: 0.0, position: Vec3::ZERO });
        }
        assert_eq!(m.voices.len(), VOICES);
        assert_eq!(m.voices_of(Sound::Roll), 1, "a loop keeps its voice");
    }

    #[test]
    fn a_loop_seam_is_smooth() {
        // a ramp, whose ends do not meet: the seam's crossfade joins them
        let ramp: Vec<i16> = (0..4800).map(|i| (i * 6) as i16).collect();
        let wav_bytes = wav(1, 48000, &ramp);
        let defaults: Vec<(&str, &[u8])> = vec![("roll.wav", &wav_bytes)];
        let mut m = Mixer::new(Arc::new(Bank::load(&[], &defaults, &mut |_| {})));
        let now = Instant::now();
        m.set_loops(&rolling(8.0), now);
        let mut out = vec![0.0; 48000];
        m.mix(&mut out, 48000, 1.0, now);
        assert!(largest_step(&out) < 0.02, "{}", largest_step(&out));
    }
}

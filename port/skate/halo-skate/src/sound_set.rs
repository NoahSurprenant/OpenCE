//! The skater's sound set: which samples play for each sound and surface.
//!
//! A sound set is a folder of WAV files named for what they play
//! (`README.md`, "Sound"): `<sound>[_<surface>][-<variant>].wav`, as `roll.wav`,
//! `roll_metal.wav`, `land-2.wav` or `grind_wood-3.wav`. A sound's samples are
//! taken from the first place that has any for it: `HALO_SKATE_SOUNDS`, then
//! `sounds` beside the converted Skate 3 data (`skate-data/sounds`), then the
//! set built into the game (`port/assets/skate-sounds`). So a folder that has
//! `land.wav` replaces every landing sample of the built-in set, and a sound no
//! place has is silent (said once in the log), never an error.
//!
//! Each sound and surface looks for its samples in this order: the surface's
//! own (`grind_metal`), then the sound's (`grind`), then those of the sound it
//! falls back on (`slide_start` falls back on `grind_start`). Of several
//! variants one is picked at random each time.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::sync::Arc;

/// What the board is on, from the level's material under it (skate.c).
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
#[repr(i32)]
pub(crate) enum Surface {
    /// Stone, concrete, asphalt, and anything not listed below.
    Concrete = 0,
    Metal = 1,
    Wood = 2,
    /// Dirt, sand, snow, grass, leaves, water.
    Rough = 3,
}

impl Surface {
    pub(crate) const ALL: [Self; 4] = [Self::Concrete, Self::Metal, Self::Wood, Self::Rough];

    pub(crate) fn name(self) -> &'static str {
        match self {
            Self::Concrete => "concrete",
            Self::Metal => "metal",
            Self::Wood => "wood",
            Self::Rough => "rough",
        }
    }

    pub(crate) fn from_index(index: i32) -> Self {
        Self::ALL.get(usize::try_from(index).unwrap_or(0)).copied().unwrap_or(Self::Concrete)
    }
}

/// Every sound the skater makes. Loops play while their state lasts; the
/// rest play once.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub(crate) enum Sound {
    /// The wheels rolling (loop): louder and higher the faster.
    Roll,
    /// The trucks grinding (loop): 50-50, 5-0, smith and feeble (backslash).
    Grind,
    /// The deck sliding (loop): boardslide, tailslide and noseslide (tipslide),
    /// darkslide.
    Slide,
    /// The wheels sliding sideways (loop).
    Powerslide,
    Pop,
    Land,
    GrindStart,
    GrindEnd,
    SlideStart,
    SlideEnd,
    /// The board's deck or trucks hitting something, or the board falling
    /// on its own.
    BoardImpact,
    Bail,
    /// Stepping off the board, and back on.
    StepOff,
    StepOn,
}

impl Sound {
    pub(crate) const ALL: [Self; 14] = [
        Self::Roll,
        Self::Grind,
        Self::Slide,
        Self::Powerslide,
        Self::Pop,
        Self::Land,
        Self::GrindStart,
        Self::GrindEnd,
        Self::SlideStart,
        Self::SlideEnd,
        Self::BoardImpact,
        Self::Bail,
        Self::StepOff,
        Self::StepOn,
    ];

    pub(crate) fn name(self) -> &'static str {
        match self {
            Self::Roll => "roll",
            Self::Grind => "grind",
            Self::Slide => "slide",
            Self::Powerslide => "powerslide",
            Self::Pop => "pop",
            Self::Land => "land",
            Self::GrindStart => "grind_start",
            Self::GrindEnd => "grind_end",
            Self::SlideStart => "slide_start",
            Self::SlideEnd => "slide_end",
            Self::BoardImpact => "board_impact",
            Self::Bail => "bail",
            Self::StepOff => "step_off",
            Self::StepOn => "step_on",
        }
    }

    pub(crate) fn looped(self) -> bool {
        matches!(self, Self::Roll | Self::Grind | Self::Slide | Self::Powerslide)
    }

    /// The sound whose samples play when this one has none.
    pub(crate) fn fallback(self) -> Option<Self> {
        match self {
            Self::Slide => Some(Self::Grind),
            Self::SlideStart => Some(Self::GrindStart),
            Self::SlideEnd => Some(Self::GrindEnd),
            Self::GrindStart => Some(Self::Land),
            Self::StepOn => Some(Self::BoardImpact),
            Self::StepOff => Some(Self::BoardImpact),
            _ => None,
        }
    }

    fn from_name(name: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|s| s.name() == name)
    }
}

/// A sample: mono, at its own rate.
#[derive(Debug)]
pub(crate) struct Clip {
    pub(crate) samples: Vec<f32>,
    pub(crate) rate: u32,
    /// Where it came from, for the log.
    pub(crate) name: String,
}

/// A file's name parsed: the sound, its surface (None for any) and variant.
pub(crate) fn parse_name(file: &str) -> Option<(Sound, Option<Surface>, u32)> {
    let stem = file.strip_suffix(".wav").or_else(|| file.strip_suffix(".WAV"))?;
    let (stem, variant) = match stem.rsplit_once('-') {
        Some((stem, digits)) => (stem, digits.parse::<u32>().ok().filter(|v| (1..=99).contains(v))?),
        None => (stem, 1),
    };
    if let Some(sound) = Sound::from_name(stem) {
        return Some((sound, None, variant));
    }
    let (sound, surface) = stem.rsplit_once('_')?;
    let surface = Surface::ALL.into_iter().find(|s| s.name() == surface)?;
    Some((Sound::from_name(sound)?, Some(surface), variant))
}

/// A sound set: each sound's samples, by surface (None: any surface).
#[derive(Default)]
pub(crate) struct Bank {
    clips: BTreeMap<(Sound, Option<Surface>), Vec<Arc<Clip>>>,
    /// Where each sound's samples came from, for the log.
    sources: BTreeMap<Sound, String>,
}

/// Each sound's samples in one place: a folder, or the built-in set.
type Found = BTreeMap<Sound, BTreeMap<(Option<Surface>, u32), Arc<Clip>>>;

impl Bank {
    /// The samples of `sound` on `surface`: the surface's own, else the
    /// sound's, else those of the sound it falls back on. Empty when none.
    pub(crate) fn lookup(&self, sound: Sound, surface: Surface) -> &[Arc<Clip>] {
        let mut next = Some(sound);
        while let Some(sound) = next {
            for key in [(sound, Some(surface)), (sound, None)] {
                if let Some(clips) = self.clips.get(&key).filter(|c| !c.is_empty()) {
                    return clips;
                }
            }
            next = sound.fallback();
        }
        &[]
    }

    /// Where `sound`'s own samples came from, or None.
    pub(crate) fn source(&self, sound: Sound) -> Option<&str> {
        self.sources.get(&sound).map(String::as_str)
    }

    /// Makes the set from `folders`, in order, then `defaults` (file names
    /// and their bytes), each sound from the first that has any sample for
    /// it. Unreadable files are skipped, each told of in `log`.
    pub(crate) fn load(folders: &[PathBuf], defaults: &[(&str, &[u8])], log: &mut dyn FnMut(String)) -> Self {
        let mut places: Vec<(String, Found)> = Vec::new();
        for folder in folders {
            let Ok(entries) = std::fs::read_dir(folder) else {
                continue;
            };
            let mut files: Vec<(String, PathBuf)> = entries
                .filter_map(|e| e.ok())
                .filter_map(|e| Some((e.file_name().into_string().ok()?, e.path())))
                .collect();
            files.sort();
            let mut found = Found::new();
            for (name, path) in files {
                if !name.to_ascii_lowercase().ends_with(".wav") {
                    continue;
                }
                let Some((sound, surface, variant)) = parse_name(&name) else {
                    log(format!("halo-skate: sound: {} is not a name the sound set uses, ignored", path.display()));
                    continue;
                };
                match std::fs::read(&path).map_err(|e| e.to_string()).and_then(|b| decode_wav(&b)) {
                    Ok((samples, rate)) => {
                        let clip = Arc::new(Clip { samples, rate, name: path.display().to_string() });
                        found.entry(sound).or_default().insert((surface, variant), clip);
                    }
                    Err(e) => log(format!("halo-skate: sound: {} is not played: {e}", path.display())),
                }
            }
            places.push((folder.display().to_string(), found));
        }
        let mut built_in = Found::new();
        for (name, bytes) in defaults {
            let Some((sound, surface, variant)) = parse_name(name) else {
                continue;
            };
            match decode_wav(bytes) {
                Ok((samples, rate)) => {
                    let clip = Arc::new(Clip { samples, rate, name: format!("built-in {name}") });
                    built_in.entry(sound).or_default().insert((surface, variant), clip);
                }
                Err(e) => log(format!("halo-skate: sound: the built-in {name} is not played: {e}")),
            }
        }
        places.push(("the built-in set".into(), built_in));
        let mut bank = Bank::default();
        for sound in Sound::ALL {
            let Some((place, clips)) = places.iter().find_map(|(place, found)| Some((place, found.get(&sound)?))) else {
                continue;
            };
            for (&(surface, _), clip) in clips {
                bank.clips.entry((sound, surface)).or_default().push(Arc::clone(clip));
            }
            bank.sources.insert(sound, place.clone());
        }
        bank
    }

    /// The folders a set is looked for in, for the converted data in
    /// `assets` (`skate-data/assets`): `HALO_SKATE_SOUNDS`, then
    /// `skate-data/sounds`, then `skate-data/assets/sounds`.
    pub(crate) fn folders(assets: Option<&Path>) -> Vec<PathBuf> {
        let mut folders = Vec::new();
        if let Some(folder) = std::env::var_os("HALO_SKATE_SOUNDS").filter(|f| !f.is_empty()) {
            folders.push(PathBuf::from(folder));
        }
        if let Some(assets) = assets {
            if let Some(parent) = assets.parent() {
                folders.push(parent.join("sounds"));
            }
            folders.push(assets.join("sounds"));
        }
        folders
    }
}

/// Longest sample kept, seconds.
const LONGEST: usize = 30;

/// A RIFF WAVE file's samples, mixed down to mono, and its rate. Takes PCM of
/// 8, 16, 24 or 32 bits and 32-bit float, in one or more channels (also as
/// WAVE_FORMAT_EXTENSIBLE).
pub(crate) fn decode_wav(bytes: &[u8]) -> Result<(Vec<f32>, u32), String> {
    let word = |at: usize| -> Option<u32> { Some(u32::from_le_bytes(bytes.get(at..at + 4)?.try_into().ok()?)) };
    let half = |at: usize| -> Option<u16> { Some(u16::from_le_bytes(bytes.get(at..at + 2)?.try_into().ok()?)) };
    if bytes.get(0..4) != Some(b"RIFF") || bytes.get(8..12) != Some(b"WAVE") {
        return Err("not a RIFF WAVE file".into());
    }
    let mut at = 12;
    let mut format: Option<(u16, u16, u32, u16)> = None;
    let mut data: Option<&[u8]> = None;
    while at + 8 <= bytes.len() {
        let id = &bytes[at..at + 4];
        let size = word(at + 4).ok_or("a short chunk")? as usize;
        let body = at + 8;
        let end = body.saturating_add(size).min(bytes.len());
        match id {
            b"fmt " => {
                let mut tag = half(body).ok_or("a short fmt chunk")?;
                let channels = half(body + 2).ok_or("a short fmt chunk")?;
                let rate = word(body + 4).ok_or("a short fmt chunk")?;
                let bits = half(body + 14).ok_or("a short fmt chunk")?;
                if tag == 0xFFFE {
                    // the sub-format GUID's first two bytes
                    tag = half(body + 24).ok_or("a short extensible fmt chunk")?;
                }
                format = Some((tag, channels, rate, bits));
            }
            b"data" => data = Some(&bytes[body..end]),
            _ => {}
        }
        at = body.saturating_add(size).saturating_add(size & 1);
    }
    let (tag, channels, rate, bits) = format.ok_or("no fmt chunk")?;
    let data = data.ok_or("no data chunk")?;
    if channels == 0 || !(4000..=192_000).contains(&rate) {
        return Err(format!("{channels} channels at {rate} Hz"));
    }
    let width = match (tag, bits) {
        (1, 8) => 1,
        (1, 16) => 2,
        (1, 24) => 3,
        (1, 32) | (3, 32) => 4,
        _ => return Err(format!("format {tag} with {bits} bits (PCM of 8, 16, 24 or 32 bits, or 32-bit float)")),
    };
    let sample = |b: &[u8]| -> f32 {
        match (tag, width) {
            (1, 1) => (f32::from(b[0]) - 128.0) / 128.0,
            (1, 2) => f32::from(i16::from_le_bytes([b[0], b[1]])) / 32768.0,
            (1, 3) => (i32::from_le_bytes([0, b[0], b[1], b[2]]) >> 8) as f32 / 8_388_608.0,
            (1, _) => i32::from_le_bytes([b[0], b[1], b[2], b[3]]) as f32 / 2_147_483_648.0,
            _ => f32::from_le_bytes([b[0], b[1], b[2], b[3]]),
        }
    };
    let frame = width * channels as usize;
    let samples: Vec<f32> = data
        .chunks_exact(frame)
        .take(LONGEST * rate as usize)
        .map(|f| {
            let sum: f32 = f.chunks_exact(width).map(sample).sum();
            let mono = sum / f32::from(channels);
            if mono.is_finite() { mono.clamp(-1.0, 1.0) } else { 0.0 }
        })
        .collect();
    if samples.is_empty() {
        return Err("no samples".into());
    }
    Ok((samples, rate))
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    /// A WAV file of 16-bit PCM.
    pub(crate) fn wav(channels: u16, rate: u32, samples: &[i16]) -> Vec<u8> {
        let data: Vec<u8> = samples.iter().flat_map(|s| s.to_le_bytes()).collect();
        let mut out = Vec::new();
        out.extend_from_slice(b"RIFF");
        out.extend_from_slice(&(36 + data.len() as u32).to_le_bytes());
        out.extend_from_slice(b"WAVEfmt ");
        out.extend_from_slice(&16u32.to_le_bytes());
        out.extend_from_slice(&1u16.to_le_bytes());
        out.extend_from_slice(&channels.to_le_bytes());
        out.extend_from_slice(&rate.to_le_bytes());
        out.extend_from_slice(&(rate * u32::from(channels) * 2).to_le_bytes());
        out.extend_from_slice(&(channels * 2).to_le_bytes());
        out.extend_from_slice(&16u16.to_le_bytes());
        // an odd-sized chunk the reader skips, padded
        out.extend_from_slice(b"LIST");
        out.extend_from_slice(&3u32.to_le_bytes());
        out.extend_from_slice(&[1, 2, 3, 0]);
        out.extend_from_slice(b"data");
        out.extend_from_slice(&(data.len() as u32).to_le_bytes());
        out.extend_from_slice(&data);
        out
    }

    #[test]
    fn names_are_parsed() {
        assert_eq!(parse_name("roll.wav"), Some((Sound::Roll, None, 1)));
        assert_eq!(parse_name("roll_metal.wav"), Some((Sound::Roll, Some(Surface::Metal), 1)));
        assert_eq!(parse_name("land-2.wav"), Some((Sound::Land, None, 2)));
        assert_eq!(parse_name("grind_start_wood-3.wav"), Some((Sound::GrindStart, Some(Surface::Wood), 3)));
        assert_eq!(parse_name("grind_start.wav"), Some((Sound::GrindStart, None, 1)));
        assert_eq!(parse_name("board_impact.WAV"), Some((Sound::BoardImpact, None, 1)));
        assert_eq!(parse_name("roll_lava.wav"), None);
        assert_eq!(parse_name("roll-x.wav"), None);
        assert_eq!(parse_name("roll.ogg"), None);
        assert_eq!(parse_name("music.wav"), None);
    }

    #[test]
    fn wav_files_are_decoded_to_mono() {
        let (samples, rate) = decode_wav(&wav(2, 22050, &[16384, -16384, 32767, 32767])).unwrap();
        assert_eq!(rate, 22050);
        assert_eq!(samples.len(), 2);
        assert!(samples[0].abs() < 1e-6);
        assert!((samples[1] - 1.0).abs() < 1e-3);
        assert!(decode_wav(b"RIFF\0\0\0\0WAVE").is_err());
        assert!(decode_wav(b"not a wav file at all").is_err());
        assert!(decode_wav(&[]).is_err());
        // a truncated file keeps what it has
        let mut short = wav(1, 44100, &[1000, 2000, 3000]);
        short.truncate(short.len() - 2);
        assert_eq!(decode_wav(&short).unwrap().0.len(), 2);
    }

    fn scratch(name: &str) -> PathBuf {
        let folder = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(name);
        let _ = std::fs::remove_dir_all(&folder);
        std::fs::create_dir_all(&folder).unwrap();
        folder
    }

    #[test]
    fn surfaces_and_fallbacks_are_looked_up_in_order() {
        let tone = wav(1, 44100, &[100; 64]);
        let defaults: Vec<(&str, &[u8])> = vec![
            ("roll.wav", &tone),
            ("roll_metal.wav", &tone),
            ("grind.wav", &tone),
            ("land.wav", &tone),
            ("land-2.wav", &tone),
        ];
        let bank = Bank::load(&[], &defaults, &mut |_| {});
        assert_eq!(bank.lookup(Sound::Roll, Surface::Metal)[0].name, "built-in roll_metal.wav");
        assert_eq!(bank.lookup(Sound::Roll, Surface::Wood)[0].name, "built-in roll.wav");
        // slide falls back on grind, grind_start on land (both variants)
        assert_eq!(bank.lookup(Sound::Slide, Surface::Concrete)[0].name, "built-in grind.wav");
        assert_eq!(bank.lookup(Sound::GrindStart, Surface::Concrete).len(), 2);
        // and nothing at all is silence
        assert!(bank.lookup(Sound::Bail, Surface::Concrete).is_empty());
        assert!(bank.lookup(Sound::Powerslide, Surface::Rough).is_empty());
    }

    #[test]
    fn a_folder_replaces_the_built_in_samples_of_each_sound_it_has() {
        let folder = scratch("sound-set-folder");
        std::fs::write(folder.join("roll.wav"), wav(1, 32000, &[5; 32])).unwrap();
        std::fs::write(folder.join("pop-2.wav"), wav(1, 32000, &[5; 32])).unwrap();
        std::fs::write(folder.join("bail.wav"), b"broken").unwrap();
        std::fs::write(folder.join("theme.wav"), wav(1, 32000, &[5; 32])).unwrap();
        std::fs::write(folder.join("notes.txt"), b"ignored quietly").unwrap();
        let tone = wav(1, 44100, &[100; 64]);
        let defaults: Vec<(&str, &[u8])> =
            vec![("roll.wav", &tone), ("roll_metal.wav", &tone), ("pop.wav", &tone), ("bail.wav", &tone)];
        let mut lines = Vec::new();
        let missing = folder.join("missing");
        let bank = Bank::load(&[missing, folder.clone()], &defaults, &mut |line| lines.push(line));
        // the folder's roll, even on metal, where the built-in set has its own
        let roll = bank.lookup(Sound::Roll, Surface::Metal);
        assert_eq!(roll.len(), 1);
        assert_eq!(roll[0].rate, 32000);
        assert_eq!(bank.lookup(Sound::Pop, Surface::Concrete)[0].rate, 32000);
        // a file that is not a WAV leaves the sound to the built-in set
        assert_eq!(bank.lookup(Sound::Bail, Surface::Concrete)[0].name, "built-in bail.wav");
        assert_eq!(bank.source(Sound::Roll), Some(folder.display().to_string().as_str()));
        assert_eq!(bank.source(Sound::Bail), Some("the built-in set"));
        assert_eq!(lines.len(), 2, "{lines:?}");
        assert!(lines.iter().any(|l| l.contains("bail.wav is not played")));
        assert!(lines.iter().any(|l| l.contains("theme.wav is not a name")));
    }

    #[test]
    fn the_folders_follow_the_converted_data() {
        let folders = Bank::folders(Some(Path::new("skate-data/assets")));
        let tail = &folders[folders.len() - 2..];
        assert_eq!(tail, [PathBuf::from("skate-data/sounds"), PathBuf::from("skate-data/assets/sounds")]);
    }
}

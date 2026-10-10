//! The sound set built into the game (`port/assets/skate-sounds`, credited in
//! its CREDITS.txt): each file's name and bytes. A folder of the same names
//! replaces them sound by sound (`sound_set.rs`).

macro_rules! built_in {
    ($($name:literal),* $(,)?) => {
        &[$(($name, include_bytes!(concat!("../../../assets/skate-sounds/", $name)))),*]
    };
}

pub(crate) const DEFAULTS: &[(&str, &[u8])] = built_in![
    "roll.wav",
    "roll_rough.wav",
    "grind.wav",
    "slide.wav",
    "powerslide.wav",
    "pop.wav",
    "land.wav",
    "board_impact.wav",
    "bail.wav",
];

#[cfg(test)]
mod tests {
    use super::*;
    use crate::sound_set::{Bank, Sound, Surface, decode_wav, parse_name};

    /// Every built-in file has a name the set uses and decodes, and every
    /// sound but a few has samples (directly or by its fallback).
    #[test]
    fn the_built_in_set_loads() {
        for (name, bytes) in DEFAULTS {
            assert!(parse_name(name).is_some(), "{name}");
            let (samples, rate) = decode_wav(bytes).unwrap_or_else(|e| panic!("{name}: {e}"));
            assert_eq!(rate, 44100, "{name}");
            assert!(samples.len() > rate as usize / 10, "{name}");
        }
        let mut lines = Vec::new();
        let bank = Bank::load(&[], DEFAULTS, &mut |l| lines.push(l));
        assert!(lines.is_empty(), "{lines:?}");
        let silent: Vec<Sound> =
            Sound::ALL.into_iter().filter(|&s| bank.lookup(s, Surface::Concrete).is_empty()).collect();
        assert_eq!(silent, [Sound::GrindEnd, Sound::SlideEnd]);
        assert!(bank.lookup(Sound::Roll, Surface::Rough)[0].name.contains("roll_rough"));
    }
}

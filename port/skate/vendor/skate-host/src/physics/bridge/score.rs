//! OpenCE: the skater's tricks and score as the engine's own scoring
//! (`scoring_runtime.rs`, Skate 3's ScoreModule) keeps them: a read-only look
//! after a tick, taken by halo-skate's `tricks.rs` (the host's bots learn
//! from it what they landed); nothing here changes the simulation.
use super::Session;

/// The scoring after one simulated tick.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ScoreObservation {
    /// A trick was announced this tick (named as Skate 3's score movie names
    /// it: `trick_name`), or one under way became another (a flip caught
    /// into a grab, a manual into a nose manual); empty otherwise.
    pub new_trick: bool,
    pub modified_trick: bool,
    pub trick_name: String,
    /// A sequence (the tricks from leaving the ground, or starting a grind or
    /// a manual, to rolling away clean) settled this tick: its points, and
    /// whether a bail ended it (it scores nothing then).
    pub published: Option<(f32, bool)>,
    /// The sequence under way's points so far, its combo multiplier in.
    pub sequence_score: f32,
    /// The combo multiplier (1 with no combo going).
    pub multiplier: f32,
}

impl Session {
    pub fn score_observation(&self) -> ScoreObservation {
        let scoring = &self.skater.scoring;
        ScoreObservation {
            new_trick: scoring.new_trick,
            modified_trick: scoring.modified_trick,
            // (named only when one is new: the rest of the time, nothing to copy)
            trick_name: if scoring.new_trick || scoring.modified_trick {
                scoring.trick_name().to_owned()
            } else {
                String::new()
            },
            published: scoring.published,
            sequence_score: scoring.sequence_score(),
            multiplier: scoring.session.combo.multiplier,
        }
    }
}

/// OpenCE: the right stick's gestures of the joystick pattern file `file`
/// (`skater.pat`, `skater_air.pat` ...) of the converted data at `root`, as
/// the engine's recognisers read them (`input/gesture_input.rs`): each
/// pattern's name and its points, in the file's order, in the files' axes
/// (the stick's Y negated: +Y down).
pub fn gesture_patterns(root: &std::path::Path, file: &str) -> Result<Vec<(String, Vec<[f32; 2]>)>, String> {
    let path = root.join("private/stock/data/joystick").join(file);
    Ok(skate_data::gesture_patterns::load(&path)?.into_iter().map(|p| (p.name, p.points)).collect())
}

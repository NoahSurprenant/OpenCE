//! OpenCE: what the skater's sounds are made from. A read-only look at the
//! engine's own state after a tick, taken by halo-skate's `sound.rs` after
//! each simulated tick; nothing here changes the simulation.
use super::Session;
use skate_core::physics::board::BodyId;

/// The engine's state after one simulated tick, as the sounds need it.
/// Positions and velocities are Skate space (Y up, metres).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SoundObservation {
    pub tick: u64,
    /// `PhysicalStateId` as its number (100 PhysicsGround, 400..405 grinds,
    /// 500..502 off the board, ...).
    pub state: u32,
    /// The board's parts touching the world this tick (`BoardGroundState`):
    /// the wheels right front, left front, right back, left back, then the
    /// front and back trucks and the deck.
    pub wheels: [bool; 4],
    pub trucks: [bool; 2],
    pub deck: bool,
    /// The strongest closing speed of a board part against this tick's
    /// contacts, m/s (`BoardGroundState::maximum_closing_speed`): how hard
    /// the board hit what it touches.
    pub closing_speed: f32,
    pub board_position: [f32; 3],
    pub board_velocity: [f32; 3],
    /// The trajectory last launched into the air: a number that changes with
    /// each launch (from where it launched), and whether the skater jumped
    /// (`LaunchInfo::player_jumped`: an ollie, a nollie, a pop off a grind)
    /// rather than rolled off an edge. None before any launch.
    pub launch: Option<(u64, bool)>,
    /// The skater's body is a ragdoll (a bail).
    pub ragdoll: bool,
}

impl Session {
    pub fn sound_observation(&self) -> SoundObservation {
        let ground = &self.physics.riding.ground;
        let deck = self.physics.board.bodies()[BodyId::Deck.index()].rates;
        let launch = self.skater.trajectory.selector.launch_info().map(|info| {
            let p = info.board_position;
            let fingerprint = (u64::from(p[0].to_bits()) << 32 | u64::from(p[1].to_bits()))
                ^ u64::from(p[2].to_bits()).rotate_left(17);
            (fingerprint, info.player_jumped)
        });
        SoundObservation {
            tick: self.physics.ticks,
            state: self.skater.player_state.current() as u32,
            wheels: std::array::from_fn(|i| ground.parts[i].in_contact),
            trucks: [
                ground.parts[BodyId::FrontTruck.index()].in_contact,
                ground.parts[BodyId::BackTruck.index()].in_contact,
            ],
            deck: ground.parts[BodyId::Deck.index()].in_contact,
            closing_speed: if ground.maximum_closing_speed.is_finite() {
                ground.maximum_closing_speed.max(0.0)
            } else {
                0.0
            },
            board_position: [deck.position.x, deck.position.y, deck.position.z],
            board_velocity: [deck.linear_velocity.x, deck.linear_velocity.y, deck.linear_velocity.z],
            launch,
            ragdoll: self.skater.skeleton_collision.is_ragdoll,
        }
    }
}

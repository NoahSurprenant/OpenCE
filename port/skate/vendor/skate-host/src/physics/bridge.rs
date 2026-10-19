use super::{GamePhysics, PlayerControls, SkaterRuntime};
use crate::{camera::CameraRuntime, graph_runtime::StockGraphs, input::ControllerInput};
use bevy::prelude::*;
use skate_data::skate_map::{Collision, Geometry, Rail, SkateMap};
use std::path::Path;

mod sound;
pub use sound::SoundObservation;
/// halo-skate: the host's moving objects the board and skater collide with
/// (`Session::set_obstacles`).
pub use skate_core::{
    math::{Basis3, Vector3},
    physics::kinematic_obstacle::{
        HeldHit, KinematicObstacle, MAXIMUM_MESH_TRIANGLES, MAXIMUM_OBSTACLES, ObstacleKind, ObstacleMesh,
        ObstacleShape,
    },
};

#[derive(Clone, Copy, Default)]
pub struct Controls {
    pub buttons: u16,
    pub triggers: [u8; 2],
    pub left: [i16; 2],
    pub right: [i16; 2],
}
#[derive(Clone)]
pub struct Session {
    physics: GamePhysics,
    skater: SkaterRuntime,
    controls: PlayerControls,
    graphs: StockGraphs,
    input: ControllerInput,
    camera: CameraRuntime,
    markers: crate::session_marker::Runtime,
}
/// halo-skate: what the game has not been told yet of hits on held obstacles
/// (`KinematicObstacle::held`, `Session::take_held`): the fastest contact
/// with one since, whether a bail was held (the engine wanted the skater to
/// fall, and the host decides), and of a bail the host decided
/// (`Session::force_bail`), the tick it was requested on, or the state it
/// was given up in (the skater never rode in time).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct HeldReport {
    pub hit: Option<HeldHit>,
    pub bail_held: bool,
    pub bail_held_tick: u64,
    pub forced: Option<u64>,
    pub dropped: Option<String>,
    /// ... and of a person run over (`KinematicObstacle::run_over`), the
    /// closing speed (m/s) and the tick.
    pub run_over: Option<(f32, u64)>,
}
/// halo-skate: what took a blast's push (`Session::apply_impulse`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Impulse {
    /// The board and the skater riding it.
    Riding,
    /// The skater walking off the board.
    Walking,
    /// The skater's ragdoll, bailing.
    Bailing,
    /// The body alone: off the board in the air, on BipedAir's trajectory.
    Air,
    /// Nothing: the push was not finite.
    Refused,
}
pub struct Pose {
    pub root: Mat4,
    pub bones: Vec<Mat4>,
    pub names: Vec<String>,
    pub camera: Option<(Vec3, Mat3, f32)>,
    pub velocity: Vec3,
    /// halo-skate: the skater is off the board (state category 500:
    /// BipedGround, BipedAir, a moving object), as the camera publication
    /// tells its subject (`off_board`). `velocity` is the board's, always.
    pub off_board: bool,
    /// halo-skate: the skater's own velocity off the board (PhysOut
    /// OffBoard64: the Biped's, or BipedAir's trajectory), metres a second.
    pub skater_velocity: Vec3,
    /// halo-skate: the way the skater faces: the skeleton root's forward,
    /// turned round in the mirrored stance (Processed2476 bit 2), as the
    /// camera's compass takes it (`effective_skeleton_root`).
    pub facing: Vec3,
    pub tick: u64,
    pub state: String,
}
impl Session {
    pub fn new(
        root: &Path,
        triangles: Vec<[[f32; 3]; 3]>,
        rails: Vec<Vec<[f32; 3]>>,
        spawn: [f32; 3],
        heading: f32,
    ) -> Result<Self, String> {
        let started = std::time::Instant::now();
        eprintln!("IW4L_SKATE_LOAD begin");
        skate_data::input_config::StockGameplayConfig::load(root).map_err(|e| e.to_string())?;
        let assets = skate_data::GameAssets::load(root).map_err(|e| e.to_string())?;
        let graphs = StockGraphs::load(root, &assets)?;
        let map = collision_map(triangles, rails, spawn, heading);
        eprintln!("IW4L_SKATE_LOAD graphs {}ms", started.elapsed().as_millis());
        let physics = GamePhysics::load_with_map(root, Some(&map))?;
        eprintln!(
            "IW4L_SKATE_LOAD physics {}ms",
            started.elapsed().as_millis()
        );
        let skater = SkaterRuntime::load(root, &graphs, &physics, "easy")?;
        eprintln!("IW4L_SKATE_LOAD skater {}ms", started.elapsed().as_millis());
        Ok(Self {
            physics,
            skater,
            controls: PlayerControls::load(root)?,
            graphs,
            input: ControllerInput::default(),
            camera: CameraRuntime::load(root)?,
            markers: crate::session_marker::Runtime::load(root)?,
        })
    }
    /// A builder for collision to swap in later, usable on another thread.
    pub fn collision_builder(&self) -> CollisionBuilder {
        CollisionBuilder {
            material: self.physics.floor_material(),
        }
    }
    /// Swaps in collision built by `collision_builder`: the world the skater
    /// rides, climbs and grinds from the next tick.
    pub fn install_collision(&mut self, prepared: PreparedCollision) -> Result<(), String> {
        self.physics
            .install_world(prepared.world, std::sync::Arc::clone(&prepared.grind))?;
        self.skater.trajectory.bind_grind_world(prepared.grind);
        Ok(())
    }
    /// halo-skate: the obstacles the board and the skater collide with from
    /// the next tick (`kinematic_obstacle.rs`), where they are now and how
    /// they move; they carry on at their velocities until the next call, and
    /// are dropped half a second after it. Those that are not valid are left
    /// out, and no more than MAXIMUM_OBSTACLES are taken. Running into a
    /// person or vehicle faster than `bail_speed` (m/s) is a bail
    /// (`kinematic_obstacle::knocks_over`). Returns how many were taken.
    pub fn set_obstacles(&mut self, obstacles: &[KinematicObstacle], bail_speed: f32) -> usize {
        self.physics.obstacles.clear();
        self.physics.obstacles.extend(
            obstacles
                .iter()
                .filter_map(|o| o.clone().validated())
                .take(MAXIMUM_OBSTACLES),
        );
        self.physics.obstacle_age = 0.0;
        // (and to the off-board controller, which sweeps triangles: where
        // they are now, a tick's carry at most behind)
        let triangles = self.physics.obstacles.iter().flat_map(|o| o.triangles_at(0.0)).collect();
        self.physics.world.set_offboard_obstacles(triangles);
        if bail_speed.is_finite() && bail_speed >= 0.0 {
            self.physics.obstacle_bail_speed = bail_speed;
        }
        self.physics.obstacles.len()
    }
    /// halo-skate: how many contacts the last tick's solve had with the
    /// obstacles, and the obstacle the fastest of them closed on with its
    /// closing speed (metres a second), if any.
    pub fn obstacle_contacts(&self) -> (usize, Option<(ObstacleKind, f32)>) {
        (self.physics.obstacle_contacts, self.physics.obstacle_hit)
    }
    /// halo-skate: what there is to tell of hits on held obstacles since the
    /// last call (`HeldReport`), and none after.
    pub fn take_held(&mut self) -> HeldReport {
        std::mem::take(&mut self.physics.held)
    }
    /// halo-skate: a bail the host decided (`closing`, metres a second, its
    /// hit's speed): requested on the first tick the skater rides, as running
    /// into a person that fast is, through the same wipeout requests, whether
    /// the engine wanted it or not; given up after half a second of not riding
    /// (bailing already, walking, on a rail).
    pub fn force_bail(&mut self, closing: f32) {
        let closing = if closing.is_finite() { closing.max(0.0) } else { 0.0 };
        self.physics.forced_bail = Some((closing, self.physics.ticks + super::wipeout::FORCE_TICKS));
    }
    /// halo-skate: the skater's physical state, by name (`PhysicsGround`,
    /// `GrindFiftyFifty`, `BipedAir`, ...), as `Pose::state` has it.
    pub fn state_name(&self) -> String {
        format!("{:?}", self.skater.player_state.current())
    }
    pub fn period(&self) -> f32 {
        self.physics.period().as_secs_f32()
    }
    pub fn set_aspect_ratio(&mut self, aspect_ratio: f32) {
        if aspect_ratio.is_finite() && aspect_ratio > 0. {
            self.camera.set_aspect_ratio(aspect_ratio);
        }
    }
    /// Eagerly decode immutable animation banks before a map is ready.
    pub fn preload(root: &Path) -> Result<(), String> {
        crate::skater_animation::AnimationSource::load(root).map(|_| ())
    }
    /// Reuse the complete world and animation session. The original teleport path
    /// resets physical bodies and animation state at the new MW2 position.
    pub fn activate(&mut self, spawn: [f32; 3], heading: f32) -> Result<Pose, String> {
        self.input = ControllerInput::default();
        // (halo-skate: a hit held or a bail forced before is no longer this
        // skater's)
        self.physics.held = HeldReport::default();
        self.physics.held_contact_tick = None;
        self.physics.forced_bail = None;
        self.physics.run_through.clear();
        self.physics.run_over_slowing = None;
        self.markers.suspend();
        if self.physics.ticks == 0 {
            self.tick(Controls::default())?;
        }
        let mut transform = Mat4::from_rotation_translation(
            Quat::from_rotation_y(heading),
            Vec3::from_array(spawn),
        )
        .to_cols_array_2d();
        transform[3][3] = 0.;
        self.skater.travel_to(transform)?;
        for _ in 0..4 {
            self.tick(Controls::default())?;
        }
        self.input = ControllerInput::default();
        Ok(self.pose())
    }
    /// halo-skate: a teleporter: the skater put on the board at `spawn` facing
    /// `heading`, as `activate` does, and then moving at `velocity` (metres a
    /// second, the board's and the body's alike), so that they leave the
    /// exit as fast as they came in.
    pub fn teleport(&mut self, spawn: [f32; 3], heading: f32, velocity: [f32; 3]) -> Result<Pose, String> {
        self.activate(spawn, heading)?;
        let v = skate_core::math::Vector3::new(velocity[0], velocity[1], velocity[2]);
        for body in self.physics.board.bodies_mut() {
            body.rates.linear_velocity = v;
        }
        for body in self.skater.skeleton.bodies_mut() {
            body.rates.linear_velocity = v;
        }
        self.skater.animated_skeleton.motion.velocity_world = [v.x, v.y, v.z, 0.];
        self.skater.player_input.processed.vectors_544_560_592_608[3] = [v.x, v.y, v.z, 0.].map(f32::to_bits);
        Ok(self.pose())
    }
    /// halo-skate: a blast's push, as Halo's moves a player on foot: `velocity`
    /// (metres a second) added to the skater's. Riding, to the board's and
    /// the riding body's alike (as `teleport` sets them), and faster than the
    /// obstacle bail speed it knocks them over on the next tick (`wipeout.rs`);
    /// walking, to the off-board controller's movement, which its own
    /// acceleration then brings back to the stick's; bailing, to the ragdoll.
    /// In the air off the board (BipedAir's trajectory, worked out at the
    /// jump) only the body takes it. Returns what took it.
    pub fn apply_impulse(&mut self, velocity: [f32; 3]) -> Impulse {
        use skate_core::player::state::PhysicalStateId;
        let [x, y, z] = velocity;
        if !(x.is_finite() && y.is_finite() && z.is_finite()) {
            return Impulse::Refused;
        }
        let add = |v: &mut skate_core::math::Vector3| *v = skate_core::math::Vector3::new(v.x + x, v.y + y, v.z + z);
        for body in self.skater.skeleton.bodies_mut() {
            add(&mut body.rates.linear_velocity);
        }
        let state = self.skater.player_state.current();
        if state == PhysicalStateId::BipedGround {
            let motion = &mut self.skater.biped_ground.controller.state.motion;
            for (axis, push) in velocity.into_iter().enumerate() {
                motion.velocity_480[axis] += push;
            }
            let m = motion.velocity_480;
            motion.speed_704 = (m[0] * m[0] + m[1] * m[1] + m[2] * m[2]).sqrt();
            return Impulse::Walking;
        }
        if self.skater.player_input.physical.state.category_12 == 500 {
            return Impulse::Air;
        }
        if format!("{state:?}").starts_with("Wipeout") {
            return Impulse::Bailing;
        }
        for body in self.physics.board.bodies_mut() {
            add(&mut body.rates.linear_velocity);
        }
        let w = &mut self.skater.animated_skeleton.motion.velocity_world;
        *w = [w[0] + x, w[1] + y, w[2] + z, w[3]];
        let processed = &mut self.skater.player_input.processed.vectors_544_560_592_608[3];
        let pushed = [x, y, z, 0.];
        *processed = std::array::from_fn(|i| (f32::from_bits(processed[i]) + pushed[i]).to_bits());
        self.physics.blast_speed = self.physics.blast_speed.max((x * x + y * y + z * z).sqrt());
        Impulse::Riding
    }
    /// halo-skate: running over people on foot (`KinematicObstacle::run_over`):
    /// a contact closing at `speed` (m/s) or faster goes through them, the
    /// board and the skater keeping `keep` (0 to 1) of their speed; a speed
    /// not finite or below 0 runs nobody over. Returns the speed kept.
    pub fn set_run_over(&mut self, speed: f32, keep: f32) -> f32 {
        self.physics.run_over_speed = if speed.is_finite() && speed >= 0.0 { speed } else { f32::INFINITY };
        self.physics.run_over_keep = if keep.is_finite() { keep.clamp(0.0, 1.0) } else { 1.0 };
        self.physics.run_over_speed
    }
    /// halo-skate: the riding board and skater slowed to `keep` of their
    /// speed (a person run over), as `teleport` sets their velocity: the
    /// board's and the body's alike, and the motion the animation reads.
    /// Nothing off the board, or bailing.
    fn slow(&mut self, keep: f32) {
        if self.skater.player_input.physical.state.category_12 == 500
            || format!("{:?}", self.skater.player_state.current()).starts_with("Wipeout")
        {
            return;
        }
        let scale = |v: &mut skate_core::math::Vector3| {
            *v = skate_core::math::Vector3::new(v.x * keep, v.y * keep, v.z * keep)
        };
        for body in self.physics.board.bodies_mut() {
            scale(&mut body.rates.linear_velocity);
        }
        for body in self.skater.skeleton.bodies_mut() {
            scale(&mut body.rates.linear_velocity);
        }
        let w = &mut self.skater.animated_skeleton.motion.velocity_world;
        *w = [w[0] * keep, w[1] * keep, w[2] * keep, w[3]];
        let processed = &mut self.skater.player_input.processed.vectors_544_560_592_608[3];
        *processed = std::array::from_fn(|i| {
            if i < 3 { (f32::from_bits(processed[i]) * keep).to_bits() } else { processed[i] }
        });
    }
    pub fn collect(&mut self, frame: InputFrame, dt: f32) {
        self.input.collect(frame.samples);
        self.markers.collect_time(f64::from(dt));
    }
    pub fn suspend_input(&mut self) {
        self.input = ControllerInput::default();
        self.markers.suspend();
    }
    pub fn advance(&mut self) -> Result<(), String> {
        self.input.publish_actions();
        self.advance_published()
    }
    fn advance_published(&mut self) -> Result<(), String> {
        let published = self.input.tick_input();
        self.markers
            .advance(&self.input, &self.physics, &mut self.skater);
        let mut actions = published.actions();
        self.controls.update_for_physics(
            &mut actions,
            &self.physics,
            &self.skater,
            &self.camera,
        )?;
        self.controls.publish_gestures(
            self.physics.animation_profile.physics_mode,
            self.skater.player_input.physical.state.state_16,
        );
        let advanced = super::frame::advance(
            &mut self.physics,
            &mut self.skater,
            &mut self.controls,
            &self.graphs,
            &mut actions,
            published.controller_available(),
            &mut self.camera,
        );
        // (a blast's push knocks over on the tick after it, once)
        self.physics.blast_speed = 0.0;
        // (a person run over slows the board and the skater, once)
        if let Some(keep) = self.physics.run_over_slowing.take() {
            self.slow(keep);
        }
        advanced
    }
    /// Deterministic raw-packet entry point for playback/diagnostics.
    pub fn tick(&mut self, input: Controls) -> Result<(), String> {
        crate::input::sample(
            &mut self.input,
            skate_core::input::xbox::XboxState {
                buttons: input.buttons,
                triggers: input.triggers,
                left: input.left,
                right: input.right,
            },
        );
        self.advance_published()
    }
    pub fn pose(&self) -> Pose {
        let v = self.physics.board.bodies()[skate_core::physics::board::BodyId::Deck.index()]
            .rates
            .linear_velocity;
        Pose {
            root: crate::animation::native_matrix(
                self.skater.animated_skeleton.roots.animation_to_world,
            ),
            bones: self
                .skater
                .render_pose
                .iter()
                .map(|m| crate::animation::native_matrix(*m))
                .collect(),
            names: self.skater.animation.evaluator.frames.bone_names.clone(),
            camera: self.camera.frame.as_ref().map(|f| {
                (
                    Vec3::new(f.position[0], f.position[1], f.position[2]),
                    Mat3::from_cols_array_2d(&f.basis.columns),
                    f.field_of_view_degrees,
                )
            }),
            velocity: Vec3::new(v.x, v.y, v.z),
            off_board: self.skater.player_input.physical.state.category_12 == 500,
            skater_velocity: {
                let v = self.skater.player_input.physical.off_board.vector_64.map(f32::from_bits);
                Vec3::new(v[0], v[1], v[2])
            },
            facing: {
                let forward = self.skater.animated_skeleton.roots.animation_to_world[2];
                let forward = Vec3::new(forward[0], forward[1], forward[2]);
                if self.skater.player_input.processed.flags_2476 & 4 != 0 { -forward } else { forward }
            },
            tick: self.physics.ticks,
            state: format!("{:?}", self.skater.player_state.current()),
        }
    }
}

/// Collision for `Session::install_collision`, built off the simulation.
pub struct PreparedCollision {
    world: skate_core::physics::board_world::BoardWorld,
    grind: std::sync::Arc<crate::grind_world::StaticProvider>,
}

#[derive(Clone, Copy)]
pub struct CollisionBuilder {
    material: skate_core::physics::contact::RetailContactMaterial,
}

impl CollisionBuilder {
    pub fn build(
        &self,
        triangles: Vec<[[f32; 3]; 3]>,
        rails: Vec<Vec<[f32; 3]>>,
    ) -> Result<PreparedCollision, String> {
        let map = collision_map(triangles, rails, [0.; 3], 0.);
        Ok(PreparedCollision {
            world: crate::skate_world::collision_world(&map, self.material)?,
            grind: std::sync::Arc::new(crate::grind_world::StaticProvider::new(Some(&map))?),
        })
    }
}

/// IW4L's collision as a Skate map: one material, the triangles and rails.
fn collision_map(
    triangles: Vec<[[f32; 3]; 3]>,
    rails: Vec<Vec<[f32; 3]>>,
    spawn: [f32; 3],
    heading: f32,
) -> SkateMap {
    SkateMap {
            version: 14,
            name: "IW4L collision".into(),
            spawn,
            heading,
            environment: vec![],
            materials: vec![skate_data::skate_map::Material {
                name: "MW2".into(),
                flags: 0,
                friction: 0.8,
                restitution: 0.,
                color: [1.; 3],
                roughness: 1.,
                emissive: 0.,
                textures: [0; 5],
                indirect_strength: 1.,
                alpha_mode: 0,
                alpha_cutoff: 0.5,
                audio: 0,
                physics: 0,
                pattern: 0,
                depth_layer: None,
                retail_definition: None,
            }],
            textures: vec![],
            geometry: Geometry {
                vertices: vec![],
                indices: vec![],
                collision: triangles
                    .into_iter()
                    .map(|points| Collision {
                        points,
                        surface: 0,
                        material: 1,
                        native_edges: None,
                    })
                    .collect(),
            },
            rails: rails
                .into_iter()
                .enumerate()
                .map(|(i, p)| Rail {
                    name: format!("iw4_edge_{i}"),
                    closed: false,
                    points: p,
                    native: None,
                })
                .collect(),
            doors: vec![],
            lights: vec![],
            routes: vec![],
            extensions: vec![],
    }
}

/// The source engine's raw XInput transport. No Bevy deadzones, button remaps,
/// trigger reconstruction or rounding are inserted ahead of its native Pad.
#[derive(Default)]
pub struct ControllerTransport {
    capabilities: [crate::input::platform::CapabilityCache; 4],
}
pub struct InputFrame {
    samples: [Result<crate::input::platform::DevicePacket, crate::input::platform::DeviceError>; 4],
}
impl ControllerTransport {
    pub fn poll(&mut self) -> InputFrame {
        InputFrame {
            samples: std::array::from_fn(|i| {
                crate::input::platform::poll_cached(i, &mut self.capabilities[i])
            }),
        }
    }
}
impl InputFrame {
    pub fn neutral() -> Self {
        Self {
            samples: std::array::from_fn(|_|Err(crate::input::platform::DeviceError::Disconnected)),
        }
    }
    /// One controller in the first slot, already in XInput's layout:
    /// button bits, trigger bytes and signed stick axes.
    pub fn from_pad(buttons: u16, triggers: [u8; 2], left: [i16; 2], right: [i16; 2], packet: u32) -> Self {
        let mut frame = Self::neutral();
        frame.samples[0] = Ok(crate::input::platform::DevicePacket {
            number: packet,
            state: skate_core::input::xbox::XboxState { buttons, triggers, left, right },
            subtype: 1,
        });
        frame
    }
    pub fn controller(&self) -> Option<usize> {
        self.samples.iter().position(Result::is_ok)
    }
    pub fn buttons(&self) -> u16 {
        self.samples
            .iter()
            .find_map(|s| s.as_ref().ok().map(|s| s.state.buttons))
            .unwrap_or(0)
    }
}

use super::state::{IDENTITY, UP, ZERO};
use super::*;
fn metrics() -> [Option<ClipMetric>; 3] {
    [
        Some(ClipMetric {
            translation_z: -2.0,
            end_time: 1.0,
        }),
        Some(ClipMetric {
            translation_z: -4.0,
            end_time: 1.0,
        }),
        Some(ClipMetric {
            translation_z: -8.0,
            end_time: 1.0,
        }),
    ]
}
fn graph<const N: usize>(value: f32) -> PointGraph<N> {
    PointGraph {
        x: std::array::from_fn(|i| i as f32),
        y: [value; N],
    }
}
fn settings() -> Settings {
    Settings {
        movement_intent: movement_intent::Settings {
            sprint_speed: graph(2.0),
            normal_speed: graph(1.0),
            sprint_blend: graph(0.0),
            sprint_time_cap: 1.0,
            slide_steering: graph(0.0),
        },
        movement_velocity: movement_velocity::Settings {
            slope_speed_scalar: graph(1.0),
            slope_mode_speed: graph(1.0),
            turn_vs_speed: graph(0.0),
            turn_delta_vs_speed: graph(1.0),
        },
        slide_vs_slope: graph(0.0),
        slide_vs_speed: graph(0.0),
    }
}
fn job() -> GroundJob {
    GroundJob {
        contact_position: ZERO,
        contact_normal: UP,
        support_frame: IDENTITY,
        target_position: ZERO,
        target_normal: UP,
        edge_position: ZERO,
        edge_normal: UP,
        flags: 1,
        support_id: 0,
        collision_displacements: [ZERO; 2],
        animation_motion: [0.0, 0.0, 1.0, 0.0],
        animation_velocity: ZERO,
        desired_direction: [0.0, 0.0, 1.0, 0.0],
        animation_position: ZERO,
        requested_duration: 1.0,
        mirrored: false,
        requested_phase: -1.0,
        override_duration: 1.0,
        animation_directed: false,
        movement: 1.0,
        steering: 0.0,
        sprint_pressed: false,
        suppress_lean: false,
        suppress_minimum: false,
        target_frame_present: false,
        edge_active: false,
        target_frame: IDENTITY,
        ignore_obstacle: false,
    }
}
#[test]
fn constructor_uses_clip_metrics_and_original_initial_fields() {
    let s = State::new(metrics());
    assert_eq!(
        s.thresholds.0,
        [0.01, 3.0, 6.0, f32::from_bits(0x5015_02f9)]
    );
    assert_eq!(s.motion.frame_0, IDENTITY);
    assert_eq!(s.surface.surface_normal, UP);
    assert_eq!(s.velocity_override_remaining_772, -1.0);
    assert_eq!(s.motion.target_scale_784, -1.0);
    assert_eq!(s.cadence.phase.duration, None);
}

#[test]
fn tiny_velocity_delta_keeps_ground_publication_finite() {
    let mut controller = Controller::new(settings(), metrics());
    controller.state.motion.velocity_480 = [0.0, 1.0e-21, 1.0, 0.0];
    controller.state.motion.speed_704 = 1.0;
    let result = controller.step_ground(&job());
    assert!(result.position.into_iter().all(f32::is_finite));
    assert!(result.velocity.into_iter().all(f32::is_finite));
    assert!(result.physical_frame.into_iter().flatten().all(f32::is_finite));
    assert!(result.animation_frame.into_iter().flatten().all(f32::is_finite));
    assert!(controller.state.surface.lean.into_iter().all(f32::is_finite));
    assert!(controller.state.cadence.phase.phase.is_finite());
}
#[test]
fn absent_clip_queries_have_original_zero_speed() {
    let s = State::new([None; 3]);
    assert_eq!(s.thresholds.0[1], 0.0);
    assert_eq!(s.thresholds.0[2], 0.0);
}
#[test]
fn reset_preserves_phase_and_correction_vectors_only() {
    let mut s = State::new(metrics());
    s.cadence.phase.phase = 0.75;
    s.cadence.phase.duration = Some(2.0);
    s.motion.correction_576 = [1.0; 4];
    s.correction_target_592 = [2.0; 4];
    s.motion.correction_enabled_711 = true;
    s.motion.velocity_480 = [3.0; 4];
    s.intent.speed = 9.0;
    s.reset();
    assert_eq!(s.cadence.phase.phase, 0.75);
    assert_eq!(s.cadence.phase.duration, Some(2.0));
    assert_eq!(s.motion.correction_576, [1.0; 4]);
    assert_eq!(s.correction_target_592, [2.0; 4]);
    assert!(!s.motion.correction_enabled_711);
    assert_eq!(s.motion.velocity_480, ZERO);
    assert_eq!(s.intent.speed, 0.0);
}
fn placement(current: u32, previous: u32) -> PlacementInput {
    PlacementInput {
        frame: IDENTITY,
        velocity: [3.0, 0.0, 4.0, 0.0],
        body_position: [1.0, 2.0, 3.0, 0.0],
        current_state: current,
        previous_state: previous,
        previous_frame: IDENTITY,
    }
}
#[test]
fn landing_preserves_eligible_correction_from_air_target() {
    let mut s = State::new(metrics());
    s.correction_target_592 = [0.0, 0.0, 2.0, 0.0];
    s.place(placement(500, 501));
    assert!(s.motion.correction_enabled_711);
    assert_eq!(s.motion.correction_576, [0.0, 0.0, -2.0, 0.0]);
    assert_eq!(s.position_368, [1.0, 2.0, 3.0, 0.0]);
    assert_eq!(s.motion.speed_704, 5.0);
}
#[test]
fn air_placement_clears_correction_without_resetting_movement_timers() {
    let mut s = State::new(metrics());
    s.intent.sprint_time = 4.0;
    s.correction_target_592 = [1.0; 4];
    s.motion.correction_enabled_711 = true;
    s.place(placement(501, 500));
    assert_eq!(s.intent.sprint_time, 4.0);
    assert_eq!(s.correction_target_592, ZERO);
    assert!(!s.motion.correction_enabled_711);
}
#[test]
fn other_placement_keeps_existing_correction() {
    let mut s = State::new(metrics());
    s.motion.correction_576 = [1.0; 4];
    s.motion.correction_enabled_711 = true;
    s.place(placement(600, 500));
    assert_eq!(s.motion.correction_576, [1.0; 4]);
    assert!(s.motion.correction_enabled_711);
}
#[test]
fn full_normal_dispatch_moves_canonical_velocity_and_publishes() {
    let mut c = Controller::new(settings(), metrics());
    let result = c.step_ground(&job());
    assert!(!result.alternate);
    assert!(c.state.motion.velocity_480[2] > 0.0);
    assert!(result.physical_frame[3][2] > 0.0);
    assert_eq!(result.velocity, c.state.frame_output.velocity);
    assert_eq!(result.position, c.state.position_368);
}
#[test]
fn alternate_dispatch_skips_cadence_and_position_but_exports() {
    let mut c = Controller::new(settings(), metrics());
    let mut j = job();
    j.flags = 2;
    j.target_position = [0.0, 0.0, 1.0, 0.0];
    c.state.motion.velocity_480 = [0.0, 5.0, 0.0, 0.0];
    c.state.frame_output.velocity = [0.0, 5.0, 0.0, 0.0];
    c.state.position_368 = [9.0; 4];
    c.state.cadence.phase.phase = 0.75;
    let r = c.step_ground(&j);
    assert!(r.alternate);
    assert_eq!(c.state.position_368, [9.0; 4]);
    assert_eq!(c.state.cadence.phase.phase, 0.75);
    assert!(c.state.motion.velocity_480[1] < 5.0);
}
#[test]
fn exporter_degenerate_frame_is_identity() {
    assert_eq!(build_frame(UP, UP), IDENTITY);
    let f = build_frame([0.0, 2.0, 0.0, 0.0], [0.0, 0.0, 3.0, 0.0]);
    for axis in 0..3 {
        for lane in 0..3 {
            assert!((f[axis][lane] - IDENTITY[axis][lane]).abs() < 1e-6);
        }
    }
    assert_eq!(f[3], ZERO);
}

// halo-skate: Build 28 on hangemhigh. Off the board (BipedGround), X and the
// left stick: "Nonfinite BipedAir launch packet: velocity [4.836101,
// 4.1468215, 0.25886396, 0.0], secondary [0.9707678, 0.0, -0.24002063, NaN],
// position [24.839907, -23.282875, 36.295986, NaN], up [0.009803514,
// 0.99915934, 0.03980608, 0.0], forward [0.97345155, 0.0, -0.22889303, 0.0]".
// Only the w lanes were NaN: the entry frame came from the skeleton with its
// native fourth lanes (not 0), and the support velocity grew frame0's w by the
// golden ratio a tick, to infinity in about 180 ticks, then NaN, which reached
// the packet through the frame output (its up, and the frame that places the
// packet's position).
const LOG_UP: Vector = [0.009803514, 0.99915934, 0.03980608, 0.0];
const LOG_FORWARD: Vector = [0.97345155, 0.0, -0.22889303, 0.0];
const LOG_POSITION: Vector = [24.839907, -23.282875, 36.295986, 0.0];

/// The entry frame as the skeleton hands it: geometric x, y and z, and the
/// fourth lanes the native permutes leave (physics_bone_frame keeps them).
fn native_entry_frame() -> Frame {
    let [ux, uy, uz, _] = LOG_UP;
    let [fx, fy, fz, _] = LOG_FORWARD;
    let right = [uy * fz - uz * fy, uz * fx - ux * fz, ux * fy - uy * fx];
    let n = (right[0] * right[0] + right[1] * right[1] + right[2] * right[2]).sqrt();
    let right = right.map(|v| v / n);
    [
        [right[0], right[1], right[2], fz],
        [ux, uy, uz, right[0]],
        [fx, fy, fz, uy],
        [LOG_POSITION[0], LOG_POSITION[1], LOG_POSITION[2], 1.0],
    ]
}

fn finite(name: &str, tick: usize, v: Vector) {
    assert!(v.iter().all(|x| x.is_finite()), "{name} at tick {tick}: {v:?}");
}

/// Stand on one support for `ticks` after getting off the board.
fn walk_off(ticks: usize) -> (Controller, GroundResult) {
    let mut c = Controller::new(settings(), metrics());
    let mut entry = placement(500, 500);
    entry.frame = native_entry_frame();
    entry.velocity = [-0.02, 0.0, -0.08, 1.0];
    entry.body_position = [LOG_POSITION[0], LOG_POSITION[1] + 0.9, LOG_POSITION[2], 1.0];
    c.place(entry);
    let mut j = job();
    j.movement = 0.0;
    j.support_id = 7;
    let mut result = None;
    for tick in 0..ticks {
        let at = c.state.motion.frame_0[3];
        j.contact_position = [at[0], at[1], at[2], 0.0];
        j.target_position = j.contact_position;
        j.animation_position = j.contact_position;
        let r = c.step_ground(&j);
        for (k, row) in r.animation_frame.iter().enumerate() {
            finite(&format!("animation frame row {k}"), tick, *row);
        }
        finite("frame0 position", tick, c.state.motion.frame_0[3]);
        finite("velocity", tick, r.velocity);
        result = Some(r);
    }
    (c, result.unwrap())
}

#[test]
fn standing_off_the_board_keeps_the_frame_w_lanes_zero() {
    // three times the 180 ticks that took w to infinity
    let (c, r) = walk_off(600);
    for row in r.animation_frame.iter().chain(r.physical_frame.iter()) {
        assert_eq!(row[3], 0.0, "{:?}", r.animation_frame);
    }
    assert_eq!(c.state.motion.frame_0[3][3], 0.0);
    assert_eq!(c.state.motion.support_velocity_256[3], 0.0);
    assert_eq!(r.velocity[3], 0.0);
}

#[test]
fn a_jump_from_walking_makes_a_finite_launch_packet() {
    use super::super::{air_launch, air_selector};
    let (c, r) = walk_off(600);
    // ground_sync's prepare_air: mode 4 (category 500, the jump flag), then the
    // packet's position is the animation frame times the skeleton's point.
    let processed = air_launch::Processed {
        board_position_112: [8.118, -11.901, -7.950, 0.0],
        forward_224: r.animation_frame[2],
        up_544: LOG_UP,
        position_592: LOG_POSITION,
        velocity_608: [-0.02, 0.0, -0.08, 0.0],
        velocity_912: r.velocity,
        departure_geometry: None,
        flags_2472: 0,
        flags_2476: 0x80000,
        flags_2480: 0,
        previous_state_2504: 500,
        current_state_2508: 500,
        current_category_2512: 500,
        previous_category_2516: 500,
        // the left stick, [23652, 30746] of 32767
        raw_x_2692: 0.72,
        raw_z_2688: 0.94,
    };
    let mut packet = air_launch::Packet::initialized(0.0);
    let jump = air_launch::Settings { jump_speed_scalar: 0.8, jump_height: 0.9 };
    air_launch::produce(
        &mut packet,
        &c.state,
        &c.settings.movement_velocity.turn_vs_speed,
        jump,
        &processed,
        false,
    )
    .unwrap();
    let frame = r.animation_frame;
    let point = [0.0, 0.95, 0.1, 1.0];
    packet.position_32 = std::array::from_fn(|i| {
        frame[2][i].mul_add(point[2], frame[1][i].mul_add(point[1], frame[0][i].mul_add(point[0], frame[3][i])))
    });
    assert_eq!((packet.kind_108, packet.kind_112), (6, 3));
    for v in [packet.velocity_0, packet.secondary_velocity_16, packet.position_32, packet.up_48, packet.forward_64] {
        finite("launch packet", 600, v);
    }
    assert_eq!(packet.secondary_velocity_16[3], 0.0);
    assert_eq!(packet.position_32[3], 0.0);
    // and the selector that refused it takes it: six launch candidates and
    // three along the secondary direction
    let mut selector = air_selector::Selector::default();
    let requests = selector
        .begin_launch(
            packet,
            [0.0, -9.8, 0.0, 0.0],
            air_selector::Settings { height: 0.9, sphere_radius: 0.3, start_index: 3 },
        )
        .unwrap();
    assert_eq!(requests.len(), 9);
}

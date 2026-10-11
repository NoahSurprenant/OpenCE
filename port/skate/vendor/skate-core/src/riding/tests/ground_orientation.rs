use super::*;
fn constant(value: f32) -> PointGraph<8> {
    PointGraph { x: [0.0,1.0,2.0,3.0,4.0,5.0,6.0,7.0], y: [value;8] }
}
fn settings() -> GroundOrientationSettings {
    GroundOrientationSettings {
        ground_normal_smoothing: [1.0,0.0,0.0,1.0],
        up_vector_smoothing_slow: [1.0,0.0,0.0,1.0],
        up_vector_smoothing_fast: [1.0,0.0,0.0,1.0],
        dynamic_up_vs_ground_y: constant(1.0), ground_vector_blend: constant(0.0),
        deck_angle_usage_vs_speed: constant(0.0), up_vector_smoothing_vs_speed: constant(0.0),
        up_vector_max_delta_vs_speed: constant(1.0), ground_blend_max_delta:0.1,
        up_vector_max_acceleration:2.0, anti_wobble_damping:0.5, extra_side_damping:0.6,
        minimum_wheels_for_ground_blend:0,
    }
}
fn input(normal: Vector3) -> GroundOrientationInput {
    GroundOrientationInput { com_to_deck: UP, ground_normal:normal,dynamic_up:normal,
        speed:0.0,wheel_contact_count:4,animation_balance:0.0,deck_angle_curve_input:0.0,
        board_up:UP,board_forward:Vector3::new(0.0,0.0,1.0),
        effective_board_forward:Vector3::new(0.0,0.0,1.0),
        previous_reckoning_right:Vector3::new(1.0,0.0,0.0),prevent_up_behind_board:false }
}
#[test]
fn stationary_filter_startup_has_no_false_tilt_or_acceleration() {
    let s=settings();let mut state=GroundOrientation::new(&s);
    for _ in 0..120 {state.update(&s,input(UP));}
    assert_eq!(state.up,UP);assert_eq!(state.ground_normal,UP);
    assert_eq!(state.up_velocity,Vector3::ZERO);
}
#[test]
fn changed_contact_normal_does_not_instantly_replace_the_skater_up() {
    let s=settings();let mut state=GroundOrientation::new(&s);
    let wall=Vector3::new(1.0,0.0,0.0);state.update(&s,input(wall));
    assert_eq!(state.ground_normal,wall);
    assert_ne!(state.up,state.ground_normal);
    assert!(state.up.y>0.99 && state.up.x>0.0 && state.up.x<0.04);
    assert_eq!(&state.slow_filter.words()[4..8],&lanes(state.up).map(f32::to_bits));
}

// halo-skate: on Hang 'Em High the skater's up and up velocity had decayed to
// within 1e-20 of the floor's normal (a filtered normal a hair off the axis,
// standing still), and the next update's up came out NaN: the tiny
// difference refined to NaN (native_arithmetic). The state and input as
// logged, its filters settled on that normal.
#[test]
fn an_up_within_a_hair_of_the_ground_normal_stays_finite() {
    let s = settings();
    let mut state = GroundOrientation::new(&s);
    let near_up = Vector3::new(-5.3292727e-21, 1.0, 1.841287e-20);
    state.up = near_up;
    state.target = near_up;
    state.ground_normal = near_up;
    state.up_velocity = Vector3::new(-7.6350136e-21, 0.0, 2.6356823e-20);
    let mut logged = input(UP);
    logged.ground_normal = near_up;
    logged.com_to_deck = Vector3::new(-0.029552555, 0.857896, 0.03783522);
    logged.dynamic_up = Vector3::new(-0.07502986, 0.9970856, -0.013820447);
    logged.speed = 0.09738713;
    logged.board_up = Vector3::new(0.0004357875, 0.9999889, -0.0046860888);
    logged.board_forward = Vector3::new(-0.9982508, 0.00015799282, -0.05911818);
    logged.effective_board_forward = logged.board_forward;
    logged.previous_reckoning_right = Vector3::new(-0.07084398, -1.8744153e-20, 0.9974874);
    for _ in 0..600 {
        state.update(&s, logged);
        for v in [state.up, state.up_velocity, state.target, state.ground_normal] {
            assert!(v.x.is_finite() && v.y.is_finite() && v.z.is_finite(), "{state:?}");
        }
    }
    assert!(state.up.y > 0.99, "{:?}", state.up);
}

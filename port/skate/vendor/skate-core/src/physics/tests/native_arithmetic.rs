use super::*;

#[test]
fn host_math_normalizes_a_direction_without_using_its_homogeneous_lane() {
    let direction = [3.0, 4.0, 0.0, 100.0];
    let inverse_length = reciprocal_square_root_estimate(dot3(direction, direction));
    let unit = direction.map(|v| v * inverse_length);
    assert!((dot3(unit, unit) - 1.0).abs() < 1.0e-6);
    assert_eq!(dot3(direction, [4.0, -3.0, 0.0, 50.0]), 0.0);
    assert_eq!(dot4(direction, [0.0, 0.0, 0.0, 1.0]), 100.0);
}

#[test]
fn host_math_handles_scalar_geometry_and_ordinary_float_boundaries() {
    assert_eq!(reciprocal_estimate(4.0), 0.25);
    assert_eq!(reciprocal_square_root_estimate(16.0), 0.25);
    assert_eq!(reciprocal_estimate(-0.0), f32::NEG_INFINITY);
    assert_eq!(reciprocal_square_root_estimate(f32::INFINITY), 0.0);
    assert!(reciprocal_square_root_estimate(-1.0).is_nan());
    assert_eq!(vector_min(-2.0, 3.0), -2.0);
    assert_eq!(vector_max(-2.0, 3.0), 3.0);
}

// halo-skate: a vector whose squared length is subnormal has length 0 (the
// Xbox 360's vector unit treats denormals as zero): Hang 'Em High's ground
// normal decayed to (-1e-20, 1, 3.7e-20), and the refined reciprocal square
// root of its tiny up velocity's squared length overflowed to NaN.
#[test]
fn a_subnormal_squared_length_refines_to_zero_not_nan() {
    let tiny = [-7.6350136e-21_f32, 0.0, 2.6356823e-20, 0.0];
    let squared = dot3(tiny, tiny);
    assert!(squared.is_subnormal() && squared > 0.0);
    let mut inverse = reciprocal_square_root_estimate(squared);
    for _ in 0..2 {
        let correction = (-squared).mul_add(inverse * inverse, 1.0);
        inverse = (inverse * 0.5).mul_add(correction, inverse);
    }
    assert_eq!(inverse, 0.0);
    assert_eq!(squared * inverse, 0.0);
    // the smallest normal squared length still refines to its true value
    let smallest = f32::MIN_POSITIVE;
    let mut inverse = reciprocal_square_root_estimate(smallest);
    for _ in 0..2 {
        let correction = (-smallest).mul_add(inverse * inverse, 1.0);
        inverse = (inverse * 0.5).mul_add(correction, inverse);
    }
    assert!(inverse.is_finite() && (inverse * smallest.sqrt() - 1.0).abs() < 1e-6);
}

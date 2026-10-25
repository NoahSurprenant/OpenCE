//! Refinement sequence used by S3 82D861D8/82D88518/82D886B8.
//! Seeds use existing independent PC math, not a Xenon bit-exact claim.
use crate::fused::FusedMulAdd as _;
use crate::physics::native_arithmetic;

pub(super) fn reciprocal(value: f32) -> f32 {
    let mut result = native_arithmetic::reciprocal_estimate(value);
    for _ in 0..2 {
        let error = (-result).fused_mul_add(value, 1.0);
        result = result.fused_mul_add(error, result);
    }
    result
}

pub(super) fn inverse_square_root(value: f32) -> f32 {
    let mut result = native_arithmetic::reciprocal_square_root_estimate(value);
    for _ in 0..2 {
        let square = result * result;
        let half = result * 0.5;
        let error = (-value).fused_mul_add(square, 1.0);
        result = half.fused_mul_add(error, result);
    }
    result
}

pub(super) fn square_root(value: f32) -> f32 {
    let result = value * inverse_square_root(value);
    // The original vector select explicitly chooses zero for a zero radicand.
    if value == 0.0 { 0.0 } else { result }
}

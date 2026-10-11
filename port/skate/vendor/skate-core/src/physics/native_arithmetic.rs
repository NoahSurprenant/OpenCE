//! Independent host math for the operations used by recovered TU3 callers.
//!
//! Uses Rust floating-point operations without external estimate tables or
//! translator-specific special cases. Callers retain their recovered refinement
//! steps. This is working PC math, not bit-exact Xenon instruction emulation.

pub(crate) fn reciprocal_estimate(value: f32) -> f32 {
    value.recip()
}

pub(crate) fn reciprocal_square_root_estimate(value: f32) -> f32 {
    // halo-skate: a squared length that is subnormal (below 1.2e-38: a vector
    // shorter than about 1.1e-19) is a zero length, as on the Xbox 360, whose
    // vector unit treats denormals as zero. Its true reciprocal square root
    // (above 9.2e18) is finite, but the callers' Newton refinements square it,
    // which overflows to infinity, and the refined value is then -inf or NaN
    // where their `squared == 0.0` guards (true there under that
    // denormals-are-zero mode) do not catch it here. On a real level a
    // filtered vector decays toward an axis it nearly lies on: Hang 'Em High's
    // floor, a ground normal of (-1e-20, 1, 3.7e-20) whose up velocity's
    // length came out NaN (ground_orientation.rs), then the board's heading
    // torque, and the step failed (`Non-finite torque_acceleration before
    // shared solve`). Zero refines to zero: such a vector has length 0, and
    // normalizes to zero or the caller's fallback.
    if value.is_subnormal() {
        return 0.0;
    }
    value.sqrt().recip()
}

pub(crate) fn dot3(left: [f32; 4], right: [f32; 4]) -> f32 {
    left[0] * right[0] + left[1] * right[1] + left[2] * right[2]
}

pub(crate) fn dot4(left: [f32; 4], right: [f32; 4]) -> f32 {
    dot3(left, right) + left[3] * right[3]
}

pub(crate) fn vector_min(left: f32, right: f32) -> f32 {
    left.min(right)
}

pub(crate) fn vector_max(left: f32, right: f32) -> f32 {
    left.max(right)
}

#[cfg(test)]
#[path = "tests/native_arithmetic.rs"]
mod tests;

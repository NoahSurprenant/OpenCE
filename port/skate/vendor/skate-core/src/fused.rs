//! A fused multiply-add (`a * b + c` rounded once, as `mul_add` is), inline.
//!
//! `f32::mul_add` compiles to a call of the C library's `fmaf` wherever the
//! target lacks the FMA instruction - the game's 32-bit x86 build, a
//! portable x86-64 one, WebAssembly - and the engine makes more than a
//! hundred thousand of them a step: the calls were most of a skater's step.
//! `fused_mul_add` gives the same result (correctly rounded, bit for bit)
//! without a call: the FMA instruction where the build has it, else the
//! product and sum in `f64` (the product of two `f32`s is exact there) with
//! the sum rounded to odd (Boldo and Melquiond: a sum rounded to odd in at
//! least 2 + 24 bits, then rounded to `f32`, is the correctly rounded sum).

/// `mul_add` without a call.
pub trait FusedMulAdd {
    fn fused_mul_add(self, a: Self, b: Self) -> Self;
}

impl FusedMulAdd for f32 {
    #[inline(always)]
    fn fused_mul_add(self, a: f32, b: f32) -> f32 {
        fused_f32(self, a, b)
    }
}

impl FusedMulAdd for f64 {
    #[inline(always)]
    fn fused_mul_add(self, a: f64, b: f64) -> f64 {
        self.mul_add(a, b)
    }
}

/// `x * y + z`, rounded once.
#[cfg(target_feature = "fma")]
#[inline(always)]
pub fn fused_f32(x: f32, y: f32, z: f32) -> f32 {
    x.mul_add(y, z)
}

/// `x * y + z`, rounded once.
#[cfg(not(target_feature = "fma"))]
#[inline(always)]
pub fn fused_f32(x: f32, y: f32, z: f32) -> f32 {
    let p = f64::from(x) * f64::from(y);
    let s = p + f64::from(z);
    // Rounding s to f32 rounds the exact sum correctly unless s fell
    // exactly on a midpoint between two f32s (its low 29 bits 1 then 0s):
    // s is on the exact sum's side of every other midpoint, which f64
    // holds exactly. (A result too small for an f32's 24 bits, rare, goes
    // the careful way too; a zero is exact.)
    if (s.to_bits() as u32) & 0x1FFF_FFFF != 0x1000_0000 && (s.abs() >= f64::from(f32::MIN_POSITIVE) || s == 0.0) {
        return s as f32;
    }
    fused_f32_careful(p, f64::from(z), s)
}

/// `p + z` (p exact) rounded to f32 once, s their sum rounded to f64: the
/// sum rounded to odd, then to f32.
#[cfg(not(target_feature = "fma"))]
#[inline(never)]
fn fused_f32_careful(p: f64, z: f64, s: f64) -> f32 {
    // the sum's rounding error, exactly (Knuth's two-sum)
    let zz = s - p;
    let error = (p - (s - zz)) + (z - zz);
    // (inexact, and rounded to an even last bit: the neighbour toward the
    // exact sum instead, which is odd - the sum rounded to odd)
    if error != 0.0 && s.is_finite() && s.to_bits() & 1 == 0 {
        let away = (error > 0.0) == (s > 0.0);
        let bits = s.to_bits();
        return f64::from_bits(if away { bits + 1 } else { bits - 1 }) as f32;
    }
    s as f32
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A few million operands, random and chosen near the hard cases
    /// (products that cancel, sums at a rounding tie): the same bits as the
    /// library's `fmaf`.
    #[test]
    fn same_as_mul_add() {
        let mut state = 0x9E37_79B9_7F4A_7C15u64;
        let mut next = move || {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            state
        };
        let mut checked = 0;
        for i in 0..3_000_000u32 {
            let r = next();
            let x = f32::from_bits(r as u32);
            let y = f32::from_bits((r >> 32) as u32);
            let z = match i % 4 {
                0 => f32::from_bits(next() as u32),
                // (cancelling: z near -x*y)
                1 => -(x * y),
                2 => -(x * y) * (1.0 + f32::EPSILON),
                // (a small z beside a large product: a tie in the low bits)
                _ => (x * y) * f32::EPSILON / 2.0,
            };
            let want = x.mul_add(y, z);
            let got = fused_f32(x, y, z);
            assert!(
                want.to_bits() == got.to_bits() || (want.is_nan() && got.is_nan()),
                "{x:e} * {y:e} + {z:e}: {want:e} ({:#x}) but {got:e} ({:#x})",
                want.to_bits(),
                got.to_bits()
            );
            checked += 1;
        }
        // small, ordinary operands too (the engine's)
        for i in 0..1_000_000u32 {
            let r = next();
            let x = (r as u32 as f32 / u32::MAX as f32 - 0.5) * 20.0;
            let y = ((r >> 32) as u32 as f32 / u32::MAX as f32 - 0.5) * 20.0;
            let z = if i % 2 == 0 { -(x * y) } else { (next() as u32 as f32 / u32::MAX as f32 - 0.5) * 400.0 };
            assert_eq!(x.mul_add(y, z).to_bits(), fused_f32(x, y, z).to_bits(), "{x} * {y} + {z}");
            checked += 1;
        }
        assert_eq!(checked, 4_000_000);
    }
}

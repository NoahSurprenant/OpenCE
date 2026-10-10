//! The heading the game's following camera keeps behind a skater
//! (`halo_skate_follow_heading`, port/linux/game/skate.c). It follows the way
//! the skater travels rather than the way the body faces: off a wall, Skate 3
//! sends the skater back rolling fakie, still facing the wall.

use std::f32::consts::{PI, TAU};

/// An angle in 0 (inclusive) to 2 pi (exclusive), as the player's desired
/// yaw must be (player_control.c's `valid_euler_angles2d`).
pub fn wrap(angle: f32) -> f32 {
    if !angle.is_finite() {
        return 0.0;
    }
    let wrapped = angle.rem_euclid(TAU);
    // (rem_euclid of a tiny negative angle rounds up to 2 pi itself)
    if wrapped >= TAU { 0.0 } else { wrapped }
}

/// The heading turned `fraction` (0 to 1) of the way toward the horizontal
/// velocity's heading, the shorter way round, when the skater moves faster
/// than `minimum_speed` along the ground; else the heading as it was, so
/// that the camera neither spins at a standstill nor wobbles at a crawl.
/// Always within 0 to 2 pi.
pub fn follow(heading: f32, velocity: [f32; 2], minimum_speed: f32, fraction: f32) -> f32 {
    let heading = wrap(heading);
    let [x, y] = velocity;
    let speed_squared = x * x + y * y;
    if !(speed_squared.is_finite() && speed_squared > minimum_speed.max(0.0).powi(2) && speed_squared > 0.0) {
        return heading;
    }
    let target = y.atan2(x);
    // the turn toward it, in -pi to pi (a reversal exactly behind turns left)
    let mut turn = (target - heading).rem_euclid(TAU);
    if turn > PI {
        turn -= TAU;
    }
    let fraction = if fraction.is_finite() { fraction.clamp(0.0, 1.0) } else { 1.0 };
    wrap(heading + turn * fraction)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn close(a: f32, b: f32) {
        assert!((a - b).abs() < 1e-4, "{a} != {b}");
    }

    fn in_range(a: f32) {
        assert!((0.0..TAU).contains(&a), "{a} is not in 0..2pi");
    }

    #[test]
    fn wraps_into_range() {
        close(wrap(-0.5), TAU - 0.5);
        close(wrap(TAU + 0.25), 0.25);
        assert_eq!(wrap(TAU), 0.0);
        assert_eq!(wrap(-1e-9), 0.0);
        assert_eq!(wrap(f32::NAN), 0.0);
        for i in -100..100 {
            in_range(wrap(i as f32 * 0.37));
        }
    }

    #[test]
    fn holds_when_slow() {
        // standing still, or crawling backwards, keeps the heading
        close(follow(1.0, [0.0, 0.0], 0.33, 0.2), 1.0);
        close(follow(1.0, [-0.2, 0.0], 0.33, 0.2), 1.0);
        close(follow(1.0, [f32::NAN, 0.0], 0.33, 0.2), 1.0);
    }

    #[test]
    fn turns_toward_travel() {
        // heading +X, travelling +Y: a fifth of the quarter turn
        close(follow(0.0, [0.0, 2.0], 0.33, 0.2), PI / 2.0 * 0.2);
        // all the way with a fraction of 1
        close(follow(0.0, [0.0, 2.0], 0.33, 1.0), PI / 2.0);
    }

    #[test]
    fn turns_the_short_way_across_zero() {
        // heading just above 0, travelling just below it (-0.2 rad): turns
        // down through 0 and comes out near 2 pi, not round the long way
        let target = -0.2f32;
        let next = follow(0.1, [target.cos() * 3.0, target.sin() * 3.0], 0.33, 0.5);
        close(next, TAU - 0.05);
        in_range(next);
        // and back up from near 2 pi past 0
        let next = follow(TAU - 0.1, [1.0, 0.2], 0.33, 1.0);
        close(next, 0.2f32.atan2(1.0));
        in_range(next);
    }

    #[test]
    fn a_reversal_turns_over_a_few_ticks() {
        // rolling back off a wall: the velocity now points behind the camera
        let mut heading = 0.0;
        let mut ticks = 0;
        while (heading - PI).abs() > 0.1 {
            heading = follow(heading, [-3.0, 0.0], 0.33, 0.2);
            in_range(heading);
            ticks += 1;
            assert!(ticks < 100);
        }
        // at 30 ticks a second, a few tenths of a second
        assert!((8..=20).contains(&ticks), "{ticks} ticks");
    }

    #[test]
    fn stays_in_range_from_anywhere() {
        for h in -20..40 {
            for v in 0..16 {
                let angle = v as f32 * TAU / 16.0;
                in_range(follow(h as f32 * 0.4, [angle.cos(), angle.sin()], 0.33, 0.18));
            }
        }
    }
}

//! The heading the game's following camera keeps behind a skater
//! (`halo_skate_follow`, port/linux/game/skate.c).
//!
//! On the board it follows the way the board travels rather than the way the
//! body faces: off a wall, Skate 3 sends the skater back rolling fakie, still
//! facing the wall. Off the board (walking, or jumping off it, the board
//! thrown or left lying) it follows the skater instead, as Skate 3's own
//! camera does (its compass takes the skeleton root for its subject off the
//! board): the way the skater walks, and, stood still a while, round behind
//! the way it faces. The heading only ever turns part of the way a tick, the
//! shorter way round, so getting off and back on turns it, never snaps it.

use std::f32::consts::{PI, TAU};

/// Off the board and slower than the minimum speed this long (seconds), the
/// camera comes round behind the way the skater faces (Skate 3's compass
/// lines up after its `time_before_lineup`).
pub const LINEUP_DELAY: f32 = 1.0;
/// The part of the way to the skater's facing it turns each tick when lining
/// up: slower than following, about two seconds to come round.
pub const LINEUP_TURN: f32 = 0.05;

/// What the camera follows this tick, along the ground (Halo's X and Y).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Subject {
    /// The skater is off the board (the engine's state category 500).
    pub off_board: bool,
    /// The board's velocity, world units a second.
    pub board_velocity: [f32; 2],
    /// The skater's own velocity off the board, world units a second.
    pub skater_velocity: [f32; 2],
    /// The way the skater faces (any length).
    pub facing: [f32; 2],
}

/// The camera's heading and how long the skater has stood still off the
/// board.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct Follow {
    /// Radians, 0 to 2 pi.
    pub heading: f32,
    /// Seconds off the board slower than the minimum speed.
    pub still: f32,
}

impl Follow {
    /// The next heading, `dt` seconds on: toward the way the board travels
    /// on it, the way the skater walks off it, each `fraction` of the way a
    /// tick when faster than `minimum_speed`; off it and slower for
    /// `LINEUP_DELAY`, toward the way the skater faces, `LINEUP_TURN` of the
    /// way; else held.
    pub fn step(&mut self, subject: &Subject, minimum_speed: f32, fraction: f32, dt: f32) -> f32 {
        if !subject.off_board {
            self.still = 0.0;
            self.heading = follow(self.heading, subject.board_velocity, minimum_speed, fraction);
            return self.heading;
        }
        if faster_than(subject.skater_velocity, minimum_speed) {
            self.still = 0.0;
            self.heading = follow(self.heading, subject.skater_velocity, minimum_speed, fraction);
            return self.heading;
        }
        let dt = if dt.is_finite() { dt.max(0.0) } else { 0.0 };
        self.still = if self.still.is_finite() { (self.still + dt).min(LINEUP_DELAY) } else { 0.0 };
        self.heading = if self.still >= LINEUP_DELAY {
            follow(self.heading, subject.facing, 0.0, LINEUP_TURN)
        } else {
            wrap(self.heading)
        };
        self.heading
    }
}

fn faster_than(velocity: [f32; 2], minimum_speed: f32) -> bool {
    let [x, y] = velocity;
    let speed_squared = x * x + y * y;
    speed_squared.is_finite() && speed_squared > minimum_speed.max(0.0).powi(2) && speed_squared > 0.0
}

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
    if !faster_than(velocity, minimum_speed) {
        return heading;
    }
    let [x, y] = velocity;
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

    const DT: f32 = 1.0 / 30.0;
    const SPEED: f32 = 0.33;
    const TURN: f32 = 0.2;

    fn riding(board: [f32; 2]) -> Subject {
        Subject { off_board: false, board_velocity: board, skater_velocity: [9.0, 9.0], facing: [0.0, -1.0] }
    }

    fn walking(skater: [f32; 2], board: [f32; 2], facing: [f32; 2]) -> Subject {
        Subject { off_board: true, board_velocity: board, skater_velocity: skater, facing }
    }

    /// The turn from one heading to the next, -pi to pi.
    fn turn(from: f32, to: f32) -> f32 {
        let t = (to - from).rem_euclid(TAU);
        if t > PI { t - TAU } else { t }
    }

    #[test]
    fn on_the_board_it_follows_the_board_as_before() {
        // the same as follow(): the skater's velocity and facing are not read
        let mut f = Follow { heading: 0.0, still: 0.5 };
        let next = f.step(&riding([0.0, 2.0]), SPEED, TURN, DT);
        close(next, follow(0.0, [0.0, 2.0], SPEED, TURN));
        assert_eq!(f.still, 0.0);
        // off a wall, rolling back fakie: round to the travel in a few ticks
        let mut ticks = 0;
        while (f.heading - PI / 2.0).abs() > 0.05 {
            f.step(&riding([0.0, 3.0]), SPEED, TURN, DT);
            ticks += 1;
            assert!(ticks < 40);
        }
        // slow: held, however long
        let held = f.heading;
        for _ in 0..200 {
            f.step(&riding([0.01, 0.0]), SPEED, TURN, DT);
        }
        close(f.heading, held);
    }

    #[test]
    fn off_the_board_it_follows_the_skater_not_the_thrown_board() {
        // heading +X; the skater walks +Y while the thrown board flies -X
        let mut f = Follow { heading: 0.0, still: 0.0 };
        for _ in 0..60 {
            f.step(&walking([0.0, 1.0], [-5.0, 0.0], [0.0, 1.0]), SPEED, TURN, DT);
            in_range(f.heading);
        }
        close(f.heading, PI / 2.0);
        // the board lying still and the skater walking -Y: round to -Y
        for _ in 0..60 {
            f.step(&walking([0.0, -1.0], [0.0, 0.0], [0.0, -1.0]), SPEED, TURN, DT);
        }
        close(f.heading, 3.0 * PI / 2.0);
    }

    #[test]
    fn stood_still_off_the_board_it_lines_up_behind_the_facing_slowly() {
        let mut f = Follow { heading: 0.0, still: 0.0 };
        let facing = [0.0, 1.0];
        // held for the delay
        let ticks = (LINEUP_DELAY / DT) as usize;
        for _ in 0..ticks - 1 {
            f.step(&walking([0.0, 0.0], [0.0, 0.0], facing), SPEED, TURN, DT);
            assert_eq!(f.heading, 0.0);
        }
        // then round, a little a tick
        let mut previous = f.heading;
        for _ in 0..200 {
            f.step(&walking([0.0, 0.0], [0.0, 0.0], facing), SPEED, TURN, DT);
            let step = turn(previous, f.heading);
            assert!((0.0..=PI / 2.0 * LINEUP_TURN + 1e-4).contains(&step), "{step}");
            previous = f.heading;
        }
        close(f.heading, PI / 2.0);
        // walking again: following at once, the stillness forgotten
        f.step(&walking([2.0, 0.0], [0.0, 0.0], facing), SPEED, TURN, DT);
        assert_eq!(f.still, 0.0);
    }

    #[test]
    fn getting_off_and_back_on_turns_without_a_snap_or_a_spin() {
        // ride +X, get off and walk back the other way (-X), get back on and
        // ride +Y: no tick turns more than the fraction of a half turn, and
        // each turn is the short way
        let mut f = Follow { heading: 0.0, still: 0.0 };
        let mut subjects = Vec::new();
        subjects.extend(std::iter::repeat_n(riding([5.0, 0.0]), 30));
        subjects.extend(std::iter::repeat_n(walking([0.0, 0.0], [4.0, 0.0], [1.0, 0.0]), 10));
        subjects.extend(std::iter::repeat_n(walking([-1.0, 0.1], [0.0, 0.0], [-1.0, 0.0]), 60));
        subjects.extend(std::iter::repeat_n(riding([0.0, 4.0]), 60));
        let mut previous = f.heading;
        let mut total = 0.0;
        for s in &subjects {
            f.step(s, SPEED, TURN, DT);
            in_range(f.heading);
            let step = turn(previous, f.heading);
            assert!(step.abs() <= PI * TURN + 1e-4, "a snap of {step}");
            total += step.abs();
            previous = f.heading;
        }
        close(f.heading, PI / 2.0);
        // round to -X (a half turn) and then to +Y (a quarter): never a spin
        assert!(total < PI * 1.5 + 0.1, "turned {total} in all");
    }

    #[test]
    fn bad_input_holds_the_heading() {
        let mut f = Follow { heading: 1.0, still: f32::NAN };
        f.step(&walking([f32::NAN, 0.0], [0.0, 0.0], [f32::NAN, 1.0]), SPEED, TURN, f32::NAN);
        in_range(f.heading);
        assert!(f.still.is_finite());
        f.step(&riding([f32::INFINITY, 0.0]), SPEED, TURN, DT);
        close(f.heading, 1.0);
    }
}

//! Putting the skater back on the board when a step of the engine fails.
//!
//! The engine checks some of its numbers as it goes and gives up a step on
//! one that is not finite (a bail's launch packet, `Nonfinite BipedAir launch
//! packet`). The session itself survives that: `Session::activate`, the way
//! every press of J gets on, resets the skater's bodies and animation from any
//! state. So the worker puts the skater back on the board where the last good
//! pose had it, facing the same way, rather than dropping the session and
//! making it again with the map (the best part of 8 seconds). A spot that
//! keeps failing gives up after a few tries, and the session is made again as
//! before.

use bevy_math::{Mat4, Vec3};

/// Most recoveries in a row, each within `WINDOW` steps of the last, before
/// the worker gives up on the session.
pub(crate) const MAXIMUM: u32 = 3;
/// Good steps (game ticks, 30 a second) after which earlier recoveries no
/// longer count: 4 seconds.
pub(crate) const WINDOW: u32 = 120;

/// Counts recent recoveries.
#[derive(Default)]
pub(crate) struct Recovery {
    recent: u32,
    steps_since: u32,
}

impl Recovery {
    /// A step went well.
    pub(crate) fn stepped(&mut self) {
        self.steps_since = self.steps_since.saturating_add(1);
        if self.steps_since >= WINDOW {
            self.recent = 0;
        }
    }

    /// The skater got on the board afresh (J): nothing before counts.
    pub(crate) fn reset(&mut self) {
        *self = Self::default();
    }

    /// A step failed: whether to put the skater back on the board (counted),
    /// or give up on the session.
    pub(crate) fn allow(&mut self) -> bool {
        if self.recent >= MAXIMUM {
            return false;
        }
        self.recent += 1;
        self.steps_since = 0;
        true
    }

    /// Recoveries counted toward `MAXIMUM`.
    pub(crate) fn recent(&self) -> u32 {
        self.recent
    }
}

/// Where to put the skater back on the board from a pose's root (Skate space,
/// Y up): its position, and the heading `Session::activate` takes (a turn
/// about +Y; at 0 the board faces +Z). None when the root is not finite. A
/// root facing straight up or down keeps the heading of its right side.
pub(crate) fn respawn(root: &Mat4) -> Option<([f32; 3], f32)> {
    if !root.is_finite() {
        return None;
    }
    let position = root.w_axis.truncate();
    let flat = |v: Vec3| Vec3::new(v.x, 0.0, v.z);
    let forward = flat(root.z_axis.truncate());
    let heading = if forward.length_squared() > 1e-6 {
        forward.x.atan2(forward.z)
    } else {
        // +X turned by the heading is (cos, 0, -sin)
        let right = flat(root.x_axis.truncate());
        if right.length_squared() > 1e-6 {
            (-right.z).atan2(right.x)
        } else {
            0.0
        }
    };
    Some((position.to_array(), heading))
}

#[cfg(test)]
mod tests {
    use super::*;
    use bevy_math::Quat;

    fn near(a: f32, b: f32) -> bool {
        (a - b).abs() < 1e-4
    }

    #[test]
    fn respawn_keeps_the_position_and_heading() {
        for heading in [0.0f32, 0.5, 1.5, 3.0, -2.0, -0.1] {
            let position = Vec3::new(3.0, -1.5, 40.0);
            let root = Mat4::from_rotation_translation(Quat::from_rotation_y(heading), position);
            let (spawn, found) = respawn(&root).unwrap();
            assert_eq!(spawn, position.to_array());
            assert!(near(found.sin(), heading.sin()) && near(found.cos(), heading.cos()), "{heading} {found}");
        }
    }

    #[test]
    fn respawn_of_a_tilted_root_keeps_its_heading_across_the_ground() {
        let heading = 1.0f32;
        let rotation = Quat::from_rotation_y(heading) * Quat::from_rotation_x(0.6) * Quat::from_rotation_z(0.3);
        let root = Mat4::from_rotation_translation(rotation, Vec3::ZERO);
        let (_, found) = respawn(&root).unwrap();
        let forward = rotation * Vec3::Z;
        assert!(near(found, forward.x.atan2(forward.z)));
    }

    #[test]
    fn respawn_of_a_root_facing_straight_up_takes_its_right_side() {
        let heading = 0.7f32;
        let rotation = Quat::from_rotation_y(heading) * Quat::from_rotation_x(-std::f32::consts::FRAC_PI_2);
        let root = Mat4::from_rotation_translation(rotation, Vec3::ONE);
        let (_, found) = respawn(&root).unwrap();
        assert!(near(found, heading), "{found}");
    }

    #[test]
    fn respawn_of_a_nonfinite_root_is_none() {
        let mut root = Mat4::IDENTITY;
        root.w_axis.x = f32::NAN;
        assert!(respawn(&root).is_none());
        root = Mat4::IDENTITY;
        root.z_axis.z = f32::INFINITY;
        assert!(respawn(&root).is_none());
    }

    #[test]
    fn recovery_gives_up_on_a_spot_that_keeps_failing() {
        let mut recovery = Recovery::default();
        for _ in 0..MAXIMUM {
            assert!(recovery.allow());
            for _ in 0..WINDOW - 1 {
                recovery.stepped();
            }
        }
        assert!(!recovery.allow());
        // and stays given up until the skater gets on again
        assert!(!recovery.allow());
        recovery.reset();
        assert!(recovery.allow());
    }

    #[test]
    fn recovery_forgets_failures_after_a_while_of_good_steps() {
        let mut recovery = Recovery::default();
        for _ in 0..10 {
            for _ in 0..MAXIMUM {
                assert!(recovery.allow());
            }
            assert_eq!(recovery.recent(), MAXIMUM);
            for _ in 0..WINDOW {
                recovery.stepped();
            }
            assert_eq!(recovery.recent(), 0);
        }
    }
}

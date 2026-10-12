//! The game's moving objects the board and the skater collide with: other
//! bipeds as capsules, vehicles and scenery as boxes
//! (`halo_skate_set_obstacles`; the engine's side is skate-core's
//! `kinematic_obstacle.rs`). Converted here from Halo's space (Z up, world
//! units) to Skate's (Y up, metres) as everything else is (`to_skate`).

use crate::{METRES, to_skate};
use bevy_math::Vec3;
use skate_host::bridge::{Basis3, KinematicObstacle, ObstacleKind, ObstacleShape, Vector3};

/// `halo_skate_obstacle` (include/halo_skate.h).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct HaloSkateObstacle {
    /// 0 a biped, 1 a vehicle, 2 scenery (anything else solid)
    pub kind: i32,
    /// 0 a box, 1 a capsule along `up`
    pub shape: i32,
    /// its middle, world units
    pub center: [f32; 3],
    /// a box's axes, unit vectors (a capsule's axis is `up`)
    pub forward: [f32; 3],
    pub left: [f32; 3],
    pub up: [f32; 3],
    /// a box's half sizes along forward, left and up; a capsule's radius,
    /// then 0, then half its height (caps included), world units
    pub half_extents: [f32; 3],
    /// world units a second
    pub velocity: [f32; 3],
}

pub const KIND_BIPED: i32 = 0;
pub const KIND_VEHICLE: i32 = 1;
pub const KIND_SCENERY: i32 = 2;
pub const SHAPE_BOX: i32 = 0;
pub const SHAPE_CAPSULE: i32 = 1;

fn direction(v: [f32; 3]) -> Vector3 {
    let v = to_skate(Vec3::from_array(v)) / METRES;
    Vector3::new(v.x, v.y, v.z)
}
fn point(p: [f32; 3]) -> Vector3 {
    let p = to_skate(Vec3::from_array(p));
    Vector3::new(p.x, p.y, p.z)
}

impl HaloSkateObstacle {
    /// The obstacle in the engine's space, or None for a kind or shape this
    /// does not know. (The engine checks the numbers: `Session::set_obstacles`.)
    pub fn to_engine(&self) -> Option<KinematicObstacle> {
        let kind = match self.kind {
            KIND_BIPED => ObstacleKind::Biped,
            KIND_VEHICLE => ObstacleKind::Vehicle,
            KIND_SCENERY => ObstacleKind::Scenery,
            _ => return None,
        };
        let center = point(self.center);
        let [a, _, c] = self.half_extents.map(|v| v * METRES);
        let shape = match self.shape {
            SHAPE_CAPSULE => ObstacleShape::Capsule {
                center,
                axis: direction(self.up),
                half_length: (c - a).max(0.0),
                radius: a,
            },
            SHAPE_BOX => {
                let column = |v: [f32; 3]| {
                    let d = direction(v);
                    [d.x, d.y, d.z]
                };
                let [x, y, z] = self.half_extents.map(|v| v * METRES);
                ObstacleShape::Box {
                    center,
                    basis: Basis3 {
                        columns: [column(self.forward), column(self.left), column(self.up)],
                    },
                    half_extents: Vector3::new(x, y, z),
                }
            }
            _ => return None,
        };
        let v = to_skate(Vec3::from_array(self.velocity));
        Some(KinematicObstacle {
            kind,
            shape,
            linear_velocity: Vector3::new(v.x, v.y, v.z),
        })
    }
}

pub fn kind_name(kind: ObstacleKind) -> &'static str {
    match kind {
        ObstacleKind::Biped => "biped",
        ObstacleKind::Vehicle => "vehicle",
        ObstacleKind::Scenery => "scenery",
    }
}

/// Tells the log when the skater runs into an obstacle (the first tick of a
/// run of ticks touching one), and whether a bail followed within a second.
#[derive(Default)]
pub struct HitLog {
    touching: bool,
    /// the engine tick of the last hit and what was hit, until a bail or a
    /// second on
    pending: Option<(u64, ObstacleKind)>,
}

/// Engine ticks (60 a second) after a hit that a bail still counts as its.
const BAIL_AFTER: u64 = 60;

impl HitLog {
    /// After each engine tick: how many contacts with obstacles it had, the
    /// fastest one's obstacle and closing speed (m/s), the skater's state
    /// and the tick. Returns the lines to log.
    pub fn observe(
        &mut self,
        contacts: usize,
        hit: Option<(ObstacleKind, f32)>,
        state: &str,
        tick: u64,
    ) -> Vec<String> {
        let mut lines = Vec::new();
        let touching = contacts > 0;
        if touching && !self.touching {
            let (kind, speed) = hit.unwrap_or((ObstacleKind::Scenery, 0.0));
            lines.push(format!(
                "halo-skate: hit a {} at {:.1} m/s ({state}, engine tick {tick})",
                kind_name(kind),
                speed.max(0.0)
            ));
            self.pending = Some((tick, kind));
        }
        self.touching = touching;
        if let Some((at, kind)) = self.pending {
            if state.starts_with("Wipeout") {
                lines.push(format!(
                    "halo-skate: bailed {} engine ticks after hitting a {}",
                    tick.saturating_sub(at),
                    kind_name(kind)
                ));
                self.pending = None;
            } else if tick.saturating_sub(at) > BAIL_AFTER {
                lines.push(format!("halo-skate: rode on after hitting a {} ({state})", kind_name(kind)));
                self.pending = None;
            }
        }
        lines
    }

    /// Whether `observe` has anything to tell with no contact: a hit
    /// last tick, or one waiting to be told what followed.
    pub fn listening(&self) -> bool {
        self.touching || self.pending.is_some()
    }

    pub fn reset(&mut self) {
        *self = Self::default();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn close(a: Vector3, b: [f32; 3]) {
        assert!(
            (a.x - b[0]).abs() < 1e-5 && (a.y - b[1]).abs() < 1e-5 && (a.z - b[2]).abs() < 1e-5,
            "{a:?} != {b:?}"
        );
    }

    #[test]
    fn a_biped_is_a_standing_capsule_in_metres_y_up() {
        // a biped 0.7 world units tall, radius 0.2, its middle 0.35 up
        let o = HaloSkateObstacle {
            kind: KIND_BIPED,
            shape: SHAPE_CAPSULE,
            center: [1.0, 2.0, 0.35],
            up: [0.0, 0.0, 1.0],
            half_extents: [0.2, 0.0, 0.35],
            velocity: [1.0, 0.0, 0.0],
            ..Default::default()
        };
        let e = o.to_engine().unwrap();
        assert_eq!(e.kind, ObstacleKind::Biped);
        let ObstacleShape::Capsule { center, axis, half_length, radius } = e.shape else {
            panic!("a capsule")
        };
        close(center, [1.0 * METRES, 0.35 * METRES, -2.0 * METRES]);
        close(axis, [0.0, 1.0, 0.0]);
        assert!((radius - 0.2 * METRES).abs() < 1e-5);
        assert!((half_length - 0.15 * METRES).abs() < 1e-5, "the segment between the caps");
        close(e.linear_velocity, [METRES, 0.0, 0.0]);
        assert!(e.validated().is_some());
    }

    #[test]
    fn a_vehicle_is_a_box_whose_axes_turn_with_it() {
        // a warthog-sized box facing +Y (turned a quarter left)
        let o = HaloSkateObstacle {
            kind: KIND_VEHICLE,
            shape: SHAPE_BOX,
            center: [0.0, 0.0, 0.3],
            forward: [0.0, 1.0, 0.0],
            left: [-1.0, 0.0, 0.0],
            up: [0.0, 0.0, 1.0],
            half_extents: [0.8, 0.4, 0.3],
            velocity: [0.0, 3.0, 0.0],
        };
        let e = o.to_engine().unwrap();
        let ObstacleShape::Box { basis, half_extents, .. } = e.shape else {
            panic!("a box")
        };
        let column = |i: usize| Vector3::new(basis.columns[i][0], basis.columns[i][1], basis.columns[i][2]);
        close(column(0), [0.0, 0.0, -1.0]);
        close(column(1), [-1.0, 0.0, 0.0]);
        close(column(2), [0.0, 1.0, 0.0]);
        close(half_extents, [0.8 * METRES, 0.4 * METRES, 0.3 * METRES]);
        close(e.linear_velocity, [0.0, 0.0, -3.0 * METRES]);
        assert!(e.validated().is_some());
    }

    #[test]
    fn unknown_kinds_and_shapes_and_bad_numbers_are_left_out() {
        let good = HaloSkateObstacle {
            kind: KIND_SCENERY,
            shape: SHAPE_BOX,
            forward: [1.0, 0.0, 0.0],
            left: [0.0, 1.0, 0.0],
            up: [0.0, 0.0, 1.0],
            half_extents: [0.3, 0.3, 0.3],
            ..Default::default()
        };
        assert!(good.to_engine().and_then(|o| o.validated()).is_some());
        assert!(HaloSkateObstacle { kind: 7, ..good }.to_engine().is_none());
        assert!(HaloSkateObstacle { shape: 2, ..good }.to_engine().is_none());
        let nan = HaloSkateObstacle { center: [f32::NAN, 0.0, 0.0], ..good };
        assert!(nan.to_engine().and_then(|o| o.validated()).is_none());
        let flat = HaloSkateObstacle { half_extents: [0.3, 0.0, 0.3], ..good };
        assert!(flat.to_engine().and_then(|o| o.validated()).is_none());
    }

    #[test]
    fn the_hit_log_tells_each_hit_once_and_what_followed() {
        let mut log = HitLog::default();
        assert!(log.observe(0, None, "PhysicsGround", 1).is_empty());
        let hit = log.observe(3, Some((ObstacleKind::Biped, 6.3)), "PhysicsGround", 2);
        assert_eq!(hit, vec!["halo-skate: hit a biped at 6.3 m/s (PhysicsGround, engine tick 2)".to_string()]);
        assert!(log.observe(2, Some((ObstacleKind::Biped, 1.0)), "PhysicsGround", 3).is_empty(), "the same hit");
        assert_eq!(
            log.observe(0, None, "WipeoutGround", 10),
            vec!["halo-skate: bailed 8 engine ticks after hitting a biped".to_string()]
        );
        assert!(log.observe(0, None, "WipeoutGround", 11).is_empty());
        log.observe(1, Some((ObstacleKind::Vehicle, 1.0)), "PhysicsGround", 100);
        assert!(log.observe(0, None, "PhysicsGround", 150).is_empty());
        assert_eq!(
            log.observe(0, None, "PhysicsGround", 161),
            vec!["halo-skate: rode on after hitting a vehicle (PhysicsGround)".to_string()]
        );
    }
}

//! halo-skate: kinematic obstacles, the host's moving objects the board and
//! the skater collide with (in Halo: other bipeds, vehicles, scenery).
//!
//! Skate 3 shares its solve with other assemblies (cars, other skaters) as
//! attached bodies; this is the same path for bodies the host moves itself.
//! Each obstacle is one shape (a capsule or a box) on a body that the solve
//! treats as fixed: state flags 1 (never integrated, inverse mass 0, as the
//! static world), but with the obstacle's velocity, so that a contact's
//! relative velocity, and so its response, is the one against a moving
//! object. Nothing pushes the obstacle back: the host owns its motion.
//!
//! Its contacts with the board are reported to the board as the world's
//! (`board_reports::collect`, `AttachedStep::kinematic_from`): riding into
//! one is riding into a wall (the board's closing speed, its ground state).
//! Its contacts with the skater's body are classified by `contact_group`, as
//! Skate 3's own other assemblies are (`SkeletonCollisionFeedback`).
use crate::math::{Basis3, Vector3};

use super::{
    assembly::BodySnapshot,
    board_step::{BoardCollision, CollisionBody},
    board_world::BoardWorldVolume,
    contact::{RetailContactInput, RetailContactMaterial, combine_contact_materials},
    rigid_body::{RetailBodyRates, RetailInertiaDynamics, RetailQuaternion},
    world_contact::{ContactPrimitive, PrimitivePairSettings, primitive_pair_contacts},
};

/// The most obstacles a tick takes; the host sends the nearest.
pub const MAXIMUM_OBSTACLES: usize = 32;
/// Largest obstacle dimension taken (metres): past this a shape is not an
/// object but a building, and a box would fill its doors and arches.
pub const MAXIMUM_EXTENT: f32 = 50.0;
/// Rounding of an obstacle box's edges (metres), so that the board slides
/// off a corner rather than catching on it.
pub const BOX_ROUNDING: f32 = 0.02;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ObstacleKind {
    /// A person: another player, or an AI.
    Biped,
    Vehicle,
    /// Anything else solid that does not move by itself (crates, barrels,
    /// devices).
    Scenery,
}

impl ObstacleKind {
    /// The Skate 3 collision group the skater's body feedback classifies a
    /// contact by (`collision_update.rs`): another skater (5) for a person,
    /// a car (8, with its own bail threshold, Wipeout_GroundVehicleContact)
    /// for a vehicle, the world (0) for scenery.
    pub fn contact_group(self) -> u32 {
        match self {
            Self::Biped => 5,
            Self::Vehicle => 8,
            Self::Scenery => 0,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum ObstacleShape {
    /// A segment `center` +- `axis` * `half_length` (axis a unit vector),
    /// grown by `radius`.
    Capsule {
        center: Vector3,
        axis: Vector3,
        half_length: f32,
        radius: f32,
    },
    /// A box: `basis` columns are its unit axes, `half_extents` along each.
    Box {
        center: Vector3,
        basis: Basis3,
        half_extents: Vector3,
    },
}

/// One obstacle, where it is when the host sends it, in the engine's space
/// (Y up, metres) and moving at `linear_velocity` (metres a second).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct KinematicObstacle {
    pub kind: ObstacleKind,
    pub shape: ObstacleShape,
    pub linear_velocity: Vector3,
}

impl KinematicObstacle {
    /// The obstacle with its axes made unit length, or None when it is not
    /// one the solve can take: not finite, no size, a degenerate axis, or
    /// larger than MAXIMUM_EXTENT.
    pub fn validated(self) -> Option<Self> {
        if !finite(self.linear_velocity) {
            return None;
        }
        let shape = match self.shape {
            ObstacleShape::Capsule {
                center,
                axis,
                half_length,
                radius,
            } => {
                let axis = unit(axis)?;
                if !finite(center)
                    || !(radius > 0.0 && half_length >= 0.0)
                    || 2.0 * (half_length + radius) > MAXIMUM_EXTENT
                {
                    return None;
                }
                ObstacleShape::Capsule {
                    center,
                    axis,
                    half_length,
                    radius,
                }
            }
            ObstacleShape::Box {
                center,
                basis,
                half_extents,
            } => {
                let column = |i: usize| {
                    let c = basis.columns[i];
                    unit(Vector3::new(c[0], c[1], c[2]))
                };
                let (a, b, c) = (column(0)?, column(1)?, column(2)?);
                let extents = [half_extents.x, half_extents.y, half_extents.z];
                if !finite(center)
                    || extents.iter().any(|&e| !(e > 0.0) || 2.0 * e > MAXIMUM_EXTENT)
                {
                    return None;
                }
                ObstacleShape::Box {
                    center,
                    basis: Basis3 {
                        columns: [[a.x, a.y, a.z], [b.x, b.y, b.z], [c.x, c.y, c.z]],
                    },
                    half_extents,
                }
            }
        };
        Some(Self { shape, ..self })
    }

    /// Where the obstacle is `age` seconds after the host sent it: carried
    /// on at its velocity, as Halo carries a unit on between network updates.
    pub fn center_at(&self, age: f32) -> Vector3 {
        let center = match self.shape {
            ObstacleShape::Capsule { center, .. } | ObstacleShape::Box { center, .. } => center,
        };
        add(center, scale(self.linear_velocity, age))
    }

    /// Its contact shape `age` seconds on. A box keeps BOX_ROUNDING inside
    /// its extents, so that its outside stays where the host put it.
    pub fn primitive_at(&self, age: f32) -> ContactPrimitive {
        let center = self.center_at(age);
        match self.shape {
            ObstacleShape::Capsule {
                axis,
                half_length,
                radius,
                ..
            } => ContactPrimitive::Capsule {
                center,
                axis,
                half_length,
                radius,
            },
            ObstacleShape::Box {
                basis,
                half_extents,
                ..
            } => {
                let rounding = BOX_ROUNDING
                    .min(half_extents.x * 0.5)
                    .min(half_extents.y * 0.5)
                    .min(half_extents.z * 0.5);
                ContactPrimitive::RoundedBox {
                    center,
                    basis,
                    half_extents: Vector3::new(
                        half_extents.x - rounding,
                        half_extents.y - rounding,
                        half_extents.z - rounding,
                    ),
                    radius: rounding,
                }
            }
        }
    }

    /// Its body in the solve `age` seconds on: fixed (state flags 1, no
    /// inverse mass or inertia, never integrated) and moving at its velocity.
    pub fn body_at(&self, age: f32) -> BodySnapshot {
        BodySnapshot {
            state_flags: 1,
            rates: RetailBodyRates {
                orientation: RetailQuaternion::IDENTITY,
                basis: IDENTITY_BASIS,
                world_inverse_inertia: Basis3 {
                    columns: [[0.0; 3]; 3],
                },
                position: self.center_at(age),
                linear_velocity: self.linear_velocity,
                angular_velocity: Vector3::ZERO,
                force_acceleration: Vector3::ZERO,
                torque_acceleration: Vector3::ZERO,
                kinetic_energy: 0.0,
                cool_down: 0,
            },
            inertia: RetailInertiaDynamics {
                inverse_tensor: Vector3::ZERO,
                inverse_mass: 0.0,
                spherical: 0.0,
                maximum_linear_velocity: 0.0,
                maximum_angular_velocity: 0.0,
                linear_drag: 0.0,
                angular_drag: 0.0,
            },
        }
    }

    /// Its contact volume `age` seconds on, for the attached body `body`.
    pub fn volume_at(
        &self,
        age: f32,
        body: CollisionBody,
        material: RetailContactMaterial,
    ) -> BoardWorldVolume {
        BoardWorldVolume {
            body,
            primitive: self.primitive_at(age),
            linear_velocity: self.linear_velocity,
            material,
        }
    }
}

/// Contacts between the local volumes (the board's and the skater's) and the
/// obstacles' volumes, in that order (A local, B the obstacle; the normal
/// points from the obstacle toward the local volume), with the materials
/// combined. Pairs whose enclosing spheres are apart (with `padding`) are
/// not tested.
pub fn obstacle_contacts(
    local: &[BoardWorldVolume],
    obstacles: &[BoardWorldVolume],
    padding: f32,
    contacts: &mut Vec<BoardCollision>,
) {
    let settings = PrimitivePairSettings::skater_self_collision();
    let obstacle_bounds: Vec<_> = obstacles.iter().map(|o| enclosing_sphere(o.primitive)).collect();
    for a in local {
        let (a_center, a_radius) = enclosing_sphere(a.primitive);
        for (b, &(b_center, b_radius)) in obstacles.iter().zip(&obstacle_bounds) {
            let reach = a_radius + b_radius + padding;
            let d = sub(a_center, b_center);
            if !(dot(d, d) <= reach * reach) {
                continue;
            }
            let Some(manifold) = primitive_pair_contacts(a.primitive, b.primitive, settings) else {
                continue;
            };
            let material = combine_contact_materials(a.material, b.material);
            for points in &manifold.points[..manifold.count] {
                contacts.push(BoardCollision {
                    body_a: a.body,
                    body_b: b.body,
                    contact: RetailContactInput {
                        position_on_a: points.a,
                        position_on_b: points.b,
                        normal: manifold.normal,
                        restitution: material.restitution,
                        static_friction: material.static_friction,
                        dynamic_friction: material.dynamic_friction,
                        tag: 0,
                    },
                });
            }
        }
    }
}

/// A sphere around a contact shape: its centre and radius.
pub fn enclosing_sphere(primitive: ContactPrimitive) -> (Vector3, f32) {
    match primitive {
        ContactPrimitive::Sphere(s) => (s.center, s.radius),
        ContactPrimitive::Capsule {
            center,
            half_length,
            radius,
            ..
        } => (center, half_length + radius),
        ContactPrimitive::RoundedBox {
            center,
            half_extents,
            radius,
            ..
        } => (center, dot(half_extents, half_extents).sqrt() + radius),
        ContactPrimitive::Triangle(t) => {
            let v = t.vertices;
            let center = scale(add(add(v[0], v[1]), v[2]), 1.0 / 3.0);
            let reach = v
                .iter()
                .map(|&p| {
                    let d = sub(p, center);
                    dot(d, d).sqrt()
                })
                .fold(0.0, f32::max);
            (center, reach + t.fatness)
        }
    }
}

const IDENTITY_BASIS: Basis3 = Basis3 {
    columns: [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]],
};

fn finite(v: Vector3) -> bool {
    v.x.is_finite() && v.y.is_finite() && v.z.is_finite()
}
fn unit(v: Vector3) -> Option<Vector3> {
    let length = dot(v, v).sqrt();
    (finite(v) && length > 1e-6).then(|| scale(v, 1.0 / length))
}
fn add(a: Vector3, b: Vector3) -> Vector3 {
    Vector3::new(a.x + b.x, a.y + b.y, a.z + b.z)
}
fn sub(a: Vector3, b: Vector3) -> Vector3 {
    Vector3::new(a.x - b.x, a.y - b.y, a.z - b.z)
}
fn scale(v: Vector3, s: f32) -> Vector3 {
    Vector3::new(v.x * s, v.y * s, v.z * s)
}
fn dot(a: Vector3, b: Vector3) -> f32 {
    a.x * b.x + a.y * b.y + a.z * b.z
}

#[cfg(test)]
#[path = "tests/kinematic_obstacle.rs"]
mod tests;

//! halo-skate: kinematic obstacles, the host's moving objects the board and
//! the skater collide with (in Halo: other bipeds, vehicles, scenery).
//!
//! Skate 3 shares its solve with other assemblies (cars, other skaters) as
//! attached bodies; this is the same path for bodies the host moves itself.
//! Each obstacle is one shape on a body that the solve treats as fixed:
//! state flags 1 (never integrated, inverse mass 0, as the static world), but
//! with the obstacle's velocity, so that a contact's relative velocity, and
//! so its response, is the one against a moving object. Nothing pushes the
//! obstacle back: the host owns its motion. The shape is a capsule or a box,
//! the shapes the skater's own parts are made of, or a mesh: an object's own
//! collision as triangles the host placed (a vehicle's hull, a door, a lift),
//! each triangle a volume of the one body, as the static world's triangles
//! are, with its edges' convexity found from its neighbours (`ObstacleMesh`).
//!
//! Its contacts with the board are reported to the board as the world's
//! (`board_reports::collect`, `AttachedStep::kinematic_from`): riding into
//! one is riding into a wall (the board's closing speed, its ground state).
//! Its contacts with the skater's body are classified by `contact_group`, as
//! Skate 3's own other assemblies are (`SkeletonCollisionFeedback`).
use crate::math::{Basis3, Vector3};
use std::{collections::HashMap, sync::Arc};

use super::{
    assembly::BodySnapshot,
    board_step::{BoardCollision, CollisionBody},
    board_world::BoardWorldVolume,
    collision::{Triangle, TriangleFeature},
    contact::{RetailContactInput, RetailContactMaterial, combine_contact_materials},
    rigid_body::{RetailBodyRates, RetailInertiaDynamics, RetailQuaternion},
    world_contact::{
        ContactPrimitive, PrimitivePairSettings, primitive_pair_contacts, triangle_from_volume,
    },
};

/// The most obstacles a tick takes; the host sends the nearest.
pub const MAXIMUM_OBSTACLES: usize = 32;
/// Largest box or capsule dimension taken (metres): past this a shape is not
/// an object but a building, and a box would fill its doors and arches. A
/// mesh is its own shape, and is taken at any size.
pub const MAXIMUM_EXTENT: f32 = 50.0;
/// The most triangles a tick's meshes take between them.
pub const MAXIMUM_MESH_TRIANGLES: usize = 4096;
/// Rounding of an obstacle box's edges (metres), so that the board slides
/// off a corner rather than catching on it.
pub const BOX_ROUNDING: f32 = 0.02;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ObstacleKind {
    /// A person: another player, or an AI.
    Biped,
    /// Another skater: their body and their board (another machine's, posed
    /// from what it sends), a person to the solve and the bail.
    Skater,
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
            Self::Biped | Self::Skater => 5,
            Self::Vehicle => 8,
            Self::Scenery => 0,
        }
    }
}

/// An object's collision as triangles where the host placed them, each wound
/// counterclockwise seen from outside, made into the native triangle volume
/// the static world's are (`skate_world.rs`): one-sided, each edge convex or
/// not by the neighbour it shares it with (a crease between two faces of the
/// hull is no edge to catch on; an outside corner is), and a vertex amid one
/// flat face disabled.
#[derive(Clone, Debug)]
pub struct ObstacleMesh {
    triangles: Vec<Triangle>,
    /// a sphere around them: its centre and radius
    center: Vector3,
    radius: f32,
}

impl PartialEq for ObstacleMesh {
    fn eq(&self, other: &Self) -> bool {
        self.triangles.len() == other.triangles.len()
            && self
                .triangles
                .iter()
                .zip(&other.triangles)
                .all(|(a, b)| a.vertices == b.vertices)
    }
}

/// Vertices this near (metres) are one, for finding a mesh's neighbours.
const WELD: f32 = 0.001;

impl ObstacleMesh {
    /// The mesh of `triangles`, leaving out any not finite or with no area;
    /// None when none is left, or more than MAXIMUM_MESH_TRIANGLES are given.
    pub fn new(triangles: &[[Vector3; 3]]) -> Option<Self> {
        if triangles.len() > MAXIMUM_MESH_TRIANGLES {
            return None;
        }
        let triangles: Vec<[Vector3; 3]> = triangles
            .iter()
            .copied()
            .filter(|t| {
                t.iter().all(|&p| finite(p)) && {
                    let n = cross(sub(t[1], t[0]), sub(t[2], t[0]));
                    dot(n, n) > 1e-12
                }
            })
            .collect();
        if triangles.is_empty() {
            return None;
        }
        let key = |p: Vector3| [p.x, p.y, p.z].map(|v| (v / WELD).round() as i64);
        let mut ids = HashMap::new();
        let vertices: Vec<[usize; 3]> = triangles
            .iter()
            .map(|t| {
                t.map(|p| {
                    let next = ids.len();
                    *ids.entry(key(p)).or_insert(next)
                })
            })
            .collect();
        let normals: Vec<Vector3> = triangles
            .iter()
            .map(|t| normalized(cross(sub(t[1], t[0]), sub(t[2], t[0]))))
            .collect();
        // (flag and cosine index `edge` is the edge from corner `edge` to
        // the next, as skate_world.rs numbers them)
        let mut flags = vec![TriangleFeature::ONE_SIDED | TriangleFeature::USE_EDGE_COSINES | 0xe0; triangles.len()];
        let mut cosines = vec![[1.0f32; 3]; triangles.len()];
        let mut edges = HashMap::<(usize, usize), Vec<(usize, usize, usize, usize)>>::new();
        for (i, v) in vertices.iter().enumerate() {
            for edge in 0..3 {
                let (a, b) = (v[edge], v[(edge + 1) % 3]);
                edges.entry((a.min(b), a.max(b))).or_default().push((i, edge, a, b));
            }
        }
        for group in edges.values() {
            for &(i, edge, a, b) in group {
                // (the neighbour across it the other way round, the flattest
                // of them where more than two faces meet)
                let Some(&(other, ..)) = group
                    .iter()
                    .filter(|&&(j, _, ja, jb)| j != i && ja == b && jb == a)
                    .max_by(|l, r| dot(normals[i], normals[l.0]).total_cmp(&dot(normals[i], normals[r.0])))
                else {
                    continue;
                };
                let cosine = dot(normals[i], normals[other]).clamp(-1.0, 1.0);
                let along = sub(triangles[i][(edge + 1) % 3], triangles[i][edge]);
                let orientation = dot(along, cross(normals[i], normals[other]));
                cosines[i][edge] = cosine;
                if orientation <= -1.0e-6 || cosine >= 1.0 - 1.0e-5 {
                    flags[i] &= !(0x20 << edge);
                }
            }
        }
        let mut faces = vec![Vec::new(); ids.len()];
        for (i, v) in vertices.iter().enumerate() {
            for (corner, &id) in v.iter().enumerate() {
                faces[id].push((i, corner));
            }
        }
        for around in &faces {
            let reference = normals[around[0].0];
            if around.iter().all(|&(i, _)| (dot(reference, normals[i]) - 1.0).abs() <= 0.01) {
                for &(i, corner) in around {
                    flags[i] |= 0x200 << corner;
                }
            }
        }
        let (mut low, mut high) = (triangles[0][0], triangles[0][0]);
        for p in triangles.iter().flatten() {
            low = Vector3::new(low.x.min(p.x), low.y.min(p.y), low.z.min(p.z));
            high = Vector3::new(high.x.max(p.x), high.y.max(p.y), high.z.max(p.z));
        }
        let center = scale(add(low, high), 0.5);
        let radius = triangles
            .iter()
            .flatten()
            .map(|&p| length(sub(p, center)))
            .fold(0.0, f32::max);
        Some(Self {
            triangles: triangles
                .iter()
                .zip(flags.iter().zip(&cosines))
                .map(|(&t, (&f, &c))| triangle_from_volume(t, 0.0, c, f))
                .collect(),
            center,
            radius,
        })
    }

    pub fn triangles(&self) -> &[Triangle] {
        &self.triangles
    }

    /// A sphere around the mesh, where the host put it: its centre and radius.
    pub fn bounds(&self) -> (Vector3, f32) {
        (self.center, self.radius)
    }
}

#[derive(Clone, Debug, PartialEq)]
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
    /// Triangles where the host placed them, the body's centre `center`
    /// (the point its angular velocity turns about).
    Mesh {
        center: Vector3,
        mesh: Arc<ObstacleMesh>,
    },
}

/// One obstacle, where it is when the host sends it, in the engine's space
/// (Y up, metres) and moving at `linear_velocity` (metres a second), turning
/// at `angular_velocity` (radians a second, about its centre).
#[derive(Clone, Debug, PartialEq)]
pub struct KinematicObstacle {
    pub kind: ObstacleKind,
    pub shape: ObstacleShape,
    pub linear_velocity: Vector3,
    pub angular_velocity: Vector3,
}

impl KinematicObstacle {
    /// The obstacle with its axes made unit length, or None when it is not
    /// one the solve can take: not finite, no size, a degenerate axis, or a
    /// box or capsule larger than MAXIMUM_EXTENT.
    pub fn validated(self) -> Option<Self> {
        if !finite(self.linear_velocity) || !finite(self.angular_velocity) {
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
            // (the mesh checked its triangles as it was made)
            ObstacleShape::Mesh { center, mesh } => {
                if !finite(center) {
                    return None;
                }
                ObstacleShape::Mesh { center, mesh }
            }
        };
        Some(Self { shape, ..self })
    }

    /// How far it has been carried `age` seconds after the host sent it: on
    /// at its velocity, as Halo carries a unit on between network updates.
    pub fn carried(&self, age: f32) -> Vector3 {
        scale(self.linear_velocity, age)
    }

    /// Where the obstacle's centre is `age` seconds after the host sent it.
    pub fn center_at(&self, age: f32) -> Vector3 {
        let center = match &self.shape {
            ObstacleShape::Capsule { center, .. }
            | ObstacleShape::Box { center, .. }
            | ObstacleShape::Mesh { center, .. } => *center,
        };
        add(center, self.carried(age))
    }

    /// The velocity of its point `point` (carried `age` seconds on): its
    /// linear velocity and its turn about its centre.
    pub fn velocity_at(&self, point: Vector3, age: f32) -> Vector3 {
        add(
            self.linear_velocity,
            cross(self.angular_velocity, sub(point, self.center_at(age))),
        )
    }

    /// Its contact shape `age` seconds on. A box keeps BOX_ROUNDING inside
    /// its extents, so that its outside stays where the host put it. A mesh
    /// has no one shape (`volumes_at`): this is the sphere around it.
    pub fn primitive_at(&self, age: f32) -> ContactPrimitive {
        let center = self.center_at(age);
        match &self.shape {
            &ObstacleShape::Capsule {
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
            &ObstacleShape::Box {
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
            ObstacleShape::Mesh { mesh, .. } => {
                let (middle, radius) = mesh.bounds();
                ContactPrimitive::Sphere(super::collision::Sphere {
                    center: add(middle, self.carried(age)),
                    radius,
                })
            }
        }
    }

    /// Its outside as triangles wound to face out, `age` seconds on, for
    /// the off-board controller's queries, which sweep triangles alone
    /// (`BoardWorld::set_offboard_obstacles`): a box's twelve, a capsule as
    /// the eight-sided prism around it, as long as it with its caps (a
    /// walking skater meets its side; its ends are flat), and a mesh's own.
    pub fn triangles_at(&self, age: f32) -> Vec<[Vector3; 3]> {
        let center = self.center_at(age);
        if let ObstacleShape::Mesh { mesh, .. } = &self.shape {
            let carried = self.carried(age);
            return mesh
                .triangles()
                .iter()
                .map(|t| t.vertices.map(|p| add(p, carried)))
                .collect();
        }
        let mut out = Vec::with_capacity(28);
        let mut push = |t: [Vector3; 3]| {
            // (wound so that its normal points away from the middle)
            let n = cross(sub(t[1], t[0]), sub(t[2], t[0]));
            let middle = scale(add(add(t[0], t[1]), t[2]), 1.0 / 3.0);
            out.push(if dot(n, sub(middle, center)) < 0.0 {
                [t[0], t[2], t[1]]
            } else {
                t
            });
        };
        match self.shape {
            ObstacleShape::Box {
                basis,
                half_extents,
                ..
            } => {
                let axis = |i: usize| {
                    let c = basis.columns[i];
                    Vector3::new(c[0], c[1], c[2])
                };
                let extents = [half_extents.x, half_extents.y, half_extents.z];
                for i in 0..3 {
                    let (j, k) = ((i + 1) % 3, (i + 2) % 3);
                    let u = scale(axis(j), extents[j]);
                    let v = scale(axis(k), extents[k]);
                    for sign in [-1.0, 1.0] {
                        let face = add(center, scale(axis(i), sign * extents[i]));
                        let corner = |a: f32, b: f32| add(face, add(scale(u, a), scale(v, b)));
                        let (p0, p1) = (corner(-1.0, -1.0), corner(1.0, -1.0));
                        let (p2, p3) = (corner(1.0, 1.0), corner(-1.0, 1.0));
                        push([p0, p1, p2]);
                        push([p0, p2, p3]);
                    }
                }
            }
            ObstacleShape::Capsule {
                axis,
                half_length,
                radius,
                ..
            } => {
                const SIDES: usize = 8;
                // (two directions square to the axis and to each other)
                let other = if axis.x.abs() < 0.9 {
                    Vector3::new(1.0, 0.0, 0.0)
                } else {
                    Vector3::new(0.0, 1.0, 0.0)
                };
                let u = unit(cross(axis, other)).unwrap_or(Vector3::new(0.0, 0.0, 1.0));
                let v = cross(axis, u);
                // (the prism's sides touch the capsule's round)
                let reach = radius / (std::f32::consts::PI / SIDES as f32).cos();
                let end = half_length + radius;
                let ring = |k: usize, along: f32| {
                    let angle = k as f32 * std::f32::consts::TAU / SIDES as f32;
                    add(
                        add(center, scale(axis, along)),
                        add(scale(u, reach * angle.cos()), scale(v, reach * angle.sin())),
                    )
                };
                for k in 0..SIDES {
                    let next = (k + 1) % SIDES;
                    let (a, b) = (ring(k, -end), ring(next, -end));
                    let (c, d) = (ring(next, end), ring(k, end));
                    push([a, b, c]);
                    push([a, c, d]);
                }
                for k in 1..SIDES - 1 {
                    push([ring(0, end), ring(k, end), ring(k + 1, end)]);
                    push([ring(0, -end), ring(k, -end), ring(k + 1, -end)]);
                }
            }
            ObstacleShape::Mesh { .. } => {}
        }
        out
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
                angular_velocity: self.angular_velocity,
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

    /// Its contact volume `age` seconds on, for the attached body `body` (a
    /// box or a capsule: a mesh's are `volumes_at`'s).
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

    /// Its contact volumes `age` seconds on, for the attached body `body`,
    /// appended to `out`: a box's or a capsule's one, or each of a mesh's
    /// triangles whose sphere `near` takes (a centre and a radius: those
    /// near enough to the board or the skater to touch them).
    pub fn volumes_at(
        &self,
        age: f32,
        body: CollisionBody,
        material: RetailContactMaterial,
        near: impl Fn(Vector3, f32) -> bool,
        out: &mut Vec<BoardWorldVolume>,
    ) {
        let ObstacleShape::Mesh { mesh, .. } = &self.shape else {
            out.push(self.volume_at(age, body, material));
            return;
        };
        let carried = self.carried(age);
        let (middle, radius) = mesh.bounds();
        if !near(add(middle, carried), radius) {
            return;
        }
        for triangle in mesh.triangles() {
            let mut moved = *triangle;
            moved.vertices = triangle.vertices.map(|p| add(p, carried));
            let (center, reach) = enclosing_sphere(ContactPrimitive::Triangle(moved));
            if near(center, reach) {
                out.push(BoardWorldVolume {
                    body,
                    primitive: ContactPrimitive::Triangle(moved),
                    linear_velocity: self.velocity_at(center, age),
                    material,
                });
            }
        }
    }
}

/// The bail Skate 3 itself gives an obstacle only past its wall threshold
/// (Wipeout_GroundXZ, 9 m/s of closing speed, which riding on the flat
/// never reaches: the board stops at the wall) or a car's
/// (Wipeout_GroundVehicleContact, 9 m/s against the body). A person or a
/// vehicle run into faster than `bail_speed` (m/s along the contact normal)
/// knocks the skater over instead: the wipeout requests a body hit (0) and a
/// closing hit (2) make, two of them, so a wipeout rather than a runout
/// (`Requests::requests_runout`). Slower is the solve's bump alone. Scenery
/// is left to Skate 3's own thresholds, as the level's walls are.
pub fn knocks_over(hit: Option<(ObstacleKind, f32)>, bail_speed: f32) -> bool {
    matches!(hit, Some((kind, closing)) if kind != ObstacleKind::Scenery && closing > bail_speed)
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
fn length(v: Vector3) -> f32 {
    dot(v, v).sqrt()
}
fn normalized(v: Vector3) -> Vector3 {
    unit(v).unwrap_or(Vector3::ZERO)
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
fn cross(a: Vector3, b: Vector3) -> Vector3 {
    Vector3::new(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x)
}

#[cfg(test)]
#[path = "tests/kinematic_obstacle.rs"]
mod tests;

//! halo-skate: the host's kinematic obstacles join the shared solve
//! (`skate_core::physics::kinematic_obstacle`), after the remote actors.
use super::super::GamePhysics;
use skate_core::physics::{
    board_step::{BoardCollision, CollisionBody},
    board_world::BoardWorldVolume,
    kinematic_obstacle::{ObstacleKind, obstacle_contacts},
};

/// Obstacles not sent again for this long (seconds of simulation) are
/// dropped: the host stopped sending them.
const STALE: f32 = 0.5;
/// How far apart two shapes' enclosing spheres may be and still be tested.
const PADDING: f32 = 0.1;

/// Carries the obstacles on to this tick, makes their bodies (attached from
/// `base`), and appends their contacts with the board's and the skater's
/// world volumes to `contacts`.
pub(super) fn append(
    physics: &mut GamePhysics,
    board: &[BoardWorldVolume],
    skeleton: &[BoardWorldVolume],
    base: usize,
    contacts: &mut Vec<BoardCollision>,
) {
    let dt = physics.settings.step.simulation.time_step;
    physics.obstacle_age += dt;
    if physics.obstacle_age > STALE {
        physics.obstacles.clear();
    }
    let age = physics.obstacle_age;
    let material = physics.settings.floor_material;
    physics.obstacle_bodies.clear();
    physics
        .obstacle_bodies
        .extend(physics.obstacles.iter().map(|o| o.body_at(age)));
    let volumes: Vec<_> = physics
        .obstacles
        .iter()
        .enumerate()
        .map(|(i, o)| o.volume_at(age, CollisionBody::Attached(base + i), material))
        .collect();
    let first = contacts.len();
    obstacle_contacts(board, &volumes, PADDING, contacts);
    obstacle_contacts(skeleton, &volumes, PADDING, contacts);
    physics.obstacle_contacts = contacts.len() - first;
    physics.obstacle_hit = None;
    for c in &contacts[first..] {
        let CollisionBody::Attached(index) = c.body_b else {
            continue;
        };
        let Some(obstacle) = index.checked_sub(base).and_then(|i| physics.obstacles.get(i)) else {
            continue;
        };
        let local = board
            .iter()
            .chain(skeleton)
            .find(|v| v.body == c.body_a)
            .map_or(skate_core::math::Vector3::ZERO, |v| v.linear_velocity);
        let n = c.contact.normal;
        let v = obstacle.linear_velocity;
        // (the normal points from the obstacle toward the board or skater)
        let closing = -((local.x - v.x) * n.x + (local.y - v.y) * n.y + (local.z - v.z) * n.z);
        if physics.obstacle_hit.is_none_or(|(_, fastest)| closing > fastest) {
            physics.obstacle_hit = Some((obstacle.kind, closing));
        }
    }
}

/// The Skate 3 collision group of the attached body `index` when it is a
/// kinematic obstacle (attached from `base`), else None.
pub(in crate::physics) fn group(physics: &GamePhysics, index: usize, base: usize) -> Option<u32> {
    index
        .checked_sub(base)
        .and_then(|i| physics.obstacles.get(i))
        .map(|o| ObstacleKind::contact_group(o.kind))
}

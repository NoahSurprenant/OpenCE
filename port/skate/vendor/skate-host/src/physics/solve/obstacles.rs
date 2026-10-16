//! halo-skate: the host's kinematic obstacles join the shared solve
//! (`skate_core::physics::kinematic_obstacle`), after the remote actors.
use super::super::GamePhysics;
use skate_core::physics::{
    board_step::{BoardCollision, CollisionBody},
    board_world::BoardWorldVolume,
    kinematic_obstacle::{HeldHit, ObstacleKind, enclosing_sphere, obstacle_contacts},
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
        if !physics.world.offboard_obstacles().is_empty() {
            physics.world.set_offboard_obstacles(Vec::new());
        }
    }
    let age = physics.obstacle_age;
    let material = physics.settings.floor_material;
    physics.obstacle_bodies.clear();
    physics
        .obstacle_bodies
        .extend(physics.obstacles.iter().map(|o| o.body_at(age)));
    // (a mesh's triangles only near enough the board or the skater to touch)
    let local: Vec<_> = board
        .iter()
        .chain(skeleton)
        .map(|v| enclosing_sphere(v.primitive))
        .collect();
    let near = |center: skate_core::math::Vector3, radius: f32| {
        local.iter().any(|&(c, r)| {
            let (x, y, z) = (c.x - center.x, c.y - center.y, c.z - center.z);
            let reach = r + radius + PADDING;
            x * x + y * y + z * z <= reach * reach
        })
    };
    let mut volumes = Vec::with_capacity(physics.obstacles.len());
    for (i, o) in physics.obstacles.iter().enumerate() {
        o.volumes_at(age, CollisionBody::Attached(base + i), material, near, &mut volumes);
    }
    let first = contacts.len();
    obstacle_contacts(board, &volumes, PADDING, contacts);
    obstacle_contacts(skeleton, &volumes, PADDING, contacts);
    physics.obstacle_hit = None;
    physics.obstacle_bail_hit = None;
    physics.held_hit = None;
    // (each contact's obstacle and closing speed)
    let mut measured: Vec<Option<(usize, f32)>> = contacts[first..]
        .iter()
        .map(|c| {
            let CollisionBody::Attached(index) = c.body_b else {
                return None;
            };
            let i = index.checked_sub(base)?;
            let obstacle = physics.obstacles.get(i)?;
            let local = board
                .iter()
                .chain(skeleton)
                .find(|v| v.body == c.body_a)
                .map_or(skate_core::math::Vector3::ZERO, |v| v.linear_velocity);
            let n = c.contact.normal;
            // (where it touched, as the obstacle moves there: a turning
            // vehicle's nose faster than its middle)
            let v = obstacle.velocity_at(c.contact.position_on_b, age);
            // (the normal points from the obstacle toward the board or skater)
            Some((i, -((local.x - v.x) * n.x + (local.y - v.y) * n.y + (local.z - v.z) * n.z)))
        })
        .collect();
    let run_over_hit = run_over(physics, contacts, first, &mut measured);
    physics.obstacle_contacts = contacts.len() - first;
    for (c, m) in contacts[first..].iter().zip(&measured) {
        let Some((i, closing)) = *m else {
            continue;
        };
        let obstacle = &physics.obstacles[i];
        let n = c.contact.normal;
        if physics.obstacle_hit.is_none_or(|(_, fastest)| closing > fastest) {
            physics.obstacle_hit = Some((obstacle.kind, closing));
        }
        // (a held obstacle's hit is the host's to decide: told, not bailed by)
        if obstacle.held {
            if physics.held_hit.is_none_or(|h| closing > h.closing) {
                physics.held_hit = Some(HeldHit {
                    tag: obstacle.tag,
                    kind: obstacle.kind,
                    closing,
                    normal: n,
                    run_over: false,
                });
            }
        } else if physics.obstacle_bail_hit.is_none_or(|(_, fastest)| closing > fastest) {
            physics.obstacle_bail_hit = Some((obstacle.kind, closing));
        }
    }
    // (a person run over is told before any other hit of the tick)
    if run_over_hit.is_some() {
        physics.held_hit = run_over_hit;
    }
    if let Some(hit) = physics.held_hit {
        physics.held_contact_tick = Some(physics.ticks);
        if physics
            .held
            .hit
            .is_none_or(|h| (hit.run_over && !h.run_over) || (hit.run_over == h.run_over && hit.closing > h.closing))
        {
            physics.held.hit = Some(hit);
        }
    }
}

/// Engine ticks (60 a second) without a contact after which a person being
/// run over is parted from (half a second): till then the skater goes on
/// through them.
pub(crate) const RUN_THROUGH_TICKS: u64 = 30;

/// Running over people on foot (`KinematicObstacle::run_over`): an obstacle
/// the skater is going through keeps having its contacts dropped from
/// `contacts` (from `first`, each measured as its obstacle and closing
/// speed in `measured`, which loses theirs too), till RUN_THROUGH_TICKS
/// without one; one first touched at `run_over_speed` or faster starts
/// being gone through, slows the board and the skater after the tick
/// (`run_over_slowing`), and is the hit returned, run over.
pub(crate) fn run_over(
    physics: &mut GamePhysics,
    contacts: &mut Vec<BoardCollision>,
    first: usize,
    measured: &mut Vec<Option<(usize, f32)>>,
) -> Option<HeldHit> {
    let ticks = physics.ticks;
    physics
        .run_through
        .retain(|&(_, last)| ticks.saturating_sub(last) <= RUN_THROUGH_TICKS);
    let mut through = vec![false; physics.obstacles.len()];
    let mut hit: Option<HeldHit> = None;
    for (i, obstacle) in physics.obstacles.iter().enumerate() {
        if !obstacle.run_over {
            continue;
        }
        // (its fastest contact this tick)
        let Some((fastest, at)) = measured
            .iter()
            .enumerate()
            .filter_map(|(k, m)| m.filter(|m| m.0 == i).map(|m| (m.1, k)))
            .max_by(|a, b| a.0.total_cmp(&b.0))
        else {
            continue;
        };
        if let Some(entry) = physics.run_through.iter_mut().find(|(tag, _)| *tag == obstacle.tag) {
            entry.1 = ticks;
            through[i] = true;
        } else if fastest >= physics.run_over_speed {
            physics.run_through.push((obstacle.tag, ticks));
            through[i] = true;
            physics.run_over_slowing = Some(physics.run_over_keep);
            physics.held.run_over = Some((fastest, ticks));
            if hit.is_none_or(|h| fastest > h.closing) {
                hit = Some(HeldHit {
                    tag: obstacle.tag,
                    kind: obstacle.kind,
                    closing: fastest,
                    normal: contacts[first + at].contact.normal,
                    run_over: true,
                });
            }
        }
    }
    if through.iter().any(|&t| t) {
        let tail = contacts.split_off(first);
        let marks = std::mem::take(measured);
        for (c, m) in tail.into_iter().zip(marks) {
            if !m.is_some_and(|(i, _)| through[i]) {
                contacts.push(c);
                measured.push(m);
            }
        }
    }
    hit
}

/// The Skate 3 collision group of the attached body `index` when it is a
/// kinematic obstacle (attached from `base`), else None.
pub(in crate::physics) fn group(physics: &GamePhysics, index: usize, base: usize) -> Option<u32> {
    index
        .checked_sub(base)
        .and_then(|i| physics.obstacles.get(i))
        .map(|o| ObstacleKind::contact_group(o.kind))
}

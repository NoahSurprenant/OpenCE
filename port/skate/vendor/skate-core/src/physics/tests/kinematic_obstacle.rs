use super::*;
use crate::physics::{
    board::{BODY_COUNT, BodyId, RETAIL_TRUCK_Y_POSITION, RETAIL_WHEEL_RADIUS},
    board_runtime::{BoardMotion, BoardRuntime},
    board_step::{AttachedStep, BoardStepSettings},
    collision::Sphere,
    drive_frames::{
        AuthoredTransformInputs, RetailAffineTransform, authored_body_transforms,
        default_truck_transforms,
    },
    drive_parameters::{RetailTruckDriveSettings, retail_truck_drive_dynamics},
    rigid_body::{RetailBodyMassProperties, RetailLocalMassFrame, RetailSimulationStep},
};

const MATERIAL: RetailContactMaterial = RetailContactMaterial {
    static_friction: 0.5,
    dynamic_friction: 0.4,
    restitution: 0.0,
};

fn wall(center: Vector3, velocity: Vector3) -> KinematicObstacle {
    KinematicObstacle {
        kind: ObstacleKind::Vehicle,
        shape: ObstacleShape::Box {
            center,
            basis: IDENTITY_BASIS,
            half_extents: Vector3::new(0.5, 1.0, 2.0),
        },
        linear_velocity: velocity,
        angular_velocity: Vector3::ZERO,
        tag: 0,
        held: false,
    }
}

fn person(center: Vector3) -> KinematicObstacle {
    KinematicObstacle {
        kind: ObstacleKind::Biped,
        shape: ObstacleShape::Capsule {
            center,
            axis: Vector3::new(0.0, 2.0, 0.0),
            half_length: 0.6,
            radius: 0.3,
        },
        linear_velocity: Vector3::ZERO,
        angular_velocity: Vector3::ZERO,
        tag: 0,
        held: false,
    }
}

#[test]
fn validation_keeps_real_shapes_and_unit_axes_and_refuses_the_rest() {
    let p = person(Vector3::new(1.0, 1.0, 0.0)).validated().unwrap();
    let ObstacleShape::Capsule { axis, .. } = p.shape else {
        panic!("a capsule")
    };
    assert_eq!(axis, Vector3::new(0.0, 1.0, 0.0));
    assert!(wall(Vector3::ZERO, Vector3::ZERO).validated().is_some());

    let mut bad = person(Vector3::new(f32::NAN, 0.0, 0.0));
    assert!(bad.validated().is_none(), "a centre not finite");
    bad = person(Vector3::ZERO);
    bad.linear_velocity.x = f32::INFINITY;
    assert!(bad.validated().is_none(), "a velocity not finite");
    bad = person(Vector3::ZERO);
    if let ObstacleShape::Capsule { axis, .. } = &mut bad.shape {
        *axis = Vector3::ZERO;
    }
    assert!(bad.validated().is_none(), "no axis");
    bad = person(Vector3::ZERO);
    if let ObstacleShape::Capsule { radius, .. } = &mut bad.shape {
        *radius = 0.0;
    }
    assert!(bad.validated().is_none(), "no radius");
    let mut big = wall(Vector3::ZERO, Vector3::ZERO);
    if let ObstacleShape::Box { half_extents, .. } = &mut big.shape {
        half_extents.z = MAXIMUM_EXTENT;
    }
    assert!(big.validated().is_none(), "a building, not an obstacle");
    let mut flat = wall(Vector3::ZERO, Vector3::ZERO);
    if let ObstacleShape::Box { half_extents, .. } = &mut flat.shape {
        half_extents.y = 0.0;
    }
    assert!(flat.validated().is_none(), "no thickness");
}

#[test]
fn an_obstacle_carries_on_at_its_velocity_and_its_body_is_fixed() {
    let o = wall(Vector3::new(1.0, 2.0, 3.0), Vector3::new(2.0, 0.0, -4.0));
    assert_eq!(o.center_at(0.5), Vector3::new(2.0, 2.0, 1.0));
    let body = o.body_at(0.5);
    assert_eq!(body.state_flags, 1, "fixed: never integrated, no response");
    assert_eq!(body.inertia.inverse_mass, 0.0);
    assert_eq!(body.rates.position, Vector3::new(2.0, 2.0, 1.0));
    assert_eq!(body.rates.linear_velocity, o.linear_velocity);
    let ContactPrimitive::RoundedBox {
        center,
        half_extents,
        radius,
        ..
    } = o.primitive_at(0.5)
    else {
        panic!("a box")
    };
    assert_eq!(center, Vector3::new(2.0, 2.0, 1.0));
    // the rounded box's outside is where the host put the box's faces
    assert!((half_extents.x + radius - 0.5).abs() < 1e-6);
    assert!((half_extents.z + radius - 2.0).abs() < 1e-6);
    assert_eq!(ObstacleKind::Biped.contact_group(), 5);
    assert_eq!(ObstacleKind::Skater.contact_group(), 5);
    assert_eq!(ObstacleKind::Vehicle.contact_group(), 8);
    assert_eq!(ObstacleKind::Scenery.contact_group(), 0);
}

#[test]
fn a_volume_touching_an_obstacle_gets_contacts_pointing_away_from_it() {
    let obstacle = person(Vector3::new(0.0, 0.9, 0.0)).validated().unwrap();
    let b = obstacle.volume_at(0.0, CollisionBody::Attached(3), MATERIAL);
    let sphere = |x: f32| BoardWorldVolume {
        body: CollisionBody::Board(BodyId::Deck),
        primitive: ContactPrimitive::Sphere(Sphere {
            center: Vector3::new(x, 0.9, 0.0),
            radius: 0.2,
        }),
        linear_velocity: Vector3::ZERO,
        material: MATERIAL,
    };
    let mut contacts = Vec::new();
    obstacle_contacts(&[sphere(0.49)], &[b], 0.05, &mut contacts);
    assert!(!contacts.is_empty(), "touching");
    for c in &contacts {
        assert_eq!(c.body_a, CollisionBody::Board(BodyId::Deck));
        assert_eq!(c.body_b, CollisionBody::Attached(3));
        assert!(c.contact.normal.x > 0.9, "from the obstacle toward the board: {:?}", c.contact.normal);
    }
    contacts.clear();
    obstacle_contacts(&[sphere(2.0)], &[b], 0.05, &mut contacts);
    assert!(contacts.is_empty(), "apart");
}

fn board() -> BoardRuntime {
    let mass = RetailBodyMassProperties {
        local_mass_frame: RetailLocalMassFrame::IDENTITY,
        dynamics: RetailInertiaDynamics {
            inverse_tensor: Vector3::new(3.0, 3.0, 3.0),
            inverse_mass: 0.5,
            spherical: 0.0,
            maximum_linear_velocity: 1000.0,
            maximum_angular_velocity: 1000.0,
            linear_drag: 0.0,
            angular_drag: 0.0,
        },
    };
    BoardRuntime::new(
        [mass; BODY_COUNT],
        authored_body_transforms(AuthoredTransformInputs::STOCK),
        RetailAffineTransform {
            translation: Vector3::new(0.0, RETAIL_WHEEL_RADIUS - RETAIL_TRUCK_Y_POSITION, 0.0),
            ..RetailAffineTransform::IDENTITY
        },
        RetailSimulationStep::fixed_60_hz(30, 0.001, Vector3::ZERO),
        BoardMotion::Active,
    )
}

/// Steps a weightless board with `obstacle` for `ticks` engine ticks, its
/// wheels as the volumes; returns whether the board reported a contact with
/// the world (the obstacle) on any tick.
fn ride(board: &mut BoardRuntime, obstacle: &KinematicObstacle, ticks: usize) -> bool {
    let settings = BoardStepSettings {
        simulation: RetailSimulationStep::fixed_60_hz(30, 0.001, Vector3::ZERO),
        iterations: 25,
        base_truck_transforms: default_truck_transforms(),
        truck_dynamics: retail_truck_drive_dynamics(RetailTruckDriveSettings::STOCK),
        force_point_y_offset: 0.0,
    };
    let dt = settings.simulation.time_step;
    let mut reported = false;
    for tick in 0..ticks {
        let age = tick as f32 * dt;
        let poses = board.part_transforms();
        let wheels: Vec<_> = BodyId::ORDER[..4]
            .iter()
            .map(|&id| BoardWorldVolume {
                body: CollisionBody::Board(id),
                primitive: ContactPrimitive::Sphere(Sphere {
                    center: poses[id.index()].translation,
                    radius: RETAIL_WHEEL_RADIUS,
                }),
                linear_velocity: board.bodies()[id.index()].rates.linear_velocity,
                material: MATERIAL,
            })
            .collect();
        let mut body = obstacle.body_at(age);
        let before = (body.rates.position, body.rates.linear_velocity);
        let mut volumes = Vec::new();
        obstacle.volumes_at(age, CollisionBody::Attached(0), MATERIAL, |_, _| true, &mut volumes);
        let mut contacts = Vec::new();
        obstacle_contacts(&wheels, &volumes, 0.05, &mut contacts);
        board.advance_attached(
            &contacts,
            [0.0; 2],
            settings,
            AttachedStep {
                bodies: vec![&mut body],
                contacts: &mut [],
                joints: &mut [],
                drives: &mut [],
                kinematic_from: 0,
            },
        );
        assert_eq!(
            (body.rates.position, body.rates.linear_velocity),
            before,
            "the solve never moves an obstacle"
        );
        for report in board.contact_reports() {
            assert_eq!(report.other, CollisionBody::StaticWorld, "reported as the world");
            reported = true;
        }
        for b in board.bodies() {
            let p = b.rates.position;
            assert!(p.x.is_finite() && p.y.is_finite() && p.z.is_finite());
        }
    }
    reported
}

fn deck_velocity(board: &BoardRuntime) -> Vector3 {
    board.bodies()[BodyId::Deck.index()].rates.linear_velocity
}

#[test]
fn a_board_rolling_into_a_standing_obstacle_stops_at_it_as_at_a_wall() {
    let mut b = board();
    for body in b.bodies_mut() {
        body.rates.linear_velocity = Vector3::new(0.0, 0.0, 6.0);
    }
    // the board rolls along +Z (weightless: no floor); a box 1.5 m ahead,
    // from below the wheels to above the deck
    let obstacle = KinematicObstacle {
        kind: ObstacleKind::Vehicle,
        linear_velocity: Vector3::ZERO,
        angular_velocity: Vector3::ZERO,
        tag: 0,
        held: false,
        shape: ObstacleShape::Box {
            center: Vector3::new(0.0, 0.5, 2.0),
            basis: IDENTITY_BASIS,
            half_extents: Vector3::new(2.0, 1.0, 0.5),
        },
    };
    let reported = ride(&mut b, &obstacle, 60);
    let v = deck_velocity(&b);
    assert!(reported, "the board reports hitting it, as a wall");
    assert!(v.z < 1.0, "stopped (or bounced) at it: {v:?}");
    // (the wheels, the shapes it collides with here: weightless, the board
    // may tip up the face)
    let poses = b.part_transforms();
    let front = BodyId::ORDER[..4]
        .iter()
        .map(|id| poses[id.index()].translation.z)
        .fold(f32::MIN, f32::max);
    assert!(front < 1.5 + RETAIL_WHEEL_RADIUS, "not through it: the front wheel at z {front}");
}

#[test]
fn a_moving_obstacle_carries_a_standing_board_with_it() {
    let mut b = board();
    let obstacle = KinematicObstacle {
        kind: ObstacleKind::Vehicle,
        shape: ObstacleShape::Box {
            center: Vector3::new(0.0, 0.5, -1.0),
            basis: IDENTITY_BASIS,
            half_extents: Vector3::new(2.0, 1.0, 0.5),
        },
        linear_velocity: Vector3::new(0.0, 0.0, 3.0),
        angular_velocity: Vector3::ZERO,
        tag: 0,
        held: false,
    };
    let reported = ride(&mut b, &obstacle, 60);
    let v = deck_velocity(&b);
    assert!(reported);
    assert!(v.z > 2.0, "pushed along at its speed: {v:?}");
}

#[test]
fn an_obstacle_as_triangles_faces_out_and_bounds_its_shape() {
    let cross = |a: Vector3, b: Vector3| {
        Vector3::new(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x)
    };
    for (obstacle, count, reach) in [
        // a box 1 by 2 by 4 m: its corners 2.29 m from its middle
        (wall(Vector3::new(3.0, 1.0, -2.0), Vector3::ZERO), 12, 2.30),
        // a capsule 1.8 m long, 0.3 m round: the prism's corners just past it
        (person(Vector3::new(3.0, 1.0, -2.0)), 28, 0.96),
    ] {
        let o = obstacle.validated().unwrap();
        let center = o.center_at(0.0);
        let triangles = o.triangles_at(0.0);
        assert_eq!(triangles.len(), count);
        for t in &triangles {
            let n = cross(sub(t[1], t[0]), sub(t[2], t[0]));
            let middle = scale(add(add(t[0], t[1]), t[2]), 1.0 / 3.0);
            assert!(dot(n, sub(middle, center)) > 0.0, "faces out: {t:?}");
            for p in t {
                let d = sub(*p, center);
                assert!(dot(d, d).sqrt() <= reach, "within the shape's reach: {p:?}");
            }
        }
    }
    // a person's prism: as tall as the capsule, its sides outside the round
    let o = person(Vector3::ZERO).validated().unwrap();
    let triangles = o.triangles_at(0.0);
    let top = triangles.iter().flatten().map(|p| p.y).fold(f32::MIN, f32::max);
    assert!((top - 0.9).abs() < 1e-5, "the caps' top: {top}");
    let side = triangles
        .iter()
        .flatten()
        .map(|p| (p.x * p.x + p.z * p.z).sqrt())
        .fold(f32::MIN, f32::max);
    assert!(side > 0.3 && side < 0.33, "around the round: {side}");
    // carried on at its velocity, as the solve carries it
    let moving = wall(Vector3::ZERO, Vector3::new(2.0, 0.0, 0.0)).validated().unwrap();
    let at = |age: f32| moving.triangles_at(age).iter().flatten().map(|p| p.x).fold(f32::MIN, f32::max);
    assert!((at(0.5) - at(0.0) - 1.0).abs() < 1e-5);
}

/// A box's twelve triangles about `center` (half sizes `half`), wound
/// counterclockwise seen from outside, as the host sends an object's
/// collision.
fn box_triangles(center: Vector3, half: Vector3) -> Vec<[Vector3; 3]> {
    let o = KinematicObstacle {
        kind: ObstacleKind::Vehicle,
        shape: ObstacleShape::Box {
            center,
            basis: IDENTITY_BASIS,
            half_extents: half,
        },
        linear_velocity: Vector3::ZERO,
        angular_velocity: Vector3::ZERO,
        tag: 0,
        held: false,
    };
    o.triangles_at(0.0)
}

fn mesh_obstacle(triangles: &[[Vector3; 3]], velocity: Vector3) -> KinematicObstacle {
    KinematicObstacle {
        kind: ObstacleKind::Vehicle,
        shape: ObstacleShape::Mesh {
            center: Vector3::ZERO,
            mesh: Arc::new(ObstacleMesh::new(triangles).expect("a mesh")),
        },
        linear_velocity: velocity,
        angular_velocity: Vector3::ZERO,
        tag: 0,
        held: false,
    }
}

#[test]
fn a_mesh_knows_its_creases_from_its_corners() {
    let triangles = box_triangles(Vector3::new(1.0, 2.0, 3.0), Vector3::new(0.5, 1.0, 2.0));
    let mesh = ObstacleMesh::new(&triangles).unwrap();
    assert_eq!(mesh.triangles().len(), 12);
    let (center, radius) = mesh.bounds();
    assert!(length(sub(center, Vector3::new(1.0, 2.0, 3.0))) < 1e-5);
    assert!((radius - (0.25f32 + 1.0 + 4.0).sqrt()).abs() < 1e-4);
    for t in mesh.triangles() {
        let flags = t.feature.flags;
        assert_ne!(flags & TriangleFeature::ONE_SIDED, 0, "one-sided");
        assert_ne!(flags & TriangleFeature::USE_EDGE_COSINES, 0);
        // each half of a face shares its diagonal with the other half (flat:
        // no edge to catch on) and its two outer edges with the next faces
        // (a right angle: convex)
        let convex = (0..3).filter(|&e| t.feature.edge_convex(e)).count();
        let flat = (0..3).filter(|&e| !t.feature.edge_convex(e)).count();
        assert_eq!((convex, flat), (2, 1), "{t:?}");
        for e in 0..3 {
            let c = t.feature.edge_cosines[e];
            assert!(c.abs() < 1e-4 || (c - 1.0).abs() < 1e-4, "a right angle or flat: {c}");
        }
        // a box's corners meet three faces: none is disabled
        assert_eq!(flags & 0xe00, 0);
    }
    assert!(ObstacleMesh::new(&[]).is_none(), "no triangles");
    let p = Vector3::new(1.0, 0.0, 0.0);
    assert!(ObstacleMesh::new(&[[p, p, p]]).is_none(), "no area");
    let nan = Vector3::new(f32::NAN, 0.0, 0.0);
    assert_eq!(ObstacleMesh::new(&[triangles[0], [nan, p, p]]).unwrap().triangles().len(), 1);
}

#[test]
fn a_mesh_carries_on_turns_and_gives_only_its_near_triangles() {
    let triangles = box_triangles(Vector3::new(0.0, 0.5, 2.0), Vector3::new(2.0, 1.0, 0.5));
    let mut o = mesh_obstacle(&triangles, Vector3::new(0.0, 0.0, 2.0)).validated().unwrap();
    // carried, as a box is: its triangles and its sphere
    let moved = o.triangles_at(0.5);
    assert_eq!(moved.len(), 12);
    for (a, b) in triangles.iter().zip(&moved) {
        for i in 0..3 {
            assert!(length(sub(add(a[i], Vector3::new(0.0, 0.0, 1.0)), b[i])) < 1e-5);
        }
    }
    let mut all = Vec::new();
    o.volumes_at(0.0, CollisionBody::Attached(2), MATERIAL, |_, _| true, &mut all);
    assert_eq!(all.len(), 12, "a volume a triangle, of the one body");
    assert!(all.iter().all(|v| v.body == CollisionBody::Attached(2)));
    let mut none = Vec::new();
    o.volumes_at(0.0, CollisionBody::Attached(2), MATERIAL, |_, _| false, &mut none);
    assert!(none.is_empty(), "none near");
    // (only those touching a sphere at the near face, z 1.5)
    let mut near = Vec::new();
    let probe = Vector3::new(0.0, 0.5, 1.3);
    o.volumes_at(
        0.0,
        CollisionBody::Attached(2),
        MATERIAL,
        |c, r| length(sub(c, probe)) <= r + 0.25,
        &mut near,
    );
    assert!(!near.is_empty() && near.len() < 12, "some: {}", near.len());
    // a turning obstacle's points move with the turn
    o.angular_velocity = Vector3::new(0.0, 1.0, 0.0);
    let v = o.velocity_at(Vector3::new(1.0, 0.0, 0.0), 0.0);
    assert!(length(sub(v, Vector3::new(0.0, 0.0, 1.0))) < 1e-5, "{v:?}");
    assert_eq!(o.body_at(0.0).rates.angular_velocity, Vector3::new(0.0, 1.0, 0.0));
    o.angular_velocity.x = f32::NAN;
    assert!(o.validated().is_none(), "a turn not finite");
}

#[test]
fn a_board_rolling_into_a_mesh_stops_at_its_face() {
    // the same box as the standing obstacle above, sent as its triangles
    let triangles = box_triangles(Vector3::new(0.0, 0.5, 2.0), Vector3::new(2.0, 1.0, 0.5));
    let obstacle = mesh_obstacle(&triangles, Vector3::ZERO).validated().unwrap();
    let mut b = board();
    for body in b.bodies_mut() {
        body.rates.linear_velocity = Vector3::new(0.0, 0.0, 6.0);
    }
    let reported = ride(&mut b, &obstacle, 60);
    let v = deck_velocity(&b);
    assert!(reported, "the board reports hitting it, as a wall");
    assert!(v.z < 1.0, "stopped (or bounced) at it: {v:?}");
    let poses = b.part_transforms();
    let front = BodyId::ORDER[..4]
        .iter()
        .map(|id| poses[id.index()].translation.z)
        .fold(f32::MIN, f32::max);
    assert!(front < 1.5 + RETAIL_WHEEL_RADIUS, "not through it: the front wheel at z {front}");
    // and pushed along by one driving into it
    let mut b = board();
    let driving = mesh_obstacle(
        &box_triangles(Vector3::new(0.0, 0.5, -1.0), Vector3::new(2.0, 1.0, 0.5)),
        Vector3::new(0.0, 0.0, 3.0),
    );
    assert!(ride(&mut b, &driving.validated().unwrap(), 60));
    assert!(deck_velocity(&b).z > 2.0, "pushed along at its speed: {:?}", deck_velocity(&b));
}

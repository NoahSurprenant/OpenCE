//! The biped posed as the skater. Each mapped node keeps its own bone lengths
//! and turns so the segment to its child points where the skater's matching
//! segment points; a node without a mapping turns with its parent. The pelvis
//! and the chest also take the twist of the hips and shoulders. The skeleton
//! then moves so its feet sit where the skater's feet are, and, riding, so
//! its soles rest on the deck.

use crate::{METRES, board, direction_from_skate, from_skate};
use bevy_math::{Mat3, Quat, Vec3};
use skate_host::bridge::Pose;
use std::sync::atomic::{AtomicBool, Ordering};

/// How far past the board's edge, across it, a foot still partly counts as
/// on the deck (world units): a foot stepping off fades out over it.
const FOOT_RADIUS: f32 = 0.10 / METRES;
/// The skater's ankle up to this far above the deck is riding on it...
const DECK_NEAR: f32 = 0.15 / METRES;
/// ...and from this far, off it; between, partly.
const DECK_FAR: f32 = 0.35 / METRES;
/// An ankle down to the deck's top is on it, and one this far below it (the
/// pushing foot, down at the ground beside the board) is off it; between,
/// partly.
const DECK_LOW: f32 = 0.0;
const DECK_BELOW: f32 = 0.10 / METRES;

/// The board as skinned for the pose, and how the biped stands on it.
pub struct Deck<'a> {
    /// VERTEX_FLOATS a vertex, world units
    pub vertices: &'a [f32],
    pub indices: &'a [u32],
    /// the biped's ankle node above its soles, world units; 0 for unknown,
    /// which leaves the ankles on the skater's
    pub ankle: f32,
    /// raises the biped riding, world units along the board's up
    pub offset: f32,
    /// the board's scale, for the log
    pub scale: f32,
}

/// How much of the skater's ankle `height` above the deck counts as on it:
/// 1 near it, 0 well above it (lifted, in the air) or below it (pushing),
/// and between, partly, so that a foot leaving or coming back moves nothing
/// at once.
fn near_deck(height: f32) -> f32 {
    if !height.is_finite() {
        return 0.0;
    }
    let above = ((DECK_FAR - height) / (DECK_FAR - DECK_NEAR)).clamp(0.0, 1.0);
    let below = ((height + DECK_BELOW - DECK_LOW) / DECK_BELOW).clamp(0.0, 1.0);
    above.min(below)
}

/// A world-space node transform as the game stores one: scale, then the
/// rotated axes, then the position.
#[derive(Clone, Copy)]
pub struct Matrix {
    pub scale: f32,
    pub rotation: Mat3,
    pub position: Vec3,
}

impl Matrix {
    pub fn from_floats(f: &[f32]) -> Self {
        Self {
            scale: f[0],
            rotation: Mat3::from_cols(
                Vec3::new(f[1], f[2], f[3]),
                Vec3::new(f[4], f[5], f[6]),
                Vec3::new(f[7], f[8], f[9]),
            ),
            position: Vec3::new(f[10], f[11], f[12]),
        }
    }

    fn write(&self, out: &mut [f32]) {
        out[0] = self.scale;
        out[1..4].copy_from_slice(&self.rotation.x_axis.to_array());
        out[4..7].copy_from_slice(&self.rotation.y_axis.to_array());
        out[7..10].copy_from_slice(&self.rotation.z_axis.to_array());
        out[10..13].copy_from_slice(&self.position.to_array());
    }

    fn inverse(&self) -> Self {
        let scale = if self.scale.abs() > 1e-6 { 1.0 / self.scale } else { 1.0 };
        let rotation = self.rotation.transpose();
        Self {
            scale,
            rotation,
            position: -(rotation * self.position) * scale,
        }
    }
}

pub struct NodeDefault {
    pub name: String,
    pub parent: Option<usize>,
    pub inverse: Matrix,
}

/// Where a node's segment points: at a child node, or along a fixed direction
/// of the default pose (a foot, which has no toe node).
#[derive(Clone, Copy)]
enum Aim {
    Node(usize),
    Forward,
}

struct Node {
    parent: Option<usize>,
    default: Matrix,
    /// The skater bone this node follows, the bone its segment aims at, and
    /// its own aim.
    follow: Option<(&'static str, &'static str, Aim)>,
    /// A left and right node pair whose line twists this node, with the
    /// skater's matching pair.
    twist: Option<(usize, usize, &'static str, &'static str)>,
}

pub struct Rig {
    nodes: Vec<Node>,
    order: Vec<usize>,
    feet: Option<(usize, usize)>,
    /// whether the feet on the deck were logged (once)
    feet_logged: AtomicBool,
}

/// The biped's node name, lowercase without its "bip01 " prefix.
fn short_name(name: &str) -> String {
    let lower = name.trim().to_ascii_lowercase();
    lower
        .strip_prefix("frame ")
        .unwrap_or(&lower)
        .trim_start_matches("bip01")
        .trim()
        .to_string()
}

/// The skater bone of a biped node, its child's name and that child's bone.
fn mapping(short: &str) -> Option<(&'static str, Option<(&'static str, &'static str)>)> {
    Some(match short {
        "pelvis" => ("HIPS", Some(("spine", "SPINE"))),
        "spine" => ("SPINE", Some(("spine1", "SPINE3"))),
        "spine1" => ("SPINE3", Some(("neck", "NECK"))),
        "neck" => ("NECK", Some(("head", "HEAD"))),
        "l clavicle" => ("LEFTSHOULDER", Some(("l upperarm", "LEFTARM"))),
        "l upperarm" => ("LEFTARM", Some(("l forearm", "LEFTFOREARM"))),
        "l forearm" => ("LEFTFOREARM", Some(("l hand", "LEFTHAND"))),
        "r clavicle" => ("RIGHTSHOULDER", Some(("r upperarm", "RIGHTARM"))),
        "r upperarm" => ("RIGHTARM", Some(("r forearm", "RIGHTFOREARM"))),
        "r forearm" => ("RIGHTFOREARM", Some(("r hand", "RIGHTHAND"))),
        "l thigh" => ("LEFTUPLEG", Some(("l calf", "LEFTLEG"))),
        "l calf" => ("LEFTLEG", Some(("l foot", "LEFTFOOT"))),
        "l foot" => ("LEFTFOOT", None),
        "r thigh" => ("RIGHTUPLEG", Some(("r calf", "RIGHTLEG"))),
        "r calf" => ("RIGHTLEG", Some(("r foot", "RIGHTFOOT"))),
        "r foot" => ("RIGHTFOOT", None),
        _ => return None,
    })
}

impl Rig {
    pub fn new(defaults: Vec<NodeDefault>) -> Self {
        let shorts: Vec<String> = defaults.iter().map(|n| short_name(&n.name)).collect();
        let find = |name: &str| shorts.iter().position(|s| s == name);
        let mut nodes: Vec<Node> = defaults
            .iter()
            .map(|n| Node {
                parent: n.parent,
                default: n.inverse.inverse(),
                follow: None,
                twist: None,
            })
            .collect();
        for (i, short) in shorts.iter().enumerate() {
            let Some((bone, child)) = mapping(short) else {
                continue;
            };
            nodes[i].follow = match child {
                Some((child, child_bone)) => find(child).map(|c| (bone, child_bone, Aim::Node(c))),
                None if short.ends_with("foot") => Some((
                    bone,
                    if short.starts_with('l') { "LEFTTOEBASE" } else { "RIGHTTOEBASE" },
                    Aim::Forward,
                )),
                None => None,
            };
            nodes[i].twist = match short.as_str() {
                "pelvis" => find("l thigh")
                    .zip(find("r thigh"))
                    .map(|(l, r)| (l, r, "LEFTUPLEG", "RIGHTUPLEG")),
                "spine1" => find("l clavicle")
                    .zip(find("r clavicle"))
                    .map(|(l, r)| (l, r, "LEFTSHOULDER", "RIGHTSHOULDER")),
                _ => None,
            };
        }
        // Parents before children, whatever order the model lists them in.
        let mut order = Vec::with_capacity(nodes.len());
        let mut placed = vec![false; nodes.len()];
        while order.len() < nodes.len() {
            let before = order.len();
            for i in 0..nodes.len() {
                if !placed[i] && nodes[i].parent.is_none_or(|p| placed[p] || p == i) {
                    placed[i] = true;
                    order.push(i);
                }
            }
            if order.len() == before {
                // A cycle: place the rest as roots.
                for (i, done) in placed.iter_mut().enumerate() {
                    if !*done {
                        *done = true;
                        order.push(i);
                    }
                }
            }
        }
        let feet = find("l foot").zip(find("r foot"));
        let names: Vec<&str> = defaults.iter().map(|n| n.name.as_str()).collect();
        eprintln!("halo-skate: biped nodes {names:?}");
        Self {
            nodes,
            order,
            feet,
            feet_logged: AtomicBool::new(false),
        }
    }

    pub fn mapped(&self) -> usize {
        self.nodes.iter().filter(|n| n.follow.is_some()).count()
    }
    /// How much further along the board's `up` the skeleton moves, past
    /// standing its ankles' middle on the skater's (`base`, along up), so
    /// that its soles rest on the deck. Each of the skater's feet on the deck
    /// asks for the move that puts the biped's matching ankle (`ankles`, as
    /// posed before any move) its own ankle height, plus the extra offset,
    /// above the deck's top under it; the move is those feet's, weighed by
    /// how much each is on the deck, so that while one foot pushes or is
    /// lifted the other alone holds the biped on the deck, and a foot
    /// leaving or coming back slides it over rather than jumping. As the
    /// feet all leave the deck (an ollie, a flip, the air) it fades out, and
    /// the biped follows the skater's feet there rather than snapping to a
    /// spinning board. None without a deck under either foot.
    fn onto_deck(
        &self,
        deck: &Deck,
        up: Vec3,
        bone: &dyn Fn(&str) -> Option<Vec3>,
        ankles: [Vec3; 2],
        base: f32,
    ) -> Option<f32> {
        if !(deck.ankle > 0.0) || up.length_squared() < 0.5 {
            return None;
        }
        // each foot: the skater's ankle above the deck, how much it is on
        // the deck, and the move it asks for
        let foot = |ankle: &str, toe: &str, biped: Vec3| {
            let ankle = bone(ankle)?;
            // under the middle of the foot, which is on the deck even when
            // the heel hangs off the tail
            let probe = bone(toe).map_or(ankle, |toe| (ankle + toe) * 0.5);
            let top = board::deck_top(deck.vertices, deck.indices, probe, up, FOOT_RADIUS)?;
            let height = ankle.dot(up) - top.height;
            let weight = near_deck(height) * top.weight;
            Some((height, weight, top.height + deck.ankle + deck.offset - biped.dot(up)))
        };
        let feet = [
            foot("LEFTFOOT", "LEFTTOEBASE", ankles[0]),
            foot("RIGHTFOOT", "RIGHTTOEBASE", ankles[1]),
        ];
        let total: f32 = feet.iter().flatten().map(|&(_, w, _)| w).sum();
        let on_deck = feet.iter().flatten().map(|&(_, w, _)| w).fold(0.0, f32::max);
        if !(total > 0.0) {
            return feet.iter().any(Option::is_some).then_some(0.0);
        }
        let wanted = feet.iter().flatten().map(|&(_, w, s)| w * s).sum::<f32>() / total;
        if on_deck >= 1.0 && !self.feet_logged.swap(true, Ordering::Relaxed) {
            let above = |f: &Option<(f32, f32, f32)>| f.map_or("no deck".into(), |(h, w, _)| format!("{:.3} m ({w:.2} on it)", h * METRES));
            eprintln!(
                "halo-skate: feet on the deck: the skater's ankles {} and {} above it, the biped's {:.3} m \
                 ({:.4} world units) above its soles, extra offset {:.4} world units, board scale {:.3}",
                above(&feet[0]),
                above(&feet[1]),
                deck.ankle * METRES,
                deck.ankle,
                deck.offset,
                deck.scale
            );
        }
        Some((wanted - base) * on_deck)
    }

    /// Writes every node's world matrix posed as `pose`, returning how many.
    /// With `deck` (the board skinned to the same pose), a riding biped's
    /// soles rest on the deck.
    pub fn pose(&self, pose: &Pose, deck: Option<&Deck>, out: &mut [f32]) -> usize {
        let count = self.nodes.len().min(out.len() / 13);
        let bone = |name: &str| {
            let index = pose.names.iter().position(|n| n == name)?;
            let world = pose.root * *pose.bones.get(index)?;
            Some(from_skate(world.w_axis.truncate()))
        };
        let segment = |from: &str, to: &str| Some(bone(to)? - bone(from)?).and_then(|v| v.try_normalize());

        // The skater's frame, as the biped's default pose faces +X with Z up.
        let forward = direction_from_skate(pose.root.z_axis.truncate()).normalize_or_zero();
        let up = direction_from_skate(pose.root.y_axis.truncate()).normalize_or_zero();
        let frame = if forward.length_squared() > 0.5 && up.length_squared() > 0.5 {
            let left = up.cross(forward).normalize_or_zero();
            Quat::from_mat3(&Mat3::from_cols(forward, left, forward.cross(left)))
        } else {
            Quat::IDENTITY
        };
        let root = from_skate(pose.root.w_axis.truncate());

        let mut turns = vec![frame; self.nodes.len()];
        let mut positions = vec![Vec3::ZERO; self.nodes.len()];
        for &i in &self.order {
            let node = &self.nodes[i];
            let parent = node.parent.filter(|&p| p != i);
            let inherited = parent.map_or(frame, |p| turns[p]);
            let mut turn = inherited;
            if let Some((own, target, aim)) = node.follow {
                let default_aim = match aim {
                    Aim::Node(child) => (self.nodes[child].default.position - node.default.position).try_normalize(),
                    Aim::Forward => Some(Vec3::X),
                };
                if let (Some(from), Some(to)) = (default_aim, segment(own, target)) {
                    turn = Quat::from_rotation_arc(inherited * from, to) * inherited;
                }
                if let Some((l, r, left_bone, right_bone)) = node.twist {
                    let axis = default_aim.map(|d| turn * d);
                    let current = turn * (self.nodes[r].default.position - self.nodes[l].default.position);
                    if let (Some(axis), Some(target)) = (axis, segment(left_bone, right_bone)) {
                        let flat = |v: Vec3| (v - axis * v.dot(axis)).try_normalize();
                        if let (Some(a), Some(b)) = (flat(current), flat(target)) {
                            turn = Quat::from_rotation_arc(a, b) * turn;
                        }
                    }
                }
            }
            turns[i] = turn.normalize();
            positions[i] = match parent {
                Some(p) => positions[p] + turns[p] * (node.default.position - self.nodes[p].default.position),
                None => root + frame * node.default.position,
            };
        }

        // Stand the feet on the skater's feet; riding, on the deck.
        let shift = self.feet.and_then(|(l, r)| {
            let skater = (bone("LEFTFOOT")? + bone("RIGHTFOOT")?) * 0.5;
            let base = skater - (positions[l] + positions[r]) * 0.5;
            let along_up = deck
                .and_then(|deck| self.onto_deck(deck, up, &bone, [positions[l], positions[r]], base.dot(up)))
                .unwrap_or(0.0);
            Some(base + up * along_up)
        });
        for &i in self.order.iter().filter(|&&i| i < count) {
            let node = &self.nodes[i];
            Matrix {
                scale: node.default.scale,
                rotation: Mat3::from_quat(turns[i]) * node.default.rotation,
                position: positions[i] + shift.unwrap_or(Vec3::ZERO),
            }
            .write(&mut out[i * 13..i * 13 + 13]);
        }
        count
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::board::VERTEX_FLOATS;
    use crate::to_skate;
    use bevy_math::Mat4;

    fn node(name: &str, parent: Option<usize>, position: Vec3) -> NodeDefault {
        NodeDefault {
            name: name.into(),
            parent,
            inverse: Matrix {
                scale: 1.0,
                rotation: Mat3::IDENTITY,
                position: -position,
            },
        }
    }

    /// A pelvis over two feet, the ankles 0.03 world units up.
    fn rig() -> Rig {
        Rig::new(vec![
            node("bip01 pelvis", None, Vec3::new(0.0, 0.0, 0.3)),
            node("bip01 l foot", Some(0), Vec3::new(0.0, 0.05, 0.03)),
            node("bip01 r foot", Some(0), Vec3::new(0.0, -0.05, 0.03)),
        ])
    }

    /// The skater upright at the origin with its ankles `height` world units
    /// up, 0.2 apart along Halo's X.
    fn skater(height: f32) -> Pose {
        let at = |p: Vec3| Mat4::from_translation(to_skate(p));
        Pose {
            root: Mat4::IDENTITY,
            bones: vec![at(Vec3::new(-0.1, 0.0, height)), at(Vec3::new(0.1, 0.0, height))],
            names: vec!["LEFTFOOT".into(), "RIGHTFOOT".into()],
            camera: None,
            velocity: Vec3::ZERO,
            tick: 0,
            state: String::new(),
        }
    }

    /// A flat deck with its top at `top`, 1 world unit square.
    fn deck_at(top: f32) -> (Vec<f32>, Vec<u32>) {
        let vertices = [[-0.5, -0.5], [0.5, -0.5], [0.5, 0.5], [-0.5, 0.5]]
            .iter()
            .flat_map(|[x, y]| [*x, *y, top, 0.0, 0.0, 1.0, 0.0, 0.0])
            .collect::<Vec<f32>>();
        assert_eq!(vertices.len(), 4 * VERTEX_FLOATS);
        (vertices, vec![0, 1, 2, 0, 2, 3])
    }

    /// The posed feet's mean height.
    fn feet_height(rig: &Rig, pose: &Pose, deck: Option<&Deck>) -> f32 {
        let mut out = [0.0; 3 * 13];
        assert_eq!(rig.pose(pose, deck, &mut out), 3);
        (out[13 + 12] + out[2 * 13 + 12]) * 0.5
    }

    #[test]
    fn riding_feet_rest_on_the_deck() {
        let rig = rig();
        let (vertices, indices) = deck_at(1.0);
        let deck = Deck {
            vertices: &vertices,
            indices: &indices,
            ankle: 0.03,
            offset: 0.0,
            scale: 1.0,
        };
        // the skater's ankles 9 cm over the deck: the biped's 0.03 over it
        let pose = skater(1.0 + 0.09 / METRES);
        assert!((feet_height(&rig, &pose, Some(&deck)) - 1.03).abs() < 1e-4);
        // without the deck, on the skater's ankles as before
        assert!((feet_height(&rig, &pose, None) - (1.0 + 0.09 / METRES)).abs() < 1e-4);
        // the extra offset raises it
        let raised = Deck { offset: 0.01, ..deck };
        assert!((feet_height(&rig, &pose, Some(&raised)) - 1.04).abs() < 1e-4);
    }

    #[test]
    fn feet_off_the_deck_follow_the_skater() {
        let rig = rig();
        let (vertices, indices) = deck_at(1.0);
        let deck = Deck {
            vertices: &vertices,
            indices: &indices,
            ankle: 0.03,
            offset: 0.0,
            scale: 1.0,
        };
        // half a metre up (an ollie): the skater's ankles
        let pose = skater(1.0 + 0.5 / METRES);
        assert!((feet_height(&rig, &pose, Some(&deck)) - (1.0 + 0.5 / METRES)).abs() < 1e-4);
        // the board flipped away from under the feet: no deck there
        let (far, far_indices) = deck_at(1.0);
        let moved: Vec<f32> = far
            .chunks_exact(VERTEX_FLOATS)
            .flat_map(|v| {
                let mut v = v.to_vec();
                v[0] += 5.0;
                v
            })
            .collect();
        let away = Deck {
            vertices: &moved,
            indices: &far_indices,
            ..deck
        };
        let pose = skater(1.0 + 0.09 / METRES);
        assert!((feet_height(&rig, &pose, Some(&away)) - (1.0 + 0.09 / METRES)).abs() < 1e-4);
        // an unknown ankle height leaves the ankles on the skater's
        let unknown = Deck { ankle: 0.0, ..deck };
        assert!((feet_height(&rig, &pose, Some(&unknown)) - (1.0 + 0.09 / METRES)).abs() < 1e-4);
    }

    #[test]
    fn leaving_the_deck_fades_smoothly() {
        let rig = rig();
        let (vertices, indices) = deck_at(1.0);
        let deck = Deck {
            vertices: &vertices,
            indices: &indices,
            ankle: 0.03,
            offset: 0.0,
            scale: 1.0,
        };
        // rising through the fade, the feet never jump
        let mut last = feet_height(&rig, &skater(1.0 + 0.10 / METRES), Some(&deck));
        for step in 1..=60 {
            let height = 1.0 + (0.10 + step as f32 * 0.005) / METRES;
            let now = feet_height(&rig, &skater(height), Some(&deck));
            // (at most a few times as fast as the skater's feet)
            assert!((now - last).abs() < 3.0 * 0.005 / METRES, "a jump at {height}: {last} to {now}");
            assert!(now >= last - 1e-5, "sank at {height}");
            last = now;
        }
        assert!((near_deck(0.1 / METRES) - 1.0).abs() < 1e-6);
        assert_eq!(near_deck(0.4 / METRES), 0.0);
        assert_eq!(near_deck(-0.1 / METRES), 0.0);
    }

    /// The skater upright at the origin with its ankles where given.
    fn skater_feet(left: Vec3, right: Vec3) -> Pose {
        let at = |p: Vec3| Mat4::from_translation(to_skate(p));
        Pose {
            root: Mat4::IDENTITY,
            bones: vec![at(left), at(right)],
            names: vec!["LEFTFOOT".into(), "RIGHTFOOT".into()],
            camera: None,
            velocity: Vec3::ZERO,
            tick: 0,
            state: String::new(),
        }
    }

    /// A deck 1 world unit square sloping up along X: its top at
    /// z = 1 + 0.1 x, so that each foot has its own deck height.
    fn sloped_deck() -> (Vec<f32>, Vec<u32>) {
        let vertices = [[-0.5, -0.5], [0.5, -0.5], [0.5, 0.5], [-0.5, 0.5]]
            .iter()
            .flat_map(|[x, y]| [*x, *y, 1.0 + 0.1 * x, 0.0, 0.0, 1.0, 0.0, 0.0])
            .collect::<Vec<f32>>();
        (vertices, vec![0, 1, 2, 0, 2, 3])
    }

    /// The posed left and right ankles' heights.
    fn ankles(rig: &Rig, pose: &Pose, deck: &Deck) -> (f32, f32) {
        let mut out = [0.0; 3 * 13];
        assert_eq!(rig.pose(pose, Some(deck), &mut out), 3);
        (out[13 + 12], out[2 * 13 + 12])
    }

    const RIDING: f32 = 0.09 / METRES;

    #[test]
    fn a_push_stands_on_the_other_foot() {
        let rig = rig();
        let (vertices, indices) = sloped_deck();
        let deck = Deck {
            vertices: &vertices,
            indices: &indices,
            ankle: 0.03,
            offset: 0.0,
            scale: 1.0,
        };
        // both on the deck (tops 0.99 and 1.01): the biped's ankles 0.03
        // over their middle
        let both = skater_feet(Vec3::new(-0.1, 0.0, 0.99 + RIDING), Vec3::new(0.1, 0.0, 1.01 + RIDING));
        let (l, r) = ankles(&rig, &both, &deck);
        assert!((l - 1.03).abs() < 1e-4 && (r - 1.03).abs() < 1e-4, "{l} {r}");
        // the right foot down at the ground beside the board, pushing: the
        // left alone stands on the deck, its ankle 0.03 over its top
        let push = skater_feet(Vec3::new(-0.1, 0.0, 0.99 + RIDING), Vec3::new(0.1, 0.8, 0.6));
        let (l, _) = ankles(&rig, &push, &deck);
        assert!((l - 1.02).abs() < 1e-4, "{l}");
        // the right foot lifted high over the deck: the same
        let lifted = skater_feet(Vec3::new(-0.1, 0.0, 0.99 + RIDING), Vec3::new(0.1, 0.0, 1.5));
        let (l, _) = ankles(&rig, &lifted, &deck);
        assert!((l - 1.02).abs() < 1e-4, "{l}");
        // and the extra offset still raises it
        let raised = Deck { offset: 0.01, ..deck };
        let (l, _) = ankles(&rig, &push, &raised);
        assert!((l - 1.03).abs() < 1e-4, "{l}");
    }

    #[test]
    fn a_foot_leaving_and_coming_back_moves_nothing_at_once() {
        let rig = rig();
        let (vertices, indices) = sloped_deck();
        let deck = Deck {
            vertices: &vertices,
            indices: &indices,
            ankle: 0.03,
            offset: 0.0,
            scale: 1.0,
        };
        let left = Vec3::new(-0.1, 0.0, 0.99 + RIDING);
        let start = Vec3::new(0.1, 0.0, 1.01 + RIDING);
        // a push: the right foot out over the board's side and down to the
        // ground, then back; and a lift: straight up and back down
        let push = [start, Vec3::new(0.1, 0.45, 1.01 + RIDING), Vec3::new(0.1, 0.7, 0.6), start];
        let lift = [start, Vec3::new(0.1, 0.0, 1.6), start];
        for path in [&push[..], &lift[..]] {
            let (mut last, _) = ankles(&rig, &skater_feet(left, start), &deck);
            let first = last;
            for leg in path.windows(2) {
                for step in 1..=200 {
                    let right = leg[0].lerp(leg[1], step as f32 / 200.0);
                    let (now, _) = ankles(&rig, &skater_feet(left, right), &deck);
                    // (the foot moves at most 0.005 a step)
                    assert!((now - last).abs() < 0.003, "a jump with the right foot at {right}: {last} to {now}");
                    // the left foot stays on the deck, between the two
                    // heights the feet ask for
                    assert!((1.02 - 1e-4..=1.03 + 1e-4).contains(&now), "{now} with the right foot at {right}");
                    last = now;
                }
            }
            assert!((last - first).abs() < 1e-5);
        }
    }
}

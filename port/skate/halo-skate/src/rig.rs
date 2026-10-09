//! The biped posed as the skater. Each mapped node keeps its own bone lengths
//! and turns so the segment to its child points where the skater's matching
//! segment points; a node without a mapping turns with its parent. The pelvis
//! and the chest also take the twist of the hips and shoulders. The skeleton
//! then moves so its feet sit where the skater's feet are.

use crate::{direction_from_skate, from_skate};
use bevy_math::{Mat3, Quat, Vec3};
use skate_host::bridge::Pose;

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
        Self { nodes, order, feet }
    }

    pub fn mapped(&self) -> usize {
        self.nodes.iter().filter(|n| n.follow.is_some()).count()
    }

    /// Writes every node's world matrix posed as `pose`, returning how many.
    pub fn pose(&self, pose: &Pose, out: &mut [f32]) -> usize {
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

        // Stand the feet on the skater's feet.
        let shift = self.feet.and_then(|(l, r)| {
            let skater = (bone("LEFTFOOT")? + bone("RIGHTFOOT")?) * 0.5;
            Some(skater - (positions[l] + positions[r]) * 0.5)
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

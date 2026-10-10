//! The skateboard, drawn by the game (port/linux/game/skate.c).
//!
//! The board's meshes come from the skater model the Skate 3 converter
//! writes, `private/skater.glb`: the primitives of its skinned meshes whose
//! material is named for the board ("Skate"), as the mashup's board export
//! (`crates/assets/src/skate_board.rs`) picks them. Each tick the game takes
//! the board skinned by the skater's last pose, in world units, and draws it.

use crate::{direction_from_skate, from_skate};
use bevy_math::{Mat4, Vec2, Vec3, Vec4};
use skate_host::bridge::Pose;
use std::path::Path;

/// Larger textures are box-filtered down to fit, as the mashup does.
const TEXTURE_LIMIT: u32 = 512;
/// Floats in a skinned vertex: position, normal, texture coordinate.
pub const VERTEX_FLOATS: usize = 8;

pub struct Joint {
    pub name: String,
    pub inverse_bind: Mat4,
}

pub struct Vertex {
    pub position: Vec3,
    pub normal: Vec3,
    pub uv: Vec2,
    pub joints: [u16; 4],
    pub weights: [f32; 4],
}

pub struct Surface {
    pub first_index: u32,
    pub index_count: u32,
    pub texture: u32,
}

pub struct Texture {
    pub width: u32,
    pub height: u32,
    pub rgba: Vec<u8>,
}

pub struct Board {
    pub joints: Vec<Joint>,
    pub vertices: Vec<Vertex>,
    pub indices: Vec<u32>,
    pub surfaces: Vec<Surface>,
    pub textures: Vec<Texture>,
}

impl Board {
    pub fn load(assets: &Path) -> Result<Self, String> {
        let path = assets.join("private").join("skater.glb");
        let bytes = std::fs::read(&path).map_err(|e| format!("cannot read {}: {e}", path.display()))?;
        Self::from_glb(&bytes).map_err(|e| format!("{}: {e}", path.display()))
    }

    pub fn from_glb(bytes: &[u8]) -> Result<Self, String> {
        let gltf = gltf::Gltf::from_slice(bytes).map_err(|e| e.to_string())?;
        let blob = gltf.blob.as_deref();
        let buffers = |buffer: gltf::Buffer| match buffer.source() {
            gltf::buffer::Source::Bin => blob,
            gltf::buffer::Source::Uri(_) => None,
        };
        let mut board = Board {
            joints: Vec::new(),
            vertices: Vec::new(),
            indices: Vec::new(),
            surfaces: Vec::new(),
            textures: Vec::new(),
        };
        // (each image once, however many board materials share it)
        let mut texture_of_image: Vec<(usize, u32)> = Vec::new();
        for node in gltf.nodes() {
            let (Some(mesh), Some(skin)) = (node.mesh(), node.skin()) else {
                continue;
            };
            let board_primitives: Vec<_> = mesh
                .primitives()
                .filter(|p| p.mode() == gltf::mesh::Mode::Triangles)
                .filter(|p| p.material().name().is_some_and(|n| n.contains("Skate")))
                .collect();
            if board_primitives.is_empty() {
                continue;
            }

            // this skin's joints, after those of any skin before it
            let joint_base = board.joints.len();
            let inverse_binds: Vec<Mat4> = skin
                .reader(buffers)
                .read_inverse_bind_matrices()
                .map(|m| m.map(|c| Mat4::from_cols_array_2d(&c)).collect())
                .unwrap_or_default();
            for (i, joint) in skin.joints().enumerate() {
                board.joints.push(Joint {
                    name: joint.name().unwrap_or_default().to_owned(),
                    inverse_bind: inverse_binds.get(i).copied().unwrap_or(Mat4::IDENTITY),
                });
            }
            let joint_count = board.joints.len() - joint_base;

            for primitive in board_primitives {
                let material = primitive.material();
                let reader = primitive.reader(buffers);
                let positions: Vec<[f32; 3]> = reader.read_positions().ok_or("a board primitive has no positions")?.collect();
                let count = positions.len();
                let normals: Vec<[f32; 3]> = reader
                    .read_normals()
                    .map(|n| n.collect())
                    .unwrap_or_else(|| vec![[0.0, 0.0, 1.0]; count]);
                let uvs: Vec<[f32; 2]> = reader
                    .read_tex_coords(0)
                    .map(|t| t.into_f32().collect())
                    .unwrap_or_else(|| vec![[0.0; 2]; count]);
                let joints: Vec<[u16; 4]> = reader
                    .read_joints(0)
                    .ok_or("a board primitive is not skinned")?
                    .into_u16()
                    .collect();
                let weights: Vec<[f32; 4]> = reader
                    .read_weights(0)
                    .ok_or("a board primitive is not skinned")?
                    .into_f32()
                    .collect();
                if normals.len() != count || uvs.len() != count || joints.len() != count || weights.len() != count {
                    return Err(format!("{}: vertex streams disagree", material.name().unwrap_or("")));
                }

                let vertex_base = board.vertices.len() as u32;
                for v in 0..count {
                    let mut vertex_joints = [0u16; 4];
                    let mut vertex_weights = weights[v];
                    for i in 0..4 {
                        let joint = joints[v][i] as usize;
                        if joint >= joint_count || !(vertex_weights[i] > 0.0) {
                            vertex_weights[i] = 0.0;
                        } else {
                            vertex_joints[i] = (joint_base + joint) as u16;
                        }
                    }
                    let sum: f32 = vertex_weights.iter().sum();
                    if sum <= 0.0 {
                        return Err(format!("{}: a vertex has no joint", material.name().unwrap_or("")));
                    }
                    board.vertices.push(Vertex {
                        position: Vec3::from_array(positions[v]),
                        normal: Vec3::from_array(normals[v]),
                        uv: Vec2::from_array(uvs[v]),
                        joints: vertex_joints,
                        weights: vertex_weights.map(|w| w / sum),
                    });
                }
                let first_index = board.indices.len() as u32;
                match reader.read_indices() {
                    Some(indices) => board.indices.extend(indices.into_u32().map(|i| vertex_base + i)),
                    None => board.indices.extend(vertex_base..vertex_base + count as u32),
                }
                board.indices.truncate(first_index as usize + (board.indices.len() - first_index as usize) / 3 * 3);
                if board.indices[first_index as usize..].iter().any(|&i| i >= vertex_base + count as u32) {
                    return Err(format!("{}: an index is out of range", material.name().unwrap_or("")));
                }

                let image = material
                    .pbr_metallic_roughness()
                    .base_color_texture()
                    .map(|info| info.texture().source());
                let texture = match image {
                    Some(image) => match texture_of_image.iter().find(|(i, _)| *i == image.index()) {
                        Some(&(_, texture)) => texture,
                        None => {
                            let texture = board.textures.len() as u32;
                            board.textures.push(decode_image(&image, blob)?);
                            texture_of_image.push((image.index(), texture));
                            texture
                        }
                    },
                    // untextured: white
                    None => {
                        board.textures.push(Texture {
                            width: 1,
                            height: 1,
                            rgba: vec![255; 4],
                        });
                        board.textures.len() as u32 - 1
                    }
                };
                board.surfaces.push(Surface {
                    first_index,
                    index_count: board.indices.len() as u32 - first_index,
                    texture,
                });
            }
        }
        if board.surfaces.is_empty() {
            return Err("no skateboard meshes (skinned, with a material named for the board)".into());
        }
        Ok(board)
    }

    /// Writes the board's vertices skinned by `pose` (VERTEX_FLOATS each:
    /// position in world units, unit normal, texture coordinate), grown
    /// `scale` times about its middle (Master Chief is larger than the
    /// skater the board was made for), and returns how many, or an error
    /// naming a joint the pose lacks.
    pub fn skin(&self, pose: &Pose, scale: f32, out: &mut [f32]) -> Result<usize, String> {
        // the converter's bone basis to the engine's (the mashup's `rb`)
        let basis = Mat4::from_cols(Vec4::X, -Vec4::Z, Vec4::Y, Vec4::W);
        let matrices = self
            .joints
            .iter()
            .map(|joint| {
                let index = pose.names.iter().position(|n| *n == joint.name);
                index
                    .and_then(|i| pose.bones.get(i))
                    .map(|bone| pose.root * *bone * basis * joint.inverse_bind)
            })
            .collect::<Vec<_>>();
        let count = self.vertices.len().min(out.len() / VERTEX_FLOATS);
        for (vertex, out) in self.vertices.iter().zip(out.chunks_exact_mut(VERTEX_FLOATS)) {
            let mut position = Vec3::ZERO;
            let mut normal = Vec3::ZERO;
            for (&joint, &weight) in vertex.joints.iter().zip(&vertex.weights) {
                if weight == 0.0 {
                    continue;
                }
                let joint = joint as usize;
                let matrix = matrices[joint].ok_or_else(|| format!("the skater has no bone {}", self.joints[joint].name))?;
                position += matrix.transform_point3(vertex.position) * weight;
                normal += matrix.transform_vector3(vertex.normal) * weight;
            }
            out[0..3].copy_from_slice(&from_skate(position).to_array());
            out[3..6].copy_from_slice(&direction_from_skate(normal).normalize_or(Vec3::Z).to_array());
            out[6..8].copy_from_slice(&vertex.uv.to_array());
        }
        scale_about_middle(&mut out[..count * VERTEX_FLOATS], scale);
        Ok(count)
    }
}

/// Grows skinned vertices (VERTEX_FLOATS each) `scale` times about their
/// middle, the mean of their positions, which moves and turns with the
/// board. Normals keep their direction under a uniform scale.
pub fn scale_about_middle(vertices: &mut [f32], scale: f32) {
    let count = vertices.len() / VERTEX_FLOATS;
    if count == 0 || !scale.is_finite() || scale <= 0.0 || scale == 1.0 {
        return;
    }
    let position = |v: &[f32]| Vec3::new(v[0], v[1], v[2]);
    let middle = vertices
        .chunks_exact(VERTEX_FLOATS)
        .fold(Vec3::ZERO, |sum, v| sum + position(v))
        / count as f32;
    for v in vertices.chunks_exact_mut(VERTEX_FLOATS) {
        let scaled = middle + (position(v) - middle) * scale;
        v[0..3].copy_from_slice(&scaled.to_array());
    }
}

/// How the deck's top was found under a foot (`deck_top`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DeckHit {
    /// a triangle crosses the line through the foot along the board's up
    Triangle,
    /// no triangle does: the highest vertex within the radius
    Vertex,
}

/// The height along `up` (a unit vector) of the top of the skinned board
/// (VERTEX_FLOATS a vertex) under `point`: the highest of the board's
/// triangles that the line through `point` along `up` crosses, or, when it
/// crosses none (a foot just past the deck's end), the highest vertex within
/// `radius` of that line. None when there is neither, as when the board has
/// flipped away from the foot.
pub fn deck_top(vertices: &[f32], indices: &[u32], point: Vec3, up: Vec3, radius: f32) -> Option<(f32, DeckHit)> {
    let count = vertices.len() / VERTEX_FLOATS;
    let position = |i: usize| Vec3::new(vertices[i * VERTEX_FLOATS], vertices[i * VERTEX_FLOATS + 1], vertices[i * VERTEX_FLOATS + 2]);
    // the board's plane, with `point` at its origin
    let side = up.any_orthonormal_vector();
    let other = up.cross(side);
    let flat = |p: Vec3| Vec2::new((p - point).dot(side), (p - point).dot(other));

    let mut best: Option<f32> = None;
    for triangle in indices.chunks_exact(3) {
        let [a, b, c] = [triangle[0], triangle[1], triangle[2]].map(|i| i as usize);
        if a >= count || b >= count || c >= count {
            continue;
        }
        let (pa, pb, pc) = (position(a), position(b), position(c));
        let (fa, fb, fc) = (flat(pa), flat(pb), flat(pc));
        // the barycentric coordinates of the plane's origin, where the line is
        let area = (fb - fa).perp_dot(fc - fa);
        if area.abs() < 1e-12 {
            continue;
        }
        let wb = (-fa).perp_dot(fc - fa) / area;
        let wc = (fb - fa).perp_dot(-fa) / area;
        let wa = 1.0 - wb - wc;
        if wa < 0.0 || wb < 0.0 || wc < 0.0 {
            continue;
        }
        let height = (pa * wa + pb * wb + pc * wc).dot(up);
        best = Some(best.map_or(height, |h| h.max(height)));
    }
    if let Some(height) = best {
        return Some((height, DeckHit::Triangle));
    }
    (0..count)
        .map(position)
        .filter(|&p| flat(p).length_squared() <= radius * radius)
        .map(|p| p.dot(up))
        .reduce(f32::max)
        .map(|height| (height, DeckHit::Vertex))
}

fn decode_image(image: &gltf::Image, blob: Option<&[u8]>) -> Result<Texture, String> {
    let bytes = match image.source() {
        gltf::image::Source::View { view, .. } => {
            let blob = blob.ok_or("the board's texture is outside the file")?;
            blob.get(view.offset()..view.offset() + view.length())
                .ok_or("the board's texture is outside the file")?
        }
        gltf::image::Source::Uri { .. } => return Err("the board's texture is outside the file".into()),
    };
    let mut decoder = png::Decoder::new(std::io::Cursor::new(bytes));
    decoder.set_transformations(png::Transformations::normalize_to_color8());
    let mut reader = decoder.read_info().map_err(|e| format!("board texture: {e}"))?;
    let mut pixels = vec![0; reader.output_buffer_size().ok_or("board texture too large")?];
    let frame = reader.next_frame(&mut pixels).map_err(|e| format!("board texture: {e}"))?;
    let channels = frame.color_type.samples();
    let rgba: Vec<u8> = pixels[..frame.buffer_size()]
        .chunks_exact(channels)
        .flat_map(|p| match channels {
            1 => [p[0], p[0], p[0], 255],
            2 => [p[0], p[0], p[0], p[1]],
            3 => [p[0], p[1], p[2], 255],
            _ => [p[0], p[1], p[2], p[3]],
        })
        .collect();
    Ok(shrink(frame.width, frame.height, rgba))
}

/// Box-filters an image to fit within `TEXTURE_LIMIT` on both sides, keeping
/// its aspect ratio. Smaller images pass through.
fn shrink(width: u32, height: u32, rgba: Vec<u8>) -> Texture {
    if width <= TEXTURE_LIMIT && height <= TEXTURE_LIMIT {
        return Texture { width, height, rgba };
    }
    let scale = (f64::from(TEXTURE_LIMIT) / f64::from(width)).min(f64::from(TEXTURE_LIMIT) / f64::from(height));
    let out_w = ((f64::from(width) * scale).round() as u32).max(1);
    let out_h = ((f64::from(height) * scale).round() as u32).max(1);
    let mut out = Vec::with_capacity((out_w * out_h * 4) as usize);
    for y in 0..out_h {
        let y0 = y * height / out_h;
        let y1 = ((y + 1) * height / out_h).max(y0 + 1);
        for x in 0..out_w {
            let x0 = x * width / out_w;
            let x1 = ((x + 1) * width / out_w).max(x0 + 1);
            let mut sum = [0u32; 4];
            for sy in y0..y1 {
                for sx in x0..x1 {
                    let at = ((sy * width + sx) * 4) as usize;
                    for (c, total) in sum.iter_mut().enumerate() {
                        *total += u32::from(rgba[at + c]);
                    }
                }
            }
            let n = (y1 - y0) * (x1 - x0);
            out.extend(sum.map(|s| ((s + n / 2) / n) as u8));
        }
    }
    Texture {
        width: out_w,
        height: out_h,
        rgba: out,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::METRES;

    fn png(width: u32, height: u32, rgba: &[u8]) -> Vec<u8> {
        let mut bytes = Vec::new();
        {
            let mut encoder = png::Encoder::new(&mut bytes, width, height);
            encoder.set_color(png::ColorType::Rgba);
            encoder.set_depth(png::BitDepth::Eight);
            let mut writer = encoder.write_header().unwrap();
            writer.write_image_data(rgba).unwrap();
        }
        bytes
    }

    /// A GLB of one skinned mesh: a board triangle on joint 1 ("DECK") and a
    /// skater triangle on joint 0 ("HIPS"), each with its own material.
    fn glb(texture: &[u8]) -> Vec<u8> {
        let mut bin: Vec<u8> = Vec::new();
        let mut views = Vec::new();
        let mut push = |bin: &mut Vec<u8>, data: &[u8]| {
            while bin.len() % 4 != 0 {
                bin.push(0);
            }
            views.push(serde_json::json!({"buffer": 0, "byteOffset": bin.len(), "byteLength": data.len()}));
            bin.extend_from_slice(data);
            views.len() - 1
        };
        let floats = |v: &[f32]| v.iter().flat_map(|f| f.to_le_bytes()).collect::<Vec<u8>>();
        let positions = push(&mut bin, &floats(&[1.0, 2.0, 3.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0]));
        let normals = push(&mut bin, &floats(&[0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0]));
        let uvs = push(&mut bin, &floats(&[0.0, 0.0, 1.0, 0.0, 0.0, 1.0]));
        let board_joints = push(&mut bin, &[1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]);
        let skater_joints = push(&mut bin, &[0; 12]);
        let weights = push(&mut bin, &floats(&[1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0]));
        let indices = push(&mut bin, &[0u16, 1, 2].iter().flat_map(|i| i.to_le_bytes()).collect::<Vec<u8>>());
        // joint 1's bind pose is 1 m up its own Y: its inverse moves 1 m down
        let mut binds = Mat4::IDENTITY.to_cols_array().to_vec();
        binds.extend(Mat4::from_translation(Vec3::new(0.0, -1.0, 0.0)).to_cols_array());
        let binds = push(&mut bin, &floats(&binds));
        let image = push(&mut bin, texture);
        let accessor = |view: usize, component: u32, count: usize, kind: &str| {
            serde_json::json!({"bufferView": view, "componentType": component, "count": count, "type": kind})
        };
        let mut position_accessor = accessor(positions, 5126, 3, "VEC3");
        position_accessor["min"] = serde_json::json!([0.0, 0.0, 0.0]);
        position_accessor["max"] = serde_json::json!([1.0, 2.0, 3.0]);
        let json = serde_json::json!({
            "asset": {"version": "2.0"},
            "scene": 0,
            "scenes": [{"nodes": [0, 1, 2]}],
            "nodes": [
                {"name": "HIPS"},
                {"name": "DECK"},
                {"name": "skater", "mesh": 0, "skin": 0}
            ],
            "skins": [{"joints": [0, 1], "inverseBindMatrices": 7}],
            "meshes": [{"primitives": [
                {"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2, "JOINTS_0": 4, "WEIGHTS_0": 5},
                 "indices": 6, "material": 1},
                {"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2, "JOINTS_0": 3, "WEIGHTS_0": 5},
                 "indices": 6, "material": 0}
            ]}],
            "materials": [
                {"name": "SkateDeck", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}},
                {"name": "Body"}
            ],
            "textures": [{"source": 0}],
            "images": [{"bufferView": image, "mimeType": "image/png"}],
            "accessors": [
                position_accessor,
                accessor(normals, 5126, 3, "VEC3"),
                accessor(uvs, 5126, 3, "VEC2"),
                accessor(board_joints, 5121, 3, "VEC4"),
                accessor(skater_joints, 5121, 3, "VEC4"),
                accessor(weights, 5126, 3, "VEC4"),
                accessor(indices, 5123, 3, "SCALAR"),
                accessor(binds, 5126, 2, "MAT4")
            ],
            "bufferViews": views,
            "buffers": [{"byteLength": bin.len()}]
        });
        while bin.len() % 4 != 0 {
            bin.push(0);
        }
        let mut json = serde_json::to_vec(&json).unwrap();
        while json.len() % 4 != 0 {
            json.push(b' ');
        }
        let mut out = Vec::new();
        out.extend_from_slice(b"glTF");
        out.extend_from_slice(&2u32.to_le_bytes());
        out.extend_from_slice(&((12 + 8 + json.len() + 8 + bin.len()) as u32).to_le_bytes());
        out.extend_from_slice(&(json.len() as u32).to_le_bytes());
        out.extend_from_slice(b"JSON");
        out.extend_from_slice(&json);
        out.extend_from_slice(&(bin.len() as u32).to_le_bytes());
        out.extend_from_slice(b"BIN\0");
        out.extend_from_slice(&bin);
        out
    }

    fn pose(root: Mat4, deck: Mat4) -> Pose {
        Pose {
            root,
            bones: vec![Mat4::IDENTITY, deck],
            names: vec!["HIPS".into(), "DECK".into()],
            camera: None,
            velocity: Vec3::ZERO,
            tick: 0,
            state: String::new(),
        }
    }

    fn close(a: [f32; 3], b: Vec3) {
        assert!((Vec3::from_array(a) - b).length() < 1e-5, "{a:?} != {b:?}");
    }

    #[test]
    fn loads_only_the_board() {
        let board = Board::from_glb(&glb(&png(2, 1, &[255, 0, 0, 255, 0, 0, 255, 255]))).unwrap();
        assert_eq!(board.surfaces.len(), 1);
        assert_eq!(board.vertices.len(), 3);
        assert_eq!(board.indices, vec![0, 1, 2]);
        assert_eq!(board.vertices[0].joints[0], 1);
        assert_eq!((board.textures[0].width, board.textures[0].height), (2, 1));
        assert_eq!(board.textures[0].rgba, vec![255, 0, 0, 255, 0, 0, 255, 255]);
    }

    #[test]
    fn skins_into_world_units() {
        let board = Board::from_glb(&glb(&png(1, 1, &[9, 9, 9, 255]))).unwrap();
        let mut out = vec![0.0; 3 * VERTEX_FLOATS];

        // The deck bone at its bind pose, with the skater at the origin: the
        // converter's mesh space is then Z up in metres, so a vertex lands at
        // its own coordinates over METRES, and its +Z normal points up.
        let bind = Mat4::from_translation(Vec3::new(0.0, 1.0, 0.0));
        let rb = Mat4::from_cols(Vec4::X, -Vec4::Z, Vec4::Y, Vec4::W);
        let at_bind = rb * bind * rb.inverse();
        assert_eq!(board.skin(&pose(Mat4::IDENTITY, at_bind), 1.0, &mut out).unwrap(), 3);
        close([out[0], out[1], out[2]], Vec3::new(1.0, 2.0, 3.0) / METRES);
        close([out[3], out[4], out[5]], Vec3::Z);
        assert_eq!([out[6], out[7]], [0.0, 0.0]);
        assert_eq!([out[VERTEX_FLOATS + 6], out[VERTEX_FLOATS + 7]], [1.0, 0.0]);

        // The skater 3.048 m along Skate's -Z (Halo's +Y) and turned a
        // quarter about Skate's Y (Halo's Z): the vertex turns and moves one
        // world unit along Halo's Y.
        let root = Mat4::from_translation(Vec3::new(0.0, 0.0, -METRES)) * Mat4::from_rotation_y(std::f32::consts::FRAC_PI_2);
        board.skin(&pose(root, at_bind), 1.0, &mut out).unwrap();
        // a quarter turn about Skate's +Y is a quarter turn about Halo's +Z
        let turned = Vec3::new(-2.0, 1.0, 3.0) / METRES;
        close([out[0], out[1], out[2]], turned + Vec3::Y);
        close([out[3], out[4], out[5]], Vec3::Z);
    }

    #[test]
    fn a_missing_bone_is_an_error() {
        let board = Board::from_glb(&glb(&png(1, 1, &[9, 9, 9, 255]))).unwrap();
        let mut pose = pose(Mat4::IDENTITY, Mat4::IDENTITY);
        pose.names[1] = "OTHER".into();
        assert!(board.skin(&pose, 1.0, &mut [0.0; 3 * VERTEX_FLOATS]).is_err());
    }

    /// A deck 0.8 by 0.2 world units with its top at z = 1 (two triangles), a
    /// plate under it, a truck vertex below its middle, and a kicked-up tail
    /// vertex past its end.
    fn synthetic_board() -> (Vec<f32>, Vec<u32>) {
        let points = [
            [-0.4, -0.1, 1.0],
            [0.4, -0.1, 1.0],
            [0.4, 0.1, 1.0],
            [-0.4, 0.1, 1.0],
            [-0.4, -0.1, 0.97],
            [0.4, -0.1, 0.97],
            [0.4, 0.1, 0.97],
            [-0.4, 0.1, 0.97],
            [0.0, 0.0, 0.9],
            [-0.45, 0.0, 1.03],
        ];
        let vertices = points
            .iter()
            .flat_map(|p| [p[0], p[1], p[2], 0.0, 0.0, 1.0, 0.5, 0.5])
            .collect();
        (vertices, vec![0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6])
    }

    #[test]
    fn the_deck_top_is_measured_under_a_foot() {
        let (vertices, indices) = synthetic_board();
        // above the deck, or below it: the top triangle, not the plate
        let (height, hit) = deck_top(&vertices, &indices, Vec3::new(0.1, 0.05, 1.08), Vec3::Z, 0.03).unwrap();
        assert!((height - 1.0).abs() < 1e-5 && hit == DeckHit::Triangle, "{height} {hit:?}");
        let (height, _) = deck_top(&vertices, &indices, Vec3::new(-0.3, 0.0, 0.5), Vec3::Z, 0.03).unwrap();
        assert!((height - 1.0).abs() < 1e-5, "{height}");
        // past the deck's end: the tail's vertex within the radius
        let (height, hit) = deck_top(&vertices, &indices, Vec3::new(-0.43, 0.0, 1.1), Vec3::Z, 0.03).unwrap();
        assert!((height - 1.03).abs() < 1e-5 && hit == DeckHit::Vertex, "{height} {hit:?}");
        // nowhere near the board
        assert!(deck_top(&vertices, &indices, Vec3::new(2.0, 0.0, 1.1), Vec3::Z, 0.03).is_none());
    }

    #[test]
    fn the_deck_top_follows_a_tilted_board() {
        let (mut vertices, indices) = synthetic_board();
        // the board banked 30 degrees about X, and moved
        let turn = bevy_math::Quat::from_rotation_x(0.5236);
        let offset = Vec3::new(3.0, -2.0, 0.5);
        for v in vertices.chunks_exact_mut(VERTEX_FLOATS) {
            let p = turn * Vec3::new(v[0], v[1], v[2]) + offset;
            v[0..3].copy_from_slice(&p.to_array());
        }
        let up = turn * Vec3::Z;
        let foot = turn * Vec3::new(0.2, -0.05, 1.1) + offset;
        let (height, hit) = deck_top(&vertices, &indices, foot, up, 0.03).unwrap();
        let expected = (turn * Vec3::new(0.2, -0.05, 1.0) + offset).dot(up);
        assert!((height - expected).abs() < 1e-4 && hit == DeckHit::Triangle, "{height} != {expected}");
    }

    #[test]
    fn the_board_grows_about_its_middle() {
        let (mut vertices, _) = synthetic_board();
        let before = vertices.clone();
        let middle = before
            .chunks_exact(VERTEX_FLOATS)
            .fold(Vec3::ZERO, |sum, v| sum + Vec3::new(v[0], v[1], v[2]))
            / 10.0;
        scale_about_middle(&mut vertices, 1.05);
        for (after, before) in vertices.chunks_exact(VERTEX_FLOATS).zip(before.chunks_exact(VERTEX_FLOATS)) {
            let a = Vec3::new(after[0], after[1], after[2]);
            let b = Vec3::new(before[0], before[1], before[2]);
            assert!((a - (middle + (b - middle) * 1.05)).length() < 1e-5);
            // normals and texture coordinates untouched
            assert_eq!(after[3..], before[3..]);
        }
        // the deck 5% longer and thicker
        let length = vertices[VERTEX_FLOATS] - vertices[0];
        assert!((length - 0.84).abs() < 1e-5, "{length}");
        let thickness = vertices[2] - vertices[4 * VERTEX_FLOATS + 2];
        assert!((thickness - 0.0315).abs() < 1e-5, "{thickness}");
        // its middle where it was
        let after = vertices
            .chunks_exact(VERTEX_FLOATS)
            .fold(Vec3::ZERO, |sum, v| sum + Vec3::new(v[0], v[1], v[2]))
            / 10.0;
        assert!((after - middle).length() < 1e-5);
        // a scale of 1, or one that makes no sense, leaves it alone
        let mut same = before.clone();
        scale_about_middle(&mut same, 1.0);
        scale_about_middle(&mut same, f32::NAN);
        scale_about_middle(&mut same, -2.0);
        assert_eq!(same, before);
    }

    #[test]
    fn skinning_grows_the_board() {
        let board = Board::from_glb(&glb(&png(1, 1, &[9, 9, 9, 255]))).unwrap();
        let bind = Mat4::from_translation(Vec3::new(0.0, 1.0, 0.0));
        let rb = Mat4::from_cols(Vec4::X, -Vec4::Z, Vec4::Y, Vec4::W);
        let at_bind = rb * bind * rb.inverse();
        let mut plain = vec![0.0; 3 * VERTEX_FLOATS];
        let mut grown = vec![0.0; 3 * VERTEX_FLOATS];
        board.skin(&pose(Mat4::IDENTITY, at_bind), 1.0, &mut plain).unwrap();
        board.skin(&pose(Mat4::IDENTITY, at_bind), 1.05, &mut grown).unwrap();
        scale_about_middle(&mut plain, 1.05);
        assert_eq!(plain, grown);
    }

    #[test]
    fn large_textures_shrink() {
        let texture = shrink(1024, 256, vec![100; 1024 * 256 * 4]);
        assert_eq!((texture.width, texture.height), (512, 128));
        assert!(texture.rgba.iter().all(|&c| c == 100));
    }

    #[test]
    fn a_model_without_a_board_fails() {
        let mut bytes = glb(&png(1, 1, &[9, 9, 9, 255]));
        // rename the board's material (same length, so the GLB stays valid)
        let at = bytes.windows(9).position(|w| w == b"SkateDeck").unwrap();
        bytes[at..at + 9].copy_from_slice(b"PlainDeck");
        assert!(Board::from_glb(&bytes).is_err());
    }
}

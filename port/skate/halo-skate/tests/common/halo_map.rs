//! A level's collision read from a Halo Xbox map (`HALO_SKATE_TEST_HALO_MAPS`:
//! the folder of the owner's own `.map` files, never part of this repository),
//! as the triangles the game sends `halo_skate_load` for it
//! (`port/linux/game/skate.c`, `skate_collision_triangles`).
//!
//! The cache file's layout is OpenCE's own (`source/cache/cache_files.c`,
//! `source/scenario/scenario_definitions.h`,
//! `source/structures/structure_bsp_definitions.h`,
//! `source/physics/collision_bsp_definitions.h`, `bsp3d.h`), mirrored here:
//!
//! - a 0x800-byte header (`head` ... `foot`), and after it the rest of the
//!   file deflated (zlib) on the Xbox: the game reads it decompressed, the
//!   header followed by the inflated rest;
//! - at `tag_data_offset` the tags, loaded at 0x803A6000: a tag header
//!   (`tags`) with the tag instances (0x20 bytes each) and the scenario's tag
//!   index;
//! - the scenario's structure BSP references (0x20 bytes each, its block at
//!   0x5A4), each a BSP's place in the file, loaded at its own base address,
//!   which starts with a header (`sbsp`) pointing at the `structure_bsp`;
//! - the `structure_bsp`'s collision BSP block (at 0xB0), whose first BSP is
//!   the level's collision (`global_collision_bsp`): its planes, surfaces,
//!   edges and vertices.
//!
//! The data read from a map stays in memory: nothing here writes it anywhere.
#![allow(dead_code)]

use std::io::Read;
use std::path::{Path, PathBuf};

const HEADER_SIZE: usize = 0x800;
const TAG_CACHE_BASE_ADDRESS: u32 = 0x803A_6000;
const SCENARIO_STRUCTURE_BSP_REFERENCES: u32 = 0x5A4;
const STRUCTURE_BSP_COLLISION_BSP: u32 = 0xB0;
const MAXIMUM_VERTICES_PER_COLLISION_SURFACE: usize = 8;
/// `SKATE_TWO_SIDED_FLAG` in skate.c: a two-sided surface is sent both ways.
const TWO_SIDED: u8 = 0x01;

/// The folder of Halo Xbox maps (`HALO_SKATE_TEST_HALO_MAPS`), or None (the
/// test is skipped) when it is unset. Set but without the map fails the test.
pub fn halo_maps(test: &str, map: &str) -> Option<PathBuf> {
    match std::env::var_os("HALO_SKATE_TEST_HALO_MAPS") {
        Some(path) if !path.is_empty() => {
            let path = PathBuf::from(path).join(format!("{map}.map"));
            assert!(path.is_file(), "HALO_SKATE_TEST_HALO_MAPS has no {}", path.display());
            Some(path)
        }
        _ => {
            eprintln!("{test}: skipped, HALO_SKATE_TEST_HALO_MAPS is not set");
            None
        }
    }
}

/// A collision BSP's surfaces as triangles, nine floats each, world units,
/// exactly as skate.c sends them.
pub struct HaloMap {
    pub name: String,
    /// One list for each of the scenario's structure BSPs, in its order.
    pub bsps: Vec<Vec<f32>>,
}

impl HaloMap {
    pub fn read(path: &Path) -> Result<HaloMap, String> {
        let file = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
        let map = decompress(&file)?;
        let r = Reader(&map);
        let name = cstr(&map[0x20..0x40]);
        let tag_data_offset = r.u32(0x10)? as usize;
        let tags = Space { file: tag_data_offset, base: TAG_CACHE_BASE_ADDRESS };
        if r.u32(tags.at(TAG_CACHE_BASE_ADDRESS + 0x20)?)? != u32::from_be_bytes(*b"tags") {
            return Err("no tag header".into());
        }
        let instances = r.u32(tags.at(TAG_CACHE_BASE_ADDRESS)?)?;
        let scenario_index = r.u32(tags.at(TAG_CACHE_BASE_ADDRESS + 4)?)?;
        let tag_count = r.u32(tags.at(TAG_CACHE_BASE_ADDRESS + 0xC)?)?;
        // the scenario's instance: its tag index's low half is its place
        let slot = scenario_index & 0xFFFF;
        if slot >= tag_count {
            return Err(format!("the scenario's tag {scenario_index:#x} is not one of the {tag_count}"));
        }
        let instance = instances + slot * 0x20;
        if r.u32(tags.at(instance)?)? != u32::from_be_bytes(*b"scnr") {
            return Err("the scenario tag is not a scenario".into());
        }
        let scenario = r.u32(tags.at(instance + 0x14)?)?;
        let (count, references) = r.block(tags.at(scenario + SCENARIO_STRUCTURE_BSP_REFERENCES)?)?;
        let mut bsps = Vec::new();
        for i in 0..count {
            let reference = tags.at(references + i * 0x20)?;
            let (file_offset, file_size, base) = (r.u32(reference)?, r.u32(reference + 4)?, r.u32(reference + 8)?);
            let bsp = Space { file: file_offset as usize, base };
            if file_offset as usize + file_size as usize > map.len() {
                return Err(format!("structure BSP {i} lies past the end of the map"));
            }
            if r.u32(bsp.at(base + 0x14)?)? != u32::from_be_bytes(*b"sbsp") {
                return Err(format!("structure BSP {i} has no header"));
            }
            let structure_bsp = r.u32(bsp.at(base)?)?;
            let (collision_count, collision) = r.block(bsp.at(structure_bsp + STRUCTURE_BSP_COLLISION_BSP)?)?;
            if collision_count == 0 {
                return Err(format!("structure BSP {i} has no collision"));
            }
            bsps.push(collision_triangles(&r, &bsp, collision)?);
        }
        Ok(HaloMap { name, bsps })
    }

    pub fn triangle_count(&self, bsp: usize) -> usize {
        self.bsps[bsp].len() / 9
    }
}

/// skate.c's `skate_collision_triangles` over the collision BSP at `address`:
/// each surface's polygon (`collision_surface_polygon`: its ring of edges,
/// each taken forwards or backwards as the surface is its first or second,
/// at most 8) fanned from its first point, each triangle wound
/// counterclockwise seen from in front of the surface's plane, and a
/// two-sided surface's both ways.
fn collision_triangles(r: &Reader, space: &Space, address: u32) -> Result<Vec<f32>, String> {
    // collision_bsp: bsp3d { nodes, planes }, leaves, bsp2d references,
    // bsp2d { nodes }, surfaces, edges, vertices
    let (_, planes) = r.block(space.at(address + 0x0C)?)?;
    let (surface_count, surfaces) = r.block(space.at(address + 0x3C)?)?;
    let (edge_count, edges) = r.block(space.at(address + 0x48)?)?;
    let (vertex_count, vertices) = r.block(space.at(address + 0x54)?)?;
    let point = |vertex: u32| -> Result<[f32; 3], String> {
        if vertex >= vertex_count {
            return Err(format!("vertex {vertex} is not one of the {vertex_count}"));
        }
        let at = space.at(vertices + vertex * 0x10)?;
        Ok([r.f32(at)?, r.f32(at + 4)?, r.f32(at + 8)?])
    };
    let mut out = Vec::new();
    for surface in 0..surface_count {
        let at = space.at(surfaces + surface * 0x0C)?;
        let plane_designator = r.u32(at)?;
        let first_edge = r.u32(at + 4)?;
        let flags = r.0[at + 8];
        // the polygon, ending as collision_surface_edge_ring_continues does
        let mut points: Vec<[f32; 3]> = Vec::new();
        let mut edge = first_edge;
        if edge < edge_count {
            loop {
                let e = space.at(edges + edge * 0x18)?;
                let reverse = r.u32(e + 0x14)? == surface;
                let vertex = r.u32(e + if reverse { 4 } else { 0 })?;
                points.push(point(vertex)?);
                edge = r.u32(e + if reverse { 0x0C } else { 0x08 })?;
                if edge == first_edge || edge >= edge_count || points.len() >= MAXIMUM_VERTICES_PER_COLLISION_SURFACE {
                    break;
                }
            }
        }
        if points.len() < 3 {
            continue;
        }
        let plane = space.at(planes + (plane_designator & 0x7FFF_FFFF) * 0x10)?;
        let mut normal = [r.f32(plane)?, r.f32(plane + 4)?, r.f32(plane + 8)?];
        if plane_designator & 0x8000_0000 != 0 {
            normal = normal.map(|v| -v);
        }
        let a = points[0];
        for index in 1..points.len() - 1 {
            let (b, c) = (points[index], points[index + 1]);
            let ab = [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
            let ac = [c[0] - a[0], c[1] - a[1], c[2] - a[2]];
            let n = [
                ab[1] * ac[2] - ab[2] * ac[1],
                ab[2] * ac[0] - ab[0] * ac[2],
                ab[0] * ac[1] - ab[1] * ac[0],
            ];
            let flip = n[0] * normal[0] + n[1] * normal[1] + n[2] * normal[2] < 0.0;
            let sides = if flags & TWO_SIDED != 0 { 2 } else { 1 };
            for side in 0..sides {
                let (second, third) = if flip != (side == 1) { (c, b) } else { (b, c) };
                out.extend(a);
                out.extend(second);
                out.extend(third);
            }
        }
    }
    Ok(out)
}

/// The cache file as the game reads it: an Xbox map's deflated rest inflated
/// after its header (a map whose header gives its full length is taken as
/// it is).
fn decompress(file: &[u8]) -> Result<Vec<u8>, String> {
    if file.len() < HEADER_SIZE {
        return Err("shorter than a cache file's header".into());
    }
    let r = Reader(file);
    if r.u32(0)? != u32::from_be_bytes(*b"head") || r.u32(HEADER_SIZE - 4)? != u32::from_be_bytes(*b"foot") {
        return Err("not a cache file".into());
    }
    let length = r.u32(8)? as usize;
    if file.len() == length {
        return Ok(file.to_vec());
    }
    let mut map = Vec::with_capacity(length);
    map.extend_from_slice(&file[..HEADER_SIZE]);
    flate2::read::ZlibDecoder::new(&file[HEADER_SIZE..])
        .read_to_end(&mut map)
        .map_err(|e| format!("inflating the map: {e}"))?;
    if map.len() != length {
        return Err(format!("inflated to {} bytes, not the header's {length}", map.len()));
    }
    Ok(map)
}

fn cstr(bytes: &[u8]) -> String {
    let end = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
    String::from_utf8_lossy(&bytes[..end]).into_owned()
}

/// Where data loaded at `base` lies in the file.
struct Space {
    file: usize,
    base: u32,
}

impl Space {
    fn at(&self, address: u32) -> Result<usize, String> {
        address
            .checked_sub(self.base)
            .map(|offset| self.file + offset as usize)
            .ok_or_else(|| format!("address {address:#x} is below its base {:#x}", self.base))
    }
}

/// Little-endian reads, checked against the end.
struct Reader<'a>(&'a [u8]);

impl Reader<'_> {
    fn bytes<const N: usize>(&self, at: usize) -> Result<[u8; N], String> {
        self.0
            .get(at..at + N)
            .map(|b| b.try_into().unwrap())
            .ok_or_else(|| format!("offset {at:#x} is past the end of the map"))
    }
    fn u32(&self, at: usize) -> Result<u32, String> {
        self.bytes(at).map(u32::from_le_bytes)
    }
    fn f32(&self, at: usize) -> Result<f32, String> {
        self.bytes(at).map(f32::from_le_bytes)
    }
    /// A tag block: its count and its elements' address.
    fn block(&self, at: usize) -> Result<(u32, u32), String> {
        Ok((self.u32(at)?, self.u32(at + 4)?))
    }
}

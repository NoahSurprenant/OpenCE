# Skate 3 mode

Press **J** (or click both sticks in) to drop Master Chief onto a skateboard
driven by Skate 3's own physics, tricks and grinds. Press it again to get off.

The skate engine is the one from
[2010 Rust Rewrite Mashup](https://github.com/chasmlol/2010-rust-rewrite-mashup)
(`skate/crates`), itself from
[SK8-ENGINE/skate-3-rust-engine](https://github.com/SK8-ENGINE/skate-3-rust-engine)
at `cb79689`. It is a Rust reimplementation; it reads data converted from your
own copy of Skate 3, and none of the game's files are included here.

## What you need

- A Linux or Windows build made with `--skate` (below). Android builds
  compile the hooks but never skate.
- **Skate 3 for Xbox 360, extracted** (`default.xex` with its `data` folder),
  converted once into the folder the engine reads. Use the mashup's converter,
  `iw4l-skate-convert.exe` from its Windows release, or its
  `skate/converter/iw4l_skate_convert.py` run with the `tools` folder of a
  skate-3-rust-engine checkout at `cb79689` beside it (it imports that tree):

  ```text
  iw4l-skate-convert --xex "Skate 3/default.xex" --out skate-data
  ```

  The result is `skate-data/assets`. The game reads `HALO_SKATE_ASSETS`, or
  `skate-data/assets` in the folder it starts in.
- A controller, or the keyboard (below).

## Controls

Skate 3's own controls, on a controller or on the keyboard standing in for
one. Both work at once.

| Skate 3 | controller | keyboard |
| --- | --- | --- |
| get on or off the board | click both sticks in | J |
| steer and lean | left stick | W A S D |
| flick-it tricks (ollie, flips) | right stick | arrow keys |
| push | A | Space |
| B | B | E |
| X | X | Left Shift |
| Y | Y | Q |
| grabs | LT, RT | Z, C |
| shoulders | LB, RB | 1, 3 |

The flick-it tricks are stick gestures (pull down, then flick up for an
ollie), so they play best on a stick: an arrow key only pushes it all the
way. While skating, Halo's own movement, firing and looking are off, and the
camera follows from behind, turning to the way you travel: rolling back off
a wall (fakie, still facing it), it comes round in about a third of a
second. Slower than 1 m/s it holds its heading.

On the board the crosshair is a small dot, and the weapon is holstered: a
pistol on the right thigh, a rifle or anything larger across the back
(`skate.c`), and a flag or a ball is put out of sight. Getting off puts it
back in your hands.

### Tuning at the console

A few values can be changed while you play, at the console (the backquote
key). Each one typed alone prints its value; with a number it sets it, for
this session only, and prints it. Tell us the values you settle on, and they
become the defaults.

| command | default | what |
| --- | --- | --- |
| `skate_board_scale <factor>` | 1.05 | the board grown about its middle, its length, width and thickness alike: Master Chief (about 2.1 m) is larger than the Skate 3 skater (about 1.8 m) the board was made for |
| `skate_feet_offset <world units>` | 0 | raises Master Chief (lowers, if negative) along the board's up while he rides, past where his soles meet the deck; 0.01 world units is about 3 cm |
| `skate_camera_speed <world units a second>` | 0.328 (1 m/s) | the following camera turns to the way you travel when faster than this, and holds its heading when slower |

For example, `skate_board_scale` prints `skate_board_scale 1.050 (default
1.050)`, and `skate_feet_offset 0.01` prints `skate_feet_offset 0.0100 world
units, 3.0 cm (default 0; ankles ... above the soles)`, with Master Chief's
measured ankle height.

## Build

You need a recent stable Rust (it was built with 1.99) besides the usual
requirements of each platform. `ninja` runs `cargo build` for `port/skate`
and links the engine into the game. The first build compiles the engine and
its Bevy dependencies, a few minutes.

**Linux** ([requirements](../linux/README.md)), and the 32-bit `libgcc_s` at
run time (`lib32-gcc-libs` on Arch, `lib32gcc-s1` on Debian and Ubuntu):

```text
rustup target add i686-unknown-linux-gnu
python configure.py --skate
ninja linux
```

**Windows** ([requirements](../windows/README.md); cargo compiles the
engine's bits of C with the Visual Studio Build Tools you already have):

```text
rustup target add i686-pc-windows-msvc
python configure.py --skate
ninja windows
```

On Windows the engine is built with the static C runtime, as the game is
(`.cargo/config.toml`), and its own XInput polling is left out (the
`xinput` feature of `skate-host`): the game hands it the pad, and defines
`XInputGetState` itself.

## How it works

| piece | where | what |
| --- | --- | --- |
| engine | `vendor/` | the mashup's `skate-core`, `skate-data`, `skate-net` and `skate-host` |
| C interface | `halo-skate/src/lib.rs`, `include/halo_skate.h` | a worker thread with a 32 MB stack owns the skate session; the game sends it a preload, the map, an activation or a tick's pad, and waits (at most 250 ms) only for the pose |
| grind rails | `halo-skate/src/rails.rs` | the mashup's rail finder, unchanged: walkable edges where the ground drops away and nothing rises, chained into rails |
| retarget | `halo-skate/src/rig.rs` | each of the biped's `bip01` nodes keeps its bone lengths and turns to point where the skater's matching bone points; the pelvis and chest take the hips' and shoulders' twist; the feet are put on the skater's feet, and while riding the soles on the deck (below) |
| board | `halo-skate/src/board.rs`, `../linux/src/d3d8_gl.c` | the board meshes of the converted skater model (`private/skater.glb`: the skinned primitives whose material is named for the board, as the mashup picks them), skinned to the skater each tick and grown 5% (`skate_board_scale`); the game draws them between the last two ticks, as it draws the biped, right after the objects: depth tested, lit by the level's ambient and distant lights at the skater, without point lights or fog |
| game glue | `../linux/game/skate.c` | the collision BSP as triangles, the toggle, the tick, the biped's position and node matrices, the board, the console's tuning |
| camera | `halo-skate/src/camera.rs` | the heading the following camera keeps: toward the horizontal velocity's, a fifth of the way a tick the shorter way round, when faster than `skate_camera_speed`; held when slower; always within 0 to 2 pi |
| hooks | `source/main/main.c`, `hs/hs.c`, `scenario/scenario.c`, `game/game.c`, `units/bipeds.c`, `game/player_control.c`, `camera/director.c`, `render/render.c` | preload the skater at startup; take the tuning commands before the script compiler; build each structure BSP's collision as it loads; step before the objects update and pose after it; skip a skating biped's own movement; give the pad to the engine; follow the skater with the third-person camera; draw the board after the objects |
| input | `../linux/src/sdl_platform.c`, `xinput_sdl.c` | J, and the first pad as an Xbox 360 pad |

### Loading

Nothing loads when you press J. At startup (`skate_initialize`), if the
assets folder is there, the worker makes the skate session in the background:
the animation banks, the state graphs, physics and the skater, on a flat
placeholder floor. Each time a structure BSP loads (a new level, or a switch
within one: `skate_structure_bsp_changed`) its collision goes to the worker,
which finds its grind rails and builds and swaps in its collision, also in the
background. J then gets on at once; pressed while something is still loading,
it says so in the console and gets on when it is ready. The main menu's BSP is
not loaded, and a BSP replaced before it was built is skipped.
The preload also reads the board from the skater model (`private/skater.glb`)
for the game to draw.

The engine's log lines go through the port's log (`halo_skate_set_log`, set by
`skate_initialize`): `halo.log` beside `halo.exe` on Windows, whose release
build has no console, and stderr on Linux, each line after `halo-linux: `.
(`skate-host` sends its `eprintln!` lines to `skate_host::log`, a sink the game
sets, by a crate-wide macro in its `lib.rs`; `skate-data`'s are left on stderr.)
The log times each phase: `halo-skate: preload: animation banks in`,
`preload: session in` (with `IW4L_SKATE_LOAD graphs`, `physics` and `skater`
inside it), and for each map `triangles deduplicated and sorted in`,
`rails: ... in`, `collision built in`, `collision installed in` and
`map loaded in ... in all`.

Halo is Z up in world units of 10 feet; Skate is Y up in metres. The engine
ticks at 60 Hz, so each 30 Hz game tick runs two engine ticks.

### Feet on the deck

The skater's ankles are not as high above its soles as Master Chief's, so
standing his ankles on the skater's leaves his feet off the deck (or in it).
Each tick, `rig.rs` measures instead: under the middle of each of the
skater's feet (between its ankle and toe bones) it finds the top of the board
as drawn (grown, `board.rs`'s `deck_top`): the highest board triangle the
line through the foot along the board's up crosses, the board's up being the
skater's; or, if none does (a heel off the tail), the highest board vertex
within 10 cm of that line. Chief's skeleton then moves along the board's up
so his ankles are his own ankle height above it, plus `skate_feet_offset`
(the mean of both feet, as the ankles are). That is done while both of the
skater's ankles are within 15 cm above the deck; between 15 and 35 cm, or
with no deck under a foot (a flip), it fades out, and Chief's ankles follow
the skater's as before, so he does not snap to a spinning board.

Chief's ankle height comes from his model's bind pose (`skate.c`): his foot
nodes' height above the lowest vertex within 0.15 world units of them, or,
failing that, above the model's origin. The log says what it found once
(`skate: the biped's ankles are ... above its soles`), and once what it
measured the first time the feet were on the deck (`halo-skate: feet on the
deck: the skater's ankles ... above it`).

## Known gaps

- **Play-tested in part.** The physics, the controls, the following camera
  and the pose work in a game. The Windows build was cross-compiled and linked
  on Linux, not built on Windows.
- **The board is drawn but not seen yet.** It builds, and loading and skinning
  are tested on a small model, but it has not been looked at in a game. A
  skater model without board meshes leaves the board undrawn (the log says
  why).
- **Only the level's BSP is solid.** Scenery, vehicles and other objects are
  not part of the skater's collision; they are passed through.
- **The node names are assumed.** The retarget expects the cyborg's
  `bip01 pelvis`, `bip01 l thigh` and so on. The game's console prints how many
  nodes follow the skater, and stderr lists the biped's node names.
- **Multiplayer:** the skater's position replicates, but a host clamps a
  client faster than about twice running speed, and other machines see Halo's
  own animation rather than the skating pose.
- The camera cannot be turned by hand while skating; it follows the way you
  travel.

## Licence

The skate engine is GPL-3.0 (`vendor/LICENSE-GPL-3.0`), so a build made with
`--skate` is GPL-3.0 as a whole. `halo-skate/src/rails.rs` is Apache-2.0 from
the mashup. A build without `--skate` contains none of it.

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

- A Linux build made with `--skate` (below). Windows and Android builds compile
  the hooks but never skate.
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
- A controller is best. The keyboard works too: WASD is the left stick, the
  arrow keys the right stick (the flick-it tricks), Space A, E B, Left Shift X,
  Q Y, Z and C the triggers, 1 and 3 the shoulders.

## Build

You need a recent stable Rust (it was built with 1.99) with the `i686-unknown-linux-gnu`
target, and the 32-bit `libgcc_s` at run time (`lib32-gcc-libs` on Arch,
`lib32gcc-s1` on Debian and Ubuntu), besides the usual
[Linux requirements](../linux/README.md).

```text
rustup target add i686-unknown-linux-gnu
python configure.py --skate
ninja linux
```

`ninja` runs `cargo build` in `port/skate` and links `libhalo_skate.a`. The
first build compiles the engine and its Bevy dependencies, a few minutes.

## How it works

| piece | where | what |
| --- | --- | --- |
| engine | `vendor/` | the mashup's `skate-core`, `skate-data`, `skate-net` and `skate-host` |
| C interface | `halo-skate/src/lib.rs`, `include/halo_skate.h` | a worker thread with a 32 MB stack owns the skate session; the game sends it the map, an activation or a tick's pad and waits (at most 250 ms) for the pose |
| grind rails | `halo-skate/src/rails.rs` | the mashup's rail finder, unchanged: walkable edges where the ground drops away and nothing rises, chained into rails |
| retarget | `halo-skate/src/rig.rs` | each of the biped's `bip01` nodes keeps its bone lengths and turns to point where the skater's matching bone points; the pelvis and chest take the hips' and shoulders' twist; the feet are put on the skater's feet |
| game glue | `../linux/game/skate.c` | the collision BSP as triangles, the toggle, the tick, the biped's position and node matrices |
| hooks | `source/game/game.c`, `units/bipeds.c`, `game/player_control.c`, `camera/director.c` | step before the objects update and pose after it; skip a skating biped's own movement; give the pad to the engine; follow the skater with the third-person camera |
| input | `../linux/src/sdl_platform.c`, `xinput_sdl.c` | J, and the first pad as an Xbox 360 pad |

Halo is Z up in world units of 10 feet; Skate is Y up in metres. The engine
ticks at 60 Hz, so each 30 Hz game tick runs two engine ticks.

## Known gaps

- **Not play-tested yet.** It builds and links, and the library was exercised
  from C on 32-bit (map load, rail finding, failure without Skate 3 data), but
  not in a game.
- **The board is not drawn.** The skater stands on nothing visible.
- **Only the level's BSP is solid.** Scenery, vehicles and other objects are
  not part of the skater's collision; they are passed through.
- **The node names are assumed.** The retarget expects the cyborg's
  `bip01 pelvis`, `bip01 l thigh` and so on. The game's console prints how many
  nodes follow the skater, and stderr lists the biped's node names.
- **Multiplayer:** the skater's position replicates, but a host clamps a
  client faster than about twice running speed, and other machines see Halo's
  own animation rather than the skating pose.
- The camera is locked behind the board.

## Licence

The skate engine is GPL-3.0 (`vendor/LICENSE-GPL-3.0`), so a build made with
`--skate` is GPL-3.0 as a whole. `halo-skate/src/rails.rs` is Apache-2.0 from
the mashup. A build without `--skate` contains none of it.

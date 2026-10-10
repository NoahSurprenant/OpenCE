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
| get on or off the board | click both sticks in (below) | J |
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

Halo crouches on the left stick's click and zooms on the right's, so in a
build with Skate 3 mode the first controller's stick clicks reach Halo a
tenth of a second late (`xinput_sdl.c`): a click the other stick joins in
that time is the board's alone, and Halo sees neither until both are let
go. A click held longer reaches Halo, and a quicker tap still does, briefly,
once let go.

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

## Sound

The skate engine has no sound of its own, so Skate 3 mode makes its own
from what the engine knows each tick, and plays samples for it: a set built
into the game (below), or your own, or later Skate 3's (the plan, below).

### What makes a sound

After each of the engine's ticks (60 a second) `halo-skate/src/sound.rs`
looks at its state (`SoundObservation`, `vendor/skate-host/src/physics/bridge/sound.rs`,
a read-only look added for OpenCE): the skater's state (`PhysicalStateId`),
which of the board's wheels, trucks and deck touch the world
(`BoardGroundState`), how hard the board hit what it touches (its parts'
closing speed), the trajectory last launched into the air and whether the
skater jumped into it, and whether the body is a ragdoll. The changes make:

| sound | when | how loud |
| --- | --- | --- |
| `pop` | a new trajectory launched with `player_jumped` (an ollie, nollie, a pop off a grind), the board on something within the last 10 ticks | always |
| `land` | the wheels back on the ground after 0.1 s or more off it, riding, two ticks on (onto a rail or ledge the wheels touch it a tick before the state becomes a grind: then it is `grind_start` instead); not the drop after the engine puts the skater back (`Teleporting`, after a bail) | by the closing speed, or how fast the board fell, if more (5 m/s is the loudest) |
| `grind_start`, `grind_end` | the state into or out of a truck grind (50-50, 5-0, smith and feeble: `GrindFiftyFifty`, `GrindFiveO`, `GrindBackslash`) | by the closing speed; by speed |
| `slide_start`, `slide_end` | the same for a slide on the deck (`GrindBoardslide`, `GrindTipslide`, `GrindDarkslide`) | the same |
| `board_impact` | the deck or a truck newly hitting something at 1 m/s or more, not grinding (a slap on a ledge, the board tumbling in a bail), or the board landing on its own | by the closing speed (4 m/s is the loudest) |
| `bail` | the state into `WipeoutGround`, or the body a ragdoll | by speed (8 m/s is the loudest) |
| `step_off`, `step_on` | off the board (`BipedGround`, `BipedAir`, `OffBoardPushing`) for 3 ticks in a row with no wipeout or ragdoll (a bail passes through `BipedGround` for a tick), and back on | always |

and the loops, which glide toward what each tick asks:

| loop | plays while | volume and pitch |
| --- | --- | --- |
| `roll` | any wheel on the ground, not grinding, faster than 0.15 m/s | louder and higher the faster (to 8 m/s), a little louder on four wheels than on two; silent in the air |
| `grind`, `slide` | a truck grind, or a slide on the deck | by speed |
| `powerslide` | `SlideGround` with the wheels down | by speed |

Getting on (J), or being put back on after a failed step, takes the state as
it is: it makes no sound. Each one-shot is told of in the log, with its
strength, the speed and the sample it played (`halo-skate: sound: land
(0.62, 3.1 m/s) built-in land.wav`); `skate_sound_log 0` at the console
stops that. Board catches after a flip trick are not told apart yet.

What the board is on comes from the level, not the engine (whose collision
here has a single material, so its surface audio type is always the same):
each tick `skate_sound.c` looks down from the skater for the level's material
and makes it a surface: `metal` (metal, force fields), `wood`, `rough` (dirt,
sand, snow, water, leaves) or `concrete` (stone and everything else). In the
air the last one holds.

### How it plays

`halo-skate/src/mixer.rs` plays up to 24 voices of the samples, mixed into
the game's output on the port's audio thread (`dsound_sdl.c` calls
`halo_skate_sound_mix` after the game's voices and the voice chat, before
its limiter). Volume, pan and pitch changes glide over 20 ms (pitch over
60 ms), so nothing clicks; a loop changing sample (another surface) fades the
old out as the new fades in; a loop's last 10 ms are crossfaded into its
start, so its seam is smooth whatever the file's ends; and the loops fade
out by themselves when the game stops ticking (paused) or the skater gets off.
Each voice is panned toward the board as seen from the camera, and quieter
past 1 m from it. One-shots vary their pitch by a few percent each time.

The volume is `audio.volume` times `audio.effects_volume` times
`skate_volume`: `audio.skate_volume` in config.toml (1.0, from 0 to 4), or
`skate_volume <factor>` at the console for the session (typed alone, it
prints the value).

Halo's own footsteps do not play on the board: they come from
`biped_update` (`bipeds.c`: `biped_try_to_make_footsteps`, the jumps' and
landings' material effects, and `unit_update_animation`, which plays the
animations' sound frames), which a skating biped skips.

### The sound set: file names

A sound set is a folder of WAV files (PCM of 8, 16, 24 or 32 bits, or 32-bit
float; any rate; stereo is mixed down to mono), named

```text
<sound>[_<surface>][-<variant>].wav
```

where `<sound>` is one of `roll`, `grind`, `slide`, `powerslide`, `pop`,
`land`, `grind_start`, `grind_end`, `slide_start`, `slide_end`,
`board_impact`, `bail`, `step_off`, `step_on`; `<surface>` is one of
`concrete`, `metal`, `wood`, `rough`; and `<variant>` is 2 to 99, for several
takes of one sound, one picked at random each time. So `roll.wav`,
`roll_metal.wav`, `land-2.wav`, `grind_start_wood-3.wav`.

Each sound and surface takes the surface's own samples (`grind_metal`), else
the sound's (`grind`), else those of the sound it falls back on: `slide` on
`grind`, `slide_start` and `slide_end` on `grind_start` and `grind_end`,
`grind_start` on `land`, `step_on` and `step_off` on `board_impact`. A
sound with none is silent.

The game looks for a set at startup, each sound in the first of these that
has any file for it:

1. the folder `HALO_SKATE_SOUNDS` names;
2. `sounds` beside the converted data's `assets` (`skate-data/sounds`), and
   `skate-data/assets/sounds`;
3. the set built into the game (`port/assets/skate-sounds`).

So a folder with only `land.wav` replaces every landing sample of the
built-in set and leaves the rest. A file of another name, or one that is not
a WAV it can read, is skipped with a line in the log; a sound no place has
says so once (`halo-skate: sound: no samples for step_off: silent`). The log
says how many sounds have samples and where it looked.

### The built-in set

`port/assets/skate-sounds`, built into the game (`halo-skate/src/sound_defaults.rs`
embeds the files, so a release needs nothing beside it). Seven are
recordings from [Freesound](https://freesound.org), each released there
under [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) (read from
each sound's page); two are synthesized by `synthesize.py` beside them.
`CREDITS.txt` there lists each file's source, the part used and what was done
to it (mono, 44.1 kHz, trimmed, normalised; the loops levelled and made to wrap).

| file | from | licence |
| --- | --- | --- |
| `roll.wav` (loop) | [BPS-SKATEBOARDING RIDING-City Street-Pushing-Fast-Long Strides-1](https://freesound.org/people/bspiller5/sounds/478165/) by bspiller5 | CC0 |
| `pop.wav`, `land.wav` | [skateboard ollie](https://freesound.org/people/nolimitkid/sounds/515229/) by nolimitkid | CC0 |
| `grind.wav` (loop) | [skateboard grinding on metal rail](https://freesound.org/people/21100495/sounds/655371/) by 21100495 | CC0 |
| `slide.wav` (loop) | [skateboard grinding on concreate slab](https://freesound.org/people/21100495/sounds/655372/) by 21100495 | CC0 |
| `board_impact.wav` | [skateboard drop 2](https://freesound.org/people/FOSSarts/sounds/740122/) by FOSSarts | CC0 |
| `bail.wav` | [Body fall_01](https://freesound.org/people/deleted_user_2104797/sounds/346695/) (its account since deleted) | CC0 |
| `powerslide.wav` (loop) | synthesized: band-passed noise and a squeal | this project's (GPL-3.0) |
| `roll_rough.wav` (loop) | synthesized: a low rumble with crackle, for dirt, sand, snow, grass, water | this project's (GPL-3.0) |

The set has no `grind_end`, `slide_end`, `step_off` or `step_on` of its own:
the last two fall back on `board_impact`, and a grind's end is silent (a
pop off it has its own sound). Nobody has listened to these in a game yet:
they were picked and cut by their loudness and spectra.


### Skate 3's own sounds, later

The plan is for the converter to write Skate 3's sounds into
`skate-data/sounds` under the names above, where they replace the built-in
set sound by sound with nothing else to change. What that step needs:

- **Finding them.** The mashup's converter (`iw4l_skate_convert.py`, which
  runs skate-3-rust-engine's `tools/asset_pipeline`) extracts animation,
  graphs, physics and the skater from `miscload.big`, `miscboot.big`,
  `db.big` and `createacharacter.big`, and nothing of the sound: neither it
  nor the engine at `cb79689` reads audio (the engine's surface records carry
  an `audio` type, packed from `EncodeRwSurfaceId`, which is what Skate 3's
  own sound would have keyed its wheel and grind sounds by). Skate 3's audio
  is most likely in its own `.big` archives (which the `tools/owned_game/big.py`
  reader already opens) or loose under `data/`; that has not been checked
  against a disc.
- **Decoding them.** EA's games of that time on the Xbox 360 use EA's audio
  formats: the older SCHl streams and banks (`.asf`, `.bnk`, `.abk`, `.ast`)
  or the newer EA Audio Core ones (`.snr` with `.sns`, `.sps`, `.snu`, and
  the MPF/MUS, SBR/SBS containers), with codecs such as EA-XAS, EALayer3 or
  XMA. [vgmstream](https://github.com/vgmstream/vgmstream) decodes all of
  those families (its `ea_schl*` and `ea_eaac*` readers), so the converter
  could call `vgmstream-cli` to write WAV. Which of them Skate 3 uses is not
  verified.
- **Naming them.** The hard part: mapping Skate 3's sound events (wheel roll
  per surface type, pop, land, grind and slide per surface, bail) to the file
  names above, which needs either the event names in its banks or listening.

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

### Tests with your own Skate 3 data

`halo-skate/tests/real_assets.rs` drives the C interface the way the game
does, with the real skater, animations and physics, on a small map made of
triangles in the test (a floor, a funbox, a box ledge, a rail and a tilted
pad): getting on, riding, an ollie, getting off and jumping from walking, and
a bail. They need converted data, so they pass without running unless
`HALO_SKATE_TEST_ASSETS` names its `assets` folder; the engine is slow
unoptimised, so run them with `--release`:

```text
HALO_SKATE_TEST_ASSETS=/path/to/skate-data/assets \
    cargo test --release -p halo-skate --test real_assets -- --nocapture
```

`halo-skate/tests/real_assets_sound.rs` checks the sound events the same way,
from the log: pushing makes no one-shot, an ollie pops then lands, onto the
rail the grind starts (no landing) and ends, a bail sounds once (no step
off, no landing when the engine puts the skater back), and Y steps off
(`--test real_assets_sound`).

In CI they run in the `skate-assets` job of `.github/workflows/build.yml`,
on the owner's self-hosted runner (`opence-k8s`), which has the data
read-only at `/skate-assets` and nothing else of the owner's. The job runs
only in this repository, for its own pushes and branches, never a fork's pull
request, and fails if the data is missing rather than letting the tests pass
without running. The release does not wait for it: a build of `main` is
published even when the runner is down.

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
| recovery | `halo-skate/src/recovery.rs` | where a failed step puts the skater back on the board (the last good pose's place and heading), and when it gives up and the session is made again (below, "When the engine fails") |
| hooks | `source/main/main.c`, `hs/hs.c`, `scenario/scenario.c`, `game/game.c`, `units/bipeds.c`, `game/player_control.c`, `camera/director.c`, `render/render.c` | preload the skater at startup; take the tuning commands before the script compiler; build each structure BSP's collision as it loads; step before the objects update and pose after it; skip a skating biped's own movement; give the pad to the engine; follow the skater with the third-person camera; draw the board after the objects |
| input | `../linux/src/sdl_platform.c`, `xinput_sdl.c` | J, and the first pad as an Xbox 360 pad |
| sound | `halo-skate/src/sound.rs`, `sound_set.rs`, `mixer.rs`, `include/halo_skate_sound.h`, `../linux/game/skate_sound.c`, `../linux/src/dsound_sdl.c` | the events and loops from the engine's ticks, the sound set, the voices mixed into the game's output, and the listener, surface and volume from the game (above, "Sound") |

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

### When the engine fails

The engine checks some of its numbers as it goes and gives up a step on one
that is not a number (a bail it cannot follow says `Nonfinite BipedAir launch
packet`, with the packet's values). The session survives that, so the skater
is put back on the board where the last good tick had it, facing the same
way, as J would (`recovery.rs`): the console says `skate: thrown, back on
the board`. The log tells of each failure, with where the skater was, its
state and speed, its state after, and the pad (`halo-skate: a step failed:
...`), and of the recovery (`halo-skate: the skater was put back on the
board at ...`). A spot that fails four times in a row, each within 4 seconds
of the last, or a recovery that fails, gives the session up: skating stops
(`skate: off (...)`), the biped is on foot again with its weapon in hand,
and the next J makes the session again with the map (8 seconds or so), then
gets on. The biped's skeleton is kept across that restart, so the biped is
posed and its weapon holstered as before; a skater the engine cannot pose
still has its weapon holstered.

### Feet on the deck

The skater's ankles are not as high above its soles as Master Chief's, so
standing his ankles on the skater's leaves his feet off the deck (or in it).
Each tick, `rig.rs` measures instead: under the middle of each of the
skater's feet (between its ankle and toe bones) it finds the top of the board
as drawn (grown, `board.rs`'s `deck_top`): the highest board triangle the
line through the foot along the board's up crosses, the board's up being the
skater's. Just past the board's edge it takes the nearest triangles' height
at the edge instead, counting less the further past it the foot is, down to
nothing 10 cm out.

Each foot counts as on the deck by how near the skater's ankle is to it:
fully from the deck's top to 15 cm above it, not at all from 35 cm above it
(lifted, in the air) or 10 cm below it (pushing, down at the ground), and
partly between. Each foot on the deck asks for the move along the board's up
that puts Chief's matching ankle his own ankle height, plus
`skate_feet_offset`, above the deck under it; Chief's skeleton takes those
feet's moves, weighed by how much each is on the deck. So with both feet on
it he rests on the deck by both; while one pushes or is lifted he stands on
the other alone; and a foot leaving or coming back slides him across rather
than jumping. As both feet leave the deck (an ollie, a flip) it fades out,
and his ankles follow the skater's as before, so he does not snap to a
spinning board.

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
- **The sounds are not heard in a game yet.** The events, the sound set and
  the mixer are tested on their own (`cargo test -p halo-skate`), and the
  game builds with them, but nobody has skated with them on: their levels
  and thresholds are first guesses.
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
The built-in sounds (`../assets/skate-sounds`) are CC0 recordings from
Freesound and two synthesized ones (above, "The built-in set";
`CREDITS.txt` beside them).

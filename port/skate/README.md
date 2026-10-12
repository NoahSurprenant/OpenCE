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

As in Skate 3, you can step off the board and walk (the left stick), jump,
and throw the board; that is still skating (the dot, the holstered weapon),
and J still ends it. Off the board the camera follows the skater, not the
board, wherever the board lies or flies: it turns to the way you walk, and,
stood still for a second, comes slowly round behind the way you face.
Getting off and back on, it turns from where it was, never snapping round.
The board is drawn where the engine has it (its physical body), under you
or not.

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
| `skate_obstacle_bail_speed <world units a second>` | 1.969 (6 m/s) | running into a person or a vehicle faster than this, along the contact, knocks you off the board; slower is a bump (below, "Obstacles") |

For example, `skate_board_scale` prints `skate_board_scale 1.050 (default
1.050)`, and `skate_feet_offset 0.01` prints `skate_feet_offset 0.0100 world
units, 3.0 cm (default 0; ankles ... above the soles)`, with Master Chief's
measured ankle height.

## Sound

The skate engine has no sound of its own, so Skate 3 mode makes its own
from what the engine knows each tick, and plays samples for it: a set built
into the game (below), your own, or Skate 3's own, taken from your copy of
the game (below).

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


### Skate 3's own sounds

`converter/extract_sounds.py` takes Skate 3's own sounds from your extracted
copy of the game and writes them into `skate-data/sounds` under the names
above, where they replace the built-in set sound by sound. Like the converted
data, what it writes is the game's and stays on your machine: never commit,
share or upload it.

**On Windows**, once:

1. [Python](https://www.python.org/downloads/) 3.8 or later (the script needs
   nothing but its standard library).
2. vgmstream, which decodes the sounds: `vgmstream-win64.zip` from
   [its releases](https://github.com/vgmstream/vgmstream/releases) (r2117 or
   later), unzipped into a folder of its own, say `C:\tools\vgmstream`. Keep
   the DLLs beside `vgmstream-cli.exe`: they are its FFmpeg, which Skate 3's
   sounds (XMA) need.
3. From this repository's folder, with the extracted Skate 3 (the folder with
   `default.xex` and `data`) and the `skate-data` beside `halo.exe`:

   ```text
   python port\skate\converter\extract_sounds.py "D:\Skate 3" "C:\Games\Halo\skate-data\sounds" --vgmstream C:\tools\vgmstream\vgmstream-cli.exe
   ```

On Linux the same, with `vgmstream-linux.zip` from the same releases (one
static `vgmstream-cli`, FFmpeg included): `python3
port/skate/converter/extract_sounds.py ~/Skate3 path/to/skate-data/sounds
--vgmstream path/to/vgmstream-cli`. Without `--vgmstream` it looks beside the
script, then on `PATH`.

It takes a few seconds and says what it wrote, file by file, with where each
came from and how sure the mapping is (below); `SOURCES.txt` in the folder
keeps that list. The same game gives the same files. Run again, it replaces
the sound-set WAVs in the folder and leaves anything else. `--list` prints the
mapping without decoding anything; `--min-confidence medium` (or `high`)
leaves the sounds mapped with less confidence to the built-in set. To give one
sound back to the built-in set, delete its files (`pop*.wav`).

#### Where Skate 3 keeps its sounds

On the Xbox 360 disc they are in `data/audio`, in EA's `.big` archives of
the "EB" kind (version 3, big-endian: a 0x30-byte header, then 16-byte
entries of offset, in units of `1 << shift`, size and name hash, then
fixed-width names):

| archive | holds |
| --- | --- |
| `grains.big` | 14 recordings of rolling, one per surface and wheel (`concrete_smooth_hard.grain`, `metal_smooth_hard`, `wood_ramp_hard`, `asphalt_rough_soft`, ...): a header, a table, then an EA Audio Core SNR header and its data, about 20 s each, getting faster as they go |
| `wheels.big` | the wheels spinning in the air (`Whls_spins_Jump_1.snr`, `Whls_spins_Man_1.snr`) |
| `audiofiles.big` | 428 entries: the banks of every sound effect (EA ABK banks of SNR/SNS streams, `.abk`; EA "SPLC" sample banks, `.bnk`), the AEMS project files that drive them (`.csi`, `.ems`) and the ambiences (`.sns`) |
| `ambience.big`, `post.big`, `english/*.big`, `music` | ambiences, stingers, speech and music |

The codec is XMA2 throughout, mono, at 48 kHz (the grains 44.1 kHz).
vgmstream reads the grains (as `.snr`) and the ABK banks; it does not read
SPLC banks, so the script takes them apart: patches (each a few layers, each
a choice among samples), groups of patches (a sound and its variations), and
the samples, each an SNR header followed by its data. The banks are named for
what they hold (`GRINDS.abk`, `WHEEL_SKID_BANK.abk`, `Skate_Collisions.bnk`,
`fstep_skateshoe1_sm.abk`, ...), but nothing inside names the sounds, and the
event names in the `.csi` files (`play_grind_start`, `wheelskid_smth_shuff`)
do not say which sample they play. So the mapping comes from the banks'
names and structure and from each sound itself, unheard: its length, loop
points, envelope, spectrum and tonality.

#### The mapping

`MAPPING` in the script; each row with how sure it is.

| file | from | confidence |
| --- | --- | --- |
| `roll` | `grains.big`: `concrete_smooth_hard` (also every surface without its own) | high |
| `roll_metal` | `grains.big`: `metal_smooth_hard` | high |
| `roll_wood` | `grains.big`: `wood_ramp_hard` | high |
| `roll_rough` | `grains.big`: `asphalt_rough_hard`, the roughest: Skate 3 has no dirt or grass grain | medium |
| `powerslide` (2) | `WHEEL_SKID_BANK.abk`, its third table: wheel-skid grains, joined into loops | medium |
| `grind_metal` (4) | `GRINDS.abk` 115, 116, 118, 119: loops with the ringing lines of a metal rail | medium |
| `grind` (3) | `GRINDS.abk` 10-12: broadband, gritty loops, a concrete ledge by their sound | low |
| `land` (8) | `Skate_Collisions.bnk` group 181: the board's layered hit with the most variations of the bank (11, of up to seven layers), in the set of the board's hits (groups 179-190) | medium |
| `pop` (6) | `Skate_Collisions.bnk` groups 183, 186: the short single hits of the same set | low |
| `board_impact` (6) | `Skate_Collisions.bnk` groups 191, 194: rattling multiple hits, the board bouncing | low |
| `bail` (6) | `Skate_Collisions.bnk` groups 171, 173: low, heavy thuds, a body hitting the ground | low |
| `step_off` (4) | `fstep_skateshoe1_sm.abk`, its seventh table: skate-shoe footsteps | medium |

(the number of variants in brackets). The rest stay as they are: `slide`
keeps the built-in sample (no loop of `GRINDS.abk` could be told from a
grind), `grind_start` and `slide_start` fall back on `land`, `grind_end` and
`slide_end` stay silent, `step_on` falls back on `board_impact`, and the
other surfaces of `grind` on `grind`. Nobody has listened to the mapped ones
in a game yet.

The loops are cut from the grains past their middle (3 s) or joined from the
wheel-skid grains (2 s), their last 150 ms faded into their start, and the
grind loops (which loop whole in the game) have their last 10 ms faded into
their start; all are levelled to -17 dBFS RMS like the built-in loops. The
one-shots are a patch's layers mixed, trimmed to the hit, faded out and
peak-normalised to -3 dBFS like the built-in ones. Each file is 16-bit mono
at the sound's own rate.

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
a bail. `halo-skate/tests/real_assets_camera.rs` checks the following camera
off the board the same way: walking, it follows the skater, not the board.
Both share the harness in `halo-skate/tests/common/mod.rs`. They need
converted data, so they pass without running unless `HALO_SKATE_TEST_ASSETS`
names its `assets` folder; the engine is slow unoptimised, so run them with
`--release` (one `--test` per file, or none to run every test of the crate):

```text
HALO_SKATE_TEST_ASSETS=/path/to/skate-data/assets \
    cargo test --release -p halo-skate --test real_assets \
    --test real_assets_camera -- --nocapture
```

`halo-skate/tests/real_assets_sound.rs` checks the sound events the same way,
from the log: pushing makes no one-shot, an ollie pops then lands, onto the
rail the grind starts (no landing) and ends, a bail sounds once (no step
off, no landing when the engine puts the skater back), and Y steps off
(`--test real_assets_sound`). With `HALO_SKATE_TEST_SOUNDS` naming the
`skate-data/sounds` folder `extract_sounds.py` wrote, they play Skate 3's own
sounds instead of the built-in set (found beside the converted data, as the
game finds them; the ollie checks its pop and landing came from there), and
one more test, which needs no engine, checks the loader reads every file
`SOURCES.txt` lists and takes each sound written from that folder.

`halo-skate/tests/real_assets_halo_map.rs` does the same on a real level:
Hang 'Em High's collision, read at test time from your own Halo Xbox map
(`common/halo_map.rs`: the cache file's header, its inflated tags, the
scenario's structure BSP and its collision BSP's planes, surfaces, edges and
vertices, as OpenCE's own `source/cache` and `source/physics` lay them out)
and made into triangles exactly as `skate_collision_triangles` in
`../linux/game/skate.c` does (the level alone: the game adds the scenery
placed in it, below, "Obstacles"). It checks they are what the game logs for
the level without it (`map 1 sent, 2874 triangles`, `2874 triangles
deduplicated`, `rails: 2096 walkable edges, 525 lips, 494 runs, 470 rails`),
then gets on where Build 28's jump failed, gets off, stands 600 engine ticks
and jumps with that log's pad (twice), and sweeps spots across the level's
floors, four headings between them: standing then jumping, riding hard
into walls and off edges (bailing) then jumping, and walking into walls and
jumping into them, then landing, standing and jumping again (Build 28's way
to its NaN, below, "When the engine fails"). Every frame must be finite and
no step may fail (a session given up is made again, as the next J does, and
the sweep goes on).
It needs the maps folder too (`HALO_SKATE_TEST_HALO_MAPS`, with
`hangemhigh.map`; the collision test needs only that), and skips without
either. The sweeps try 8 spots each (two headings a spot), about 75
seconds in all; `HALO_SKATE_TEST_SWEEP` sets how many (0 for all 545 found,
over an hour). Nothing read from the map is written anywhere.

```text
HALO_SKATE_TEST_ASSETS=/path/to/skate-data/assets \
HALO_SKATE_TEST_HALO_MAPS=/path/to/halo/maps \
    cargo test --release -p halo-skate --test real_assets_halo_map -- --nocapture
```

`halo-skate/tests/real_assets_obstacles.rs` rides into obstacles
(`halo_skate_set_obstacles`, below, "Obstacles") on the test map's open floor,
sent every tick as the game sends them: a person (Master Chief's collision
pill) and a parked vehicle (a Warthog-sized box). After one push (4.8 m/s)
the board stops at the vehicle's side and the skater stays on; catching up
with a person walking away (closing at 2 m/s) is a bump too; at full speed
(8.5 m/s) both are a bail, nothing passes through, and the engine puts the
skater back on the board; and a vehicle driving at 5 m/s into the slowly
rolling skater (closing at 9.8 m/s) knocks them over. Another skater on their
board, sent as the game sends another machine's (a body capsule and the real
board's box, 0.95 m long, from `halo_skate_board_obstacle`): after one push a
bump, at full speed a bail, neither through them. Off the board, walking at
a wall sent as an obstacle 2.5 m ahead stops the skater 2 m on, where with
no wall it walks 30 m in the same five seconds. Every frame must be finite.
Sent as triangles (`SHAPE_MESH`, as the game sends a vehicle now): a real
Warthog's hull, read from Blood Gulch (`common/halo_map.rs`'s
`read_collision_model`, 60 triangles), across the lane stops the board at
its real side after one push and is a bail at full speed; a wedge 1.3 m
beside the path lets the board ride past where the box around it (as a
node was sent before) strikes it; standing off the board on a lift rising
from the floor to 1.1 m, the skater rides up and stands on its top; and the
level sent again with a wall across the lane (`halo_skate_reload`, as
scenery made mid-level is) stops the board there, the state ready all the
while, and sent again without it, the board rides past. It needs the
A teleporter (`halo_skate_teleport`) taking the skater riding at 8.1 m/s puts
them at its exit turned a quarter, riding on that way at 7.7 m/s. converted data (and the maps for the Warthog), and takes some 24 seconds
(`--test real_assets_obstacles`).

In CI they run in the `skate-assets` job of `.github/workflows/build.yml`
(every `halo-skate/tests/real_assets*.rs`, and a test that skipped fails it),
on the owner's self-hosted runner (`opence-k8s`), which has the data, the
extracted game and the Halo maps read-only at `/skate-assets` (the maps in
`/skate-assets/halo/maps`) and nothing else of the owner's.
The job writes Skate 3's sounds with `extract_sounds.py` (and vgmstream's
static Linux build, a pinned release checked by its SHA-256) into its
temporary folder for the sound tests, and never uploads them. The job runs
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
| camera | `halo-skate/src/camera.rs` | the heading the following camera keeps: on the board toward the board's horizontal velocity, off it (the engine's state category 500, as its own camera tells off the board) toward the skater's, a fifth of the way a tick the shorter way round, when faster than `skate_camera_speed`; held when slower, except that off the board and still for a second it turns a twentieth of the way a tick toward the way the skater faces (Skate 3's camera lines up so); always within 0 to 2 pi |
| recovery | `halo-skate/src/recovery.rs` | where a failed step puts the skater back on the board (the last good pose's place and heading), and when it gives up and the session is made again (below, "When the engine fails") |
| hooks | `source/main/main.c`, `hs/hs.c`, `scenario/scenario.c`, `game/game.c`, `units/bipeds.c`, `game/player_control.c`, `camera/director.c`, `render/render.c` | preload the skater at startup; take the tuning commands before the script compiler; build each structure BSP's collision as it loads; step before the objects update and pose after it; skip a skating biped's own movement; give the pad to the engine; follow the skater with the third-person camera; draw the board after the objects |
| input | `../linux/src/sdl_platform.c`, `xinput_sdl.c` | J, and the first pad as an Xbox 360 pad |
| sound | `halo-skate/src/sound.rs`, `sound_set.rs`, `mixer.rs`, `include/halo_skate_sound.h`, `../linux/game/skate_sound.c`, `../linux/src/dsound_sdl.c` | the events and loops from the engine's ticks, the sound set, the voices mixed into the game's output, and the listener, surface and volume from the game (above, "Sound") |
| obstacles | `../linux/game/skate.c`, `halo-skate/src/obstacles.rs`, `vendor/skate-core/src/physics/kinematic_obstacle.rs`, `vendor/skate-host/src/physics/solve/obstacles.rs` | the bipeds, vehicles, scenery and devices near the skater, solid to the board and the skater each tick (below, "Obstacles") |
| multiplayer | `../linux/game/skate_network.c`, `../linux/game/network_distributed.c`, `halo-skate/src/board.rs` | each machine's skater's state to the others, and theirs posed and their boards drawn here (below, "Multiplayer") |

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
that is not a number (a launch it cannot follow says `Nonfinite BipedAir
launch packet`, with the packet's values). Two came from real levels'
geometry, whose surfaces are not quite along the axes, and are fixed in
`vendor/skate-core`; `halo-skate/tests/real_assets_halo_map.rs` finds both on
Hang 'Em High:

- Build 28's, a jump off the board: NaN in the unused fourth (w) lane of
  two vectors. A jump from walking into a wall restarts the jump along the
  wall's normal, which the native permute builds with X repeated in w
  (`biped_air/collision.rs`); a wall off an axis gave the launch, the
  trajectory and so the root frame a w, the landing put it into the walk,
  and the walking skater's support velocity grew it by the golden ratio a
  tick, to NaN within some 180 ticks, which the next jump's launch packet
  met. The restart normal is now a direction (w 0), the Biped placement
  takes geometric vectors and the support velocity ignores w
  (`ground_motion/support.rs`, `controller/placement.rs`).
- `Non-finite torque_acceleration before shared solve`, riding: standing
  still, the filtered up and ground normal decay to within 1e-20 of an axis
  they nearly lie on, a vector's squared length goes subnormal, and the
  Newton refinement of its reciprocal square root overflows to NaN, which
  the Xbox 360 (denormals as zero) never meets. A subnormal squared length
  is now a zero length (`physics/native_arithmetic.rs`).

The session survives a failed step, so the skater
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

### Obstacles

The level's structure BSP is the skater's world (above, "Loading"), and with
it the level's static scenery, which never moves: when a structure BSP has
loaded, on the next tick (a new map's scenery is placed after its first BSP
loads), `skate_load_map` in `skate.c` adds every piece of scenery's collision
model to the BSP's triangles, each node's BSP made into triangles as the
level's are and placed by the node (`skate_add_scenery`). To the engine a
rock, a crate or an arch is then the level: the board and the skater meet
its slopes and its top as they are, at any size; the skater walking off the
board stops at it; and its edges are grind rails, which the rail finder
(`rails.rs`) takes with the level's. The log says what went
(`skate: the level's collision: 6908 triangles, 1440 of them 23 pieces
of scenery` on Blood Gulch).

Scenery a script makes, destroys, deletes or moves later changes the level:
`skate_watch_scenery` looks at the scenery twice a second (which pieces,
where, and which permutation of their collision they show, as a hash), and
once it has not changed for a second (three at most after the first change,
so a burst of pieces is one rebuild) sends the level's collision again with
the scenery as it is then (`halo_skate_reload`, logged as `skate: the level's
collision again, its scenery changed: ...`). The engine builds it, rails and
all, on a thread of its own while the skater rides the old one (some 150 to
170 ms for Blood Gulch's 6,900 triangles: `map rebuilt aside in 171ms`), and
swaps it in between steps when the skater is neither on a rail nor in the
air, whose grind selection holds the old rails (`map rebuilt, 6854
triangles, 461 rails`). The state stays ready throughout. Until then a piece
made is an obstacle (its own collision, below) and a piece gone stays solid.

The other objects move, and go to the engine each tick, as obstacles:
`skate_send_obstacles` looks 8 world units (24 m) around the skater's biped
for collideable objects and sends the 32 nearest through
`halo_skate_set_obstacles`:

- **bipeds**, players and AI, yours and other machines' alike (another
  machine's where the host says, as for everything else), not dead, not in a
  vehicle, and not the skater: a capsule standing at the biped, its collision
  pill (`biped_get_physics_pill` in `bipeds.c`: the biped's collision radius,
  and its height as it stands or crouches; as tall for AI too, whose pill
  Halo keeps a sphere). Its velocity is taken up to 10 m/s, past any run:
  another machine's unit jumps where the host says after a respawn, a
  teleporter or a correction, and its velocity briefly says so.
- **another machine's skater** (below, "Multiplayer"): their body and their
  board as they are posed here, kind *skater* (`skate_skater_obstacles`):
  the body a capsule from between their feet (`bip01 l foot`, `bip01 r
  foot`) to their head (`bip01 head`), 0.09 world units (27 cm) round, so
  that it leans, crouches and lies down with them; the board a box around it
  as drawn, turned as their state has it (`halo_skate_board_obstacle`: the
  box around the board meshes in the deck joint's axes, `board.rs`'s
  `local_box`, grown as drawn). Both move at their unit's velocity, taken up
  to 20 m/s. A skater whose biped lacks those nodes is their pill.
- **vehicles, devices** (machines and controls: doors, lifts, switches)
  **and scenery not yet in the level** (above): their own collision, a mesh
  of triangles. Each node's collision BSP is made into triangles once, as
  the level's are (`skate_bsp_triangles`, kept until the next structure
  BSP), and each tick placed by its node as the object is now: a vehicle's
  hull, a turret, a door that opens and a lift that rises move with their
  nodes. Only the triangles within 3 world units (9 m) of the skater go
  (`skate_mesh_triangles`: a node's sphere, then each triangle's), at most
  4,096 a tick, so a building-sized device costs no more than its near side,
  and there is no size limit. A device's velocity is how far it moved since
  the last tick (Halo moves a lift by placing it); a vehicle's turn goes too,
  so a contact on its nose moves as the nose does.

Each carries on at its velocity (`translational_velocity`) until the next
tick. On the engine's side (`kinematic_obstacle.rs` in `vendor/skate-core`)
they join the one solve the board and the skater's body share, the way
Skate 3's other assemblies (cars, other skaters) join it: each obstacle is a
body the solver treats as fixed (state flags 1: never integrated, no inverse
mass, as the static world), but with the obstacle's velocity, so a contact's
relative velocity, and so its response, is the one against a moving object;
its shape is a capsule or a rounded box, the shapes the skater's own parts
are made of, or a mesh: each of its triangles near the board or the skater a
volume of its one body, made into the native triangle volume the static
world's are (one-sided, each edge convex or a crease by the face across it,
so the board slides along a hull's creases and stops at its corners:
`ObstacleMesh`), and its contacts with the board's parts and the skater's
world-colliding parts come from the engine's own primitive-pair query
(`primitive_pair_contacts`, as the board and the skater collide with each
other; `solve/obstacles.rs` in `vendor/skate-host`). Nothing on Halo's side
is pushed: the obstacle goes where Halo has it.

Off the board (walking, jumping from walking: the engine's BipedGround and
BipedAir), the skater is moved by Skate 3's own walking controller, which
sweeps lines and spheres through the static world's triangles rather than
joining the solve. The obstacles go to it as triangles: each one's outside
(a box's twelve, a capsule as the eight-sided prism around it, a mesh's
own, `KinematicObstacle::triangles_at`) where the game sent it, with the static
triangles to its sweeps and nearby-triangle queries
(`BoardWorld::set_offboard_obstacles`, `offboard/contact_toolkit/world.rs`
in `vendor/skate-host`), so a walking skater stops at a person, a vehicle or
a door as at a wall. Its seven ground probes take them too
(`ground_query/world.rs`), so a walking skater stands on a vehicle's roof, a
crate or a lift, and rises with a lift as with a slope. The controller has
no moving ground, though: it does not carry the skater along with a
platform that slides or a vehicle that drives off under them; they stay
where they stand and the platform moves away beneath.

What a hit does, riding:

- **The board** stops at it, as at a wall: its contacts with an obstacle are
  reported to the board as the world's (`board_reports.rs`), so its closing
  speed, ground state and Skate 3's own wall checks see it. Skate 3 itself
  bails a rider into a wall only past 7 to 9 m/s of closing speed
  (Wipeout_GroundXZAcceleration, by the state's mode), which riding on the
  flat hardly reaches (8.5 m/s at most): the board stops and the skater
  stays on, as `real_assets.rs`'s box ledge shows.
- **The skater's body** colliding with one counts by Skate 3's own classes
  (`collision_update.rs`): a person or another skater as another skater
  (group 5, its thresholds 1.5 times the world's), a vehicle as a car (group
  8, with its own bail past 9 m/s against the body,
  Wipeout_GroundVehicleContact), scenery as the world. Riding at 4.8 m/s into
  a person who stands still is already a bail by those (the board stops at
  their feet and the body runs into them).
- **A bail speed**: running into a person, a skater or a vehicle faster than
  `skate_obstacle_bail_speed` (6 m/s at first) along the contact, on the
  board, knocks the skater over: the wipeout requests a body hit and a
  closing hit, as Skate 3's checks do, two of them, so a bail rather than a
  run-out. Slower is the solve's bump alone. Scenery is left to Skate 3's
  thresholds, as the level's walls are.

The log tells of each hit, riding, with what followed (`obstacles.rs`):
`halo-skate: hit a biped at 8.3 m/s (PhysicsGround, engine tick 1068):
bailed 1 engine ticks later`, or `...: rode on (PhysicsGround)`; `hit a
skater` for another machine's skater.

Grind rails come from the static world alone. The engine's grind system
takes the rails as splines when the map's collision is built (with the
primitives it makes of them: `SKATE_GRIND_READY splines=... primitives=...`
in the log) into a static provider that never changes after; so static
scenery's edges are rails (above), but a vehicle's, a door's or a lift's are
not: a moving rail would need the grind world rebuilt as the object moves,
or a provider whose splines move with a frame and a grind that rides along
with them, neither of which the engine has. A parked vehicle is no rail
either, since it may drive off at any time.

## Multiplayer

In a network game (system link, internet play, co-op) everyone sees
everyone's skating: the skater's pose, their board under them and their
weapon holstered, wherever they ride. Each machine still runs the skate
engine for its own skater alone; the others see them from a small state sent
every tick (`../linux/game/skate_network.c`).

The skate rig keeps every node's bone length (`rig.rs`: a node is its
parent's place plus its parent's turn of the bind pose's offset between
them), so the pose is each node's turn from the bind pose and the root
node's place, and any machine rebuilds it from the biped's own model. The
state of a skater, 20 bytes and 4 a node (96 bytes for the cyborg's 19):

| bytes | what |
| --- | --- |
| 1 | the player (their absolute index) |
| 1 | on a board (or walking off it in the skate engine), the board's place given, walking |
| 1 | how many nodes follow (0: no pose) |
| 1 | the format, 1: a state of another format is skipped |
| 6 | the root node from the unit's origin, 16 bits a part, 1/1024 world units (at most 32 world units) |
| 6 | the board from the unit's origin, the same way |
| 4 | the board's turn: a quaternion's three smallest parts, 10 bits each, and which part was left out |
| 4 a node | each node's turn from its bind pose, the same way |

A client sends its own skater's state to the host every tick, unreliably
(`_distributed_message_skate_up`, 90), and for a second and a half after it
gets off a state that says so. The host sends each client the states of the
players on the other machines, its own skater's too
(`_distributed_message_skate_down`, 91), as often as it sends those players'
units (every tick near, less often far or out of sight), and getting off
every tick. A late state that a newer overtook is dropped, as the netcode's
other unreliable messages are. Another machine's state counts for a second
and a half; one not heard of since is no longer skating, nor is a player
who died or got into a vehicle.

Each machine poses another machine's skater from the latest state it has,
around where it has their unit, after the objects update (so it is drawn
between ticks like any biped), and holsters their weapon on that pose
(`skate.c`). The board is placed whole where the state has it (its deck's
joint, `board.rs`'s `placement` and `place`), drawn between the last two
ticks it was placed at, lit at the skater. Halo's own movement and animation
of a skater's biped stop on every machine (`bipeds.c`, as for your own); the
unit goes where the host says, gliding as other players' units do, and
carried on at its velocity between the host's words, so it keeps up rather
than trailing a tick behind each one.

The host decides where players are, as for everyone: it takes a client's
word on where its own player is within bounds of how far and how fast a
player can go (`port/linux/NETCODE.md`). While a client's state says its
player skates, the host holds them to a board's bounds instead of a
runner's: across and up 40 m/s (where a runner is held to twice as fast as
they run and jump, some 15 to 20 m/s, which a board passes downhill), up as
high as a 15 m/s launch takes them, never held to a fall (the host's copy
does not move by itself, so its airborne flag says nothing), and, once it
has taken a place of theirs, as far from it as a board goes in the ticks
since rather than within 3.5 world units of its copy, so that a few
datagrams lost never leave the copy too far behind to take again. A client
corrects no skater of its own: the skate engine has them where they are.
The host takes no hit from a skater, who has no weapon in hand.

What it costs and allows:

- **Bandwidth**: 96 bytes a skater a tick, 2.9 KB/s at 30 ticks a second,
  each way per skater, less for far players; nothing for one who is not
  skating. Nodes that follow no skater's bone (the hands) inherit their
  parent's turn but are sent anyway, to keep the format simple.
- **Cheating**: a client that says its player skates may move them as fast
  as a board goes (40 m/s), where a runner is held to twice their running
  speed: a speed hack by that much, but with no weapon (its hits are
  refused while it skates). Nothing checks that the skating is real; the
  host does not run the client's skate engine.
- **Latency**: the pose is where the client had it, a one-way trip late on
  the host and a round trip late on other clients, as their units are.
- **Lost datagrams** (`debug.network_loss`): the latest state is held, so a
  lost one shows the pose a tick older; the unit carries on at its velocity.
  `debug.network_latency` holds everything back alike, and the pose and the
  unit stay together.

Other players are solid to a skater, on the skater's machine: their bipeds are
obstacles there like any other (above, "Obstacles"), where the host has them,
carried on at their velocities as Halo carries them; riding into one bumps or
bails as into anyone. Nothing is sent for it, and nothing moves them.

Two skaters are solid to each other, each on their own machine: each machine
has the other machine's skater as their posed body and their board (above,
"Obstacles"), from the state it already gets, so riding into each other
bumps or bails both, each by their own engine (`hit a skater`), as each
sees the other: a one-way trip late on the host and a round trip late on
another client, as everything of another player's. Nothing more is sent.

Halo's side of a skating biped, on every machine, is as for any player's,
but that Halo never moves it (its own physics are off, `bipeds.c`): a player
on foot meets its pill where the skate engine has it, as Halo tests any
biped's (players pass through each other only where Halo lets them, a co-op game's
no-collisions setting); a vehicle that hits it damages it as it would any
biped, and the push it gives goes nowhere, as the skater's own machine has
the vehicle as an obstacle and bumps or bails off it; projectiles hit its
collision model as posed (the last tick's pose), and a skater killed falls
off the board.

Projectiles and blasts meet a skater as they meet anyone on foot in Halo: a
round hits the posed body and damages it, head and all, on the skater's own
machine and on the others (the host's projectiles hit its copy of another
machine's skater where it poses them, and the damage is the host's as for
anyone); a grenade thrown at a skater bounces off their body as off a
standing player's, and its blast damages them. Neither moves them: Halo's
push (`biped_accelerate`, a blast's or a vehicle's) goes into the biped's
velocity, which the skate engine's replaces the next tick, as it places the
biped (`skate_apply_frame`, which logs a push it undoes: `skate: a push of
4.3 m/s on the skater undone`). Weapons lying about and grenades are no
obstacles to the skater, as they are none to a player on foot.

A teleporter takes a skater: Halo's teleporter (`game_engine.c`'s
`game_engine_update_teleporter`, at a netgame flag's entrance) puts their
unit at its exit, turned by the exit's facing less the entrance's, as for
anyone; the next tick `skate_follow_teleport` (`skate.c`) finds the biped
more than a world unit from where it put it and puts the skate engine's
skater there (`halo_skate_teleport`): on the board, turned as the unit was,
moving as fast as before, now the same way round from the exit as they went
into the entrance (`skate: teleported 42.88 world units to (82.83 -114.53
0.70), turned 86 degrees, at 8.1 m/s`). The camera turns with them and the
board is drawn there at once, not slid across the map. A script's
`object_teleport`, or anything else that moves the unit, is followed the
same way. Each machine runs the teleporter for its own players, as for
players on foot; a client's skater the host puts more than 4 world units
from where the client has them is moved there too (`network_distributed.c`),
and followed. The other machines see the skater jump as the host has them.

Machines that cannot skate play on: a build without Skate 3 mode (Android,
or without `--skate`) still sends and takes the states, poses others' skaters
and holds the host's bounds; it draws no board. A build older than this one
drops both messages as kinds it does not know, so the network version is
unchanged: its players see a skater as Halo's own biped gliding where the
host says, and its host holds them to a runner's bounds, as before. A
machine that joins a game in progress hears the states as soon as it has
loaded. Co-op plays the same way. There is no host migration: when the host
leaves, the game ends for everyone.

### Testing multiplayer

`debug.network_test_skate` (`HALO_NETWORK_TEST_SKATE`) puts this machine's
player on a board that many seconds into a network test game
(`port/linux/NETCODE.md`, "Testing"), riding a scripted pad: pushes, a slow
carve and an ollie every four seconds, stepping back on after a fall; off
the board 40 seconds on, and on again 5 later.
`debug.network_test_skate_watch` (`HALO_NETWORK_TEST_SKATE_WATCH`) turns this
machine's player to watch another machine's skater, and a host stands its
player near them. Once a second each machine logs its own skater (where,
how fast, how big its state is, and how far the state's pose rebuilt is from
the pose: some 0.0007 world units and 0.1 degrees at worst) and every other
machine's (posed or not, the board, how old the state is, and on the host
how many of the skater's places it took and refused).
`debug.network_test_skate_block` (`HALO_NETWORK_TEST_SKATE_BLOCK`), with the
watch on a host, steps its player into the skater's way every five seconds,
1.5 world units ahead of where the host has them, the way they go (when
faster than 1.5 m/s): on the skater's machine an obstacle, another machine's
biped where the host says, for them to ride into. The skating machine logs
what is near once a second (`skate test: 3 obstacles near (1 bipeds, 0 of
skaters, 0 vehicles, 2 scenery and devices; 96 triangles), the nearest a biped 0.53 world
units away`), and the engine each hit with what followed (above,
"Obstacles").
`debug.network_test_skate_duel` (`HALO_NETWORK_TEST_SKATE_DUEL`) puts this
machine's player on a board that many seconds into the game, standing on it
(Y stepping back on after a fall); with the block on a host, the five-second
step into the skater's way is then on the board (`skate_put`: on the board
again there, facing them), skater into skater. A test player who should be
on a board and is not (dead, or a J that failed) gets on again every two
seconds once alive (the log tells of a death: `skate test: this machine's
player died at ...`), and the watcher is only ever put where nothing of the
level stands between it and the skater and there is ground under it, so that
it is never put into a rock or off a ledge to fall and die and miss its turns.
`debug.network_test_skate_scenery` (`HALO_NETWORK_TEST_SKATE_SCENERY`) deletes
the piece of scenery nearest this machine's player that many seconds in, and
makes it again four seconds later.
`debug.network_test_skate_shoot` (`HALO_NETWORK_TEST_SKATE_SHOOT`), with the watch on a host,
shoots skaters at set times with real projectiles, as weapons fire them: a
burst of three rounds of its weapon from 1.5 world units off, and a grenade
thrown at their chest, at another machine's skater and, once the host skates
(the duel), at its own; the log tells the damage (`skate test: a burst at a
skater (another machine's): shield 1.00 to 0.57, ...`), whether the grenade
bounced off them, and the skater after its blast. A grenade kills a skater,
and the watcher near, as it would anyone, so the shots are few.
`debug.network_test_skate_teleport` (`HALO_NETWORK_TEST_SKATE_TELEPORT`) puts a
host's own skater on a teleporter's entrance that many seconds in, for Halo's
teleporter to take.

`tools/test_linux_port.py`'s `test_skate_multiplayer` runs a host and a
client on Blood Gulch on one machine, the client skating, with a round trip
of 150 ms and a tenth of the datagrams lost, the host's watcher stepping into
the skater's way, on foot until 56 seconds in and on a board of its own
after, and checks those logs (about two and a half minutes): besides the
pose, the board and the places, that the watcher stepped in, that the client
had them as an obstacle, and that the skater hit them (a bump or a bail)
rather than passing through; then that the host's skater stepped in, that
the client had them as a skater's body and board, and that each machine's
engine logged a hit on a skater (the client's riding in, the host's ridden
into); that each loaded Blood Gulch with its scenery; and that the client,
deleting a piece of it 30 seconds in and making it again, built the level
again with one piece fewer, then as before, each swapped in. The host also
shoots: a burst must damage a skater on each machine, a grenade must bounce
off one, and one must go off at the host's own; and it puts its own skater on
Blood Gulch's teleporter, which must take them to its exit. It needs a build with `--skate`, the converted data and the
maps, and skips without them:

```text
HALO_TEST_GAME=build/linux/halo \
HALO_SKATE_TEST_ASSETS=/path/to/skate-data/assets \
HALO_SKATE_TEST_HALO_MAPS=/path/to/halo/maps \
    python -m pytest -q tools/test_linux_port.py -k skate_multiplayer
```

The games run headless, with the null renderer (`HALO_NULL_RENDERER`, which
the test sets); on a machine without a display or sound, add
`SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy`. Neither game exits by itself
under the null renderer, so the test stops both at a deadline, and whatever
happens.

CI runs it on every push, in the workflow's `skate-multiplayer` job, on the
owner's runner (`opence-k8s`, which has the data; see `skate-assets` in
`.github/workflows/build.yml`): it builds the game as `tools/ci_build.py linux
debug` does (`--skate --lto=off --pgo=off --portable`; the compile cache and
the engine's cargo build are kept from run to run), runs the test headless,
and fails if it skips. Like `skate-assets`, the release does not wait for it.

`tools/harness/tests/test_skate_state.py` checks the state itself, without
any data, on every build: poses of a random skeleton sent and rebuilt within
0.003 world units and a third of a degree, offsets too far to send left out,
and damaged or foreign bytes refused.

## Known gaps

- **Play-tested in part.** The physics, the controls, the following camera
  and the pose work in a game. The Windows build was cross-compiled and linked
  on Linux, not built on Windows.
- A skater model without board meshes leaves the board undrawn (the log says
  why).
- **Moving objects carry no one.** A walking skater stands on a vehicle or a
  lift (above, "Obstacles") but is not carried along by one that slides or
  drives off. A vehicle's edges and a door's are no rails to grind (the
  engine's rails are static; scenery's are rails, rebuilt as it changes).
  Items (weapons, equipment) and projectiles are passed through, as by a
  player on foot in Halo. The bail speed (`skate_obstacle_bail_speed`) is a first
  guess.
- **The sounds are not heard in a game yet.** The events, the sound set and
  the mixer are tested on their own (`cargo test -p halo-skate`), and the
  game builds with them, but nobody has skated with them on: their levels
  and thresholds are first guesses.
- **The node names are assumed.** The retarget expects the cyborg's
  `bip01 pelvis`, `bip01 l thigh` and so on. The game's console prints how many
  nodes follow the skater, and stderr lists the biped's node names.
- **In a network game, each machine decides its own skater's hits**: a
  skater riding into a player does not move them, and two skaters running
  into each other each bump or bail by what their own machine saw, which
  latency may make one-sided.
- **Another machine's board is placed whole**: its wheels and trucks do not
  turn, and a machine without the Skate 3 data draws no board at all (it
  still poses the skater).
- The camera cannot be turned by hand while skating; it follows the way you
  travel.

## Licence

The skate engine is GPL-3.0 (`vendor/LICENSE-GPL-3.0`), so a build made with
`--skate` is GPL-3.0 as a whole. `halo-skate/src/rails.rs` is Apache-2.0 from
the mashup. A build without `--skate` contains none of it.
The built-in sounds (`../assets/skate-sounds`) are CC0 recordings from
Freesound and two synthesized ones (above, "The built-in set";
`CREDITS.txt` beside them).

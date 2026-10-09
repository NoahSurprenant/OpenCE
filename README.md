# Halo: Combat Evolved for Linux, Windows and Android

[![Join our Discord](https://invidget.switchblade.xyz/9gqcHyr5km)](https://discord.gg/9gqcHyr5km)

This project is a port of the Halo: Combat Evolved decompilation to Linux,
Windows and Android. The decompilation is of the Xbox build 2342
(`cachebeta.exe`, SHA-256
`4cc87b45f721270392a96f1674ed2b5cd4a7bb4355faeab4531d1cf1884d9520`).

<img width="1289" height="995" alt="The game on Linux" src="https://github.com/user-attachments/assets/0d3ad50f-f8b8-46cf-aef8-e3661da2a7d7" />

The port starts from the decompilation of [bnunu/halo-1](https://github.com/bnunu/halo-1).
That project is a fork of [punpckhdq/halo](https://github.com/punpckhdq/halo).

> **This fork adds Skate 3 mode.** On Linux and Windows, press **J** (or
> click both sticks in) to put Master Chief on a skateboard driven by Skate
> 3's own physics, tricks and grinds. It needs your own copy of Skate 3; see
> [Skate 3 mode](#skate-3-mode) below. The rest of the game is
> [OpenCE](https://github.com/OpenCommunityEdition/OpenCE), unchanged.

## Download

GitHub Actions builds the game for each commit. These links download the
builds of the latest release:

| Platform | Release | Debug |
| --- | --- | --- |
| Linux | [halo-linux-release.zip](https://github.com/NoahSurprenant/OpenCE/releases/latest/download/halo-linux-release.zip) | [halo-linux-debug.zip](https://github.com/NoahSurprenant/OpenCE/releases/latest/download/halo-linux-debug.zip) |
| Windows | [halo-windows-release.zip](https://github.com/NoahSurprenant/OpenCE/releases/latest/download/halo-windows-release.zip) | [halo-windows-debug.zip](https://github.com/NoahSurprenant/OpenCE/releases/latest/download/halo-windows-debug.zip) |
| Android | [halo-android-release.zip](https://github.com/NoahSurprenant/OpenCE/releases/latest/download/halo-android-release.zip) | [halo-android-debug.zip](https://github.com/NoahSurprenant/OpenCE/releases/latest/download/halo-android-debug.zip) |

Use the release build to play. The debug build stops at the first failed
assertion and writes it to the log. Use the debug build to find and report
problems.

The game updates itself. At start-up it looks for a newer release, and asks
if you want to install it. Refer to "Updates" in
[port/linux/README.md](port/linux/README.md#updates).

Each build of the `main` branch that passes on all three platforms is a new
release. The [Releases](https://github.com/NoahSurprenant/OpenCE/releases)
page keeps the last five releases. If the latest build has a problem, get
an older build from that page.

## Game data

The port does not include the game data. Download an Xbox disc image
(`.xiso` or `.iso`) of Halo: Combat Evolved. All versions of the game
operate. The maps of the European (PAL) version were made for a slower
console. The port changes them to play as the North American (NTSC) maps do,
so players of the two versions can play together.

1. Start the game.
2. At the first start, the game asks for the disc image. Select it.
3. The game extracts the `maps/` folder. Then the game starts.

On Linux and Windows, the game puts `maps/` next to the executable. On
Android, copy the disc image to the phone first. The app puts `maps/` in its
data folder. Refer to [port/android/README.md](port/android/README.md).

## Skate 3 mode

Skating works in the Linux and Windows builds. It needs Skate 3's data,
converted once from your own copy of the game. None of Skate 3 is included
here. Set it up after the game itself runs (see [Game data](#game-data)).

### 1. Get Skate 3's files

You need **Skate 3 for Xbox 360, extracted**: the `default.xex` from the
root of the disc, with the `data` folder beside it.

- From an ISO of your disc, run
  [extract-xiso](https://github.com/XboxDev/extract-xiso):
  `extract-xiso -x "Skate 3.iso"`.
- From a Games on Demand copy on a 360 hard drive, open its container with a
  tool such as Velocity and extract it.

The ISO itself does not work, only the extracted folder:

```text
Skate 3/
├── default.xex
├── data/
└── ...
```

### 2. Convert them

The converter is a Windows program, `iw4l-skate-convert.exe`, from
[2010 Rust Rewrite Mashup](https://github.com/chasmlol/2010-rust-rewrite-mashup),
the project this mode's skate engine comes from. Download
`2010-Rust-Rewrite-Mashup-windows-x64.zip` from its
[latest release](https://github.com/chasmlol/2010-rust-rewrite-mashup/releases/latest)
and take `skate\iw4l-skate-convert.exe` out of it.

Run it once, writing into a `skate-data` folder next to the game
(`halo.exe` on Windows, `halo` on Linux).

**Windows**, in a Command Prompt:

```text
iw4l-skate-convert.exe --xex "C:\Games\Skate 3\default.xex" --out "C:\Games\Halo\skate-data"
```

**Linux**, with [Wine](https://www.winehq.org/):

```text
wine iw4l-skate-convert.exe --xex ~/games/skate3/default.xex --out ~/games/halo/skate-data
```

It finishes with `Skate 3 data ready` and leaves `skate-data/assets`: the
skater, the animations and the physics settings. Your Skate 3 folder is not
changed.

### 3. Skate

Start the game from its own folder, so that it finds `skate-data/assets`.
Double-clicking `halo.exe` does this; on Linux, `cd` to the folder first. To
keep the data somewhere else, set `HALO_SKATE_ASSETS` to its `assets`
folder.

Load any level and press **J**. The first time on a level, the game loads
the skater and the level's collision before Chief gets on the board, which
can take a moment. The console (the backquote key) shows `skate: on`, or why
not.

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

A controller is best: the tricks are stick gestures (pull down, then flick
up for an ollie). While skating, Halo's own movement, firing and looking are
off, and the camera stays behind the board.

### Known gaps

Skate 3 mode is new and has barely been played.

- The board itself is not drawn.
- Only the level is solid to the skater: scenery, vehicles and other
  objects are passed through.
- In multiplayer, other players see Chief in Halo's own animation, and a
  host holds a skating client back to about twice running speed.
- Android builds do not skate.

How it works, and how to build it: [port/skate/README.md](port/skate/README.md).
Builds with Skate 3 mode are GPL-3.0 as a whole, because the skate engine
is.

## Platforms

Each platform has its own instructions:

| Platform | Instructions |
| --- | --- |
| Linux (32-bit x86 executable, OpenGL 4.5, SDL3) | [port/linux/README.md](port/linux/README.md) |
| Windows (32-bit x86 executable, OpenGL 4.5, SDL3) | [port/windows/README.md](port/windows/README.md) |
| Android (arm64 app, OpenGL ES 3, SDL3) | [port/android/README.md](port/android/README.md) |

The Linux README also gives the controls, the settings and the multiplayer
functions. These are almost the same on all platforms.

## Multiplayer

The game can play system link games on a local network and on the internet:

- A system link game can have up to 128 players on up to 128 machines.
- Linux, Windows and Android machines can play in the same game.
- An invite link lets a machine join a game on the internet. No server of
  this project is necessary.
- The netcode is new. Each machine moves its own player at once,
  and the host makes the decisions for the game. Refer to
  [port/linux/NETCODE.md](port/linux/NETCODE.md).

## Build the game

You do not need the Xbox SDK. The port supplies the SDK declarations that
the game uses. Refer to [port/include/xdk](port/include/xdk/README.md).

To build the game:

1. Install Python and [ninja](https://ninja-build.org/).
2. Install the tools for your platform. Refer to the README for the
   platform.
3. In the root folder of the repository, enter `python configure.py`.
4. Enter `ninja` with the target for the platform:

| Target | Result |
| --- | --- |
| `ninja linux` | `build/linux/halo` |
| `ninja windows` (on Windows) | `build/windows/halo.exe` and `SDL3.dll` |
| `ninja android_apk` | `port/android/app/build/outputs/apk/debug/app-debug.apk` |

If you enter `ninja` without a target, ninja builds the game for the
computer that you use.

`tools/ci_build.py` makes the same builds as GitHub Actions. For example,
enter `python tools/ci_build.py linux release`.

### Build options

Give these options to `configure.py`:

| Option | Result |
| --- | --- |
| (none) | A debug build. A failed assertion stops the game. |
| `--release` | A release build. The game does not examine assertions, as in the retail game. |
| `--portable` | The Linux and Windows builds operate on all x86-64 processors. Use this option for builds that you give to other persons. |
| `--lto=thin`, `--lto=off` | Less link-time optimization. The link is faster. |
| `--pgo=off` | No profile-guided optimization. |
| `--pgo=train` | Records a new optimization profile. Refer to "Optimization profiles". |

Without `--portable`, the Linux and Windows builds use all the instructions
of the processor that builds them (`-march=native`). Such a build does not
always start on a different computer.

### Optimization profiles

The builds use profiles of the game to optimize the code:

- `pgo/halo_linux.profdata` for Linux and Android.
- `pgo/halo_windows.profdata` for Windows.

The profiles need clang 22 or later. With an older clang, the builds do not
use the profiles.

To record a new profile:

1. Delete the profile.
2. Enter `python configure.py --pgo=train`.
3. Enter `ninja linux` or `ninja windows`.

The build then plays the main menu and the first minute of each campaign
level. This procedure continues for approximately 15 minutes. The game
data must be in `assets/`.

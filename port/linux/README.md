# Linux

`ninja linux` compiles the game with clang for 32-bit x86 Linux. The result
is a native executable, `build/linux/halo`. The game shows its graphics with
OpenGL 4.5. It plays sound through SDL3. It accepts keyboard, mouse and
gamepad input.

The game is 32-bit code because its data (tags, cache files, saved games)
contains 32-bit pointers, as on the Xbox.

## Requirements

You do not need the Xbox SDK. The declarations that the game uses are in
`port/include/xdk`.

To build:

- Python and ninja.
- clang. The option `--linux-cc` of `configure.py` selects a different
  compiler.
- The 32-bit glibc development files: `lib32-glibc` on Arch Linux,
  `gcc-multilib` and `libc6-dev-i386` on Debian and Ubuntu.
- The 32-bit SDL3: `lib32-sdl3` on Arch Linux, `libsdl3-dev:i386` on Debian
  and Ubuntu. The portable build does not need it: refer to "Portable
  build".

To start the game:

- glibc 2.29 or later (32-bit), for the builds from GitHub Actions, which
  are built against glibc 2.31. They start on SteamOS 3 (Steam Deck). They
  should also start on Debian 11, Ubuntu 20.04, Fedora 32 and later
  distributions, but this is not tested. A build that you make without
  `--portable` needs the glibc of the computer that built it, or a later
  one.
- The 32-bit OpenGL libraries (`lib32-mesa`).
- The 32-bit PipeWire or PulseAudio client libraries (`lib32-pipewire` or
  `lib32-libpulse`).
- The 32-bit X11 libraries (`lib32-libx11`, `lib32-libxext`). In a Wayland
  session, the game uses them through XWayland. SDL uses Wayland itself
  only if the 32-bit Wayland libraries are version 1.20 or later
  (`lib32-wayland`, `lib32-libxkbcommon`; Debian 11, Ubuntu 20.04 and
  Fedora 32 have older ones) and the compositor has the fifo-v1 protocol.
  On GNOME, the window then has borders only with the 32-bit libdecor
  (`lib32-libdecor`).

SteamOS has all of these. Its system is read-only, and the builds from
GitHub Actions need no package: they bring their own SDL3
(`libSDL3.so.0`, next to the executable).

### Portable build

The builds from GitHub Actions are portable builds (`--portable`; refer to
"Build options" in the main [README](../../README.md#build-options)). The
portable build starts on more systems than the build machine's:

- It is compiled and linked against the 32-bit glibc 2.31 of Debian 11, the
  glibc of the Steam Runtime 3 ("sniper"), not against the glibc of the
  build machine. An executable needs the glibc version that it was built
  against, or a later one.
- It brings SDL 3 (`libSDL3.so.0`), built from source against the same
  glibc. The executable looks for it in its own folder first (a `DT_RPATH`
  of `$ORIGIN`, which comes before `LD_LIBRARY_PATH`), so that it uses this
  SDL even when Steam sets `LD_LIBRARY_PATH`. Few distributions have a
  32-bit SDL 3. This SDL loads X11, Wayland, libdecor, PipeWire, PulseAudio
  and ALSA when it starts, so it starts with whichever of them the system
  has. It is linked only to glibc. Its license is `SDL3-LICENSE.txt`.

At the first build, `tools/linux_sysroot.py` downloads the Debian packages
(about 25 MB) and the source of SDL. Each package comes from
`archive.debian.org` or `deb.debian.org`, or else from
`snapshot.debian.org`; the source of SDL comes from GitHub or from
`libsdl.org`. It checks each download against its SHA-256 sum. The system root is in
`build/linux/third_party/sysroot`. `tools/linux_sysroot.cmake` builds SDL
against it.

To make the portable build you need, in addition to the tools above,
CMake, `pkg-config` (`pkgconf`) and `wayland-scanner` (`wayland` on Arch
Linux, `libwayland-bin` on Debian and Ubuntu). You do not need the 32-bit
SDL3. You need the 32-bit glibc only for the tests.

To see the glibc version that a build needs, enter
`readelf -V build/linux/halo | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1`.

## Build the game

1. Go to the root folder of the repository.
2. Enter `python configure.py`.
3. Enter `ninja linux`.

## Start the game

Enter `build/linux/halo`.

To play on a Steam Deck, unpack `halo-linux-release.zip` into a folder (in
Desktop Mode), and add `halo` to Steam as a non-Steam game ("Add a Game" in
the Games menu of Steam). The game then starts in Game Mode as well. Refer
to "Steam Deck".

The game data is the folder that contains `maps/`, from an Xbox disc image
of any version of the game. The game looks for this folder in this
sequence:

1. `paths.data` in `config.toml`.
2. The current folder.
3. The folder of the executable.
4. `assets/` in the current folder, and `assets/` in the repository that
   contains the executable.

If the game finds no data, it asks for an Xbox disc image (`.xiso` or
`.iso`). This occurs at the first start:

- Select "No" to stop the game.
- Select "Yes" to open a file picker. Select the disc image. The game copies
  `maps/` next to the executable and shows the progress.

The game writes the copy to `maps.partial`. When the copy is complete, the
game changes the name to `maps`. If the copy stops before it is complete,
the game asks for the disc image again at the next start.

### Steam Deck

Do the set-up in Desktop Mode: unpack the release, put `maps/` next to
`halo` (or start the game one time to copy it from a disc image), and add
`halo` to Steam. Then start the game from the library in Game Mode.

- Screen: the game fills the screen without borders (`display.mode` empty,
  thus borderless). The 3D view and the HUD have the shape of the screen
  (16:10) and are drawn at its resolution; the menus are at the center.
- Controls: Steam Input gives the game a virtual controller, which the game
  operates as the controller of the Xbox. With the template that Steam selects
  ("Gamepad With Joystick Trackpad"), the buttons, sticks and triggers have
  the functions of the same controls on an Xbox controller, the right
  trackpad operates as the right stick, and the back buttons (L4, L5, R4,
  R5) do nothing until you assign them in the controller settings of the
  game in Steam. With a template that makes a trackpad a mouse, the mouse
  aims in the game and moves the pointer in the menus.
- Text: names of profiles and gametypes use the keyboard of the game, which
  the controller operates. The text fields of the menus (the name and the
  password in Server Setup, and the password of a game in the server
  browser) open the keyboard of Steam. Type the text, then select Enter on
  that keyboard.
- Frame rate: the game shows one frame for each refresh of the display. It
  follows the refresh rate and the frame limit of Quick Access >
  Performance (40 to 60 Hz on the LCD model, up to 90 Hz on the OLED model).
  The world is calculated at 30 Hz at all rates. Keep `display.vsync =
  true`. Refer to "Frame rate".
- Sleep: the clocks of the game do not count the time that the Deck sleeps,
  so the game continues from where it stopped. A network game does not
  wait: the host drops a machine that it has not heard from for 15 seconds,
  and while a Deck that hosts sleeps, the other players have no host.

## Files and folders

| Xbox drive | Folder |
| --- | --- |
| `d:\` | The data root: the folder that contains `maps/`. |
| `z:\` | `z/` in the save root. This folder contains the cache (approximately 800 MB of map data) and the saved games. |
| `u:\` | `u/` in the save root. This folder contains the user data. |

The save root is `paths.saves` in `config.toml`. If that setting is empty,
the save root is `$XDG_DATA_HOME/halo-linux` (usually
`~/.local/share/halo-linux`).

The game makes the folders when it needs them. Names of files and folders
are not case-sensitive, as on the Xbox.

These files are in the data root:

| File | Contents |
| --- | --- |
| `debug.txt` | The log of the game (in the folder of `paths.logs` instead, if it is set; a dedicated server's is in the save root). At start-up, the game shows the data root in the terminal. A crash writes its report (the faulting address and the calls that led to it) here as well; the `reference address` line at the top of each session places those addresses in the build. |
| `init.txt` | Console commands that the game does at start-up. For example, `map_name levels\a10\a10` starts the first campaign level. |
| `tags/` | Sound tag files that replace the sounds of the maps (`audio.loose_sounds`). |

The settings are in `config.toml` next to the executable. Refer to
"Settings". Internet play's MQTT brokers are in `brokers.txt` next to it
(`network.brokers_file`).

If the game stops because of a fatal signal, it writes the address and a
backtrace to the standard error. To find the function at the address, enter
`addr2line -e build/linux/halo <address>`.

## Controls

The keyboard and the mouse are a control scheme of their own for the player
of controller 1: each action has up to two keys or mouse buttons, which
Settings > Controls Setup (or `[controls]` in `config.toml`) changes. The
game adds the input of the first gamepad to controller 1. The other
gamepads operate controllers 2 to 4. With two or more players on this
computer (co-op, or split screen in a network game) and only one gamepad,
that gamepad is controller 2 (player 2) and the keyboard and mouse stay
controller 1. The gamepad changes controller only when none of its buttons
is held. The profile's button layout (Settings > Gamepads) is the
gamepads' only.

| Action | Keys and buttons (default) |
| --- | --- |
| move | W, A, S, D |
| aim | mouse (direct aim) |
| fire | left mouse button |
| throw a grenade | right mouse button, G |
| jump (and skip a cutscene) | space |
| crouch | left ctrl, C (C alone in the web build) |
| melee | F, mouse button 4 |
| reload | R |
| action (pick up, hold to swap weapons, enter or leave a vehicle; never reloads) | E |
| change the weapon | mouse wheel, 1 |
| change the grenade | X |
| flashlight | Q |
| zoom | Z, middle mouse button |
| show the scores (hold) | tab |
| pause menu | escape |
| screenshot | F10 |
| talk in voice chat (hold) | V |

Always: \` opens the developer console, F12 releases or captures the mouse,
F11 changes between fullscreen and window. Screenshot (default F10,
rebindable under Controls Setup > Actions, below Pause Menu; not on Android) saves a PNG of the completed
frame to `screenshots/` beside `maps/`, named `YYYY-MM-DD_HH.MM.SS.png` in
local time, and prints the filename in the console. Captures in the same
second get a numeric suffix so previous screenshots are preserved.

One movement of the mouse wheel changes the weapon one time. A second
movement after a short pause changes it again.

In the menus, the mouse moves a pointer:

- The item below the pointer gets the focus.
- A left click selects the item. On a setting with values, a click on the
  left or right half changes the value. On a button in the key of a screen
  (for example "B = Back"), a click pushes that button.
- On the on-screen keyboard (a profile's name), a left click presses the
  key below the pointer, or pushes the "B =BACK" or "A =ENTER" legend.
- A right click goes back.
- The mouse wheel moves through the items.

The keyboard also operates the menus, with keys of its own: the arrow keys
(and W, A, S, D) move, space or enter selects, escape or backspace goes
back (escape resumes the game from the pause menu), delete deletes. When the game continues, the mouse aims again. A mouse button that you hold from the menu does not fire until
you push it again.

## Menus

The menus are the PC version's (Halo Custom Edition's): its main menu and
every screen it leads to (campaign, profiles, multiplayer with its server
browser, direct IP and server setup, the gametype editor, and the profile's
settings: controls, gamepads, mouse, audio, video, network and colour),
laid out as it has them. Their pictures are the high-res redraws; the few
that are not yet (3D renders, screenshots) are drawn from the Xbox's own
menus where it has the same, else a placeholder, listed in
`port/assets/menus/NON_HANDDRAWN.md`.

The campaign is wired: Campaign, on player 1's profile (the one last used),
continues its saved game, starts a level it has reached at a difficulty (New
Game), or loads or deletes any profile's saved game (Load Game). Many of the
other functions behind the screens are not wired to this game yet
(`port/assets/menus/UNWIRED.md`): the lists they fill (profiles, maps,
servers, key bindings) are empty, and the settings they change do not
change. The screens open, close and move between each other as the PC
version's. `display.menus = "xbox"` gives the Xbox's menus, which start every
kind of game.

Multiplayer > CO-OP CAMPAIGN is the Xbox's cooperative play, which the PC
version does not have: two players on this computer play the campaign in
split screen. Player 1 is the player who chose it, on the current profile.
Player 2 then chooses a profile with their own controller (a gamepad), and
New Game's levels are those either profile has reached. Either player's
controller chooses the level and the difficulty. A co-op game does
not continue a saved game of one player.

Network games have split screen too: up to 4 players on each computer. In
the game lobby, another controller presses START to join, and the new
player's profile is chosen on the ADD PLAYER screen that opens (with any
controller). With one gamepad, choose the lobby's ADD PLAYER button first:
until then that gamepad shares controller 1 with the keyboard. Two players on one profile get different names from the host. A
player's B in the lobby leaves the game alone, and the last player of the
computer leaves it for all of them. In the game, each player's pause menu
opens on their part of the screen, and its LEAVE GAME is theirs: their part
of the screen stays until the game ends. A game under way shows its own
screen before JOIN GAME: players join there the same way, START or ADD
PLAYER then START, and JOIN GAME brings them all into the game.

In a multiplayer game, the pause menu (escape) has SETTINGS, which opens
the profile's settings while the game goes on, and for the host END GAME.
END GAME ends the game as its time limit does: the players stay, and after
the carnage report the host picks the next map and gametype (PICK GAME).
LEAVE GAME of the host still ends the game for everyone. The buttons go into
the map's own pause menu, before LEAVE GAME, so the buttons of a custom map
stay. The Xbox's pause box is drawn taller to hold them (a redraw,
`port/assets/menus/port_svg/pause`); a custom map's box keeps its size, and
what is below its list moves down. The few pictures of the settings that
come from the main menu's map are not drawn there.

In a single-player campaign, the pause menu has SETTINGS too, before REVERT
TO SAVED, in the same box: a list with room for it centres its rows, and
one without keeps its size, its rows closer. It opens the same settings,
while the game stays paused, with only Controls, Gamepads, Mouse, Audio and
Video Setup; Gamepads' OK saves the profile at once.

The menus are XML files in `port/assets/menus` (`tools/ce_menus.py` writes
them from the PC version's tags), which the game contains. To change them,
put files in a `menus` folder next to `config.toml`: a file with the same
path replaces one of the game's, and another `.xml` file is added.
`port/assets/menus/README.md` describes the files. If a file has a problem,
the game uses the Xbox's menus and the log names the file, the line and the
problem.

## Settings

The settings are in `config.toml` next to the executable
(`build/linux/config.toml`). At the first start, the game writes the file
with the default values and a comment for each setting. To get the default
values again, delete the file.

The game reads the file at start-up. If a key is not correct, or a value
has the wrong type, the game writes the line to the log and uses the default
value. The Settings menu (Video, Mouse, Audio, Network and Controls Setup)
changes the useful settings, writes them into the file (only their lines
change) and applies them at once, but `audio.enabled`, and `display.menus`
from the next main menu.

Each setting has an environment variable. The environment variable changes
the setting for one start of the game. It has priority over the file.

| Setting | Default | Environment variable | Function |
| --- | --- | --- | --- |
| `display.mode` | `""` | `HALO_DISPLAY_MODE` | `"fullscreen"`: the display, taken at the mode of `display.resolution` (the nearest the display has), or at its desktop mode. `"borderless"`: a window over the whole desktop, whose mode does not change. `"windowed"`: a window of `display.window_size`. Empty: `display.fullscreen` decides (`true`: borderless). F11 changes between the window and the fullscreen mode. Video Setup sets it. |
| `display.fullscreen` | `true` | `HALO_FULLSCREEN` | `true`: borderless, as `display.mode = "borderless"`. `false`: a window, as `display.mode = "windowed"`. Used when `display.mode` is empty. |
| `display.resolution` | `"native"` | `HALO_RESOLUTION` | What fullscreen and borderless draw at: `"native"`, the display's own resolution, or `"<width>x<height>"`, such as `"1920x1080"`, 640x480 or more. Fullscreen sets the display to it. Borderless draws at it and scales the picture to the display, where the display has room for it. The picture has 480 lines of the game and the width of the resolution's shape. A window draws at its own size instead. Video Setup's Resolution sets it, from the display's modes; it shows with Fullscreen and Borderless. |
| `display.resolution_scaling` | `"native"` | `HALO_RESOLUTION_SCALING` | `"native"`: the game draws at the resolution of the window, or of the display (or `display.resolution`) fullscreen. `"original"`: the game draws the 640x480 picture of the Xbox and scales it up to the window or the display, whatever `display.mode` is. Video Setup sets it. |
| `display.window_size` | `""` | `HALO_WINDOW_SIZE` | The size of the window, as `"<width>x<height>"`, such as `"1920x1080"`, 640x480 or more. You can change the size of the window; the game's picture takes its shape. Empty: `display.window_scale` decides. Video Setup's Window Size sets it, from sizes of each shape (4:3, 16:10, 16:9 and 21:9) that fit the desktop; it shows with Windowed. |
| `display.window_scale` | `2` | `HALO_WINDOW_SCALE` | Used when `display.window_size` is empty: the size of the window, as a multiple of 640x480. |
| `display.vsync` | `true` | `HALO_NO_VSYNC=1` sets `false` | `true`: each frame waits for the display. |
| `display.max_fps` | `0` | `HALO_MAX_FPS` | With vsync off, the most frames each second. `0`: twice the display's refresh rate. `-1`: no limit, which can hang some Intel graphics (Raptor Lake), resetting the desktop's graphics too. |
| `display.anti_aliasing` | `"off"` | `HALO_ANTI_ALIASING` | The smoothing of jagged edges, which the Xbox did not have. `"off"`: none, as on the Xbox. `"fxaa"` or `"smaa"`: a pass over the 3D view after the game draws it. The HUD and the menus stay sharp. SMAA is the sharper and costs more. `"ssaa2x"`: the game draws at two times the resolution in each direction (at most the GPU's largest texture), and the picture is scaled down. The GPU does four times the work. Not with `display.resolution_scaling = "original"`. `"msaa2x"`, `"msaa4x"` or `"msaa8x"`: each pixel of the 3D view has that many samples (at most the GPU's). On Android, `"smaa"` gives FXAA and `"ssaa2x"` none. A change applies from the next frame. Refer to "Anti-aliasing" in "What operates". |
| `debug.gpu_flush_draws` | `-1` | `HALO_GPU_FLUSH_DRAWS` | Flush the GPU's pipeline every this many draws. `-1`: every 3 on Intel graphics with Mesa's driver, which can otherwise hang in the game's long runs of small draws and reset the desktop's graphics too. `0`: never. |
| `display.interpolation` | `true` | `HALO_INTERPOLATION` | `true`: one frame for each refresh of the display. `false`: 30 frames each second, as on the Xbox. Refer to "Frame rate". |
| `display.direct_camera` | `true` | `HALO_DIRECT_CAMERA` | `true`: in first person, on foot, the view points where the player aims in each frame, not where the last tick left it. Refer to "Frame rate". |
| `display.fov` | `0.0` | `HALO_FOV` | The first-person view's field of view on foot, in degrees across at 16:9, from 20 to 150. `0`: the stock view. Refer to "Field of view". |
| `display.viewmodel_fov` | `0.0` | `HALO_VIEWMODEL_FOV` | The first-person weapon's and hands' field of view, in degrees across at 16:9, from 20 to 150. `0`: the weapon's stock view, also when `display.fov` widens the world. Refer to "Field of view". |
| `display.viewmodel_visible` | `true` | `HALO_VIEWMODEL_VISIBLE` | `true`: the first-person weapon, hands and what is attached to them are drawn. `false`: they are not; firing, animation, sound and lights go on, and other players' models are drawn. |
| `display.viewmodel_shield` | `false` | `HALO_VIEWMODEL_SHIELD` | `true`: the energy shield's flare (its hits, its charging, its fall) is drawn on the first-person arms too, as on the body, with the unit's own shield shader, kept closer to the arms than to the body. The stock game draws it on the body only. |
| `display.first_person_legs` | `false` | `HALO_FIRST_PERSON_LEGS` | `true`: your own body is drawn from the waist down in first person, so you see your legs when you look down, moving as the body's animation moves them. Refer to "Field of view". |
| `display.shield_color` | `default` | `HALO_SHIELD_COLOR` | Your energy shield's flare as this machine draws it: `default` (the shield's own) or a multiplayer armor color (`white`, `black`, `red`, `blue`, `gray`, `yellow`, `green`, `pink`, `purple`, `cyan`, `cobalt`, `orange`, `teal`, `sage`, `brown`, `tan`, `maroon`, `salmon`), whatever the armor's, on the body and, with `display.viewmodel_shield`, on the first-person arms. The shield's own shader is drawn with its two colors replaced at their brightness; the flare is added to what is behind it, so `black` is drawn a faint grey. Only this machine sees it. |
| `display.high_res_hud` | `true` | `HALO_HIGH_RES_HUD` | `true`: the HUD (meters, counters, panels and their outlines, the motion sensor, reticles, waypoints, scopes) is drawn from the high-res assets in `port/assets/hud`, 8x the size of the maps' bitmaps. The bitmaps with English text keep the maps' own. `false`: the maps' own bitmaps. |
| `display.high_res_text` | `true` | `HALO_HIGH_RES_TEXT` | `true`: the menus' and HUD's text is drawn with the fonts in `port/assets/fonts` (Overpass, in place of the maps' Interstate) at the resolution the game draws at, laid out as before; the menus' titles are drawn from the high-res pictures in `port/assets/titles`, and the controller button icons from those in `port/assets/buttons`. `false`: the maps' bitmap fonts, titles and button icons. |
| `display.shadow_resolution` | `128` | `HALO_SHADOW_RESOLUTION` | The size of the maps that the shadows of the objects are drawn in, in pixels each way: `128`, `256`, `512` or `1024` (other values go down to one of these). The game draws the shadow of each object into a map of 128x128 pixels, blurs it and projects it onto the ground. On a large screen, the edges of these shadows show steps that move when the object moves. A larger map makes the edges smooth; the blur is made wider to match, so the shadows are as soft as on the Xbox. Each doubling adds two passes of the blur. `128`: as on the Xbox. |
| `display.menus` | `"pc"` | `HALO_MENUS` | `"pc"`: the PC version's menus, from the files in `port/assets/menus` and a `menus` folder next to `config.toml`. Refer to "Menus". `"xbox"`: the Xbox's menus. |
| `display.player_names` | `"all"` | `HALO_PLAYER_NAMES` | In multiplayer, whose names are drawn above their heads: `"all"`, `"allies"`, `"enemies"` or `"none"`. An ally's name is drawn above the triangle the game shows over teammates. An enemy's name shows only within the motion sensor's reach, while the enemy is in sight and not camouflaged, so it never shows where an enemy hides. The gametype's motion tracker setting also applies: no names if it shows no players, only allies' if it shows only friends. |
| `display.player_name_scale` | `1.0` | `HALO_PLAYER_NAME_SCALE` | How large the players' names are drawn: `1.0` is three quarters of the size of the HUD's text, from `0.25` to `4`. With high-res text, larger names are rasterized at their size, so they stay sharp. |
| `display.scoreboard_team_layout` | `"teams"` | `HALO_SCOREBOARD_TEAM_LAYOUT` | How the multiplayer scoreboard (hold BACK, or tab) lists a team game's players. `"teams"`: a column for each team, red on the left and blue on the right. `"score"`: all the players in order of score. With more players than fit, the mouse wheel and Page Up / Page Down scroll the scoreboard. |
| `display.scoreboard_background` | `true` | `HALO_SCOREBOARD_BACKGROUND` | `true`: the multiplayer scoreboard (hold BACK, or tab) has a panel behind its text, for clearer text. |
| `display.scoreboard_background_color` | `"16, 16, 16, 150"` | `HALO_SCOREBOARD_BACKGROUND_COLOR` | The colour of the scoreboard's panel: `"red, green, blue, alpha"`, each from `0` to `255`. Alpha `0` is see-through, `255` is solid. |
| `display.per_pixel_lighting` | `false` | `HALO_PER_PIXEL_LIGHTING` | `false`: the models (characters, weapons, vehicles, scenery) are lit at each vertex and the light is blended between them, as on the Xbox. The light across a curved surface then shows facets, and a point light that passes close lights only the vertices it reaches. `true`: the models are lit at each pixel by the same lights (the ambient light, two distant lights and two point lights), which changes their look. |
| `display.shield_glow` | `false` | `HALO_SHIELD_GLOW` | `true`: as a unit's energy shield flares (hit, charging, falling), it lights what is around it and itself, as a plasma bolt does, in the shield shader's own color (a Spartan's gold, an Elite's blue), rising and falling with the flare seen on the body. An ordinary dynamic light of the game's, drawn with a light definition the map has (`shield_glow.c`). Up to 8 units near the camera. |
| `display.shield_glow_intensity` | `default` | `HALO_SHIELD_GLOW_INTENSITY` | The shield glow's reach and brightness: `default`, or `light_show` (three times as far, and at full strength from a third of the flare: a light is no brighter than full). The units glowing are those near any local player's camera, so each of a split screen's views has the same. |
| `audio.enabled` | `true` | `HALO_NO_AUDIO=1` sets `false` | `false`: no audio device. The sound continues without output. |
| `audio.volume` | `1.0` | `HALO_VOLUME` | The master volume. |
| `audio.music_volume` | `1.0` | `HALO_MUSIC_VOLUME` | The music's volume, of the master volume. |
| `audio.effects_volume` | `1.0` | `HALO_EFFECTS_VOLUME` | The volume of the other sounds (effects and speech), of the master volume. |
| `audio.skate_volume` | `1.0` | `HALO_SKATE_VOLUME` | The volume of Skate 3 mode's sounds (a build with `--skate`, [port/skate](../skate/README.md#sound)), 0 to 4, of the effects volume; `skate_volume` at the console changes it for the session. |
| `audio.reverb` | `true` | `HALO_REVERB` | `true`: the sounds of the world reverberate as the place the player is in does: the sound environments of the maps (a corridor, a cave, a large hall, outdoors) set the reverberation, as the I3DL2 reverb of the Xbox did. A sound behind a wall or a door is muffled in it too. `false`: no reverberation (sounds behind a wall are still muffled). |
| `audio.voice_chat` | `"push_to_talk"` | `HALO_VOICE_CHAT` | How you talk in voice chat: `"push_to_talk"` (while `controls.push_to_talk` is held; the microphone opens when you first press it), `"open_mic"` (when the microphone hears speech), or `"off"`. You hear the other players in every case. Refer to "Voice chat". |
| `audio.voice_volume` | `1.0` | `HALO_VOICE_VOLUME` | The volume of the voices of the other players, `0` to `2`. |
| `audio.output_device`, `audio.input_device` | `"default"` | `HALO_AUDIO_OUTPUT_DEVICE`, `HALO_AUDIO_INPUT_DEVICE` | The speakers and the microphone, by the name that Settings > Audio shows, or `"default"` for the device of the system. If the device is not found, the game uses the device of the system. Not on Android. |
| `audio.loose_sounds` | `false` | `HALO_LOOSE_SOUNDS` | For those who make sounds. `true`: each sound of a map that has a sound tag file of its name in `tags/` in the data root (for example `tags/sound/sfx/weapons/assault rifle/fire.sound`) plays from that file. The files are Halo PC tag files, as the Halo Editing Kit and Invader write them. At the console, `loose_sounds_reload` reads the files again, and `loose_sounds false` plays the sounds of the map again. When a file changes, all sounds stop. |
| `input.mouse_sensitivity` | `1.0` | `HALO_MOUSE_SENSITIVITY` | The multiplier for the mouse aim. |
| `input.mouse_vertical_sensitivity` | `0.0` | `HALO_MOUSE_VERTICAL_SENSITIVITY` | The multiplier for the vertical mouse aim. `0`: the same as `input.mouse_sensitivity`. |
| `input.invert_mouse` | `false` | `HALO_MOUSE_INVERT=1` sets `true` | `true`: the vertical mouse aim is inverted. |
| `input.mouse_aim_assist` | `false` | `HALO_MOUSE_AIM_ASSIST` | `true`: the magnetism of the controller also operates for the mouse. `false`: when the mouse moved after the right stick, the view is not slowed or dragged by a target. The autoaim of the bullets operates in both cases. |
| `controls.<action>` | (the table in "Controls") | `HALO_KEY_<ACTION>` | The keys and mouse buttons of an action, up to two, separated by a comma: `move_forward`, `move_backward`, `strafe_left`, `strafe_right`, `jump`, `crouch`, `fire`, `throw_grenade`, `melee`, `reload`, `zoom`, `switch_weapon`, `switch_grenade`, `action`, `flashlight`, `scoreboard`, `pause`, `screenshot`, `push_to_talk`. Keys by their names (`"W"`, `"Space"`, `"Left Ctrl"`, `"F1"`), and `"Mouse Left"`, `"Mouse Right"`, `"Mouse Middle"`, `"Mouse 4"`, `"Mouse 5"`, `"Wheel"` (either way), `"Wheel Up"`, `"Wheel Down"`. |
| `game.console_log` | `"important"` | `HALO_CONSOLE_LOG` | What the console shows on the screen. `"important"`: bans, players that the host drops for cheating, the reasons that the game refuses a command, and the asserts that stop the game. `"all"`: all the lines. `"none"`: only the asserts that stop the game. The output of a command always shows. `debug.txt` gets all the lines. |
| `game.language` | `""` | `HALO_LANGUAGE` | The language of the menus: `ja`, `de`, `fr`, `es` or `it`. Empty: English. |
| `game.long_nose_chief` | `true` | `HALO_LONG_NOSE_CHIEF` | `true`: Master Chief has a long gold nose out of his visor, a toothy smile under it and a golden orange visor, on every player, in cutscenes and on the skateboard. Only what your machine draws changes; the other players see their own setting. At the console, `long_nose_chief true` or `long_nose_chief false` changes it until the game quits. The nose and the smile are made by the game when a map loads (`port/linux/game/long_nose_chief.c`); no file is needed. `false` keeps the original Chief. |
| `game.enhanced_animations` | `true` | `HALO_ENHANCED_ANIMATIONS` | `true`: the player bipeds' grenade throws keep their legs moving, blended by speed and direction (crouched throws stay crouched, throws in the air use the jump's legs), Warthog and Scorpion riders stay seated to throw and let go of the grips to throw and reload, and a player turns with the aim while throwing, as while meleeing. `false`: the original animations, which freeze the legs during a throw and stand a rider up. Only in config.toml, not in the menus. |
| `paths.data` | `""` | `HALO_DATA_ROOT` | The data root. Refer to "Start the game". |
| `paths.saves` | `""` | `HALO_SAVE_ROOT` | The save root. Refer to "Files and folders". |
| `paths.custom_maps` | `""` | `HALO_CUSTOM_MAPS_ROOT` | The `custom_maps` folder (Custom Edition maps, and the maps that other hosts share). Empty: `custom_maps` in the data root; on a dedicated server, `custom_maps` in the save root if the data root has none. |
| `paths.logs` | `""` | `HALO_LOG_ROOT` | The folder of `debug.txt`, the log of the game. Empty: the data root; on a dedicated server, the save root. |
| `network.address` | `""` | `HALO_NET_ADDRESS` | The IPv4 address of this machine for system link. Refer to "Play on one computer". |
| `network.broadcast` | `""` | `HALO_NET_BROADCAST` | IPv4 addresses, with commas between them, that get the broadcasts of the game. Empty: 255.255.255.255. |
| `network.online` | `true` | `HALO_NET_ONLINE` | `true`: internet play. `false`: system link on the local network only. |
| `network.spectate` | `false` | `HALO_SPECTATE` | `true`: join games as a spectator, with no player. Refer to "Spectators". |
| `network.spectate_camera` | `"follow"` | `HALO_SPECTATE_CAMERA` | The first camera of a spectator: `follow`, `first`, `free` or `director`. Refer to "Spectators". |
| `network.join_from_clipboard` | `true` | `HALO_NET_JOIN_FROM_CLIPBOARD` | `true`: when the game comes to the front, it joins the game of an invite link on the clipboard. |
| `network.tunnel_port` | `0` | `HALO_NET_TUNNEL_PORT` | The UDP port for internet play. `0`: the game selects a port. Refer to "Internet play". |
| `network.allow_upnp` | `true` | `HALO_NET_ALLOW_UPNP` | `true`: internet play can ask the router to forward its port (UPnP). `false`: the game does not ask. Refer to "Internet play". |
| `network.public_lobby` | `true` | `HALO_NET_PUBLIC_LOBBY` | `true`: the server browser. Public games are listed, and Join Game > Server Browser shows them. `false`: no games are listed or shown. Refer to "Server browser". |
| `network.host_public` | `true` | `HALO_NET_HOST_PUBLIC` | `true`: a new game of Create Game > Internet starts as PUBLIC. `false`: it starts as PRIVATE. LISTING in Server Setup changes it for each game. Refer to "Server browser". |
| `network.coop_friendly_fire` | `"on"` | `HALO_NET_COOP_FRIENDLY_FIRE` | Whether the players of an online co-op game hurt each other: `"off"`, `"on"`, `"shields_only"` or `"explosives_only"`. FRIENDLY FIRE in co-op's Server Setup > Co-op Options writes its choice here. Their AI allies they always can, as in the campaign. |
| `network.coop_player_collisions` | `true` | `HALO_NET_COOP_PLAYER_COLLISIONS` | Whether the players of an online co-op game bump into each other. `false`: they walk through each other, so that one cannot block a doorway or stand on another; they still bump into the AI's characters. PLAYER COLLISIONS in co-op's Server Setup > Co-op Options writes its choice here. |
| `network.coop_enemies_mode` | `"per_player"` | `HALO_NET_COOP_ENEMIES_MODE` | Online co-op's extra enemies: `"none"`; `"per_player"`, each squad of enemies that a level places grows by `network.coop_enemies` for each player past the first; or `"multiplier"`, each squad is `network.coop_enemies_multiplier` times as large, for any number of players. The extra enemies stand around the squad's places, and those that a dropship has no seats for drop out of it after its passengers. EXTRA ENEMIES in co-op's Server Setup > Co-op Options writes its choice here. |
| `network.coop_enemies` | `50` | `HALO_NET_COOP_ENEMIES` | The extra enemies per player, a percentage from `25` to `200`: for each player past the first, each squad of enemies gets this much of itself more (`100`: as many again, so four players meet four times the squad), up to 8 times the squad however many players there are. PER PLAYER in co-op's Server Setup > Co-op Options writes its choice here. |
| `network.coop_enemies_multiplier` | `2` | `HALO_NET_COOP_ENEMIES_MULTIPLIER` | The static multiplier of the enemies, `2` to `32`: each squad of enemies is this many times as large. MULTIPLIER in co-op's Server Setup > Co-op Options writes its choice here. |
| `network.voice_lobby` | `true` | `HALO_NET_VOICE_LOBBY` | When you host: `true`, all players hear all players in the lobby, before and after a game. |
| `network.voice_mode` | `"team_global_enemy_proximity"` | `HALO_NET_VOICE_MODE` | When you host: who hears whom during a game. `"off"`; `"team_proximity"` (teammates who are near); `"team_enemy_proximity"` (all players who are near); `"team_global"` (all teammates); `"team_global_enemy_proximity"` (all teammates, and enemies who are near). Refer to "Voice chat". |
| `network.voice_kbps` | `24` | `HALO_NET_VOICE_KBPS` | When you host: the voice quality in the lobby and in a game, in kilobits per second, `8` to `64`. |
| `network.voice_proximity` | `15.0` | `HALO_NET_VOICE_PROXIMITY` | When you host: the distance in world units (1 unit is approximately 3 metres) at which players are near, for voice chat. `5` to `100`. |
| `network.votekick` | `true` | `HALO_NET_VOTEKICK` | When you host: `true`, the players can vote to kick a player. Refer to "Security". `false`: no votes. VOTE KICK in Server Setup > Teamplay Options (in co-op, Voice and Voting) writes its choice here. |
| `network.votekick_minutes` | `5` | `HALO_NET_VOTEKICK_MINUTES` | When you host: the minutes that a player must play on the server before the player can start a vote to kick (`0` to `60`). To vote, a player must play for 2 minutes, or for this time if it is less. |
| `network.votekick_ban_minutes` | `30` | `HALO_NET_VOTEKICK_BAN_MINUTES` | When you host: the minutes that a player who is kicked by a vote cannot join again (`1` to `1440`). |
| `network.map_share` | `true` | `HALO_NET_MAP_SHARE` | When you host: send a player who joins without the game's Custom Edition map the map, and the resource maps that it needs and that the player does not have. SHARE MAPS in Server Setup > Map Sharing writes its choice here. Refer to "Map sharing". |
| `network.map_share_upload` | `1024` | `HALO_NET_MAP_SHARE_UPLOAD` | When you host: the most kilobytes per second that the game sends of maps (`16` to `65536`). UPLOAD LIMIT in Server Setup > Map Sharing writes its choice here. |
| `network.map_seed` | `true` | `HALO_NET_MAP_SEED` | When you play in the game of another machine: help send its map to players who join without it. HELP SEND MAPS in Settings > Network Setup > Map Sharing. |
| `network.map_seed_upload` | `256` | `HALO_NET_MAP_SEED_UPLOAD` | When you play in the game of another machine: the most kilobytes per second that the game sends of maps to other players (`16` to `65536`). UPLOAD LIMIT in Settings > Network Setup > Map Sharing. |
| `network.map_download` | `true` | `HALO_NET_MAP_DOWNLOAD` | When you join a game on a Custom Edition map that you do not have: download it from the players of the game. `false`: leave the game and show which map is missing. DOWNLOAD MAPS in Settings > Network Setup > Map Sharing. |
| `network.coop_public` | `false` | `HALO_NET_COOP_PUBLIC` | `true`: an online co-op game (Create Game > Internet, a SINGLEPLAYER map) starts as PUBLIC. `false`: it starts as PRIVATE. LISTING in co-op's Server Setup writes its choice here. Refer to "Server browser". |
| `network.brokers_file` | `"brokers.txt"` | `HALO_NET_BROKERS_FILE` | The file of the public MQTT brokers that let the machines of an invite find each other, and that carry the listings of the server browser: next to `config.toml`, unless a full path. One `host:port` on each line, up to 4; `#` starts a comment. |
| `network.server_name` | `""` | `HALO_NET_SERVER_NAME` | The name of the listing of a dedicated server (or a `debug.network_test` host) in the server browser. Empty: the name of the machine. A game from the menus uses the name of Server Setup. |
| `network.public_address` | `""` | `HALO_NET_PUBLIC_ADDRESS` | When you host: the public address at which the router forwards `network.tunnel_port` to this machine, `"<host>"` or `"<host>:<port>"` (the host an IPv4 address or a name; the port, if not given, the port of `network.tunnel_port`). The other machines try this address first. Refer to "Dedicated server". Empty: none. |
| `network.stun_servers` | Google and Cloudflare | `HALO_NET_STUN` | The public STUN servers (`host:port`, with commas between them) that give the internet address of a machine. |
| `discord.application_id` | the application of the project | `HALO_DISCORD_APPLICATION` | The Discord application for invites. Empty: no Discord. |
| `update.auto` | `true` | `HALO_UPDATE_AUTO` | `true`: at start-up, the game looks for a new version. Refer to "Updates". `false`: the game does not look. |
| `crash_reports.upload` | `"ask"` | `HALO_CRASH_REPORTS` | Windows only. `"yes"`: the game sends a report of each crash to the developers. `"no"`: the game sends no reports. `"ask"`: the game asks at the next crash and writes the answer here. Refer to "Crash reports" in [port/windows/README.md](../windows/README.md#crash-reports). |
| `stream.enabled` | `false` | `HALO_STREAM` | `true`: the Linux build streams a game it joins as a spectator, with no window. Refer to "Streaming". |
| `stream.width`, `stream.height` | `1280`, `720` | `HALO_STREAM_WIDTH`, `HALO_STREAM_HEIGHT` | The stream's picture size. |
| `stream.fps` | `30` | `HALO_STREAM_FPS` | The stream's frames a second. |
| `stream.video`, `stream.audio` | `""` | `HALO_STREAM_VIDEO`, `HALO_STREAM_AUDIO` | The pipes the stream's raw picture and sound go to. Empty: none. |
| `stream.ready_file` | `""` | `HALO_STREAM_READY_FILE` | A file written once the stream's first frame is read, and removed at exit. |
| `stream.camera` | `"director"` | `HALO_STREAM_CAMERA` | The stream's camera: `director`, `follow`, `first` or `free`. |
| `server.max_spectators` | `16` | `HALO_SERVER_MAX_SPECTATORS` | The most spectators a hosted game takes: 0 for none, -1 for no limit. Refer to "Spectators". |
| `server.dedicated` | `false` | `HALO_DEDICATED` | `true`: the game is a dedicated server, without a window, sound or player. Refer to "Dedicated server". |
| `server.maps` | `"bloodgulch"` | `HALO_SERVER_MAPS` | The maps of a dedicated server, with commas between them, in turn. `"<map>:<game type>"` gives a map its own game type. |
| `server.game_types` | `"slayer"` | `HALO_SERVER_GAME_TYPES` | The game types (with commas between them) of each map of `server.maps` that has no game type of its own, in turn: `slayer`, `team_slayer`, `ctf`, `king`, `oddball`, `juggernaut`, `race` and the other built-in game types. |
| `server.time_limit` | `15` | `HALO_SERVER_TIME_LIMIT` | The longest time of a game of a dedicated server, in minutes. `0`: the limit of the game type (the built-in game types have none). |
| `server.score_limit` | `0` | `HALO_SERVER_SCORE_LIMIT` | The score to win a game of a dedicated server. `0`: the score of the game type. |
| `server.bots_fill` | `false` | `HALO_SERVER_BOTS_FILL` | `server.bots` is how many players the game has in all: a bot leaves as a person joins, and comes back as one leaves. Refer to "Bots". |
| `server.max_players` | `0` | `HALO_SERVER_MAX_PLAYERS` | The most players (people and bots) in a dedicated server's games; 0 for the build's 128. |
| `server.bots` | `0` | `HALO_SERVER_BOTS` | The number of bots in the games that this machine hosts (a dedicated server, or a game of the menus), 0 to 127, or 0 to 128 with `server.bots_fill`. Refer to "Bots". |
| `debug.bot_seed` | `0` | `HALO_BOT_SEED` | The start of the bots' random numbers, so that the bots of a test do the same each run. |
| `debug.game_seed` | `0` | `HALO_GAME_SEED` | The start of the game's random numbers instead of the time (0: the time), so that a test's games go the same way each run. |
| `server.infection_run_over` | `0` | `HALO_SERVER_INFECTION_RUN_OVER` | Infection: the damage of an infected skater's run-over, in percent of any skater's; `0`: the game type's own. Refer to "Infection". |
| `server.infection_damage_taken` | `0` | `HALO_SERVER_INFECTION_DAMAGE_TAKEN` | Infection: the damage the infected take, in percent of anyone's; `0`: the game type's own (35). |
| `server.infection_speed` | `0` | `HALO_SERVER_INFECTION_SPEED` | Infection: the infected's boards' speed, in percent of any skater's; `0`: the game type's own (140). |
| `server.infection_turning` | `0` | `HALO_SERVER_INFECTION_TURNING` | Infection: how sharply the infected's boards turn, in percent of any skater's (their trucks steer that much further, 100 to 200); `0`: the game type's own (175). |
| `debug.gametype_edit` | `""` | `HALO_GAMETYPE_EDIT` | A test's gametype (`debug.network_test`'s host), edited as the gametype editor's spinners do it: `<spinner>=<value>` pairs with commas between, each set to the spinner's choice nearest the value. |

| `debug.infection_reach` | `0` | `HALO_INFECTION_REACH` | Infection's tuning: how much wider a survivor is to an infected skater's board, in hundredths of a world unit; `0`: the game type's own (none: the board has to hit them). |
| `debug.infection_out_of_reach` | `0` | `HALO_INFECTION_OUT_OF_REACH` | Infection's tests and tuning: the seconds a survivor may stay where no board reaches before it hurts them; `0`: the game type's own (10), below `0` never. |
| `debug.infection_test_place` | `""` | `HALO_INFECTION_TEST_PLACE` | Infection's tests: players the host puts somewhere once their first unit is in the game, `<player>:<x>,<y>,<z>` (world units; `client` for the person on another machine) with semicolons between. |

| `debug.infection_first` | `""` | `HALO_INFECTION_FIRST` | Infection's tests: the players (absolute indices, with commas between) who start infected, instead of a random choice; `client`: the people on other machines than the host; `bots`: every bot. |
| `server.trick_minutes` | `0` | `HALO_SERVER_TRICK_MINUTES` | Trick Attack: the round's minutes (a shorter `server.time_limit` wins); `0`: the game type's own (5). Refer to "Trick Attack". |
| `server.trick_bank_delay` | `0` | `HALO_SERVER_TRICK_BANK_DELAY` | Trick Attack: tenths of a second a combo must ride on clean after its last landing before it banks; `0`: the game type's own (20). |
| `server.trick_steal` | `0` | `HALO_SERVER_TRICK_STEAL` | Trick Attack: the percent of a rammed skater's unbanked combo their rammer takes; `0`: the game type's own (50). |
| `server.trick_hot_spot_multiplier` | `0` | `HALO_SERVER_TRICK_HOT_SPOT_MULTIPLIER` | Trick Attack: the hot spot's multiplier, in tenths (10 to 100); `0`: the game type's own (20, x2). |
| `server.trick_hot_spot_seconds` | `0` | `HALO_SERVER_TRICK_HOT_SPOT_SECONDS` | Trick Attack: the seconds the hot spot stays before it moves; `0`: the game type's own (60). |
| `server.trick_ramming` | `-1` | `HALO_SERVER_TRICK_RAMMING` | Trick Attack: ramming a skater mid-combo knocks them off and takes a share of it: `1` on, `0` off, `-1` the game type's own (on). |
| `debug.trick_test_place` | `""` | `HALO_TRICK_TEST_PLACE` | Trick Attack's tests: players the host puts somewhere once their first unit is in the game, as `debug.infection_test_place`. |
| `debug.trick_hot_spot` | `""` | `HALO_TRICK_HOT_SPOT` | Trick Attack's tests: the first hot spot, `<x>,<y>,<z>` (world units), instead of a random spot a board reaches. |
| `debug.trick_mark_points` | `0` | `HALO_TRICK_MARK_POINTS` | Trick Attack's tests: the unbanked combo from which a skater is marked on everyone's HUD, hunted by hard bots and, banked, called out; `0`: the game type's own (5000). |
| `debug.trick_hud_log` | `false` | `HALO_TRICK_HUD_LOG` | Trick Attack's tests: every machine logs what its HUD shows its player every five seconds. |
| `debug.skate_camera_log` | `false` | `HALO_SKATE_CAMERA_LOG` | Skate 3 mode's tests: log where the local skater's following camera is against where it should settle, every frame for three seconds after each get-on, then every second. |
| `debug.skate_get_on_crouch` | `0.0` | `HALO_SKATE_GET_ON_CROUCH` | Skate 3 mode's camera test: the local player is crouched this far (0 to 1) as it gets on a board, as a crouch pressed just before is; `0` never. |
| `debug.skate_test_under_floor` | `0.0` | `HALO_SKATE_TEST_UNDER_FLOOR` | Skate 3 mode's test: that many seconds into a game, the host's first bot skater riding on a floor gets off, is left a little in the floor and gets on again; where its board is, logged. |
| `debug.skate_test_spot` | `""` | `HALO_SKATE_TEST_SPOT` | Skate 3 mode's test: `"<map> <seconds> <x> <y> <z> <yaw> [session]"`: that many seconds into the first game on that map, the host's first bot skater riding (that skate session's, when given) gets off, is put at the spot facing yaw (degrees) and gets on there; where it goes for five seconds, logged. |
| `debug.infection_test_press` | `false` | `HALO_INFECTION_TEST_PRESS` | Infection's tests: the infected press fire, melee, grenade, action, swap and reload every tick, and bots ask off their boards; none of it may happen, and the host logs what it refused. |
| `debug.bot_nav_build_delay` | `0.0` | `HALO_BOT_NAV_BUILD_DELAY` | Seconds more that a bots' walking graph not in the cache takes to build (on its own thread: the game goes on), for the tests. Refer to "The walking graph". |
| `debug.bot_nav_check` | `false` | `HALO_BOT_NAV_CHECK` | A host builds the bots' walking graph of each map it plays (and, on a map with vehicles, the graph that vehicles drive), logs how much of the map it reaches, and ends the game. Refer to "Bots". |
| `debug.bot_trace` | `false` | `HALO_BOT_TRACE` | Each bot's way in the log every second. |
| `debug.bot_difficulties` | `""` | `HALO_BOT_DIFFICULTIES` | The difficulty of each bot that `server.bots` adds, in the order they join, with commas between them (`"easy,normal,hard,legendary"`), for games of bots of each difficulty against each other. Past the end of the list: `server.bot_difficulty`. |
| `debug.bot_goal` | `""` | `HALO_BOT_GOAL` | `"x y z"`: every bot goes again and again to the walking graph's node at this point, and logs each arrival. Use it to check a map's links. |
| `server.bot_difficulty` | `normal` | `HALO_SERVER_BOT_DIFFICULTY` | How well the bots fight: `peaceful` (they never fight), `easy`, `normal`, `hard` or `legendary`. Refer to "Bots". |
| `debug.update_answer` | `""` | `HALO_UPDATE_ANSWER` | The answer to the update question, for automatic tests: `yes`, `no` or `never`. Empty: the game asks. |
| `debug.exit_after` | `0.0` | `HALO_EXIT_AFTER` | The game stops after this number of seconds (without a window, as on SIGTERM). `0`: never. |
| `debug.screenshot_directory`, `debug.screenshot_every` | `""`, `0` | `HALO_SCREENSHOT_DIR`, `HALO_SCREENSHOT_EVERY` | The game writes each Nth frame to this folder as a BMP file. |
| `debug.hidden_window`, `debug.null_renderer` | `false` | `HALO_HIDDEN_WINDOW`, `HALO_NULL_RENDERER` | `true`: no visible window, or no graphics. |
| `debug.gpu_stats`, `debug.gpu_trace_frame`, `debug.gpu_trace_constants`, `debug.gpu_dump_shaders`, `debug.texture_dump_directory`, `debug.texture_log`, `debug.gl_debug`, `debug.texture_no_cache` | off | `HALO_GPU_STATS`, `HALO_GPU_TRACE`, `HALO_GPU_TRACE_CONSTANTS`, `HALO_GPU_DUMP_SHADERS`, `HALO_TEXTURE_DUMP`, `HALO_TEXTURE_LOG`, `HALO_GL_DEBUG`, `HALO_TEXTURE_NO_CACHE` | Tools to find problems in the graphics: counts for each frame, all the GL state of one frame, the GLSL code, the textures. |
| `debug.menu_open` | `""` | `HALO_MENU_OPEN` | Start on this screen of the menus (`main_menu/settings_select/...`, as `port/assets/menus` names it), a player profile being edited, to look at it. |
| `debug.gpu_skip_vertex_shaders`, `debug.gpu_debug_expression`, `debug.gpu_debug_flat`, `debug.gpu_debug_texture0` | off | `HALO_GPU_SKIP_VS`, `HALO_GPU_DEBUG_EXPR`, `HALO_GPU_DEBUG_FLAT`, `HALO_GPU_DEBUG_T0` | Tools to find problems in the graphics: skip the draws of a vertex shader, or replace the output of all pixel shaders with a GLSL expression (for example `t0.rgb`). |
| `debug.network_test`, `debug.network_test_start`, `debug.network_test_kill`, `debug.network_test_score`, `debug.network_test_shoot`, `debug.network_test_vehicle`, `debug.network_test_pickup`, `debug.network_test_pickup_weapon`, `debug.network_test_public`, `debug.network_test_time`, `debug.network_test_skate`, `debug.network_test_skate_watch`, `debug.network_test_skate_block`, `debug.network_test_skate_duel`, `debug.network_test_skate_scenery`, `debug.network_test_skate_shoot`, `debug.network_test_skate_teleport`, `debug.network_test_skate_fall`, `debug.skate_status`, `debug.skate_hitbox_test`, `debug.skate_hitbox_dump`, `debug.test_input` | off | `HALO_NETWORK_TEST`, `HALO_NETWORK_TEST_START`, `HALO_NETWORK_TEST_KILL`, `HALO_NETWORK_TEST_SCORE`, `HALO_NETWORK_TEST_SHOOT`, `HALO_NETWORK_TEST_VEHICLE`, `HALO_NETWORK_TEST_PICKUP`, `HALO_NETWORK_TEST_PICKUP_WEAPON`, `HALO_NETWORK_TEST_PUBLIC`, `HALO_NETWORK_TEST_TIME`, `HALO_NETWORK_TEST_SKATE`, `HALO_NETWORK_TEST_SKATE_WATCH`, `HALO_NETWORK_TEST_SKATE_BLOCK`, `HALO_NETWORK_TEST_SKATE_DUEL`, `HALO_NETWORK_TEST_SKATE_SCENERY`, `HALO_NETWORK_TEST_SKATE_SHOOT`, `HALO_NETWORK_TEST_SKATE_TELEPORT`, `HALO_NETWORK_TEST_SKATE_FALL`, `HALO_SKATE_STATUS`, `HALO_SKATE_HITBOX_TEST`, `HALO_SKATE_HITBOX_DUMP`, `HALO_TEST_INPUT` | Automatic tests of system link (`game/network_test.c`; the skate ones `game/skate.c`, `game/skate_network.c` and `game/skate_hitbox.c`). Refer to `NETCODE.md`. |
| `debug.touch_targets` | `false` | `HALO_TOUCH_TARGETS` | Outlines the tap targets of the menus (item green, value blue, list slot yellow, legend button red, the band beside the slots of a list orange, keys of the on-screen keyboard white), marks where the last finger went down and the last tap landed for 3 seconds, and logs each tap with the target that it hit (for a value, also where it splits into previous and next): to judge the accuracy of touch. |
| `debug.network_latency`, `debug.network_loss`, `debug.network_corrupt`, `debug.network_corrupt_stream`, `debug.network_corrupt_after` | `0` | `HALO_NETWORK_LATENCY`, `HALO_NETWORK_LOSS`, `HALO_NETWORK_CORRUPT`, `HALO_NETWORK_CORRUPT_STREAM`, `HALO_NETWORK_CORRUPT_AFTER` | The game holds all the data that it receives for this number of milliseconds, ignores this percentage of the datagrams, and damages this percentage of the datagrams it receives, and this percentage of its reads of streams, at random (bytes changed, cut short, stretched or replaced), from this many seconds after the start. Use the first two to test the netcode as on the internet, and the others to test that nothing another machine sends can crash the game (a damaged stream is closed, so a little goes a long way; a host's messages to its own client are damaged too, so start damaging once the game has started). |
| `debug.voice_test` | `false` | `HALO_VOICE_TEST` | Automatic tests of voice chat: a tone replaces the microphone, and each voice that the game hears is written to the log once each second. |
| `debug.telnet_console`, `debug.telnet_console_port` | `false`, `2323` | `HALO_TELNET_CONSOLE`, `HALO_TELNET_CONSOLE_PORT` | The game listens on 127.0.0.1, on this port, for a script console (connect with telnet). The console has no password, so only this computer can reach it. |
| `debug.profile_record`, `debug.profile_record_when`, `debug.profile_memory` | `false`, `"start"`, `256` | `HALO_PROFILE_RECORD`, `HALO_PROFILE_RECORD_WHEN`, `HALO_PROFILE_MEMORY` | Profiling builds only (`configure.py --profile`). Record without a command; `"start"` records from the first frame until the first map change, `"game"` records each game outside the main menu. Memory is 4–1024 MB. Numbered `profile_<stamp>_<role>.part<n>.json` files remain in `profiles/` after recording. Refer to "Profiling builds" in the main README. |


With Mesa drivers, the game sends its GL calls through the GL thread of
Mesa. To stop this, set the environment variable `mesa_glthread=false`.

## Updates

The builds from GitHub Actions (refer to the main [README](../../README.md#download))
can update themselves. At start-up, the game asks GitHub for the latest
release. The game does not wait for the answer. If the latest release is not
newer, the game does nothing.

If the latest release is newer, the game asks: "Do you want to update?"

- Select "Yes" to update. The game downloads the release for this platform,
  replaces its files and starts the new version. The old files get the
  extension `.old`. The new version deletes them.
- Select "No" to continue. The game asks again at the next start.
- Select "Do not ask again", then "Yes", to stop the questions. The game
  writes `auto = false` in the `[update]` section of `config.toml`. To get
  the questions again, set `auto = true`.

The game downloads through HTTPS. It examines the certificate of the server
against the certificate authorities of the system: on Linux, the bundle of
the distribution (`src/posix_update.c`, with Mbed TLS); on Windows, the
certificate store of Windows (WinHTTP). The folder of the executable must
let the game write to it.

Builds that you make yourself have no build number. They do not look for
updates.

## Frame rate

The game calculates its world at 30 Hz, as on the Xbox. On the Xbox, the
game showed one frame for each calculation (tick). This port shows one frame
for each refresh of the display, for example at 60, 120 or 240 Hz.

Each frame shows the world between the last two ticks
(`game/render_interpolation.c`):

- After each tick, the game keeps the camera, the position of each part of
  each object, and the first-person weapon.
- Each frame mixes the last two ticks. The mix agrees with the time since
  the last tick.
- Rotations use quaternions. Positions and scales are linear.
- A teleport, a respawn or a cut of the camera does not mix. It jumps.
- After a long frame (several ticks in one frame), the camera mixes the
  last tick only, as the objects do.
- In cinematics, a camera that moves with an object (the lifepod in a30,
  a Pelican) moves with the object as it is drawn.

Thus the frames are one tick (33 ms) after the calculation. The calculation
does not change.

The direction of the view is an exception. The game reads the mouse and the
sticks in each frame. In first person, on foot, each frame points the view
where the player aims at that time (`display.direct_camera`). Thus the view
turns in the frame that the mouse moves. In a vehicle and in cinematics, the
view mixes as the other things do. On Android, the view mixes as before.

To get 30 frames each second, set `display.interpolation = false`.

To see the frame rate:

1. Push \` to open the developer console.
2. Enter `display_framerate true`.

In the game of another host, the console runs only the commands that change
nothing of the game (such as `display_framerate`), and the game puts back
cheats, the game speed and the settings of the drawing that show more of
the world (such as `rasterizer_wireframe`). Refer to `NETCODE.md`.

The frame rate shows at the bottom right of the screen. It is the mean over
half a second.

## Field of view

Video Setup's FOV AND VIEWMODELS screen sets the field of view of the
first-person view (`display.fov`) and of the weapon and hands
(`display.viewmodel_fov`), from 80 to 150 degrees in steps of 5, and can
hide the weapon (`display.viewmodel_visible`). DEFAULT, the setting's `0`,
keeps the stock view. `config.toml` takes any angle from 20 to 150.

- An angle is across the screen at 16:9. Another shape keeps the same
  angle up and down, as the stock view does.
- The field of view applies on foot in first person only. Vehicles, death,
  cinematics and scripted cameras keep their own view.
- A scope's zoom levels keep their stock view: the extra width fades out as
  the zoom comes in. A view narrower than the stock one narrows the zoom
  too.
- The reticles scale with the view, so that they stay on the aim. The
  scopes' pictures keep their place.
- By default the weapon keeps its stock view when the world is wider:
  arms and a gun right against the camera stretch at a wide angle.

LEGS (`display.first_person_legs`) draws your own body in first person
from the waist down, so you see your legs when you look down, walking,
running and crouching as everyone else sees them. It is the body others
see, posed as they see it: the upper body is folded away, and the body is
set back behind the eye as far as keeps the open waist below the view
(the way Halo 2 keeps its first-person body's open top out of sight), more
the further down you look. It is drawn with the world's camera, so it keeps
its place and size at any field of view. Hit boxes, markers and everything
the game does with the body are unchanged.

Only this machine's view changes. Nothing the machines send each other
changes, so players with different settings play together.

## System link

The Xbox game lets 16 players on 4 machines play a system link game. This
port lets up to 128 players on up to 128 machines play. Each machine can
have up to 4 players (split screen).

- `include/halo_port_limits.h` sets the limits.
- `include/halo_port_capacity.h` sets the memory for the limits. The game
  state is 16 MB at `0x81A00000` (3.3 MB on the Xbox). The pools of objects,
  effects, particles, contrails, lights and sounds are also larger.

Obey these rules:

- All the machines in a game must use a build with the same limits.
- The port uses protocol version 2. It does not see the Xbox game or older
  builds of the port. They do not see the port.

These are the differences from the Xbox:

- The host waits up to 60 seconds (15 seconds on the Xbox) for the other
  machines to load the map.
- If a machine does not read the messages of the host for two seconds, the
  host removes it from the game.
- The saved games contain all the game state. Thus a saved game is 16 MB.
  Saved games from older builds of the port do not operate.
- In campaign and in games of up to 16 players, the game removes garbage
  (bodies, dropped weapons) as on the Xbox. In larger games, it keeps more
  garbage, in proportion to the players.
- The lobby shows the local machine and the first three remote machines.
  The other machines are also in the game.
- In free-for-all games, each player is a team.

Linux, Windows and Android machines can play in the same game. Each machine
simulates the players from the same inputs, and the host does not correct
all of the game. Thus each machine must calculate the same floating-point
results, and all the ports:

- Compile without fused multiply-add (`-ffp-contract=off`).
- Use the math functions of musl (`port/include/halo_math.h`,
  `port/third_party/musl-math`), not the math functions of the system.

### Play on one computer

More than one copy of the game can play on one computer. Each copy must
have a different loopback address. A copy with an address gets no
broadcasts. Thus each copy must send its broadcasts to the other copies.

For a host and two clients, enter these commands in three terminals:

```sh
HALO_NET_ADDRESS=127.0.0.200 HALO_NET_BROADCAST=127.0.0.201,127.0.0.202 build/linux/halo
HALO_NET_ADDRESS=127.0.0.201 HALO_NET_BROADCAST=127.0.0.200 build/linux/halo
HALO_NET_ADDRESS=127.0.0.202 HALO_NET_BROADCAST=127.0.0.200 build/linux/halo
```

Do not give 127.0.0.1 to a copy. Each copy gets to its own address through
127.0.0.1. Linux and Windows send all of 127.0.0.0/8 to the loopback
interface.

### Test with many machines

`tools/system_link_bots.py` adds simple machines to a game. Each machine has
one player. The machines obey the system link protocol, but they do not
calculate the game or move their players.

1. Start a game on the host.
2. Enter `python tools/system_link_bots.py --host 127.0.0.200 --machines 127 --start`.

Each machine uses its own loopback address, from 127.0.0.2. The option
`--start` starts the game when all the machines are in the lobby. If the
host has no `network.address`, do not give `--host`.

## Internet play

Machines with an invite link can play system link on the internet. This
project has no server.

When a copy of the game starts to host a system link game, it makes an
invite link: `halo://join/<64 hexadecimal digits>`. The game writes the link
to the standard error and puts it on the clipboard. The links of older
versions of the game (44 digits) do not operate. The game writes a message
when it gets one.

To join a game, do one of these steps:

- Open the link. The game is the handler of `halo://` links. If the game
  already operates, the new copy gives the link to it and stops. A key in a
  file that only the user can read (`halo-ce-universal.key` in
  `$XDG_RUNTIME_DIR`, else `~/.halo-ce-universal.key`; on Windows in
  `%LOCALAPPDATA%`) encrypts the link, so the programs of other users cannot
  read it.
- Copy the link (or the 64 digits) and go to the game.
- Enter `halo <link>`.
- Accept a Discord invite. Refer to "Discord".

The game also takes the address of the web build's page with an invite
(`https://halocombatevolved.com/?join=<64 digits>`), as a link.

When the machines connect, the game of the host shows in Multiplayer,
System Link. Join the game as on a local network. System link on a local
network does not need an invite.

### Browsers

The web build ([port/web/README.md](../web/README.md)) plays internet games
with the Linux, Windows and Android builds: it hosts and joins by invite, and
the server browser lists its public games and theirs. A browser has no UDP
socket, only WebRTC, so a native build takes a browser's WebRTC on the port
of its tunnel (`network.tunnel_port`), as little of it as one data channel
needs (`src/p2p_webrtc.c`):

- ICE: the native build is an ICE-lite agent. It answers the checks of the
  browser, and the browser chooses the path. The credentials come from the
  secret of the session, which both machines have. Until the browser
  connects, the native build sends STUN indications to the addresses of the
  browser, which open its NAT to the checks of the browser.
- DTLS 1.2 (`src/posix_dtls.c`, Mbed TLS: `port/third_party/mbedtls`): the
  native build is the server, with a certificate that it makes when it
  starts (ECDSA P-256). Each machine sends the SHA-256 of its certificate in
  its signalling messages, which the token seals and the proof of the
  session authenticates; the other machine accepts only that certificate.
- SCTP, over DTLS: the browser opens the association, and one data channel,
  agreed before (unordered, without retransmissions), carries the packets of
  the tunnel. They are sealed as on UDP, and KCP makes the streams reliable
  as on UDP.

Two browsers connect to each other with their own WebRTC. There is no relay:
as between native builds, two networks that do not let a direct connection
through (some company and school networks block WebRTC) cannot play
together.

### Voice chat

Network games have voice chat, on the local network and on the internet.
Hold V (`controls.push_to_talk`, Settings > Controls Setup) to talk, or set
VOICE CHAT in Settings > Audio to OPEN MIC. VOICE VOLUME sets the volume of
the other players. On Android, set VOICE CHAT to OPEN MIC to talk: the game
then asks for the microphone.

OUTPUT DEVICE and INPUT DEVICE in Settings > Audio select the speakers and
the microphone (SYSTEM DEFAULT, the default, follows the system). The list
has the devices that the computer had when the game started.

The host sets voice chat for all players in Server Setup > Teamplay Options
(`network.voice_*` in `config.toml`):

- In the lobby, before and after a game, all players hear all players
  (LOBBY VOICE CHAT).
- During a game (VOICE CHAT): OFF; TEAM NEAR; ANYONE NEAR; TEAM; or TEAM,
  ENEMIES NEAR (the default). Near is VOICE NEAR DISTANCE (45 metres is the
  default), and both players must be alive. A player who is near sounds
  quieter when further, and from their direction. In a game without teams
  all players are enemies; in co-op, all are teammates.
- The quality (VOICE QUALITY), 8 to 64 kilobits per second (24 is the
  default), in the lobby and in the game.

In co-op, Server Setup has no Teamplay Options: the same settings are in
Server Setup > Voice and Voting.

A speaker shows next to the name of a player who talks: in the lobby, on
the scoreboard, in a list at the left of the screen during a game, and
beside the name above their head (when names are shown there:
`display.player_names`). To mute a player, open the scoreboard,
right-click, click the player, and click Mute voice. You no longer hear
them, and nobody else is told.

The host sends each voice on to the players who can hear it. The host
accepts a voice only from the machine of the player (with a key that it
gives each machine on its connection), only at the quality it set, at most
50 frames each second from a machine, and from at most 8 players at a time.
The voices are compressed with Opus (`port/third_party/opus`).

### Server browser

Server Setup in Create Game > Internet has a LISTING row:

- PUBLIC (the default): the game also shows in Join Game > Server Browser
  on every machine. Anyone can see and join the game.
- PRIVATE: only players with the invite link can join.

Each new game starts as PUBLIC (`network.host_public = false` makes new
games start as PRIVATE). An online co-op game starts as PRIVATE, and keeps
the last choice of its LISTING (`network.coop_public`). A LAN game is never listed. `network.public_lobby = false` turns the server browser off.

A PUBLIC game can also have a PASSWORD (a row of Server Setup, below
LISTING). The Server Browser shows a lock at the left of a game with a
password. A player who selects that game must type the password, and JOIN
GAME joins only with the correct password. The invite link of the game joins
it without the password. The host keeps the password only while the game
runs.

In the Server Browser, select a game to join it. The game joins the invite
of the game, as for a link. When it reaches the host, it opens the lobby.
If it cannot reach the host in 30 seconds, it marks the game FAILED. A game
that is full or starting shows CLOSED. REFRESH asks the hosts for their
listings again.

How it operates (`src/p2p_lobby.c`):

- The host of a public game publishes a listing: the invite, and the name,
  map, gametype and player counts of the game. The listing goes to the
  same MQTT brokers as the invites (`network.brokers_file`), retained,
  to a topic of the host (`hceu/3/lobby/s/<hash of its key>`).
- The key of the host signs the listing (Ed25519). The key is the key of the
  invite, so no other machine can list the invite of the host, change its
  listing, or list a game with false details.
- The host publishes the listing again every 30 seconds, when the game
  changes, and when a browser asks. If the topic of the host is empty or
  holds another listing, the host publishes again in 5 seconds or less. Thus
  a broker that deletes the listing does not remove the game.
- When the host stops (it stops hosting, the game becomes private, or the
  game quits), it publishes a closed listing, and then empties its topic.
  If the host loses its connection, the broker empties its topic (the will
  of the connection). A browser removes a game that it does not hear for 90
  seconds.
- When a public game becomes private, the host makes a new invite. Thus a
  player who saw the listing cannot join with the old invite.
- The listing of a game with a password does not hold the invite in clear
  text. The secret part of the invite (its token) is encrypted with a key
  from the password (Argon2id, salted with the key of the host, then
  XChaCha20-Poly1305). The browser makes the key from the password that the
  player types, and opens the invite only if the password is correct. When
  the host sets or changes the password, it makes a new invite. A player who
  has the listing can try passwords on their own machine without the host,
  so use a long password.

A public game does not publish the address of the host. But any machine
with the invite can ask the host to connect, and the host then sends its
addresses. Thus anyone can learn the address of the host of a public game,
as for any public server.

The brokers are in `brokers.txt` next to the executable (from
`port/assets/network/brokers.txt`; on Android, the app writes it next to
`config.toml` at each start), one `host:port` on each line. The game uses
all of them at once (up to 4), so one that works is enough. An update
replaces `brokers.txt`: to use brokers of your own, put them in another
file and name it in `network.brokers_file`. All the players must use the
same broker to see each other's games. The game uses
MQTT 5 if the broker has it, else MQTT 3.1.1. A broker that does not keep
retained messages, or does not let clients subscribe with wildcards, carries
only invites, not listings.

### Map sharing

When you join a game on a Custom Edition map that you do not have, the game
downloads the map from the players of the game, if the host shares its maps
(SHARE MAPS in Server Setup > Map Sharing, `network.map_share`, on by
default). This operates on a local network and
on the internet. It also downloads the resource maps that the map needs
(`bitmaps.map`, `sounds.map`, `loc.map`) if you do not have them. A line at
the top of the screen shows the progress. In the lobby, the game starts when
every player has the map. If the game already operates, you join it when
the download is complete. If no player sends any of the map for a minute,
you leave the game, and the main menu tells you why.

All the players help (HELP SEND MAPS in Settings > Network Setup > Map
Sharing, `network.map_seed`), as the peers of a torrent do:
the downloaded map comes in pieces from the host and from each player who
has it or has a part of it. The game checks each piece against its hash, so
a player cannot send a different map. A player who sends a bad piece is not
asked again. To keep the upload free for the game, the host sends at most
`network.map_share_upload` (1024 KB/s: UPLOAD LIMIT in Server Setup > Map
Sharing) and each other player at most `network.map_seed_upload` (256 KB/s:
UPLOAD LIMIT in Settings > Network Setup > Map Sharing). Set these lower on
a slow connection. DOWNLOAD MAPS there (`network.map_download`) turns
downloading off: you then leave a game whose map you do not have.

The name of each map in `custom_maps` ends with the start of its hash: when
the map lists find `hugeass.map`, the game renames it `hugeass_0e2b3ee4.map`
(and its picture and description). Thus two versions of a map have different
names, and a game always loads the version of the host. The menus show the
map without the hash, and with it only when you have two versions of the
map. The downloaded map has the same name. A download that stops leaves a
`.part` file in `custom_maps`; the next download of that map continues from
it. Maps in the folder of a Halo Custom Edition install
(`paths.custom_edition`) keep their names, and the host does not share them.
Copy a map to `custom_maps` to share it.

Over the internet, the players of a game connect only to the host. To send
each other a map, a player answers the other players of the game (who have
its invite) as the host does, and a player who downloads connects to them in
the same way that it connected to the host. Map sharing uses port 5153 for
its connections. Refer to `NETCODE.md`, "Map sharing".

### Security

Only machines with the invite can find the game:

- Each copy of the game makes an Ed25519 key pair when it starts, and from
  it an X25519 key pair (Monocypher, `port/third_party/monocypher`). Its
  identifier is from the hash of its X25519 public key. The Ed25519 key
  signs the listing of a public game.
- The link contains a 16-byte hash of the public key of the host and a
  random 16-byte token. The identifier of the host is from the first 6
  bytes of the hash.
- The machines exchange their public keys and addresses through public MQTT
  brokers (`network.brokers_file`). The topics are HMACs of the token.
  A key from the token encrypts and authenticates the messages
  (`src/p2p_signal.c`, `src/p2p_crypto.c`). The host authenticates its answer
  with a key that only it and the player can calculate. Its public key must
  agree with the hash in the link. The hash is long, so no other machine can
  find a key with the same hash.
- Then the player shows in the same way that it has the private key of its
  public key. Only then does the host make a session for the player. Thus
  other machines with the invite cannot make sessions in the name of a
  player (such a session would keep the player out).
- Each two machines get the keys of their packets from their key pairs and
  a random number from each. The keys do not go through the brokers. Thus
  other machines with the invite cannot read or change the packets.
- Each packet is encrypted and authenticated, with a different key in each
  direction. A machine ignores a packet that it already received. To and
  from a browser, the packets also go through DTLS, with the certificates
  that the signalling messages named.
- A machine can send only to the ports of the game on the other machine.
- The host makes one session from each request of a player. If a person
  sends a copy of an old request again, the host ignores it. A player that
  must ask again sends a new request.
- The host tries to reach at most 8 new players at the same time. The
  other players ask again.
- The host answers a request that is not proven at most one time each
  second through each broker. It answers at most 20 of these requests each
  second, after a first 32. Each answer goes only through the broker that
  brought the request. Thus a flood of requests does not use much of the
  bandwidth of the host.
- The host does the key work of at most 20 requests each second from keys
  that it does not know, after a first 32. It keeps the key work of the
  last 256 keys. Thus the proof of a player does not need more key work. A
  flood of requests can make players join more slowly. A player asks again
  for 90 seconds.
- The host refuses the predicted movement of a player whose game runs
  faster than time (a speed hack). If the messages on the player's
  connection were also ahead for ten seconds, the host drops and bans the
  player: each player sees who in red on the console, and the host adds a
  line to `cheaters.txt` and `bans.txt` (beside `debug.txt`) with the
  address and hardware id of the player, and the Discord name and id that
  the game of the player told it, marked `(self-reported)` (a player can
  change these). The host refuses a machine whose address or hardware id is
  in `bans.txt`. If only the player's datagrams were ahead, the host does
  not drop the player, because another machine can send datagrams with the
  player's address: it adds an `unverified` line to `cheaters.txt`.
- The host can ban a player with `ban <player name>` in the developer
  console (Tab completes the name). Remove a line from `bans.txt` to unban.
  Refer to `NETCODE.md`. `kick <player name>` drops the player the same
  way, but keeps nothing: no line in `bans.txt`, and the player can join
  again at once. In co-op, `bringto` brings every player to the host.
- Players can vote to kick a player. The host turns this on or off with
  VOTE KICK in Server Setup > Teamplay Options (`network.votekick`; in
  co-op, Server Setup > Voice and Voting). Hold the scoreboard key, right-click to
  show the pointer, and click the name of the player. Then click **Start
  a vote to kick**. Other players vote in the same way, and see the vote on
  the scoreboard. `votekick <player name>` in the developer console does
  the same. The host also gets **Kick** and **Ban** in this menu: these do
  the same as the `kick` and `ban` commands (click **Ban** two times). The
  host counts the votes, and these rules prevent abuse:
  - The vote passes when more than half of the players vote for it, and at
    least two players. The player of the vote is counted, but cannot vote.
    Thus, in a game of two equal teams, one team cannot kick a player of
    the other team without help.
  - The host counts one vote for each internet address (for internet
    play, the real address of the player, not the address of the tunnel).
    Two machines at one address, or with one hardware id, have one vote.
  - To start a vote, a player must have played for
    `network.votekick_minutes` (5) on this server. To vote, a player must
    have played for 2 minutes (or less, if that setting is less). The host
    counts the time. When a player joins again, the time starts again.
    Players who cannot vote yet are not counted.
  - The host only accepts a vote that comes on the connection of the
    player, not a datagram, which another machine can send with the
    address of the player.
  - One vote runs at a time, for 45 seconds, with 30 seconds before the
    next vote. If a vote fails, the player who started it cannot start a
    vote for 5 minutes, and nobody can start a vote against the same player
    for 10 minutes.
  - Nobody can vote to kick a player of the host.
  - A player kicked by a vote cannot join again for
    `network.votekick_ban_minutes` (30), by address and hardware id.
  - A player who leaves during a vote against them is kept out the same
    way, as if the vote passed. While the vote runs, the host refuses the
    player if they try to join again. Thus nobody can avoid a vote by
    leaving.
  So that every player can be named, the host trims the spaces around a
  name and removes characters that draw as nothing. A
  letter with a mark is typed as the plain letter (`ban jose` for "José").
  A name with nothing left to type becomes "Player", and a name that another
  player already has gets a number ("Player 2"). The game refuses a profile
  name that is blank, and a multiplayer game refuses a profile whose name was
  made blank before this check.
- An invite operates while the copy of the game that made it operates.

### Connection

Each machine gets its public address from public STUN servers. Then the two
machines send packets to each other until the packets get through (UDP hole
punching). There is no relay.

Some networks give a different port for each destination (for example some
mobile and company networks). Two machines behind such networks cannot
connect. To connect, forward `network.tunnel_port` on the router of one of
the machines.

The game can ask the router to forward the port (UPnP,
`src/posix_upnp.c`, with `port/third_party/miniupnpc`):

- The host asks its router when a player uses its invite.
- A player that joins asks its router when it does not reach the host in
  5 seconds.
- The forwarded port is one more address that the machine gives to the
  other machine.
- The forward has a duration of one hour. The game makes it longer while
  it operates. When the game stops normally, it removes the forward. It
  does not remove the forward after a crash, or if a request to the router
  is still under way 3 seconds after the game starts to stop. Some routers only make forwards without a duration.
- When the game finds the router, it removes the forwards to this machine
  that have the description "Halo internet play" and that no copy of the
  game uses now (forwards that a copy of the game did not remove).
- UPnP does not help behind a second NAT, for example the NAT of a mobile
  network provider. Then the router has a private address, and the game
  does not ask.

To stop all UPnP requests, set `network.allow_upnp` to `false`.

In the game, each machine has an address in 100.64.0.0/10:

- `src/xnet.c` gives the datagrams that the bound UDP sockets of the game
  send to such an address to `src/p2p.c`. Other datagrams (of sockets that
  are not bound yet, or that are connected to the address) and the TCP
  connections of the game go through local sockets on 127.0.0.1 (or
  `network.address`). The traffic from the other machines comes to the
  game from local sockets too.
- `src/p2p.c` sends that traffic through one UDP socket. UDP datagrams go
  as they are. TCP connections go as KCP streams (`port/third_party/kcp`).
- The broadcasts of the game go to all the machines. Thus the game of the
  host shows on the other machines.

### Discord

If the Discord desktop client operates, the game of the host shows in
Discord (through the application of `discord.application_id`). The activity
has a private party with the invite as its join secret. The host can send
the invite with the invite button of Discord. When a person accepts it, that
person joins the game. If the game does not operate, Discord starts it.
The game sends the activity only to a Discord client of the same user.

## Dedicated server

The game can run as a dedicated server: a host without a window, sound or
player of its own, for a machine that nobody sits at (a server, a
container). Set `server.dedicated = true`, or start the game with
`HALO_DEDICATED=1`:

```
HALO_DEDICATED=1 HALO_DATA_ROOT=/path/to/halo ./halo
```

No X, Wayland or `SDL_VIDEO_DRIVER` is necessary. The server:

- Hosts the maps of `server.maps` in turn, each with the game types of
  `server.game_types` in turn, and starts again at the first map after the
  last. `server.time_limit` ends each game after that many minutes, so
  that the next map comes when no player reaches the score.
- Has no player of its own. Its games start without players, and the
  players join them in progress. The game engine counts only the players
  who joined.
- Draws nothing and makes no sound. It runs at the 30 frames each second
  of the game and sleeps between them: about 1-2% of a CPU core and 120 MB
  of memory with two players.
- Writes its log to the standard error: players who join and leave, the
  start of each game, its end and its scores, and every line of
  `debug.txt`.
- Writes nothing to the data root, which can be read-only: `debug.txt`
  (`paths.logs`), `gamestate.txt` and `custom_maps` (`paths.custom_maps`,
  unless the data root has its own) are in the save root.
- Checks its maps before it starts. The maps folder of the data root
  must have `ui.map` and a map file for each map of `server.maps`. If one
  is missing or is not a map, the server writes one line to the standard
  error and exits with status 2, before it starts its network:

  ```
  halo-linux: server: no usable maps in /data/maps (looked for ui.map, bloodgulch.map); exiting
  ```

  The server needs only these maps: for example `ui.map` and
  `bloodgulch.map` for the default `server.maps`.
- Exits with status 1 after a fatal error, for example a map that it
  cannot read, with a line `server: a fatal error (...); exiting`. A
  player's game shows an error screen in this case; a server has nobody
  to show it to.
- Stops on SIGTERM or SIGINT. It ends a game that is in progress, tells
  the players that the host goes, removes its listing from the server
  browser and exits with status 0. A second signal stops it at once. If
  the game has not stopped 5 seconds after the signal, it exits at once
  with status 128 plus the number of the signal (143 for SIGTERM). This
  applies to every game, not only to a server.
- Does not use the clipboard, Discord or the self-updater.
- Needs no Skate 3 data, and no build with `--skate`. A server built
  without the mode still sends each skater's state to the other players,
  holds the skaters to a board's bounds and decides their hits (refer to
  `port/skate/README.md`, "Multiplayer"). The players who skate need the
  mode and its data on their own machines. `test_dedicated_server_skaters`
  in `tools/test_linux_port.py` tests this with a server built without
  `--skate` and two skating clients.

For internet play (`network.online`), it makes an invite link as other
hosts do. If `network.host_public` is `true`, the server browser lists it
under `network.server_name`. Set `network.host_public = false` to keep it
private: then only players with the invite link can join.

Behind a NAT that gives a different port for each destination (refer to
"Connection"), the other machines cannot reach the server through its STUN
address. Then forward a fixed `network.tunnel_port` (for example 27015, UDP)
on the router to the server, and set `network.public_address` to the
public address of the router: an IPv4 address or a name, with
`:<port>` if the router forwards another port. The server gives this
address to the joining machines first. The server looks up a name again
every five minutes, so a dynamic DNS name follows the router's address.

Players in a browser (the web build, [port/web/README.md](../web/README.md))
join the server too. They open the invite in the web build: the address of
its page with `?join=` and the 64 digits of the server's `halo://join/`
link, for example `https://noahsurprenant.github.io/OpenCE/?join=<64 digits>`
(refer to "Browsers" above). The page must be a build of the server's
network version. The server needs no player of its own for this, and
`network.host_public = false` does not stop it: the invite is enough. The
browser reaches the server over WebRTC on the same UDP port
(`network.tunnel_port`), so the one port forward serves the native and the
browser players alike. The server gives the browser
`network.public_address` first among its addresses, as it does to a native
build, and the browser's ICE checks go to it. There is no TURN relay: a
browser on a network that lets no WebRTC or UDP through (some company and
school networks) cannot join.

`port/web/tests/dedicated.mjs` tests this. It starts a dedicated server
(invite-only) behind a UDP forward that the server names as its
`network.public_address`, a local MQTT broker, and a headless Chromium that
imports `ui.map` and `bloodgulch.map`, opens the invite of the server and
plays. The server must log the player who joins and a WebRTC connection,
the browser must log its player each second, and the ICE path that the
browser chooses must be the forwarded address. The test needs the web build,
the Linux build, the maps, Node.js with Playwright, and mosquitto. It takes
about 30 seconds:

```
node port/web/tests/dedicated.mjs build/web/site /path/to/halo/maps build/linux/halo
```

CI runs this test in the `web-dedicated` job of
[.github/workflows/build.yml](../../.github/workflows/build.yml), after the
`web` job, with the release site of that job and a server built as the
server image builds it. The job runs on the `opence-k8s` runner, because only
that runner has the maps; its image has Node.js, Playwright 1.58 with its
Chromium headless shell, and mosquitto. Everything stays on the runner: the
broker listens on loopback, the forward on the runner's own address, and the
server of the test asks no STUN server (`HALO_NET_STUN` is empty).

If `HALO_WEBJOIN_INVITE` has the 64 digits
of a server that runs, the test starts only the browser and joins that
server, for example a server container behind a NAT (the header of the
script gives the details).

The telnet console (`debug.telnet_console`) gives the console of the
server on 127.0.0.1.

A container image of the server, `ghcr.io/noahsurprenant/opence-server`, is
built from [docker/server/Dockerfile](../../docker/server/Dockerfile), and a
Helm chart deploys it on Kubernetes:
[charts/opence-server](../../charts/opence-server/README.md). Neither
contains game data: the maps are mounted read-only at `/data`.

`debug.network_test`, `debug.network_test_time` and
`debug.network_test_score` still operate. If `debug.network_test` is not
empty, the server hosts its list instead of `server.maps`.

## Infection

A game type of the port's: Team Slayer whose infected skate (Skate 3 mode,
port/skate/README.md) and infect the survivors by running them over.
Choose **Infection** among the standard game types, or give a dedicated
server `infection` in `server.game_types` or `server.maps`
(`bloodgulch:infection`).

- **The start:** one player in eight (at least one) starts infected, chosen
  at random (the game's random numbers: fixed by `debug.game_seed`). The
  survivors are red; the infected are blue, their armour green.
- **The infected** are always on a skateboard: they cannot get off it,
  fire, strike with melee, throw grenades, pick anything up or get into a
  vehicle, and have no weapons. They come back three seconds after a death,
  on a board again. Their run-over (a skater riding into a person on foot,
  port/skate/README.md's "Multiplayer") deals 300% of any skater's
  (`server.infection_run_over`, the game type's `infection_run_over`); the
  board has to hit the survivor (`infection_reach`, BOARD REACH, can make a
  survivor wider to it; none by default). They are tougher: they take 35% of
  the damage (`server.infection_damage_taken`, the game type's
  `infection_damage_taken`), their boards are 40% quicker, pushing and at
  speed (their skate sessions step 1.4 times as fast: `server.infection_speed`,
  the game type's `infection_speed`), and they carve more sharply than
  anyone (their trucks steer 1.75 times as far: `server.infection_turning`,
  the game type's `infection_turning`).
- **Out of reach:** a survivor who stays 10 seconds where no skateboard
  reaches (a base's roof, a cliff ledge, Hang 'Em High's tower top) is
  hurt each second until they come down - a quarter of their health, the
  shield gone - and one who dies of it comes back infected (the game type's
  `infection_out_of_reach`, OUT OF REACH: 5 to 30 seconds, or off).
  "RETURN TO COMBAT" in the middle of their screen counts the seconds down
  first, and stays while it hurts. Where a board reaches is the bots'
  walking graph of the map ("The walking graph"): from the main ground a
  board rides all round, what it rolls up (no steeper than a riding bot
  takes), down, or drops onto - no jumps, ladders or teleporters; a spot
  hardly above such ground (under a world unit, within three) counts as
  reached. The host decides and hurts (no wire change: the vitality goes
  out as any damage's); every machine builds the graph (a client too, aside,
  as a host does) to warn its own players. Until the graph is built,
  nobody is hurt.
- **Editing it:** Edit Gametypes (or Server Setup's game type) on a Slayer
  gametype: INFECTION OPTIONS... opens Infection's screen - INFECTION
  (on or off: any Slayer gametype can be Infection), INFECTED SPEED,
  INFECTED DAMAGE TAKEN, RUN-OVER DAMAGE, INFECTED TURNING, BOARD REACH,
  FIRST INFECTED, ROUND TIME and OUT OF REACH, the built-in's values at first. They are kept in the gametype
  and travel with the game; a dedicated server's `server.infection_*`
  settings override its own.
- **Tuned with bots:** eight normal bots, five-minute rounds on Blood
  Gulch, Sidewinder and Hang 'Em High (tools/test_infection.py's balance
  test, seeds 3 to 10 on each): with the board having to hit, the infected
  won 12 rounds of 24 (Blood Gulch 4 of 8, Sidewinder 2, Hang 'Em High 6)
  at 140% speed, 175% turning and 35% damage taken. At 130% and 50% they
  won 3 of 12, at 140% and 50% 2 of 12, at 140% and 40% 4 of 12: they have to
  get to the survivors through their fire, and many of their hits come too
  slow to run anyone over. (Before, a survivor was 0.8 world units wider to their
  boards, and they won 5 rounds of 8 on Blood Gulch at 115%.)
- **The survivors** play as in Team Slayer, with their weapons (no vehicles:
  one in a vehicle could not be run over). A survivor who dies, however,
  comes back infected; a player who joins during the game starts infected.
- **The end:** the infected win once everyone is infected; the survivors
  win if any of them is left when the round's five minutes are up (or the
  game's time limit, if shorter: a dedicated server's `server.time_limit`).
- **The score:** an infected's score is the survivors they infected; a
  survivor's, the infected they killed, and the time they survived beside it
  (`3 (2:15)`). The team scores show the players on each side.
- **The HUD** says INFECTED or SURVIVOR at the top, and how many survivors
  are left.

Who is infected is the player's team, which the host sets and every machine
takes from the host's units, as the gametype's auto team balance moves
players; the game type's settings travel in bytes of Team Slayer's
variant that the Xbox's game left unused. So the network format is
unchanged. A build without Skate 3 mode (or without its data) leaves the
infected on foot, unarmed.

## Trick Attack

A game type of the port's: Slayer, every skater for themselves, on Skate 3
mode's boards (port/skate/README.md), scored by the tricks each one lands.
Choose **TRICK ATTACK** among the standard game types, or give a dedicated
server `trick_attack` in `server.game_types` or `server.maps`
(`bloodgulch:trick_attack`).

- **Everyone skates, nobody shoots:** every player is always on a board,
  with Infection's rules for the infected - no getting off, firing, melee,
  grenades, picking up or vehicles, no weapons. A run-over hurts nobody;
  falling off the level still kills, and a player comes back three seconds
  later, on a board.
- **Tricks:** what Skate 3's own scoring (the skate engine's) makes of a
  skater's riding - flips, grabs, grinds, manuals - one sequence at a time,
  from leaving the ground (or starting a grind, a manual) to rolling away
  clean, with the sequence's own multiplier.
- **Combos:** each sequence landed joins the skater's combo, which is worth
  its sequences' points times one for each trick in it (x2, x3 ..., to
  x10). It banks - into the player's score - once they have ridden on clean
  for the bank delay (two seconds) after their last landing, with nothing
  under way; landing another sequence before then chains it on. A bail (or
  anything else that takes them off the board, a death too) loses the
  combo.
- **Ramming:** a skater who knocks into one with a combo going makes them
  bail (the host's verdict on the hit, port/skate/README.md's
  "Multiplayer") and takes half of the combo (the steal share) into their
  own score at once; whoever came at the other faster is the rammer.
- **The hot spot:** one spot of the map's main ground, where a board
  reaches and rides away (the bots' walking graph, as Infection's out of
  reach: "The walking graph"), counts double for whatever is landed within
  five world units of it, for a minute; then it moves at least 20 world
  units away. Every machine draws its boundary on the ground as King of
  the Hill draws its hill (the hill's band, in the same shader): a
  sixteen-sided outline five world units round it, and what is landed
  inside that outline (on its edge too), seen from above, is what counts. Chosen with the game's random numbers (fixed by
  `debug.game_seed`).
- **The end:** the round's five minutes (or the game's time limit, if
  shorter); the highest score wins, as in Slayer. There is no score limit.
- **The HUD:** the player's combo and multiplier bottom centre, with the
  sequence under way and its last trick under it; the five best scores top
  right (and the player's own place past them); the hot spot's multiplier,
  time left and distance at the top, and a marker over it; every skater
  with 5000 or more unbanked has it shown over their head; a combo of 5000
  or more banked is called out to everyone ("Ash landed a 12,400 combo").
  A spectator's director ("Spectators") prefers the
  biggest combo going.
- **Editing it:** Edit Gametypes (or Server Setup's game type) on a Slayer
  gametype: TRICK ATTACK OPTIONS... opens Trick Attack's screen - TRICK
  ATTACK (on or off: any Slayer gametype can be Trick Attack), ROUND TIME
  (1 to 30 minutes), BANK DELAY (0.5 to 5 seconds), STEAL SHARE (10% to
  100%), HOT SPOT MULTIPLIER (x1.5 to x5), HOT SPOT DURATION (15 to 180
  seconds) and RAMMING (on or off), the built-in's values at first. They
  are kept in the gametype and travel with the game; a dedicated server's
  `server.trick_minutes`, `server.trick_bank_delay` (tenths of a second),
  `server.trick_steal`, `server.trick_hot_spot_multiplier` (tenths),
  `server.trick_hot_spot_seconds` and `server.trick_ramming` (1 on, 0 off)
  override its own (the chart's `server.trickAttack`).
- **Bots** ride, flick and grind as they always do on a board
  ("Bots on boards"), without the caution a fight would ask for, and chain
  their tricks into combos (a normal or better bot tricks again soon enough
  to land within the bank delay); past a combo of their own size (1500 for
  an easy bot, 4000 normal, 8000 hard, 12000 legendary) - or, hard and
  better, with another skater close - they start nothing new and ride on
  to bank it. They head for the hot spot and ride about it; hard and
  legendary bots hunt any combo of 5000 or more within 40 world units and
  ride into its skater. Peaceful bots never ride at anyone.

The host keeps the scores. Its own skaters' tricks (its player's, its
bots') come from its skate engine; a client's skater's from the client, in
the skate report message (`_distributed_message_skate_report`): an entry of
a kind of its own, the running totals of its engine's scoring a few times a
second, so that a lost datagram loses nothing, taken only for that
machine's player and capped. The banked scores reach every machine as
Slayer's always have (the game type's state); the combos going and the hot
spot, a few times a second, in entries of their own in the skate verdict
message. Builds without Trick Attack skip those entries as of another
format, and the game type's settings travel in bytes of Slayer's variant
that the Xbox's game left unused, so the network version is unchanged; an
older client plays Slayer on foot (or on a board, with Skate 3 mode) and
scores nothing. Without Skate 3 mode (or its data) nobody skates and
nobody scores.

## Bots

A host can have bots
: players that the host plays itself. Set `server.bots`
(`HALO_SERVER_BOTS`) to the number of bots, from 0 to 127, or 0 to 128 (a
full lobby) with `server.bots_fill` below. The bots join the
lobby of each game that the host makes (a dedicated server's, or a game
that the menus host), and stay from game to game as the other players do.
Co-op games have no bots.

- A bot's name starts with `[BOT] `, for example `[BOT] Alpha`. Nothing
  else tells the other machines that a player is a bot.
- To every machine, a bot is a player of another machine: it is on the
  scoreboard, in the kill messages and on the teams, and it moves as other
  players move. A bot needs no change to what the machines send each other,
  so builds without bots (and upstream OpenCE builds of the same network
  version) play with them. Refer to `NETCODE.md`, "Bots".
- Only the host runs the bots. They give their input each tick of the host,
  as the host's own players do.
- The bots are on machine slots of their own, from the last slot (127)
  down, four bots to a slot. No connection uses these slots.
- When a person joins a game that is full, a bot leaves at once to make
  room for them (one of the larger team, so that the teams stay even), and
  comes back when a person leaves. With `server.bots_fill`, `server.bots`
  is how many players the game has in all: a bot leaves as each person
  joins. `server.max_players` sets how many players a dedicated server's
  games take (0: 128).
- Without `server.bots_fill`, `server.bots` stays at 127 at most, so a
  person can always join; with it, a full lobby of 128 is allowed, since a
  person who joins one still gets in (a bot leaves at once, as above).
- A host that the menus make sets them in Server Setup's BOTS screen
  (BOTS: 0 to 16, then 20, 24, 32, 48, 64, 96, 127 and 128; DIFFICULTY and
  FILL, which write these settings); a change in the
  lobby takes effect at once.
- A bot plays Slayer: it roams the map along the walking graph (below),
  takes better weapons, powerups and grenades lying about, and fights the
  players of the other team (anyone, without teams) that it sees or hears.
  It plays the other game types too ("Game types", below).
- With Skate 3 mode and its data a bot rides a board to travel and steps off
  to fight (`port/skate/README.md`, "Bots on boards"); `server.bot_skaters`
  can limit how many ride at once (none at first: every bot may). Riding,
  it does tricks by its difficulty: flips, grabs, grinds, manuals and combos,
  as a person does them through the pad, never with an enemy about.

### Fighting

A bot sees as a person does (`game/bots_combat.c`): a few times a second it
looks for enemies in front of it (its field of view), near enough, with
nothing of the level or its scenery between its eyes and their head or body
(the game's line of sight test, which sees through fences and grates). It
never sees through walls. When it is hurt, it turns to where the shots came
from (roughly, as a person who is hit from behind does). It remembers where
it last saw its target for 4 seconds and goes there.

- It waits its reaction time after it first sees a target before it fires.
- Its aim turns at most so fast, and is off by an error that wanders, that
  shrinks while it keeps the target in view, and that grows when it is hit.
  It leads a moving target by the flight time of its weapon's projectile.
- It fires in bursts, and only when its aim is near enough the target.
- While it fights it strafes (only onto floor that the walking graph has),
  closes on a far target, backs off a near one, and strikes one at hand.
- It reloads an empty magazine, and one below a third when it fights nobody.
- It does not fire when a team mate is in the line of fire, and does not
  strike when a team mate is at hand in front of it.

A bot hears as a person does (`game/bots_hearing.c`). When it sees nobody
to fight, it hears an enemy who fires, and an enemy's grenade or rocket in
the air, within its hearing range. Its ear is no better than a person's: it
knows the direction and a rough place, less exact the further the noise,
and it does not follow someone who has stopped making noise. It turns to
look that way, and then it goes there along the walking graph to see. Any
bot (a peaceful one too) runs from a live grenade that lies or flies near
it.

A bot throws grenades (`game/bots_grenades.c`), now and then while it
fights:

- At the enemy that it sees, where they will be when the grenade lands; or
  where it last saw an enemy who went out of sight (around a corner, behind
  a wall: in a lob over it). More often at a group, and at an enemy who
  hides.
- A plasma grenade at one enemy in view (it sticks; the bot then backs
  away), a frag grenade at a group or at an enemy out of sight.
- It aims as the game throws a person's grenade (the same hand, speed and
  fall), and follows the flight through the level first. It throws only
  when the grenade lands near the point and far enough from itself and from
  each team mate (where they are and where they are going), and it comes no
  nearer its target while the grenade goes off: it does not kill itself
  with its grenades, and hardly ever a team mate (one who runs into the
  blast). A frag grenade that would hit a wall near it is not thrown,
  because it would bounce back.
- It carries the grenades that the game gives at spawn and those that it
  takes on the map. It goes for grenades lying about when it lacks them.

A bot moves in a fight as a person does (`game/bots_cover.c`,
`game/bots_items.c`):

- With its shields nearly gone (under a quarter; and, if it sees its target
  no longer, its body hurt), it falls back to cover: a node of the walking
  graph 2.5 to 12 world units away that its target cannot see from where it
  stands, not toward the target, the nearest such. It keeps shooting at a
  target it sees on the way. It waits there while its shields come back,
  and fights again when they are back, when the target comes within 3.5
  world units, or after 8 seconds. Easy bots stand and fight.
- It goes for powerups: an overshield and active camouflage always, a
  health pack when it is hurt. It goes for power weapons from further off:
  a rocket launcher, a sniper rifle, a fuel rod gun, a shotgun. With both
  hands full, it takes only a weapon better than the worse of its two (the
  one it drops), and ammunition for a weapon that runs low.
- It fights with the weapon that suits the range: a shotgun near, a sniper
  rifle far, a rocket launcher or fuel rod gun at middle range but never
  within its own blast (it does not fire one at a target within 4 world
  units), and the pistol over the assault rifle beyond 15. Between fights,
  the weapon for a middling range is in its hand.

`server.bot_difficulty` sets how well the bots fight:

| Difficulty | Reaction | Aim turns | Aim error | Sees | Field of view | Hears | Grenades |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `peaceful` | never fights | - | - | - | - | - | never |
| `easy` | 700 ms | 120 degrees/s | large | 25 world units | 90 degrees | 18 world units | rarely, 12 s apart at least |
| `normal` (default) | 470 ms | 205 degrees/s | medium | 35 world units | 110 degrees | 26 world units | sometimes, 7 s apart |
| `hard` | 220 ms | 345 degrees/s | small | 45 world units | 120 degrees | 32 world units | often, 5 s apart |
| `legendary` | 120 ms | 520 degrees/s | very small | 55 world units | 130 degrees | 38 world units | very often, 3.5 s apart |

An easy bot does not fall back to cover when its shields are gone; the
others do.

How the difficulties do against each other, from bots alone (four bots, one
of each, three-minute Slayer games, every game seeded with
`debug.game_seed`; the order the difficulties join in turned round from
game to game, four games of each order). Each cell is kills/deaths:

| Map | `easy` | `normal` | `hard` | `legendary` |
| --- | --- | --- | --- | --- |
| Hang 'Em High | 30/36 | 33/44 | 43/37 | 46/35 |
| Chill Out | 15/35 | 20/27 | 28/27 | 46/20 |
| Beaver Creek | 10/18 | 13/13 | 18/15 | 18/13 |
| Prisoner | 7/16 | 10/13 | 17/9 | 14/11 |
| **all 16 games** | **62/105 (0.59)** | **76/97 (0.78)** | **106/88 (1.20)** | **124/79 (1.57)** |

Before tuning, the same games came out 0.46, 1.10, 1.23 and 1.44: easy
was weak, but normal was nearly as strong as hard. Tuned: easy reacts in
700 ms (was 600) with more aim error; normal in 470 ms (was 400) with a
little more aim error; hard in 220 ms (was 250), turning its aim at 345
degrees/s (was 290) with less aim error; legendary in 120 ms (was 170),
turning at 520 degrees/s (was 410) with less aim error. A single map's few
games still go either way (Beaver Creek and Prisoner have few kills in
three minutes); the order holds over all of them.
`tools/test_bots.py`'s `test_bots_difficulties` keeps the order.

### Game types

A bot plays each game type's objective (`game/bots_objectives.c`), from what
every machine sees: the flags and balls (weapons the game marks so), the
places the game marks for the HUD's navigation points, and the map's
netgame flags. It still fights what it meets on the way.

- **Capture the Flag:** a bot that carries the enemy's flag runs home with
  it; it does not shoot, change weapons or press the action or reload
  button, which would drop the flag (it can still strike a blow), and
  keeps off teleporters, which would lose it. A bot
  whose own flag lies dropped returns it, if it is the nearest of its team.
  When its flag is carried off, its team's defender, its nearest bot and any
  bot within 12 world units hunt the carrier; the attackers go on (a
  team's only bot goes for the other flag, unless the carrier is near). The
  others share the roles: the first, third and so on of a team's bots
  attack (they go for the enemy's flag wherever it is, or escort a team
  mate who has it), and the second, fourth and so on defend (they stay
  within 5 world units of their flag's stand). With one flag, its team
  defends it and the other attacks.
- **King of the Hill:** to the hill, and stays on it, moving about within
  it, fighting from it and not chasing anyone off it, nor going for an item
  more than 3 world units off it (an overshield 8 world units away had bots
  leave the hill for it and come back, again and again); a moving hill is
  followed.
- **Oddball:** to a ball lying about, to take it; with it, away from its
  enemies (to the place furthest from them of a few). An enemy who has it
  is hunted, a team mate who has it escorted. In Juggernaut and reverse tag
  the bot who is "it" fights, and the others hunt it.
- **Race:** to the race's flags, each in turn, driving a Warthog or Ghost
  where the map has them ("Bots in vehicles").
- **Team Slayer:** a bot that sees nobody goes over to a team mate now and
  then, so that team mates fight together.

`server.game_types` takes `juggernaut` and `infection` too. In Infection
an infected bot is always on its board and hunts the survivors on it: it
heads for the nearest survivor and rides at one it sees within 25 world
units, at every difficulty but peaceful (a peaceful infected rides about and
never rides at anyone on purpose), steering hard for where they will be when
it gets there (its board has to hit them). A survivor bot stays on foot and
fights with its weapons, steps sideways off the line of an infected skater
coming at it (within 9 world units; a normal bot only within 4.5; or away
from one nearer than 2.5), and, out of reach for three seconds, heads for
the nearest ground a board reaches.

### Bots in vehicles

Bots drive Warthogs, rocket Warthogs, Ghosts and Scorpions
(`game/bots_vehicle.c`) on maps that have them: of the stock maps, Blood
Gulch and Sidewinder. Everything goes through
the bot's input, as a person's does, so nothing is new on the wire:

- A bot thinks of a ride when its goal is far (40 world units, about 120 m),
  it is not fighting and holds no flag or ball: the nearest free Warthog or
  Ghost within 25 world units that nobody rides, no other bot has, and no
  person stands near (a person may want it), if riding is quicker than
  walking.
- It walks to the driver's door (round the vehicle when it comes from the
  other side), stops, and presses the action button while the game offers it
  the driver's seat.
- It drives along the graph that vehicles drive (below, "The walking
  graph"), steering for a point a few world units ahead, slower in turns,
  near its goal, leaning or in the air. It brakes for anyone on foot ahead,
  bots and people alike, and slows for vehicles, but for one it means to run
  over (below, by difficulty). Peaceful bots drive too.
- Getting nowhere, it backs up with the wheels turned and goes round. Stuck
  again and again on one part of the way, it keeps off it, and after six
  times in 20 seconds it gets out and walks.
- It gets out at its goal (stopped first), or when the vehicle turns over. A
  vehicle on its side or roof it flips back over, as a person does, and gets
  back in.
- A bot that is in a vehicle it never chose (the host put it there) gets out
  at once (`bots: player N got into vehicle ... by mistake`), and a bot on
  foot presses no action button that would get it into a vehicle by
  mistake.
- `server.bot_vehicles` (`HALO_SERVER_BOT_VEHICLES`) is the most bots driving
  at once: 0 (at first) for no limit, -1 for bots that never get in. The
  report (`bots: [BOT] ... at tick`) counts each bot's rides, seconds and
  world units driven, backing up, vehicles flipped back, people braked for
  and people driven at.
- **Gunners.** In team games a bot takes the gunner's seat of a Warthog
  that a team mate drives (a bot, or a person) when it is near and slow; a
  bot driver waits a few seconds for a team mate on its way to the gun, and
  takes a Warthog whose gun a team mate already sits in, waiting for a
  driver (whichever of the two got there first). The
  gunner aims and fires the chaingun as it fights on foot (its reaction,
  aim and error by difficulty; its lead takes the vehicle's own way off),
  and gets out when nobody drives. A peaceful bot may ride a gun and never
  fires it.
- **Fighting from a vehicle.** A driver with a gunner aboard stops near an
  enemy for the gunner's shots (easy, normal) or circles it (hard,
  legendary), and drives on to another place instead of getting out at its
  goal. A crew that fights from its vehicle (a gunner who fires aboard, or
  a Ghost's or a Scorpion's driver who may fight) drives to the fight it
  knows of: a target its driver or gunner saw, or gunfire either heard, in
  the last 8 seconds. A Ghost's driver is its gunner: a bot fights from one, aiming as it
  steers, closing on a far target and backing off a near one, and a bot that
  may fight takes a Ghost near in a fight. A Warthog's driver alone gets out
  to fight on foot, unless it may run its target over.
- **Running people over, by difficulty.** Peaceful and easy bots never drive
  at anyone on purpose and brake for people on foot; normal bots drive at a
  target on foot already ahead (within 20 degrees, 12 world units); hard and
  legendary bots hunt targets on foot within 25 world units, in a Warthog
  (which they take in a fight for it) or a Ghost. Never a team mate, nor
  anyone riding. Accidents happen at every difficulty. The report counts
  the people a bot drove at and those its vehicle ran over.
- **Sharing with people.** A bot that drives with a person aboard (at its
  gun, or a passenger) drives them about and gets out only when it must (it
  goes on to another place at its goal); a person driving a Warthog near a
  bot of their team gets the bot at the gun. A bot takes no vehicle a person
  stands near (6 world units) or comes to on foot (15), and gives its
  driver's seat up to a person (a team mate in a team game) who waits at its
  door for a second and a half.
- `debug.network_test_vehicles` ("all") gives an automated test game every
  vehicle the map places (the built-in game types place Blood Gulch's
  Warthogs only); `debug.network_test_vehicle_share` scripts a person sharing
  vehicles with the bots of their team (tools/test_bots.py).
- **Scorpions.** A tank drives a graph of its own (below), wider and less
  steep. Its cannon is its driver's: a bot fights from it as from a Ghost,
  keeping further off and strafing not, and fires nothing near enough for
  the shell's blast to reach it (nor does a rocket Warthog's gunner). A
  peaceful bot drives one and never fires. Its tread seats are not taken.
- **Game types.** While a bot drives, its goal follows its game type's
  objective (a race's next flag, a flag or ball to take, a hill or flag
  stand to hold, someone to hunt); it gets out on the hill or stand it
  holds (but in a Ghost or a Scorpion, whose guns are its own, it stays
  there aboard), and takes no vehicle standing there. Nor does it take one
  standing within 10 world units of its goal: it would get out as soon as it
  got in. A bot with a flag or ball in
  hand takes none.
- **Walking, a board or a vehicle.** Where the host has Skate 3 mode, a bot
  weighs its board (about 1.3 times as quick as walking) against a vehicle
  before it takes one (`bots: player N takes vehicle ...` logs both), and
  steps off its board ("a vehicle") for one it takes.
- Not yet: flying (Banshees).

Peaceful bots roam, take weapons and play as the others do, but never fight:
they never aim at a player, shoot, throw a grenade or strike. They can still
die (a fall, another player, a skater running them over), and they are on
the scoreboard.

### The walking graph

The host builds a walking graph of each map that it plays with bots
(`game/bots_nav.c`, `game/bots_nav_game.c`), when the game's map is loaded:

- From the collision of the level and of its static scenery (teleporter
  pads, crates, rocks), in the way of Recast: the level is cut into columns
  of 0.1 world units; a floor that slopes 45 degrees or less, with 0.7
  world units of room above it, is walkable; walkable floor nearer than
  0.25 world units to an edge or a wall is dropped; and each tile of 1 by 1
  world units gives one node for each connected piece of floor in it.
- Edges join the nodes: walks between pieces that touch (with steps of up
  to 0.3), drops of up to 2.9 world units (falling damage starts at 3),
  jumps of up to 0.65 (a jump rises about 0.69; a bot crouches in the air
  for a high one), the teleporters (netgame flags 6 and 7, paired by their
  team index), and the ladders: Boarding Action's, and Sidewinder's poles
  (the climbable collision surfaces: a biped that faces one walks up it). A
  drop or a jump must be clear of walls (the game's collision test), and
  none goes past a ladder, which would catch a biped: a bot goes down beside
  a ladder with its back to it.
- Only what the spawns can reach, and what can reach the spawns, is kept.
  The "main part" below is the part of the map that the most spawns can go
  to and come back from.
- Building takes 0.1 to 6 seconds for a map (Blood Gulch's three graphs,
  with the vehicles' below, about 13). It is built on a thread of its own,
  while the game and its network go on (the game's collision test, which
  only the game's thread may make, answers the build's questions in
  batches, 20 milliseconds a frame; the graph is the same): the host used to stand still for
  it, sending nothing, and a machine loading the map then could give up on
  it (15 seconds). A host starting a game with bots waits for their graphs
  before its game's clock starts, as it waits for a machine still loading
  the map (`networking/network_server_manager.c`; at most a minute, after
  which its bots stand still until their graph is done), so the bots begin
  with the game. `debug.bot_nav_build_delay` makes the walking graph that
  many seconds slower, for `test_bots_a_client_loading_while_the_graphs_build`.
  The graph is cached in the save
  root (`z:\bots_nav_<map>_<key>.nav`; the browser's private storage in
  the web build), keyed by the map's checksum, so a machine builds the graph
  of a map once. It is made from the player's own map files, and is never in
  the repository or in a build.
- A bot plans its way with A* and follows it: it cuts across between nodes
  that lie along a line where nothing is in the way, hops up steps, jumps
  (a long jump with a run-up), drops, steps onto a teleporter (off it and on
  again if it came out on it), and climbs ladders (going up or down counts
  as getting somewhere; near the top it steps off with a hop).
- A point it comes no nearer for a moment (a wall, a railing or a player
  between) is as near as it gets: it goes on. Pushing on and hardly moving
  for a quarter of a second, it hops (a lip or a ridge the graph walks
  over). When it still gets nowhere, it jumps and steps aside, then plans
  again, then keeps off the edge it was on and picks another place. An edge
  that a bot fails costs more for every bot on that map, and a second
  failure removes it.
- It takes a weapon from the ground with the swap button when its hands
  are full (the weapon in hand is dropped), and swaps no other for 30
  seconds after. It gives up on an item that it stood at for 2 seconds
  without taking it.
- Each bot's report in the log has the seconds it was stuck (each look at
  how far it had come, a second and a half apart, that found it had not).
  Four bots roaming each stock map for 80 seconds were stuck 0 to 10 seconds
  each, under 3 on most (0 to 18 before these rules).

`debug.bot_nav_check` builds the graph of each map the host plays, logs
how much of the map it serves, and ends the game, so that a dedicated
server with `server.maps` set to the thirteen stock maps goes through them
a map at a time (about three and a half minutes). Every spawn of the 13
stock maps reaches the main part, and the main part reaches 523 of their 534
multiplayer weapons: all of them on 9 maps. The check logs each one that is
not reached, why, and what is over it.

A table of hand-checked links for a map (`bots_nav_overrides` in
`game/bots_nav_game.c`, points only) adds what the generator does not
find: Damnation's upper tier (its floor meets the main part's at a corner,
across a little hole: a hop), Wizard's raised middle (a running jump across
the moat from each corner of the walkway; bots make it about one try in
five, and give up on a corner they fail twice), and the rock by Beaver
Creek's blue base. Not reached, and why:

- Damnation, 5 of 46: they lie in a duct behind a slit that no biped
  passes.
- Longest, 4 of 40: they are on platforms 2.1 world units high, with
  nothing to climb from.
- Chillout, Hang 'Em High and Rat Race, 1 each: on ledges or beams too
  narrow for the graph.

`debug.bot_goal` sends every bot again and again to one point, to check a
map's links in the game (each arrival is logged). `debug.bot_trace` logs
each bot's way every second, each time it gets stuck, and what is in its
way.

The host's console has a `bot` command:

| Command | Effect |
| --- | --- |
| `bot add [count] [difficulty]` | Adds bots to the lobby or to the game that is in progress (1 if no count; `server.bot_difficulty` if no difficulty). |
| `bot difficulty <level> [name]` | Sets the difficulty (`peaceful`, `easy`, `normal`, `hard` or `legendary`) of every bot, or of the bot named. |
| `bot remove <name>` | Removes a bot. The name can be the full name, the name without `[BOT] `, or the start of either. |
| `bot remove all` | Removes all the bots. |
| `bot list` | Lists the bots, their teams and their difficulties. |

`kick <name>` and `ban <name>` remove a bot (a bot has no address to keep
out), and so do Kick and Ban on the scoreboard. A vote to kick a bot is
refused, and bots do not vote. On a dedicated server, the telnet console
(`debug.telnet_console`) gives the console.

`debug.bot_seed` sets the start of the bots' random numbers, so that the
bots of a test do the same each run.

`tools/test_bots.py` tests the bots in real games on one machine: a
dedicated server with bots and a client, which sees them play, and the
console's `bot`, `kick` commands in the game; a client built from before
bots (`HALO_TEST_OLD_CLIENT`) on a server with bots; a listen host with
bots and a client; the walking graph of every stock map
(`test_bots_nav_on_every_map`: each map's reach, and the cache); and four
bots roaming Boarding Action (`test_bots_roam`: goals reached, ladders
taken, no bot stuck more than 12 seconds of 80); and four bots playing a two-minute Slayer on Hang 'Em High
(`test_bots_slayer`: each kills and dies, weapons taken). CI runs them in the `bots` job on `opence-k8s`, in about 12
minutes. `tools/harness/tests/test_bots_nav.py` tests the graph's builder on
made-up levels, without the game.

On a map that places vehicles, the host builds a second graph the same way,
for vehicles to drive (`bots_nav_vehicle_parameters`): half a world unit
from walls and a world unit of room, slopes up to 35 degrees, lips of 0.3 and
drops of up to 1 driven over, no jumps, ladders or teleporters, tiles of 2
world units, and the map's vehicles as its seeds. It is cached beside the
walking graph (`z:\bots_drive_<map>_<key>.nav`). On Blood Gulch and Sidewinder
it has about 2,200 nodes, and each map's eight vehicles are on its main part.
A third graph is for tanks (`bots_nav_tank_parameters`: 0.8 world units from
walls, 1.3 of room, slopes up to 30 degrees, lips of 0.4), its seeds the
map's Scorpions (`z:\bots_tank_<map>_<key>.nav`).

## Spectators

A spectator watches a network game without playing in it. Set JOIN GAMES AS
to SPECTATOR (Settings > Network Setup > SPECTATING; `network.spectate`,
`HALO_SPECTATE`), or type `spectate on` at the console, and join a game as
usual: a dedicated server, or a game that
someone hosts from the menus. In the browser, add `?spectate` to the page's
address (with `?join=<code>` to join an internet game).

- A spectator takes no player slot, never spawns, is on no team and is not
  on the scoreboard. Its machine brings no player to the game, so it needs
  no change to what the machines send each other: hosts and players of the
  same network version, with or without spectators of their own, play with
  it. Refer to `NETCODE.md`, "Spectators".
- It sees what a player's machine sees: every player, the bots, vehicles
  and what they do.
- The lobby says how many machines watch the game ("3 of 16 players, 1
  watching").
- A host takes up to `server.max_spectators` (`HALO_SERVER_MAX_SPECTATORS`,
  16) spectators: 0 for none, -1 for no limit. A machine counts as one once it
  has been in the game ten seconds with no player; past the limit the latest
  is turned away as a full game turns a machine away. A dedicated server logs
  the count as it changes (`server: 2 watching`). A spectator does not vote,
  and cannot start a vote to kick a player.

Four cameras, which the action key (E, or X on a controller) switches
between (Network Setup's SPECTATOR CAMERA, `network.spectate_camera`, is the
first):

- **FOLLOW** (first) stays behind the player that it watches. The look keys
  or stick turn it round them, and it turns back behind them two seconds
  after the look is let go. Forward and back bring it nearer and further.
- **FIRST PERSON** looks through the eyes of that player.
- **FREE** flies where it looks, through walls. The movement keys or left
  stick move it, jump and crouch move it up and down, and the weapon and
  grenade switches (and the mouse wheel) make it faster and slower.
- **DIRECTOR** chooses whom to watch, as a person directing a broadcast
  does (`spectate_director.c`). It follows the player most worth watching
  from behind: a kill in the last seconds counts most, then a fight (their
  weapon fired, or they were hurt), carrying the flag or the ball, driving
  fast, riding a board fast or in the air (Skate 3 mode), enemies near, and
  a little the longer since they were shown. A
  shot lasts five seconds at least and fifteen at most, and cuts early only
  to someone clearly more worth watching. When the player watched dies, it
  stays on their fall two seconds and then cuts to their killer. A cut to
  someone near glides; to someone far, it is a hard cut. Picking a player
  (fire, grenade) hands the camera back to FOLLOW.

Fire (the left mouse button, the right trigger) watches the next player,
and the grenade (the right mouse button, the left trigger) the one before.
The scoreboard key (Tab, Back) shows the scores. The flashlight key hides
and shows the names over the players' heads. The top of the screen says
who is watched, with their health and shields.

The console's `spectate` command does the same: `spectate next`,
`spectate previous`, `spectate free`, `spectate follow`, `spectate first`,
`spectate player <name>` (or the start of the name), `spectate speed <n>`
(world units a second), `spectate director`, `spectate scores` (the scores shown until it is
typed again), and `spectate` alone says what it watches.

A spectator joins a game in its lobby or in progress. It needs a host of this
build or newer, which keeps and starts a machine that brings no player; an
older host refuses a machine with no player after fifteen seconds. A
spectator cannot join a lobby or a game that is full of players: the host
refuses a full game before the spectator can say that it brings no player.

## Streaming

The Linux build can stream a game for people to watch: with
`stream.enabled` (`HALO_STREAM=1`) it runs with no window and no input, joins
the game that system link finds as a spectator whose camera is the director
(`stream.camera`), and sends its picture and sound raw to two pipes, which
ffmpeg encodes. It joins again when the game is gone (the host restarted or
went) and shows the lobby and the scores between games.

- **The picture** goes to `stream.video` (`HALO_STREAM_VIDEO`), a FIFO:
  packed RGB (`rgb24`), `stream.width` x `stream.height` (1280 x 720),
  `stream.fps` (30) frames a second at a steady rate. The game draws no
  faster; a frame it draws late repeats the last one, so the rate stays
  steady. It is drawn in a window no display shows (SDL's offscreen driver,
  OpenGL through EGL: Mesa's llvmpipe on a machine without a GPU).
- **The sound** goes to `stream.audio` (`HALO_STREAM_AUDIO`), a FIFO:
  signed 16-bit little-endian stereo at 48000 Hz (`s16le`), made in real
  time.
- **Ready.** Until a reader opens a pipe, what would go to it is thrown away
  and the game goes on; a reader that goes away is waited for again. The
  first frame read logs `stream: streaming` and writes
  `stream.ready_file` (`HALO_STREAM_READY_FILE`), which a readiness probe
  can wait for; `stream: watching a game` follows once it is in a game. The
  file is removed at exit. SIGTERM leaves the game and exits at once.
- **Where to.** System link's search (`debug.network_test` "join"):
  `network.broadcast` (`HALO_NET_BROADCAST`) set to the server's address
  finds it on another network, for example a server in the same cluster. A host answers a
  search at the searcher's own address too (as well as by broadcast), so the server needs no
  address of the stream.
- **A container.** docker/server/Dockerfile's `stream` target is an image of it (the portable build,
  32-bit Mesa, ffmpeg, and `docker/stream/stream.sh`, which makes the FIFOs and runs the game and
  ffmpeg to HLS); the Helm chart's `stream` runs it beside a server (charts/opence-server/README.md,
  "Stream").

For example, with the FIFOs made first (`mkfifo video audio`):

```sh
HALO_STREAM=1 HALO_STREAM_VIDEO=video HALO_STREAM_AUDIO=audio HALO_STREAM_READY_FILE=ready \
  HALO_NET_BROADCAST=<server address> ./halo &
ffmpeg -f rawvideo -pix_fmt rgb24 -s 1280x720 -r 30 -use_wallclock_as_timestamps 1 -thread_queue_size 64 -i video \
  -f s16le -ar 48000 -ac 2 -use_wallclock_as_timestamps 1 -thread_queue_size 1024 -i audio \
  -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p -g 60 -b:v 3M -c:a aac -b:a 128k \
  -f hls -hls_time 2 -hls_list_size 6 -hls_flags delete_segments live.m3u8
```

The pipes are read as they come (`-use_wallclock_as_timestamps`), so the
picture and the sound stay together whenever ffmpeg opens them.

What it costs (measured in a container limited to 4 CPUs, with llvmpipe and
`LP_NUM_THREADS=3`; the server elsewhere):

| Picture | Frames drawn a second | The game's CPU | ffmpeg's CPU (x264 veryfast) | Memory |
| --- | --- | --- | --- | --- |
| 1280 x 720 | about 10 (30 sent) | about 2.5 cores | about 0.7 core | about 470 MB |
| 854 x 480 | about 18 (30 sent) | about 2.1 cores | about 0.5 core | about 390 MB |

llvmpipe cannot draw 30 frames a second at either size: most of a frame is
spent waiting for its threads to finish passes that the next pass reads
(render to texture). A GPU (for example AMD's integrated graphics with Mesa's
radeonsi) draws it in a fraction of a core, and VA-API encoding (ffmpeg's
`h264_vaapi`) takes the encoder's core away too. Set `LP_NUM_THREADS` to the
CPUs the container has: llvmpipe otherwise starts a thread for each of the
machine's CPUs. The first frame is sent a fraction of a second after the
start (the loading screen), and the stream watches a game in progress about
5 to 20 seconds after the start.

## Map checks

The game reads a map's tags straight into memory and uses them as its own
structures: every pointer, count, index and enum in them is the map's, and
the game writes values into tags as it runs. So before anything reads a
map's tags, the port checks every tag against a schema of its group
(`game/tag_schema_*.c`, read by `game/tag_validate.c`), and each structure
BSP as it loads:

- Every block and every piece of data must lie in the tags (or the BSP) and
  overlap no other. Otherwise the game refuses the map.
- A block with more elements than the game has room for is cut to the
  maximum. A tag reference that is not a tag of the right group becomes
  none. So do an index past its block and an enum past its values (or they
  become 0, where the game cannot take none). A string gets its terminator.
  Values that the game sets as it runs are reset.
- Checks that the schema cannot express run last: the BSPs' and the models'
  graphs, vertex and index buffers, and indices into other tags.

Each correction goes to `debug.txt`. The game's own maps need none.
`build/linux/map_validate [--strict] map.map...` runs the same checks on map
files without the game, and `tools/test_linux_port.py` runs it on the maps
in `assets/maps`. `map_validate --fuzz <runs> map.map` changes a few words
of the tags at random in each run. The checks must not crash or hang, and a
map that they let through must need no more corrections.

A map's scripts can call only the script functions that a map needs (the
allowlist in `hs/hs.c`). They cannot call the functions for files, the
saved state of the game, the console, debugging or cheats. A script that
calls one does not run. The developer console can call every function.

Halo Custom Edition maps get the same checks (those that need OpenSauce are
refused). Their own loader (`game/cache_file_formats.c`) reads them into
their tag cache at 0x40440000 and converts what Custom Edition lays out
differently, then the validator checks their tags and each of their BSPs as
it checks this build's maps, before the game converts their models, BSP
geometry and scripts. Put them with `bitmaps.map`, `sounds.map` and
`loc.map` in `custom_maps`, beside `maps`, or set `paths.custom_edition` to
a Custom Edition install; the map lists show them as CUSTOM SINGLEPLAYER and
CUSTOM MULTIPLAYER, played as campaign levels (alone, or as network co-op)
or as multiplayer maps by their scenario type, and `game.custom_edition =
false` refuses them. `map_validate` checks them too, with the resource maps
beside each map or in `--maps <folder>`. See
`docs/custom_edition_caches.md`.

Defensive checks stay in the game code too. An index into a tag block, the
tags or a tag's data that is out of range gets zeros (`tag_empty_data` in
`tag_files/tag_groups.c`), not other memory.

A map's name must be its file's: the cache file slots are found by the name
in the map's header, so a map file whose header names another map (a
renamed one) is refused, not copied again for ever. The `loading.tga` a map
pack may put in the maps folder is read only if it is an uncompressed 24-bit
picture of 320 by 240, the loading screen's texture.

A checkpoint (`savegame.bin`, in the profile's folder) and a core are
images of the game state's memory: with the data arrays' pointers to their
elements, the objects' memory pool's blocks and the references to them, and
the caches' procedures. Before one is taken, each of those is checked
against what the game made at startup (`game_state_image_accept` in
`saved games/game_state.c`): an image that does not match (a damaged or
crafted file) is refused, and the level starts over.

## What operates

| Area | Status |
| --- | --- |
| Game code | All 466 C files of the game. The changes are in "Game source changes". |
| Graphics | Direct3D 8 on OpenGL 4.5 core through SDL3 (`src/d3d8_gl.c`). The port translates the NV2A vertex shaders and register combiners to GLSL. It decodes all the Xbox texture formats. The vertex and index buffers come from a GL copy of the Xbox memory. |
| High-res HUD | The HUD is drawn from high-res assets: redraws at 8x the size of the maps' bitmaps (4x for the largest), in `port/assets/hud`. They cover the meters, counters, panels and their outlines, the motion sensor, reticles, waypoints and scopes, but no bitmap with English text. `tools/hud_assets.py` makes them from the SVG redraws (the weapons' reticles from those of the PC HUD sheet that match the Xbox's exactly, the sniper rifle's 2x and 10x set in Overpass), and the build embeds them in the executable. When the game uploads one of those bitmaps, `src/hud_hires.c` gives the high-res texture in its place, if the bitmap's pixels are those of the English maps: another language's maps keep their own. The game sizes and places the HUD from its tags as before. `display.high_res_hud = false` turns this off. |
| High-res text | The menus' and HUD's text is drawn with Overpass (`port/assets/fonts`, SIL Open Font License) in place of the maps' bitmap fonts, which are Interstate. `src/text_hires.c` rasterizes each glyph with stb_truetype (`port/third_party/stb`) at the resolution the game draws at, into an atlas that a placeholder bitmap of the game stands for. The game lays the text out from its font tags as before: each font is as tall as its tag's capitals, and narrowed where the tag's letters are narrower than the Xbox maps' (the fonts of a Custom Edition `loc.map`), so that its letters do not run into each other. The menus' titles (the screens' headers and the main menu's items) are pictures of text in the maps, so they are drawn as the high-res HUD is: `tools/title_assets.py` sets each one again in OpenCE, Roger White's public-domain Newtown respaced to match the maps' commercial title typeface (`tools/title_font.py`), at 4x the bitmap's size over its own plate or glow, each letter placed where the map's letter is, in `port/assets/titles`. The postgame carnage report's title is set over a hand-made SVG redraw of its panel (`port/assets/titles/svg`) instead. The A, B, X and Y button icons beside the menus' key labels and in their text ("Press A to Join") are drawn from hand-made SVG redraws at 8x the size of the maps' bitmaps (`port/assets/buttons`, made by `tools/button_assets.py`), in the same sizes and places. `display.high_res_text = false` turns it off. |
| Anti-aliasing | Off unless `display.anti_aliasing` is set (`src/d3d8_gl.c`, `src/xgpu_post.c`). FXAA (written in the port) and SMAA (`port/third_party/smaa`, MIT licensed, at its HIGH preset, compiled as GLSL) are passes over the 3D view of each window, after the lens flares and before the HUD and the menus (`render/render.c`). Their programs are built when the setting is chosen. Supersampling draws the render targets the size of the screen at two times the resolution in each direction, and the display blit scales them down. Multisampling draws the back buffer and its depth buffer into multisampled renderbuffers, and with them any target that is drawn together with one of them (a mirror's view, in the secondary target with the back buffer's depth buffer), so that the attachments of a framebuffer are all multisampled or none is. A target's pixels are resolved into its texture before something reads the texture (as a texture, or at the display blit). Visibility tests count samples, divided by the samples of a pixel. An alpha-tested surface (foliage, grates) covers the samples of a pixel in proportion to its alpha past the reference (`gl_SampleMask`, not on Android). |
| Sound | Xbox DirectSound on SDL3 audio (`src/dsound_sdl.c`): PCM and Xbox ADPCM, mixed at 48 kHz, with volume, pitch, mix bins, distance, stereo pan, occlusion and obstruction. There is no Doppler effect, no cones and no reverb. |
| Input | XInput on SDL3 (`src/xinput_sdl.c`): keyboard, mouse, gamepads with rumble, and the debug keyboard for the console. |
| Files | The Win32 file functions and the MSVC file functions on POSIX, with the translation of Xbox paths. |
| Threads | Threads, events, mutexes, critical sections, interlocked operations and alertable waits. |
| Memory | The port reserves the Xbox memory at `0x80000000`. Thus the game gets the fixed addresses that it expects. |
| Saved games | The Xbox `UDATA` layout, with SHA-1 signatures. |
| Networking | Winsock on BSD sockets. System link on a local network and on the internet. |
| Bink video | Not available. The game skips the movies. |

## How the port operates

### The compiler

`tools/linux_build.py` compiles the game with clang and these options, which
give the ABI of the MSVC compiler:

- `--target=i686-linux-gnu`: 32-bit x86.
- `-fms-extensions`: the MSVC extensions.
- `-fshort-wchar`: 16-bit `wchar_t`.
- `-malign-double`: 8-byte alignment of 64-bit members.
- `-fcommon`: tentative definitions, as in C89.

glibc gives only ISO C (`__STRICT_ANSI__`). Thus POSIX names, for example
`random`, do not conflict with the names of the game.

These files supply the MSVC functions that clang does not have:

| File | Contents |
| --- | --- |
| `include/halo_linux_prefix.h` | The first header of each file: the architecture macros of the SDK, MSVC `__inline`, SEH keywords, `__declspec(selectany)`. |
| `include/` | Headers that add MSVC names to the C runtime headers. |
| `port/include/xdk` | The Xbox SDK declarations. The compiler reads this folder after all the other folders. |
| `tools/linux_msvc_semantics.py` | Makes a header that declares each struct tag at file scope, as MSVC does. It also makes the header inline functions weak, as the COMDAT functions of MSVC. `game/msvc_comdat.c` gives one external copy of each. |
| `include/halo_linux_winsock_names.h` | Gives new names to the Winsock functions of the SDK. Thus they do not link to the glibc functions with the same names. |
| `include/halo_linux_source_fixups.h` | Declares the port's functions that the game's sources call. |

`tools/linux_link_check.py` stops the link if a weak reference has no
definition. Without this check, the linker gives the reference the address
0.

### The platform layer (`src/`)

- The files `posix_*.c` use glibc. The compiler uses the ABI of the host
  for these files, because some glibc structures have a different layout
  with `-malign-double`.
- The other files include the SDK declarations through `platform.h`. Thus
  the compiler examines each definition against the SDK prototype.
- `src/halo_linker_common.c` gives weak storage for some globals of the
  January link, and for `fast_ftol_C` and `main_crash`.
- `main/d3d_intimacy.cpp` reads a private structure of the Xbox Direct3D.
  The Linux build does not use this file. `src/d3d8_gl.c` gives
  `d3d_find_flipcount`.
- The build returns small structures and unions in registers
  (`-freg-struct-return`), as on Win32.
- The GPU driver cannot open the kernel's `trace_marker`
  (`src/posix_trace_marker.c`). SteamOS keeps kernel tracing on for its GPU
  performance captures (`gpu-trace.service`), and its Mesa then writes a
  marker for each traced driver function: on the Steam Frame, some 480,000
  writes a second, which took the game from the headset's 72 Hz to about
  50 frames a second. The Steam Deck runs the same service, but its Mesa
  (25.3, 32-bit and 64-bit) has no markers to write: there the refusal
  changes nothing (measured: the same frame times and power either way).
  `HALO_GPU_TRACE_MARKERS=1` lets the driver write them, to capture with
  gpuvis.

### Game source changes

Five files of the game have changes for clang. These changes do not change
the MSVC objects: a comparison of all 612 C objects showed no difference in
code or data.

| File | Change |
| --- | --- |
| `cseries/cseries.c` | The naked function `stristr` uses `[ebp+8]` and `[ebp+12]` for its parameters. |
| `bitmaps/bitmap_drawing.c` | `*((word *)p)++` is now `*(*(word **)&p)++`. |
| `rasterizer/xbox/rasterizer_xbox_hardware_bitmaps.c` | `&(T *)x` is now `(T **)&x`. |
| `hs/hs.c` | Local prototypes that did not agree with `ai_script.h` are removed. |
| `units/vehicles.c` | The local prototype of `unit_update_animation` uses the type of `units.h`. |

`math/real_math.h` had a copy of `plane2d_from_points` that did not agree
with the function in `effects/decals.c`. clang used the copy, and parts of
levels were not visible. The copy now agrees with the function.

Other changes:

| File | Change |
| --- | --- |
| `scenario/scenario.c` | The BSP connection tables have names, not MSVC offsets. |
| `rasterizer/xbox/rasterizer_xbox_environment_fog.c` | A local pointer gets its value from the file-scope array with the same name. |
| `game/player_control.c` | The mouse aims the player on controller 1 directly. |
| `sound/game_sound.c` | The game calculates the obstruction of each sound one time for each tick, not for each frame. |
| `cseries/errors.c` | `debug.txt` stays open between lines. |
| `networking/`, `game/`, `interface/`, `bungie_net/network/` and the pools of objects, effects and sounds | The system link limits and the memory for them. |
| `game/`, `objects/`, `units/`, `networking/` | The distributed netcode. Refer to `NETCODE.md`. |
| `cache/cache_files.c` | When a map's tags load and unload, the port finds the bitmaps that the high-res HUD replaces (`game/hud_hires_tags.c`), and adds the tags of the menus to the menus' map (`game/menu_tags.c`). |
| `interface/ui_widget.c`, `interface/ui_widget_event_handler_functions.c`, `interface/ui_widget_game_data_input_functions.c` | The main menu is the PC version's from `port/assets/menus` (`display.menus`); the menus' widgets can call the port's functions (`game/menu_functions.c`) and send the PC version's custom activation event; the widgets' memory is 256 KB, not 16 KB; the main menu and Multiplayer clear co-op's controllers, so that a gamepad going back to controller 1 is not a controller unplugged; the lobby's split screen players leave alone, and one who quits in game is not joined to the next game (`interface/player_ui.c`). |
| `interface/player_ui.c` | Fast setup opens the pregame screen through the port's menus (the lobby), as `interface/ui_widget.c` does. |
| `input/input_abstraction.c`, `game/player_control.c`, `game/players.c`, `game/player_queues_new.c`, `units/units.h` | The keyboard and mouse's actions (`src/xinput_sdl.c`, `include/halo_keyboard.h`) join controller 1's game controls; their reload key reloads on its own, and their action key only acts (a control flag of the port's, sent with the player's action, stops the reload the controller's X falls back to). |
| `sound/sound_manager.c`, `interface/hud.c`, `game/game_engine.c` | The music's and the other sounds' volumes; the HUD's and the scoreboard's settings are read again when Settings changes them. |
| `rasterizer/xbox/rasterizer_xbox_shadows.c` | With larger shadow maps (`display.shadow_resolution`), the blur's taps stay half a texel apart, and more passes widen it to cover the same part of the map as on the Xbox. |
| `rasterizer/rasterizer.c` | The 128 KB of vertices of the debug display of a model's vertices are static, not on the stack: the compiler inlines the function in `render_model`, and the Windows build probed the stack a page at a time for every model drawn. |
| `render/render.c` | The 3D view of each window is antialiased before the HUD is drawn (`display.anti_aliasing`). |
| `interface/hud.c` | In multiplayer, players' names are drawn above their heads (`display.player_names`, `display.player_name_scale`). |
| `rasterizer/rasterizer_text.c`, `text/draw_string.c` | Text is drawn from an atlas of the fonts' glyphs, rasterized at the resolution the game draws at (`src/text_hires.c`), when the font has every character of the string. Text can be drawn scaled about a point (`rasterizer_text_set_scale`), as the players' names are. Each glyph's advance is centred on the font tag character's, so the layout is the same, and a glyph is cut at a text box only where the font tag's character visibly was. |
| `interface/first_person_weapons.c` | The first-person hands are drawn with the unit's shield modifier shader, as its body is (`display.viewmodel_shield`). |
| `render/render_objects.c`, `objects/objects.c` | In first person, your own body is drawn from the waist down with the pose `first_person_legs.c` makes (`display.first_person_legs`); the waist it found and the mesh `object_mesh.c` read are forgotten on a new map. |
| `objects/object_lights.c` | Glows the port lights itself (`shield_glow.c`): dynamic lights attached to an object's node, colored and sized each frame by their maker, drawn with a light definition the map has. |
| `render/render_objects.c`, `interface/first_person_weapons.c`, `rasterizer/xbox/rasterizer_xbox_plasma_energy.c` | A local player's shield plasma is drawn in the chosen armor color (`display.shield_color`, `shield_color.c`), on the body and on the first-person arms. |
| `rasterizer/xbox/rasterizer_xbox_text.c`, `rasterizer/rasterizer_text.c` | The characters of a string are one draw, not a draw each (and a second for the drop shadow). The batch is drawn early before a character is cached and before the high-res glyph atlas starts over (`src/text_hires.c`). |
| `rasterizer/xbox/rasterizer_xbox_decals.c` | Decals drawn one after another with the same blend function and bitmap are one draw, with each decal's color in its vertices, not a draw each: a long fight leaves hundreds in view. Within a run of decals whose blend functions commute (multiply, add, min, max, reverse subtract), they are drawn by bitmap, so more of them share a draw. |
| `rasterizer/xbox/rasterizer_xbox_widgets.c`, `objects/widgets/light_volumes.c` | A light volume's sprites (a plasma bolt's glow, a couple of hundred) are one draw. |
| `rasterizer/rasterizer.h` | A frame holds 2048 transparent geometry groups, not 384 (`include/halo_port_capacity.h`): a co-op game with many added enemies asked for more, and the Xbox's pool drew the frame without the rest. |
| `structures/structure_render.c` | The structure's visible surfaces are put in order by a radix sort, not `qsort_4byte`: the same order. |
| `cache/cache_files.c`, `networking/network_client_manager.c`, `networking/network_server_manager.c`, `networking/network_server_message_handler.c`, `main/main.c`, `interface/interface.c` | Map sharing (`game/map_share.c`): a client that does not have the game's Custom Edition map asks the host for it instead of leaving, is not given time to precache it until it has it, and adds its players to a game in progress only then; the host names the game's map for its hash, takes the requests from any machine in the game, lobby too, and does not drop a machine that downloads; the transfers run each frame, and their progress is drawn over the screen. |
| `cutscene/cinematics.c` | A scenario's titles (some Custom Edition maps show their name for the whole game) are not drawn over a menu, as the letterbox is not: the screen's overlays are drawn after the players' menus. |
| `tag_files/tag_groups.h` | `TAG_BLOCK_GET_ELEMENT` returns an element in the block without a call, and calls `tag_block_get_element_with_size` for anything else. |
| `input/input_xbox.c`, `physics/collision_usage.c` | The clock is read only for a button held, and for collision tests only when their times are shown: a read of the clock is slow in a browser. |

The x86 inline assembly of the game is replaced by C. Thus the compiler
can optimize that code for each processor:

| File | Assembly | Replacement |
| --- | --- | --- |
| `cseries/cseries.h` | x87 `fistp` (`fast_ftol`) | `__builtin_rint` |
| `bitmaps/bitmaps_inlines.h` | x87 conversions | C conversions |
| `math/matrix_math.c` | SSE `matrix4x3_multiply` | a C loop |
| `effects/decals.c` | an x87 conversion | a C conversion |
| `cseries/profile.c` | `rdtsc` | `QueryPerformanceCounter` |
| `cseries/cseries.c` | naked `stristr` | a C `stristr` |
| `cseries/stack_walk_windows.c` | a read of EBP | `__builtin_frame_address` |
| `interface/hud_draw.c` | a read of `[ebp+4]` | `__builtin_return_address(1)` |
| `bink/bink_playback.c` | `int 3` | `__builtin_trap` |

The x87 control and status words (`_control87`, `_statusfp`, `_clearfp` in
`src/msvc_crt.c`) use `fenv.h`. On Android, they use the FPCR and FPSR. In
the web build, the control word is only remembered.

The web build's changes (`HALO_WEB`: the main loop runs one iteration for
each of the browser's frames, the cache thread starts at a function of
`CreateThread`'s type, `debug.txt`'s lines go to the page's log) and the
declarations given their definitions' types, which WebAssembly needs, are
listed in "Game source changes" in [../web/README.md](../web/README.md).

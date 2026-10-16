# Diablo VR

**The original Diablo (1996), played in first person inside a VR headset.**

A work-in-progress VR mode for [DevilutionX](https://github.com/diasurgical/devilutionX), the open-source
Diablo engine. You stand in Tristram at human scale, walk down the cathedral steps yourself, swing your
sword with your own arm and draw a bow with both hands. Underneath it is still the real game: the same
levels, monsters, items, quests, combat rules and save files.

> **Status: prototype.** It is playable from the main menu to the dungeon, but it is an early build
> shared as-is. Expect rough edges. See [Known issues](#known-issues-and-limitations).

<p align="center">
  <img src="docs/vr/town.png" width="49%" alt="Tristram at night, outside the inn">
  <img src="docs/vr/church.png" width="49%" alt="The cathedral above Tristram">
</p>
<p align="center">
  <img src="docs/vr/cathedral.png" width="49%" alt="A skeleton in the cathedral">
  <img src="docs/vr/catacombs.png" width="49%" alt="A pack of monsters in the catacombs">
</p>
<p align="center">
  <img src="docs/vr/caves.png" width="49%" alt="Horned demons in the caves">
  <img src="docs/vr/hell.png" width="49%" alt="A knight in hell">
</p>

<sub>Screenshots are the left-eye image, taken straight from the build in this repository.</sub>

- **Videos and updates:** [youtube.com/@tombo2811](https://www.youtube.com/@tombo2811)
- **Contact:** [tsullivan2811@gmail.com](mailto:tsullivan2811@gmail.com)

---

## Contents

- [What this is](#what-this-is)
- [Features](#features)
- [Requirements](#requirements)
- [Getting it running](#getting-it-running)
  - [Option A: prebuilt download](#option-a-prebuilt-download)
  - [Option B: build from source](#option-b-build-from-source)
  - [Option C: Steam Frame (standalone)](#option-c-steam-frame-standalone)
  - [Game data](#game-data)
- [Controls](#controls)
  - [Saving](#saving)
- [Settings](#settings)
- [How it works](#how-it-works)
- [Repository layout](#repository-layout)
- [The `vrassets` folder](#the-vrassets-folder)
- [Developer tools](#developer-tools)
- [Troubleshooting](#troubleshooting)
- [Known issues and limitations](#known-issues-and-limitations)
- [Roadmap](#roadmap)
- [Credits](#credits)
- [License and legal](#license-and-legal)
- [Contact](#contact)

---

## What this is

Diablo was drawn as a fixed isometric picture. This project rebuilds that picture as a place you can
stand in, without replacing the game's art or touching its rules:

- The **level layout** comes from the game's own dungeon generator. Every wall, floor, door and
  staircase is turned into real geometry and wears the original tile art.
- **Monsters, townspeople, spells and effects** are the game's original sprites, placed in the world
  at their true size and always turned to face you.
- **Objects, items, doors, props and the town's buildings** are low-poly models built from the
  original pictures and painted with the original pixels.
- **Lighting** is the game's own: the same light radius, the same light tables, the same palette.
- **Game logic is untouched.** VR input is translated into the commands the mouse, keyboard and
  gamepad already send. Hit chance, damage, attack speed, AI and loot are exactly the original's.

The goal is the 1996 game shifted to first person, not a remake: dark, muted and low-resolution, the
way it looked, only now around you.

The VR mode is an optional build flag (`DEVILUTIONX_VR`) and a launch option (`--vr`). Without
`--vr` the executable is ordinary DevilutionX.

## Features

**World**

- Full first-person 3D view of Tristram and all sixteen dungeon levels (cathedral, catacombs, caves, hell)
- Real-scale world: one dungeon tile is 1.4 m, a character about 1.8 m tall
- Walls and floors wear the original tile art; walls stand as high as their painting, darkness above
- Built stairwells you can look down into, doors that swing on their hinges, lava and water that keep moving
- Tristram at night with its houses, cathedral, trees, fences, graves and well as models
- Fog and darkness at the edge of the game's light, like the original's black fringe

**Play**

- Free, smooth movement with the left stick (single player), snap or smooth turning
- **Melee by swinging** the right controller: the swing's direction picks the neighbouring monster
- **Bows drawn with two hands**: grab the string, pull back, let go
- **Spells cast from the left hand** at whatever it points to, or with a flick of it if you turn on
  spell gestures
- A **speedbook ring** around the left hand for choosing the readied spell
- Reach out and grab to open doors and chests, pull levers, touch shrines and pick up items
- **Potions on your belt**: grab one and bring it to your mouth to drink it
- Your equipped weapon and shield are in your hands
- Life and mana on your wrists, the belt at your waist
- Left-handed play: everything the right hand does can move to the left
- A crosshair that turns red when an attack will reach, a swing arc, hit marks, and floating names
  for nearby loot and the monster you aim at
- Haptics for swings, hits, blows you take, potions, picking things up, doors and chests, and the
  interface
- Positional sound: the head is the listener, sounds pan by direction and are muffled behind walls

**Interface**

- Menus, cutscenes and the title screen on a large floating screen
- Inventory, character sheet, spellbook, quest log, stores and dialogs on a panel in front of you,
  operated with a laser pointer that's drawn over everything. Only the interface shows, over the
  world, the way the desktop mirror shows it
- Point at an item, object or monster and its description shows on a card next to it
- The automap in your left hand, with a big arrow where you are
- Automap, potions and panels on controller buttons
- A VR Settings page in the game menu, so you can change things without taking the headset off
- An autosave slot after every level change, kept apart from your own save
- Standing or seated play

**Technical**

- OpenXR, so it runs on any PC VR runtime (SteamVR, Meta Quest Link, Virtual Desktop, and others)
- Direct3D 11 renderer written for VR: single-pass stereo, HDR forward lighting, 4x MSAA
- 90 Hz target, with the headset pacing the game's frames
- Falls back gracefully: if the `vrassets` folder is missing the game still runs and shows objects
  and town as flat original sprites
- Also runs on its own on the Valve Steam Frame, no PC needed: a Linux ARM64 build where the same
  Direct3D 11 renderer runs on Vulkan through dxvk-native (see
  [Option C](#option-c-steam-frame-standalone))

## Requirements

| | |
|---|---|
| **OS** | Windows 10 or 11, 64-bit |
| **Headset** | Any PC VR headset with an OpenXR runtime. Developed and tested on a Meta Quest 2 through SteamVR |
| **Controllers** | Oculus Touch layout (thumbsticks, A/B/X/Y, trigger and grip). Runtimes that remap other controllers onto the Touch profile also work. A minimal fallback exists for the generic OpenXR "simple controller" |
| **GPU** | Direct3D 11 capable. Any card that runs PC VR is far more than enough |
| **Game data** | `DIABDAT.MPQ` from a copy of Diablo that you own. **Not included** |

That's for PC VR. The Steam Frame build runs on the headset itself; what it needs is under
[Option C](#option-c-steam-frame-standalone).

## Getting it running

### Option A: prebuilt download

1. Download the latest zip from the [Releases](../../releases) page and extract it anywhere.
2. Copy `DIABDAT.MPQ` from your own copy of Diablo into the extracted folder, next to `devilutionx.exe`
   (see [Game data](#game-data)).
3. Start your VR runtime and connect the headset (SteamVR, Quest Link or Virtual Desktop).
4. Double-click **`play-vr.bat`**.

The title screen appears on a floating screen. Start or load a single-player game and you are in the world.

### Option B: build from source

You need:

- **Visual Studio 2022** with the *Desktop development with C++* workload (this includes a Windows SDK)
- **CMake 3.22 or newer** on your `PATH` (the one from the Visual Studio installer is fine, or use
  the *x64 Native Tools Command Prompt for VS 2022*)
- **Git**
- An internet connection for the first build: CMake downloads SDL2, the OpenXR loader and the other
  libraries and builds them from source. Nothing else has to be installed.

```bat
git clone https://github.com/tomahawk2811/diablo-vr.git
cd diablo-vr
build-vr.bat
```

`build-vr.bat` configures and builds a Release executable at `build\Release\devilutionx.exe`. The first
build takes roughly ten minutes; later builds only recompile what changed.

Then copy your `DIABDAT.MPQ` to `build\Release\` and run `play-vr.bat`.

If you would rather drive CMake yourself, the script is only this:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
  -DDEVILUTIONX_VR=ON -DBUILD_TESTING=OFF -DDISABLE_ZERO_TIER=ON -DDISCORD_INTEGRATION=OFF ^
  -DDEVILUTIONX_SYSTEM_SDL2=OFF -DDEVILUTIONX_SYSTEM_SDL_IMAGE=OFF -DDEVILUTIONX_SYSTEM_SDL_AUDIOLIB=OFF ^
  -DDEVILUTIONX_SYSTEM_ZLIB=OFF -DDEVILUTIONX_SYSTEM_BZIP2=OFF -DDEVILUTIONX_SYSTEM_LUA=OFF ^
  -DDEVILUTIONX_SYSTEM_LIBSODIUM=OFF -DDEVILUTIONX_SYSTEM_SHEENBIDI=OFF -DDEVILUTIONX_SYSTEM_MAGIC_ENUM=OFF
cmake --build build --config Release --parallel
xcopy /e /i /y build\assets build\Release\assets
```

The last line puts the engine's own assets (fonts, UI art, data tables) beside the executable, where
it looks for them. The `DEVILUTIONX_SYSTEM_*=OFF` switches tell the build to compile each library itself rather than look
for an installed one. `DEVILUTIONX_VR=ON` is the only switch that is new. It needs SDL2, and builds on Windows and, for
the Steam Frame, on Linux.

### Option C: Steam Frame (standalone)

The game runs natively on the Steam Frame as a Steam devkit title. There's no prebuilt download for
it yet. You build it in Docker on an arm64 machine (an Apple Silicon Mac works) and copy it over
SSH:

```sh
tools/steam_frame/build.sh image dxvk
VR=ON tools/steam_frame/build.sh game package
FRAME_HOST=<the Frame's name or address> tools/steam_frame/deploy.sh \
    --data /path/to/DIABDAT.MPQ --launch-args "--vr --verbose"
```

Then start **DiabloVR** from the Steam library on the Frame. It has to be started from Steam,
because that's what gives it the controllers. Set it to 72 Hz in SteamVR's per-application
settings. Settings go in `~/.local/share/DiabloVR/vr-settings.env` on the Frame, which starts out
with defaults that suit its GPU.

[docs/vr/steam-frame.md](docs/vr/steam-frame.md) has the details: how it works, the Frame's
controls and settings, and what's been measured and checked so far.

### Game data

Diablo's data is copyrighted and is not part of this repository. You need `DIABDAT.MPQ` from a copy
you own:

- **GOG:** [Diablo + Hellfire](https://www.gog.com/game/diablo). After installing, `DIABDAT.MPQ` is in
  the game's folder.
- **Original CD:** `DIABDAT.MPQ` is in the root of the disc.

Put it next to `devilutionx.exe` or in `%AppData%\diasurgical\devilution\`. Saves and settings also
live in that AppData folder and are shared with a normal DevilutionX install.

See the [DevilutionX installation guide](docs/installing.md) for more detail.

## Controls

The layout below uses Touch-style controller names.

### In the world

| Input | Action |
|---|---|
| **Left stick** | Walk in any direction, relative to where you look |
| **Right stick left / right** | Turn (45 degree snap by default, smooth turning optional) |
| **Right stick up / down** | Drink a health potion / a mana potion from the belt |
| **Swing the right hand** | Melee attack on the neighbouring monster the swing goes towards |
| **Right trigger** | Click on what the right hand points at: attack, talk, walk there, pick up. The beam turns gold on anything it picks up, opens or talks to |
| **Bow equipped: bring the right hand to the left, hold the trigger, pull back, release** | Shoot along the arrow |
| **Right grip** | Take hold of what is within reach: door, chest, lever, shrine, item |
| **Left trigger** | Cast the readied spell at what the left hand points at. With spell gestures on, hold it to aim and flick the hand to cast |
| **Left grip (hold)** | Speedbook ring around the left hand; choose with the left stick, release to set |
| **Either grip, by a potion on your belt** | Take the potion; bring it to your mouth to drink it, let go anywhere else to put it back |
| **Y** | Inventory |
| **X** | Character sheet |
| **A** | Primary action (attack / interact) |
| **B** | Secondary action |
| **Left stick click** | Automap |
| **Right stick click** | Bring the screen or panel back in front of you |
| **Both sticks clicked together** | Save a screenshot of the left eye |
| **Menu button (left controller)** | Game menu (Escape) |

### On the floating screen and panels

| Input | Action |
|---|---|
| **Right hand** | Laser pointer: it is the mouse. The beam is drawn over everything, with a dot where it lands |
| **Left hand** | Points too, where the right hand isn't on a screen or panel; its trigger clicks there |
| **Right trigger** | Left click |
| **Right grip** | Right click |
| **Left stick** | Menu navigation, as on a gamepad |
| **Right stick click** | Recenter the screen in front of you |
| **Menu button** | Back / Escape |

Everything not listed goes through DevilutionX's standard gamepad scheme, so its gamepad
documentation applies. With the dominant hand set to left, read left for right above for the
hand that points, swings and grabs, and right for left for the one that casts.

### On the Steam Frame

The Frame's controllers have every button in their usual place, so A, B, X and Y do what they do
above (they're all on the right controller). On top of that:

| Input | Action |
|---|---|
| **Bumpers** | Health and mana potion from the belt. The right stick no longer drinks potions on the Frame, so turning can't cost one |
| **D-pad** | The gamepad's D-pad in menus, the inventory and shops |
| **A / B in menus** | Confirm / back, in the main menus, the game menu, shops and dialogs. In play they're still the primary and secondary action |
| **View** | In play: the help screen, which starts with these controls, or closes whatever is open. In the main menus: back. Hold it for a second to recentre |
| **Menu** (right controller) | Game menu (Escape), where Save Game is |

The help screen also comes up by itself the first time you start a game in the headset.

On the Frame the HUD strip goes on your wrists (the HUD setting): life, the menu
buttons and belt slots 1 to 4 on the left wrist, mana, belt slots 5 to 8 and the spell buttons
on the right. Point at one wrist with the other hand and pull the trigger to press what's on it.

The automap (left stick click) is held in the left hand, with where you are on it just over the
hand and a big gold arrow there pointing where you look.

### Saving

Diablo only saves when you do: Menu, Save Game. On top of that there's an autosave slot, written
each time you change level (stairs, portals, back to town). It's a separate file next to your save
(`single_0_auto.sv` and so on) and never replaces it. Load it from the game menu (Load Autosave),
from the death screen, or when you pick your hero on the title screen. It's on by default; turn it
off under VR Settings.

## Settings

The everyday settings are in the game: Menu, Options, VR Settings, in the headset. They're in
three short pages:

| Page | Settings |
|---|---|
| Comfort | Turning (snap or smooth), snap angle, turn speed, vignette, seated |
| Interface | Dominant hand, haptics strength, HUD on the wrists or as a strip, see-through panels, autosave |
| Hands | Item cards, belt grab, spell gestures |

Choosing a line steps its value and it's saved right away in `diablo.ini`. The same settings are
under VR on the title screen's Settings too.

Everything else is an environment variable read at startup. Set them before launching, for example
in a copy of `play-vr.bat`:

```bat
set DEVILUTIONX_VR_EYE_HEIGHT=1.75
start "" devilutionx.exe --vr
```

The variables for the settings above (`DEVILUTIONX_VR_SMOOTH_TURN`, `_SNAP_TURN`, `_SEATED`, and so
on) still work, but only as the starting values: once `diablo.ini` has a VR section, the game
uses that.

### Comfort and movement

| Variable | Default | What it does |
|---|---|---|
| `DEVILUTIONX_VR_SMOOTH_TURN` | `0` | `1`: the right stick turns smoothly instead of in 45 degree steps |
| `DEVILUTIONX_VR_SMOOTH_TURN_SPEED` | `2.0` | Smooth turn speed, radians per second at full stick |
| `DEVILUTIONX_VR_VIGNETTE` | `0` | How dark the edge of view gets while moving or turning (`0` off, try `0.6`) |
| `DEVILUTIONX_VR_SEATED` | `0` | `1`: seated play, the world is raised so your eyes are at standing height |
| `DEVILUTIONX_VR_SEATED_RAISE` | `0.45` | How far the world is raised when seated, meters |
| `DEVILUTIONX_VR_EYE_HEIGHT` | `1.65` | Eye height used when the runtime has no floor-level tracking, meters |
| `DEVILUTIONX_VR_FREE_MOVE` | `1` | `0`: the character steps from tile to tile like the original, with the view riding on it |
| `DEVILUTIONX_VR_WALK_SPEED` | `2.5` | Walking speed, tiles per second at full stick |
| `DEVILUTIONX_VR_TOWN_PACE` | `1.5` | How much faster you move in town |
| `DEVILUTIONX_VR_SWING_SPEED` | `2.0` | Hand speed that counts as a weapon swing, meters per second |
| `DEVILUTIONX_VR_WEAPON_TILT` | `80` | Angle of the held weapon from the pointing direction, degrees |

### Picture

| Variable | Default | What it does |
|---|---|---|
| `DEVILUTIONX_VR_REFRESH_RATE` | `90` | Refresh rate to ask the headset for, when it offers it |
| `DEVILUTIONX_VR_MSAA` | `4` | Anti-aliasing samples (2 to 8) |
| `DEVILUTIONX_VR_EXPOSURE` | `1.0` | Overall brightness |
| `DEVILUTIONX_VR_AMBIENT` | `0.004` | Light where the game's light does not reach |
| `DEVILUTIONX_VR_VIEW_TILES` | `28` | How far you can see, in tiles |
| `DEVILUTIONX_VR_TILE_SIZE` | `1.4` | Meters per dungeon tile: the scale of the whole world |
| `DEVILUTIONX_VR_MONSTER_GLIDE` | `0.11` | How smoothly monsters move between tiles (`0`: exactly as the game moves them) |
| `DEVILUTIONX_VR_FEEDBACK` | `1` | `0`: no crosshair, swing arc, hit marks or name labels |
| `DEVILUTIONX_VR_LABEL_TILES` | `10` | How far away dropped items show their names, in tiles |
| `DEVILUTIONX_VR_CARVED` | `1` | `0`: ignore the models in `vrassets/carve` and draw flat sprites |

### Screen and panels

| Variable | Default | What it does |
|---|---|---|
| `DEVILUTIONX_VR_SCREEN_DISTANCE` | `2.0` | Distance of the menu screen, meters |
| `DEVILUTIONX_VR_SCREEN_WIDTH` | `2.6` | Width of the menu screen, meters |
| `DEVILUTIONX_VR_PANEL_DISTANCE` | `1.1` | Distance of the in-game panel, meters |
| `DEVILUTIONX_VR_PANEL_WIDTH` | `1.4` | Width of the in-game panel, meters |
| `DEVILUTIONX_VR_WRIST_HUD` | `0` | `1`: the HUD strip splits in two and goes on the wrists (the Steam Frame's default) |
| `DEVILUTIONX_VR_WRIST_WIDTH` | `0.17` | Width of each wrist half, meters |
| `DEVILUTIONX_VR_LASER_LAYER` | `1` | `0`: the laser is drawn in the world again, under the screen and panels, and not over the menus |

### Files

| Variable | What it does |
|---|---|
| `DEVILUTIONX_VR_ASSETS` | Path of the `vrassets` folder, if it is not next to or above the executable |
| `DEVILUTIONX_VR_SCREENSHOTS` | Folder screenshots are saved to (default: `screenshots` beside `vrassets`) |

### Performance

These are mostly for the Steam Frame, where its defaults come from, but they work everywhere.
Without them everything is drawn as before.

| Variable | Default | What it does |
|---|---|---|
| `DEVILUTIONX_VR_MSAA` | `4` | `1` turns anti-aliasing off, which on a mobile GPU is a lot cheaper |
| `DEVILUTIONX_VR_WALL_SHADOW_SPACING` | `0` | Tiles between the samples a pixel takes towards a light (`0`: always all 16) |
| `DEVILUTIONX_VR_SUN_SHADOW_TAPS` | `9` | Shadow map samples in town: `9`, `4` or `1` |
| `DEVILUTIONX_VR_DEPTH_PREPASS` | `0` | `1`: the level is drawn depth first, then shaded only where it's seen (needs MSAA off) |
| `DEVILUTIONX_VR_BLOOM` | `1` | `0`: no glow around bright light |
| `DEVILUTIONX_VR_EYE_SIZE` | `0` | Eye image width in pixels (`0`: what the headset recommends) |
| `DEVILUTIONX_VR_SNAP_TURN` | `45` | Degrees of a snap turn |

There are more, mostly for development; search the source for `DEVILUTIONX_VR_`, or see
[docs/vr/steam-frame.md](docs/vr/steam-frame.md) for the ones added with the Steam Frame build.

## How it works

DevilutionX keeps running exactly as it does on a monitor: it simulates the game and paints its usual
8-bit 2D frame. The VR code hooks in at three points.

1. **Presenting the frame** (`VrPresent`, called from the engine's `RenderPresent`). In menus and
   cutscenes the finished 2D frame is copied to an OpenXR quad layer: the floating screen. In a game,
   the engine is told not to paint the isometric world into that frame at all, so it only carries
   panels, text and the automap, and it shrinks to a panel that appears while something is open.
2. **Drawing the world.** Each headset frame, a bridge layer reads the live game state (the dungeon
   grid, objects, items, monsters, players, missiles, light map) and fills a plain scene description
   with meshes, sprites and lights. A renderer that knows nothing about Diablo draws that scene for
   both eyes.
3. **Input.** Controller poses and buttons become the mouse events, key presses and virtual gamepad
   input the game already understands, or the same network commands a click would send.

A few small callbacks were added to the engine so the renderer can tell which file a sprite was
loaded from and whether a level is currently loading. Apart from those hooks the engine is unchanged.

### The level

The dungeon generator's output is a grid of tiles, each made of small pieces of art. For every piece
the bridge works out whether it is floor, a wall face along one of the two grid directions, an arch,
a door or a standing prop, cuts the wall faces out of the tile art and stands them upright at the
right place. Floors are laid flat. The level mesh is built once when a level loads and updated when
doors open or the level changes.

### Sprites and palette lighting

Sprites are kept exactly as the game stores them: 8-bit palette indices. They are packed into an
atlas of index plus coverage on first use. Colour is looked up in the shader through the game's
palette and its light tables, so a monster in the dark shades through the same colours it would on
a monitor, and palette cycling (lava, water) works for free. Creatures are given a slight relief so
they are not paper-thin up close.

### Models

Anything that does not move on its own is a model: objects, dropped and held items, door leaves,
tile-set props and the town. Models are loaded from `vrassets` (see below). Each is a low-poly shape
following the original picture, textured with that picture's own pixels. The parts of an object's
animation that move, such as a torch flame, stay sprites drawn over the model.

### Session and frame loop

`xr_session.cpp` finds a usable OpenXR runtime (it walks every runtime registered on the machine if
the active one has no headset), creates the session and swapchains and asks for the refresh rate.
`xr_frame.cpp` runs the wait / begin / end frame loop and composes the layers: the 3D world as a
projection layer with depth, and the 2D screen or panel as a quad layer. While the headset is worn
the desktop window is only a mirror.

## Repository layout

Everything specific to VR lives in one folder. The rest is upstream DevilutionX.

```
Source/platform/vr/
  vr.h, vr.cpp            Public entry points, per-frame presentation, the floating screen
  vr_internal.h           Shared declarations and screen/panel tuning values
  xr_d3d11.h              The OpenXR calls that depend on the graphics binding
  xr_d3d11_win32.cpp      ... with Direct3D 11 on Windows
  dxvk/                   ... with DXVK's Vulkan device on Linux, plus D3DCompile on vkd3d
  knob.h                  Environment-variable tuning values
  xr_session.cpp          OpenXR instance, runtime selection, session, swapchains
  xr_frame.cpp            Frame loop and layer composition
  xr_input.cpp            Controllers: movement, swings, bow, spells, grabbing, panels, haptics
  xr_laser.cpp            The laser as layers over everything else
  xr_haptics.cpp          What the hands feel, from what the game does
  vr_menu.cpp             VR Settings in the game menu
  input_rules.hpp         The input's decisions, kept testable (test/vr_input_rules_test.cpp)
  render/
    scene.h               Plain scene description handed from the game side to the renderer
    renderer.cpp/.h       Direct3D 11 renderer: stereo, lighting, fog, MSAA, tone mapping
    shaders.cpp           HLSL source
    gpu.cpp/.h            Device helpers: textures, buffers, shader compilation
    mesh.cpp/.h, math.h   Mesh building and vector maths
    bridge.h              Interface of everything that reads game state
    level.cpp             Dungeon and town geometry from the game's tiles
    actors.cpp            Monsters, players, towners, objects, items, held weapons, doors
    particles.cpp         Missiles, spells and effects
    sprites.cpp           Sprite atlas of palette indices
    palette.cpp           Palette and light tables as textures
    carved.cpp            Loader for the `.crv` models in vrassets/carve
    models.cpp, gltf.cpp  glTF loader, town buildings
    feedback.cpp          Crosshair, swing arc, hit marks, name labels
    debug_panel.cpp       In-world performance panel
    world.cpp/.h          Ties the above together: init, per-frame update, sound position
  debug/
    mirror.cpp            Desktop mirror and screenshots
    offscreen.cpp         Headset-free captures and the benchmark
    profile.cpp/.h        Frame timing

vrassets/                 Models used by the VR renderer (see below)
docs/vr/                  Screenshots for this page, and steam-frame.md
build-vr.bat              Configure and build
play-vr.bat               Launch in the headset
tools/steam_frame/        Steam Frame build: Docker image, build, package, deploy, launcher, tests
```

Engine files with small VR hooks: `CMakeLists.txt`, `Source/CMakeLists.txt`, `Source/diablo.cpp`,
`Source/init.cpp`, `Source/interfac.cpp/.h`, `Source/engine/dx.cpp`, `Source/engine/load_cel.cpp/.hpp`,
`Source/engine/load_cl2.cpp`, `Source/engine/render/scrollrt.cpp`, `Source/engine/sound_position.cpp`.
Every change is inside `#ifdef DEVILUTIONX_VR` or is an inert callback.

## The `vrassets` folder

The game looks for a folder named `vrassets` containing `vrassets.txt` next to the executable and up
to four folders above it (or wherever `DEVILUTIONX_VR_ASSETS` points). It is optional: without it
everything that would be a model is drawn as its flat original sprite.

```
vrassets/
  vrassets.txt            Marker file
  carve/
    objects/              Dungeon objects: barrels, chests, sarcophagi, bookcases, torches, ...
    icons/                Items, from their inventory pictures: held in the hand and lying on the floor
    doors/                Door leaves of the cathedral, catacombs and caves
    tiles/l1..l4/         Standing props of each tile set: pillars, tombs, stalagmites, ...
    town/                 Tristram's trees, fences, rocks, graves and other standing things
  models/
    houses/               Tristram's buildings and the cathedral, as glTF binary (.glb)
```

`.crv` is this project's own small binary format: a header, then per record a triangle mesh and the
texture it wears, with coordinates in art pixels so a model lines up with the sprite it replaces.
`carved.cpp` is the complete reader. The building models are standard `.glb` files.

## Developer tools

All of these work without a headset.

| | |
|---|---|
| `DEVILUTIONX_VR_OFFSCREEN=<folder>` | Renders eight views from the player's position to `<folder>` as BMP files and writes a `done` marker. With `DEVILUTIONX_VR_AUTOSTART=1` the game loads the first single-player save by itself |
| `DEVILUTIONX_VR_DEBUG_LEVEL=<n>` | Go straight to dungeon level `n` for a capture |
| `DEVILUTIONX_VR_DEBUG_TILE_X` / `_Y` / `DEVILUTIONX_VR_DEBUG_PITCH` | Put the capture camera on a given tile |
| `--vr-bench <level>` | Fixed camera path through a level, reporting frame times |
| `DEVILUTIONX_VR_PERF=1` | Performance panel floating in the world |
| `DEVILUTIONX_VR_MIRROR=1` | Writes the head view to `vr-view.bmp` next to the executable every two seconds |
| `DEVILUTIONX_VR_DEBUG_VIEW=1..6` | Renderer debug views |
| `DEVILUTIONX_VR_SHADERS=<file>` | Load the HLSL from a file and reload it when it changes |
| `DEVILUTIONX_VR_DEBUG_MAP=<file>` | Write the current level as text with its object list |
| `DEVILUTIONX_VR_NO_SINGLE_PASS=1`, `DEVILUTIONX_VR_NO_EXTENSIONS=1` | Force the fallback paths |

Run with `--verbose --log-to-file vr.log` (as `play-vr.bat` does) to see what the VR code found and
loaded; its lines start with `VR:`.

## Troubleshooting

**The game opens on the desktop but nothing shows in the headset.**
Make sure the VR runtime is running and the headset is connected *before* or shortly after starting.
Open `vr.log` and look for the `VR:` lines: they name the runtime that was tried and why it failed.

**"DIABDAT.MPQ was not found" or the game exits at once.**
Copy `DIABDAT.MPQ` next to `devilutionx.exe`. If the folder also holds a `devilutionx.mpq` left over
from a different DevilutionX version, remove it: mismatched assets can crash the game at startup.

**The world is at the wrong height.**
Reset your floor level in the runtime. If you play seated, set `DEVILUTIONX_VR_SEATED=1`.

**The screen or panel is behind me.**
Click the right stick to bring it back in front of you.

**Objects and town buildings are flat cut-outs.**
The `vrassets` folder was not found. Keep it next to the executable, or set `DEVILUTIONX_VR_ASSETS`.

**Movement makes me queasy.**
Try `DEVILUTIONX_VR_VIGNETTE=0.6`, keep snap turning, or set `DEVILUTIONX_VR_FREE_MOVE=0` for
tile-by-tile steps.

**The build fails while configuring.**
Delete the `build` folder and run `build-vr.bat` again. The first run needs internet access.

## Known issues and limitations

This is a prototype. Honest list:

- **Windows, plus the Steam Frame.** The Frame build is Linux ARM64 through dxvk-native. Other
  Linux setups and a standalone Quest build aren't there. The Frame build has been played in the
  headset, but the newest additions (VR Settings, haptics, belt grab, item cards, spell gestures,
  the autosave) were checked without anyone wearing it and still need trying properly.
- **Tested on one setup** (Quest 2 through SteamVR). Other headsets should work through OpenXR but
  have not been tried. Only Touch-style and Steam Frame controller bindings exist; Index, Vive wand and WMR
  controllers depend on the runtime remapping them.
- **Single player is the focus.** Free movement is switched off in multiplayer, where the character
  steps tile to tile. Multiplayer in VR is untested, and the build script leaves ZeroTier out.
- **Hellfire is untested**, and the crypt and hive have no models: their objects show as flat sprites.
- **Creatures are flat sprites.** They face you and have a little relief, but they are 2D art from
  eight directions, as in the original.
- **Some models are rough.** Thin things (racks, banners, levers, crucifixes) are thin from the side,
  the opened sarcophagus is a simple box, the town well and a couple of boulders have wrong colours,
  and many hell pillars are a plain extruded outline.
- **Not everything has been checked in the headset.** Individual objects, dropped items and door
  leaves were verified on captured images more than in VR.
- **The VR Settings page is short.** Turning, comfort, hands and the interface are there; the
  rest (picture, performance, screen sizes) are still environment variables.
- Runtime selection overrides a security check in the OpenXR loader so the game still finds the
  headset when started as administrator. Run it as a normal user unless you have a reason not to.

## Roadmap

Rough order, no promises:

- Monsters that catch the light from the side it comes from, with contact shadows
- Bindings for Index, Vive and WMR controllers
- Models for the remaining objects and a second pass on the weak ones
- Hellfire's crypt and hive
- Multiplayer with free movement
- More comfort options (teleport movement, height calibration)

## Credits

- **[DevilutionX](https://github.com/diasurgical/devilutionX)** and its contributors, and the
  **[Devilution](https://github.com/diasurgical/devilution)** reverse-engineering project before it.
  This repository is a fork of DevilutionX and none of it would exist without their work.
- **Blizzard North** for Diablo.
- **[OpenXR](https://www.khronos.org/openxr/)** by the Khronos Group, and the
  [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) loader.
- **[SDL](https://www.libsdl.org/)** and the other libraries DevilutionX builds on (see `3rdParty/`).
- **[Blender](https://www.blender.org/)**, used to build the models.
- Drelvak's video *I Made Diablo First Person*, the inspiration for the plain wall and floor look.

## License and legal

The source code is distributed under the same terms as DevilutionX: the
[Sustainable Use License](LICENSE.md). In short, it is free for personal and non-commercial use and
may not be sold. Third-party libraries keep their own licenses (see `3rdParty/`).

Diablo® is a registered trademark of Blizzard Entertainment, Inc. This is an unofficial fan project.
It is not affiliated with, endorsed by or supported by Blizzard Entertainment.

**No game data is included.** You need `DIABDAT.MPQ` from a copy of Diablo that you own to play. The
models in `vrassets` are built from the original game's art; that art remains the property of
Blizzard Entertainment and is included only so the mod works for people who own the game. If you are
a rights holder and want something removed, write to the address below and it will be taken down.

This project is free and will stay free.

## Contact

- **Email:** [tsullivan2811@gmail.com](mailto:tsullivan2811@gmail.com)
- **YouTube:** [youtube.com/@tombo2811](https://www.youtube.com/@tombo2811)

Bug reports are welcome through the repository's [Issues](../../issues) page. Please attach `vr.log`
and say which headset and runtime you use.

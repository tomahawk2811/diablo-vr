# Steam Frame (standalone)

The VR mode also runs on the Valve Steam Frame by itself, no PC: a native Linux ARM64 build that
Steam starts as a devkit title. The game and the VR mode are the same as on Windows. What's
different is underneath:

- The Direct3D 11 renderer is kept. [dxvk-native](https://github.com/doitsujin/dxvk) turns its
  D3D11 calls into Vulkan, and [vkd3d](https://gitlab.winehq.org/wine/vkd3d)'s HLSL compiler sits
  behind `D3DCompile`, so the shaders still compile at start-up from the same source.
- SteamVR on the Frame has no D3D11 OpenXR binding, only Vulkan. `xr_d3d11.h` holds the handful of
  OpenXR calls that depend on the binding, in D3D11 terms. On Windows they pass straight through.
  On Linux they hand the runtime DXVK's own Vulkan device and wrap the runtime's swapchain images as
  D3D11 textures, the way Proton's wineopenxr does it for Windows games.
- The Frame's GPU is a phone's, so there are settings that trade a little of the PC look for frame
  rate, and the Frame's controllers get their own bindings.

I kept the renderer instead of rewriting it for Vulkan so that the diff stays small and later
renderer changes carry over.

## What you need

- A Steam Frame with SSH access as `steamos` (developer mode, your key installed) and the devkit
  tools Steam puts in `~/devkit-utils` on it.
- A machine with Docker that builds arm64 natively. An Apple Silicon Mac with Colima works. The
  build image is Ubuntu 24.04 with gcc 13 and the pinned dxvk and vkd3d.
- Your own `DIABDAT.MPQ`.

## Build

```sh
tools/steam_frame/build.sh image dxvk          # the build image, then dxvk-native (once)
VR=ON tools/steam_frame/build.sh game package   # the game, then build-frame/package/DiabloVR
```

`package` checks the result: aarch64, glibc 2.39 and GLIBCXX 3.4.32 at most (the Frame has glibc
2.39, and 2.41 inside Steam Linux Runtime 4), every library either in the package or a base system
one, and no game data, saves or Vulkan drivers in it.

## Install and run

```sh
FRAME_HOST=<the Frame's name or address> tools/steam_frame/deploy.sh \
    --data /path/to/DIABDAT.MPQ --launch-args "--vr --verbose"
```

That copies the package to `~/devkit-game/DiabloVR`, checks every file's hash on the Frame, puts
`DIABDAT.MPQ` and the default settings in `~/.local/share/DiabloVR`, and registers the title
`DiabloVR` with Steam. Start it from the Steam library. It has to be from Steam: a game started
over SSH gets no controllers. Saves, `diablo.ini` and `run.log` go in the same data folder.

Set the title to 72 Hz in SteamVR's per-application settings. That's what it holds everywhere
with room to spare (see below).

## Settings

The VR mode's settings are still environment variables. On the Frame the launcher reads them from
`vr-settings.env` in the data folder, one `NAME=value` a line. `deploy.sh` installs
`tools/steam_frame/vr-settings.env` there once and leaves it alone after that. `launch-once.env`
and `launch-once.args` in the same folder apply to one launch and are deleted before the game
starts.

New settings, which work on Windows too. Without them everything behaves as before:

| Setting | What it does | Frame default |
| --- | --- | --- |
| `DEVILUTIONX_VR_MSAA=1` | No MSAA: the scene goes straight into the post-processing source | 1 |
| `DEVILUTIONX_VR_WALL_SHADOW_STEPS` / `_SPACING` | Samples a pixel takes towards a light: at most this many (16), and one per this many tiles of distance (0: always all of them) | spacing 0.4 |
| `DEVILUTIONX_VR_SUN_SHADOW_TAPS` | 9, 4 or 1 filtered shadow map comparisons in town (9) | 4 |
| `DEVILUTIONX_VR_NEAREST_FIRST` | Solid things drawn nearest first, carved models before the level. Same picture (1) | 1 |
| `DEVILUTIONX_VR_DEPTH_PREPASS` | Level drawn depth first, then shaded where it's seen. Without MSAA only (0) | 1 |
| `DEVILUTIONX_VR_BLOOM=0` | No bloom passes | on |
| `DEVILUTIONX_VR_DEPTH_LAYER=0` | No depth handed to the compositor | on |
| `DEVILUTIONX_VR_ANISOTROPY` | Of the wall and floor art (8) | 8 |
| `DEVILUTIONX_VR_DESKTOP_WINDOW=0` | The desktop window gets one frame a second while the headset session runs | 0 |
| `DEVILUTIONX_VR_SNAP_TURN` | Degrees of a snap turn (45) | 30 |
| `DEVILUTIONX_VR_EYE_SIZE` | Eye image width in pixels (0: what the runtime recommends) | 0 |
| `DEVILUTIONX_VR_FOVEATION` | `lo`, `med` or `hi`: the Frame's own eye-tracked foveation (launcher only) | off |

Levels with no materials, floor map or grit, which is the default look, are now always drawn with
a variant of the surface shader that leaves those lookups out. It gives the same picture, so it
has no switch.

For foveation, the Frame ships a Vulkan layer, `VK_LAYER_VALVE_fdm_injection`, that adds a
fragment density map to render passes the size of an OpenXR swapchain and follows the eyes. With
`DEVILUTIONX_VR_FOVEATION` set, the launcher turns the layer on, has the renderer draw one eye per
pass (the layer's map has one layer), and leaves the driver on its tiled path, which the map
needs. It halves the cost of the scene and makes 90 Hz at 2160x2160 possible. It's off because
nobody has judged it in the headset yet.

## Controls on the Frame

The Frame's controllers are a gamepad split over two hands, and the VR mode already feeds the game
a virtual gamepad, so the Frame bindings are the Touch ones with these differences:

| Input | What it does |
| --- | --- |
| A, B, X, Y (all on the right hand) | The same as those letters on Touch |
| Menu (right) | Game menu |
| View (left) | The gamepad's Select. Held for a second: re-centre, with a buzz in both hands |
| Bumpers | Health and mana potions, as on a gamepad. On the Frame the right stick no longer drinks potions, so turning can't cost one |
| D-pad | The gamepad's D-pad wherever the game shows its own interface (menus, inventory, shops). Nothing out in the world |

The bindings are only suggested when the runtime has `XR_VALVE_frame_controller_interaction`.
`tools/steam_frame/tests/check_bindings.py` checks every suggested path against its profile.

## Other changes that apply everywhere

- When the headset session loses focus after having had it (headset off,
  system menu open), the game pauses with its own window-focus pause, sound included, and resumes
  when focus is back. It follows the existing "Pause Game When Window Loses Focus" option. While a
  session runs, the desktop window's focus events are ignored, since they say nothing about the
  person in the headset.
- Walking, turning and the scene's clock (particles, monsters gliding between
  tiles) now step by the OpenXR display time. They used `SDL_GetTicks`, whose whole milliseconds
  make every step at 72 Hz either 13 or 14 ms long.
- Doors swing open again. The swing stepped by the time since the last update, read after that
  time had been reset, so it never moved, and a door opened in front of you stayed shut.
  `DEVILUTIONX_VR_DEBUG_OBJECT=chest`, `sarc` or `door` has the headset-free renderer look at a
  level's first one and open it 200 frames in.
- The walls on either side of a built doorway no longer wear their own pictures, which have the
  other halves of the painted arch on them and showed as dark half-arches beside the real one.
  They get the plainest of the level's common wall faces. The cathedral has none without an arch,
  since its walls are painted as a blind arcade, so there that face is made from the most common
  one: below its ledge every row repeats the two brick courses under it.
  `DEVILUTIONX_VR_DEBUG_WALLS=<folder>` saves every wall and floor texture of a level there, and
  `DEVILUTIONX_VR_DEBUG_OBJECT=doorway` with `DEVILUTIONX_VR_DEBUG_DOORWAY=<n>` has the
  headset-free renderer look straight at a level's n-th built doorway. Checked that way: the
  cathedral (level 2), the catacombs (6) and the caves (10). Hell (14) has no built doorways.
- Hell's walls. The art has two kinds: rib cages of bone, which are walls, and blocks of red flesh
  drawn as a box (its top and two front sides) with a horned bone ridge along the top's back edges.
  The blocks were taken as walls too, stretching sides, top and ridge up one 2.9 m face, and since
  the most common wall face is the red top of a block (inside a mass of wall, where no side shows),
  faces with no art of their own were red as well. Now the blocks are built from their sides,
  as high as the top stands in the art, with the ridge as a cut-out on top
  (`DEVILUTIONX_VR_HELL_BLOCKS=0` goes back), and only faces towards open ground count when the
  common face is chosen. Art found only inside masses of wall is never put on a side. 
- Many wall faces in the cathedral and hell are half a pattern: half an arch, half a rib cage.
  Where the art pairs two pieces, neighbours make the whole; where one half repeats, they didn't.
  Along each run of faces, a face is now flipped left to right when that makes its edge meet the
  previous face's edge clearly better (by brightness down the shared edge), so arches carry on
  across the seams. Hell's rib cages alternate, every other face flipped, in mirrored pairs
  (`DEVILUTIONX_VR_FLIP_SEAMS=0` turns both off).
- For testing without anyone in the headset, `DEVILUTIONX_VR_FORCE_RENDER=1` draws
  the world from a made-up head (`DEVILUTIONX_VR_DIAG_YAW` turns it). `DEVILUTIONX_VR_AUTOSTART`
  also types a hero name, so it gets into a game from a fresh install, and
  `DEVILUTIONX_VR_DEBUG_LEVEL` works in a real session too. `DEVILUTIONX_VR_DUMP` saves both eyes.
  With `--verbose` there's a `VR: pacing` line every ten seconds with missed refreshes.
  `DEVILUTIONX_VR_DEBUG_PACING` and `DEVILUTIONX_VR_DEBUG_FOCUS` are for chasing frame misses and
  testing the pause.

## Patches to third-party code

Both live in `tools/steam_frame`. Neither changes anything for other users of those libraries.

- `patches/dxvk-0001-native-openxr-extension-provider.patch`: DXVK creates the Vulkan instance and
  device, and the OpenXR runtime needs some extensions on both. On Windows DXVK asks wineopenxr.
  The patch lets a native app pass them in `DXVK_OPENXR_INSTANCE_EXTENSIONS` and
  `DXVK_OPENXR_DEVICE_EXTENSIONS`.
- `docker/vkd3d-0001-samplegrad-array-gradients.patch`: vkd3d 2.1 wants three-component gradients
  for `SampleGrad` on a `Texture2DArray`, where Direct3D's compiler takes two. With it, every shader
  compiles from the unchanged source.

## Files outside `Source/platform/vr`

| File | Change |
| --- | --- |
| `CMakeLists.txt` | `DEVILUTIONX_VR` can be turned on on Linux too |
| `Source/CMakeLists.txt` | The VR sources and libraries per platform: d3d11/dxgi/d3dcompiler on Windows, dxvk-native and vkd3d on Linux, plus the binding file for each |
| `Source/init.cpp` | Window focus events don't pause the game while a headset session runs |
| `Source/engine/dx.cpp` | The desktop window can skip frames during a headset session |

All of it is inside `DEVILUTIONX_VR`.

The game without VR is unchanged. I built the flat game (`DEVILUTIONX_VR=OFF`, Linux ARM64)
from upstream `main` and from this branch in the same folder with the same version string. Only
`init.cpp` and `dx.cpp` recompiled. With VR off, `init.cpp` preprocesses to exactly upstream's.
`dx.cpp` differs only in the line numbers that `ErrSdl()` writes into its error messages, because
of the two lines added inside `#ifdef DEVILUTIONX_VR`.

I haven't built the Windows side. Its binding file, `xr_d3d11_win32.cpp`, passes the
D3D11 calls straight through to OpenXR, and it type-checks against dxvk's D3D11 headers. The
session code around it is shared with Linux, where it runs.

## Performance on the Frame

Both eyes at 1728x1728, the runtime's recommended size. GPU time per frame from the VR mode's own
benchmark (`--vr-bench`, 20 seconds along a fixed path, no headset session):

| Level | As on Windows (4x MSAA) | Frame settings, median | Frame settings, 95th percentile |
| --- | --- | --- | --- |
| Town | about 19.5 ms | 8.4 ms | 9.1 ms |
| 2, cathedral | about 17 ms | 6.6 ms | 8.1 ms |
| 6, catacombs | | 6.4 ms | 7.1 ms |
| 10, caves | | 9.1 ms | 10.0 ms |
| 14, hell | | 7.1 ms | 8.7 ms |

The first column was measured at 1832x1920, the benchmark's default size, so it's about 18% high.

Started from Steam at 72 Hz, a session holds 72 fps with no missed refreshes, GPU about 7.3 ms in
town. 90 Hz also held in the two places I tried. The benchmark path has few monsters and lights,
and a fight costs more, which is why 72 Hz is the recommendation for now.

Where the time went, on level 2:

| Change | GPU per frame |
| --- | --- |
| 4x MSAA, as on Windows | 17.1 ms |
| MSAA off. With it, another 4 ms or so fell outside the GPU timers | 12.8 ms |
| Surfaces without the unused material lookups | 8.9 ms |
| Wall shadows one sample per 0.4 tiles | 8.5 ms |
| Turnip drawing straight into memory (`TU_DEBUG=sysmem`, set by the launcher) | 7.6 ms |

In town, nearest-first drawing saved another 1.3 ms and four sun shadow taps 0.8 ms. The depth
pre-pass is about even on average and takes 1.3 to 1.7 ms off the 95th percentile in the caves,
hell and town.

Tried and dropped: branches around texture lookups a pixel doesn't need and a one-texel path for
carved models (both slower on this GPU), drawing surfaces without cut-outs for the early depth
test (no gain), and HLSL minimum-precision types (vkd3d reads `min16float` as `float`).

## What has been checked

Checked:

- The OpenXR binding: the session, the swapchains and both eyes work, in a probe
  (`tools/steam_frame/probe`) and in the game.
- The shaders: every one compiles through vkd3d. The town, cathedral and caves drawn on lavapipe
  in the build image match the Windows screenshots (`build.sh render`).
- Stereo on the Frame: eye images were saved from sessions in town, the cathedral (two
  headings), the caves and hell. Parallax, the lens mask and heading are right.
- The Steam launch: started from Steam, the game runs in Steam Linux Runtime 4 at the title's
  refresh rate and quits cleanly on SIGTERM.
- The flat game: it boots to the main menu in the build image (`build.sh smoke`).
- Playing it: after the performance work it was played in the headset and ran fine, including the
  focus pause. What that play turned up (the laser over menus, the HUD, buttons in menus, walls)
  is in the follow-up PRs.

- Doors, chests and sarcophagi opening, drawn on lavapipe with `DEBUG_OBJECT` on level 2: the
  door swings open, the chest's lid comes up, and the sarcophagus runs through its frames to the
  open one, though its lid only moves a little, as in the art.
- Hell's walls on level 14, before and after, drawn on lavapipe.
Not checked yet:

- Foveation in the headset.
- Fights. No frame rate has been measured in one.
- The Windows build (see above).

`tools/steam_frame/unattended.sh` is what I used for the checks without a wearer: the benchmark,
short sessions over SSH, and the title started through Steam. Before each run it checks that
nothing else is running on the Frame and that the headset is in standby.

/**
 * @file vr.h
 *
 * OpenXR presentation and motion-controller input layer.
 *
 * The game keeps rendering into its normal output surface. That surface is
 * shown in the headset as a world-locked quad layer, and tracked controllers
 * are translated into the SDL mouse / gamepad events the game already handles.
 */
#pragma once

#ifdef USE_SDL3
#include <SDL3/SDL_surface.h>
#else
#include <SDL.h>
#endif

#include <vector>

namespace devilution {

/** Set by the `--vr` command line flag. */
extern bool VrEnabled;

/** Set by `--vr-bench <level>` (0 = town): no headset, fly a fixed camera path and write bench.json. -1 otherwise. */
extern int VrBenchLevel;

/** True while an OpenXR session is running (headset on and compositor accepting frames). */
bool VrSessionRunning();

/**
 * Whether the headset decides when the game pauses for lack of attention. While its session
 * runs it does: the desktop window gaining or losing focus then says nothing about the player.
 */
bool VrOwnsFocus();

/**
 * Whether this frame is left out of the desktop window. With DEVILUTIONX_VR_DESKTOP_WINDOW=0 the
 * window only gets a frame a second while the headset session runs: on a standalone headset
 * nobody sees it, and drawing it costs time every frame.
 */
bool VrSkipsDesktopFrame();

/**
 * Submits the frame to the headset and pumps controller input.
 * Blocks until the headset's next frame slot while a session is running,
 * which replaces the regular frame limiter.
 */
void VrPresent(SDL_Surface *surface);

void VrShutdown();

/**
 * Paragraphs for the top of the help screen while the headset is in use: the controls of the
 * controllers in hand, and where saving is. Same format as the help text ('$' starts a heading).
 */
const std::vector<const char *> &VrControlsHelp();

/** Shows the VR settings in the game menu; `back` is called for its Previous Menu line. */
void VrSettingsMenu(void (*back)(bool));

/** True while the headset shows the level in 3D, so the 2D renderer need not draw it. */
bool VrShowsWorld();

/**
 * What to show in the desktop window instead of the 2D frame: the headset's view, with
 * the panels of `frame` (the game's output surface) on top while one is open, as pixels
 * with that surface's size and format. Null when the 2D frame itself should be shown
 * (menus and videos).
 */
const void *VrMirrorPixels(const SDL_Surface *frame, int &pitch);

} // namespace devilution

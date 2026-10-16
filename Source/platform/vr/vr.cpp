/**
 * @file vr.cpp
 *
 * OpenXR presentation: the per-frame entry point called from the game's present.
 *
 * Menus and videos: the finished game frame is copied into an OpenXR quad layer that
 * is anchored in the room. In a game the level is drawn in 3D (render/) and the quad
 * only carries panels and the HUD strip. Session: xr_session.cpp; input: xr_input.cpp.
 */
#include "platform/vr/vr_internal.h"

#include "platform/vr/debug/profile.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "automap.h"
#include "control/control.hpp"
#include "cursor.h"
#include "diablo.h"
#include "engine/point.hpp"
#include "engine/render/scrollrt.h"
#include "game_mode.hpp"
#include "gmenu.h"
#include "help.h"
#include "interfac.h"
#include "inv.h"
#include "levels/dun_tile_data.hpp"
#include "levels/tile_properties.hpp"
#include "minitext.h"
#include "msg.h"
#include "objects.h"
#include "options.h"
#include "panels/quest_log.hpp"
#include "platform/vr/render/world.h"
#include "pfile.h"
#include "player.h"
#include "qol/stash.h"
#include "quests.h"
#include "stores.h"
#include "utils/display.h"
#include "utils/log.hpp"
#include "utils/paths.h"
#include "utils/ui_fwd.h"

namespace devilution {

bool VrEnabled;
int VrBenchLevel = -1;

namespace vr {

XrPosef screenPose;
bool screenPlaced;
/** Whether the 3D world is shown this frame (in a game, as opposed to menus and videos). */
bool world3d;
/** Current width of the 2D quad in meters. */
float quadWidth = ScreenWidth;
/** Snap-turn angle about the vertical axis, radians, counter-clockwise seen from above. */
float turnYaw;
/** World position of the tracking space origin: the player's feet. */
float worldOrigin[3];
/** Where the head was (tracking space) when the view was last centred: that spot is the character's position. */
XrVector3f headAnchor;
/** Set when the head's current spot should become the character's position (start of a level, re-centre). */
bool anchorPending = true;
/** Whether the 2D frame is shown this frame. In the 3D world that is only while a panel or menu is open. */
bool quadVisible = true;
/** In the 3D world: true while a panel/menu needs the whole 2D frame, false while only the HUD strip is shown. */
bool uiMode = true;
/** The part of the 2D frame the quad shows, in pixels. */
SDL_Rect quadRect;
XrPosef extraPose;
float extraWidth;
SDL_Rect extraRect;
SDL_Rect clearBlack;
/** SDL tick after which the next headset connection attempt may start. */
Uint32 nextAttempt;

/** Puts the 2D frame upright in front of wherever the player is currently looking. */
enum class Held : uint8_t {
	None,
	Satchel,
	Tome,
	Parchment,
};

/**
 * A panel taken out on its own in the world becomes a thing: the inventory a satchel at the hip
 * (its 10 x 4 backpack; the equipment in front of the chest), the spellbook a tome on the left
 * hand, the automap a parchment in the left hand. With anything else open they are the plain panel.
 */
Held HeldPanel()
{
	if (!world3d || !uiMode)
		return Held::None;
	if (CharFlag || QuestLogIsOpen || qtextflag || HelpFlag || ChatFlag || IsStashOpen || IsPlayerInStore() || gmenu_is_active()
	    || PauseMode != 0 || MyPlayerIsDead || SpellSelectFlag)
		return Held::None;
	const int open = (invflag ? 1 : 0) + (SpellbookFlag ? 1 : 0) + (AutomapActive ? 1 : 0);
	if (open != 1)
		return Held::None;
	return invflag ? Held::Satchel : (SpellbookFlag ? Held::Tome : Held::Parchment);
}

Held held = Held::None;
/** The part of the frame the held panel shows. */
SDL_Rect heldRect;

/** Turns `forward` (tracking space, any length) into a yaw-only orientation looking along it, tilted back by `tilt`. */
XrQuaternionf Facing(float dx, float dz, float tilt)
{
	const float yaw = std::atan2(dx, dz);
	return Multiply({ 0, std::sin(yaw / 2), 0, std::cos(yaw / 2) }, { std::sin(tilt / 2), 0, 0, std::cos(tilt / 2) });
}

/** Where the main panel splits between the wrists: belt slot 5 starts here (inv.cpp InvRect). */
constexpr int WristSplit = 321;
/** The HUD strip is on the wrists this frame (the HUD setting). */
bool wristsPlaced;

/**
 * The HUD strip in two halves, one on each wrist, turned to the eyes: life, the left of the belt
 * and the menu buttons on the left; mana, the right of the belt and the spell on the right.
 * False while a hand is not tracked.
 */
bool PlaceWrists(XrTime time, const XrPosef &head)
{
	XrPosef hands[2];
	if (!LocateHand(Left, time, hands[Left]) || !LocateHand(Right, time, hands[Right]))
		return false;
	const Rectangle &panel = GetMainPanel();
	const float metresPerPixel = WristWidth * 2 / static_cast<float>(panel.size.width);
	XrPosef *const poses[2] = { &screenPose, &extraPose };
	for (const Hand hand : { Left, Right }) {
		// Above the back of the wrist, a hand's length behind where the controller points from.
		const XrVector3f offset = Rotate(hands[hand].orientation, { 0, 0.05F, 0.14F });
		XrPosef &pose = *poses[hand];
		pose.position = { hands[hand].position.x + offset.x, hands[hand].position.y + offset.y, hands[hand].position.z + offset.z };
		const float dx = head.position.x - pose.position.x;
		const float dy = head.position.y - pose.position.y;
		const float dz = head.position.z - pose.position.z;
		pose.orientation = Facing(dx, dz, -std::atan2(dy, std::sqrt(dx * dx + dz * dz)));
	}
	quadWidth = metresPerPixel * static_cast<float>(WristSplit);
	extraWidth = metresPerPixel * static_cast<float>(panel.size.width - WristSplit);
	extraRect = { panel.position.x + WristSplit, panel.position.y, panel.size.width - WristSplit, panel.size.height };
	return true;
}

/** Places a held panel for this frame (it moves with the body or the hand). False when it cannot be placed. */
bool PlaceHeld(XrTime time, const XrPosef &head)
{
	const XrVector3f forward = Rotate(head.orientation, { 0, 0, -1 });
	const float yaw = std::atan2(-forward.x, -forward.z);
	const float fx = -std::sin(yaw);
	const float fz = -std::cos(yaw);
	extraWidth = 0;
	clearBlack = {};
	mapArrow.visible = false;
	if (held == Held::Satchel) {
		// The backpack grid (inv.cpp InvRect: 10 x 4 cells of 29 pixels from (17, 222) in the panel).
		const Rectangle &panel = GetRightPanel();
		heldRect = { panel.position.x + 17, panel.position.y + 222, 290, 116 };
		// At the right hip, its opening turned up towards the eyes.
		screenPose.position = { head.position.x + fx * 0.12F - fz * 0.22F, head.position.y - 0.75F, head.position.z + fz * 0.12F + fx * 0.22F };
		screenPose.orientation = Facing(head.position.x - screenPose.position.x, head.position.z - screenPose.position.z, -1.0F);
		quadWidth = 0.3F;
		// The equipment (the panel above the grid) in front of the chest.
		extraRect = { panel.position.x, panel.position.y, panel.size.width, 210 };
		extraPose.position = { head.position.x + fx * 0.42F, head.position.y - 0.35F, head.position.z + fz * 0.42F };
		extraPose.orientation = Facing(-fx, -fz, -0.35F);
		extraWidth = 0.32F;
		return true;
	}
	// The tome and the automap are held in the hand that casts.
	XrPosef hand;
	if (!LocateHand(LeftHanded() ? Right : Left, time, hand))
		return false;
	const float tx = head.position.x - hand.position.x;
	const float tz = head.position.z - hand.position.z;
	if (held == Held::Tome) {
		const Rectangle &panel = GetRightPanel();
		heldRect = { panel.position.x, panel.position.y, panel.size.width, panel.size.height };
		// Open on the left palm, leaning back towards the eyes.
		screenPose.position = { hand.position.x, hand.position.y + 0.12F, hand.position.z };
		screenPose.orientation = Facing(tx, tz, -0.55F);
		quadWidth = 0.26F;
		return true;
	}
	// The automap over the whole view, black shown as the parchment, held so that where the
	// player is on it sits just over the left hand.
	heldRect = { 0, 0, gnScreenWidth, gnViewportHeight };
	clearBlack = heldRect;
	screenPose.orientation = Facing(tx, tz, -0.3F);
	quadWidth = 0.5F;
	// Where the game draws the player's arrow (automap.cpp DrawAutomapPlr): the middle of the
	// view, moved by however far the map has been scrolled.
	const Player &player = *MyPlayer;
	const int px = player.position.tile.x - 2 * AutomapOffset.deltaX - ViewPosition.x;
	const int py = player.position.tile.y - 2 * AutomapOffset.deltaY - ViewPosition.y;
	const float arrowX = static_cast<float>(gnScreenWidth / 2 + (px - py) * AmLine(AmLineLength::DoubleTile));
	const float arrowY = static_cast<float>((gnScreenHeight - GetMainPanel().size.height) / 2 + (px + py) * AmLine(AmLineLength::FullTile));
	const float metresPerPixel = quadWidth / static_cast<float>(heldRect.w);
	const XrVector3f right = Rotate(screenPose.orientation, { 1, 0, 0 });
	const XrVector3f up = Rotate(screenPose.orientation, { 0, 1, 0 });
	const XrVector3f normal = Rotate(screenPose.orientation, { 0, 0, 1 });
	const XrVector3f anchor { hand.position.x, hand.position.y + 0.07F, hand.position.z };
	const float dx = (arrowX - static_cast<float>(heldRect.w) / 2) * metresPerPixel;
	const float dy = (static_cast<float>(heldRect.h) / 2 - arrowY) * metresPerPixel;
	screenPose.position = { anchor.x - right.x * dx - up.x * dy, anchor.y - right.y * dx - up.y * dy, anchor.z - right.z * dx - up.z * dy };
	// The big arrow points where the player looks, drawn as the map draws directions: a step
	// along the tile x axis goes right and down on it, one along y left and down, at 2:1.
	float headWorld[3];
	float headTurned[4];
	ToWorld(head, headWorld, headTurned);
	const XrVector3f look = Rotate({ headTurned[0], headTurned[1], headTurned[2], headTurned[3] }, { 0, 0, -1 });
	const float mapX = 2 * (look.x - look.z);
	const float mapY = look.x + look.z;
	if (mapX * mapX + mapY * mapY > 1e-4F) {
		const float angle = std::atan2(-mapX, -mapY);
		mapArrow.visible = true;
		mapArrow.pose.orientation = Multiply(screenPose.orientation, { 0, 0, std::sin(angle / 2), std::cos(angle / 2) });
		mapArrow.pose.position = { anchor.x + normal.x * 0.003F, anchor.y + normal.y * 0.003F, anchor.z + normal.z * 0.003F };
	}
	return true;
}

void PlaceScreen(XrTime time)
{
	XrPosef head;
	if (!Locate(viewSpace, time, head))
		return;
	wristsPlaced = false;
	if (held != Held::None) {
		screenPlaced = PlaceHeld(time, head);
		if (screenPlaced)
			return;
	}
	extraWidth = 0;
	clearBlack = {};
	const XrVector3f forward = Rotate(head.orientation, { 0, 0, -1 });
	const float yaw = std::atan2(-forward.x, -forward.z);
	const XrQuaternionf yawOnly { 0, std::sin(yaw / 2), 0, std::cos(yaw / 2) };
	// Three placements: the big screen for menus, a mid-size one for panels and dialogs in
	// a game, and otherwise just the HUD strip (orbs, belt, buttons) low in front, leaning back.
	const bool hud = world3d && !uiMode;
	// Talking or trading: the text and the shop stand beside the townsperson, turned to the player.
	float speaker[3];
	if (world3d && uiMode && (qtextflag || IsPlayerInStore()) && VrWorldSpeaker(speaker)) {
		// World -> tracking space (the inverse of ToWorld).
		const float dx = speaker[0] - worldOrigin[0];
		const float dz = speaker[2] - worldOrigin[2];
		const float c = std::cos(-turnYaw);
		const float s = std::sin(-turnYaw);
		XrVector3f local { c * dx + s * dz + headAnchor.x, speaker[1] - worldOrigin[1], -s * dx + c * dz + headAnchor.z };
		// Beside them, a little towards the player, at arm's length to the right of the speaker as seen.
		const float tx = head.position.x - local.x;
		const float tz = head.position.z - local.z;
		const float away = std::sqrt(tx * tx + tz * tz);
		if (away > 0.3F) {
			const float nx = tx / away;
			const float nz = tz / away;
			local.x += nx * 0.4F + nz * 0.75F;
			local.z += nz * 0.4F - nx * 0.75F;
			const float faceYaw = std::atan2(head.position.x - local.x, head.position.z - local.z);
			screenPose.orientation = { 0, std::sin(faceYaw / 2), 0, std::cos(faceYaw / 2) };
			screenPose.position = local;
			quadWidth = PanelWidth * 0.8F;
			screenPlaced = true;
			return;
		}
	}
	const float distance = hud ? 0.55F : (world3d ? PanelDistance : ScreenDistance);
	const float drop = hud ? 0.6F : (world3d ? 0.2F : 0.1F);
	const float tilt = hud ? -0.9F : 0;
	quadWidth = hud ? HudWidth : (world3d ? PanelWidth : ScreenWidth);
	screenPose.orientation = Multiply(yawOnly, { std::sin(tilt / 2), 0, 0, std::cos(tilt / 2) });
	screenPose.position = {
		head.position.x - std::sin(yaw) * distance,
		head.position.y - drop,
		head.position.z - std::cos(yaw) * distance,
	};
	if (hud && WristHud())
		wristsPlaced = PlaceWrists(time, head);
	if (anchorPending)
		headAnchor = head.position;
	anchorPending = false;
	screenPlaced = true;
}

/**
 * The item card: pointing at an item, an object, a monster or a townsperson in the world shows
 * the game's description of it (the info box of the main panel) on a card just above it, turned
 * to the eyes and as large to them wherever it is. `frame` is the 2D frame's layer, to copy.
 */
bool PlaceItemCard(XrTime time, const XrCompositionLayerQuad &frame, XrCompositionLayerQuad &card)
{
	if (!*Settings().itemCards || !world3d || uiMode || !laser.visible || laser.onQuad || !worldPointerValid || InfoString.empty())
		return false;
	if (pcursitem == -1 && ObjectUnderCursor == nullptr && PlayerUnderCursor == nullptr && pcursmonst == -1)
		return false;
	XrPosef head;
	if (!Locate(viewSpace, time, head))
		return false;
	const Rectangle &panel = GetMainPanel();
	const SDL_Rect box { panel.position.x + InfoBoxRect.position.x, panel.position.y + InfoBoxRect.position.y, InfoBoxRect.size.width, InfoBoxRect.size.height };
	const float dx = laser.to.x - head.position.x;
	const float dy = laser.to.y - head.position.y;
	const float dz = laser.to.z - head.position.z;
	const float away = std::sqrt(dx * dx + dy * dy + dz * dz);
	const float width = std::clamp(0.3F * away, 0.35F, 1.4F);
	card = frame;
	card.layerFlags = 0;
	card.subImage.imageRect.offset = { box.x, box.y };
	card.subImage.imageRect.extent = { box.w, box.h };
	card.size = { width, width * static_cast<float>(box.h) / static_cast<float>(box.w) };
	card.pose.position = { laser.to.x, laser.to.y + 0.25F + card.size.height, laser.to.z };
	const float tx = head.position.x - card.pose.position.x;
	const float ty = head.position.y - card.pose.position.y;
	const float tz = head.position.z - card.pose.position.z;
	card.pose.orientation = Facing(tx, tz, -std::atan2(ty, std::sqrt(tx * tx + tz * tz)));
	return true;
}

/** The game is held paused because the session lost the wearer's attention. */
bool focusPaused;

/**
 * Losing focus means the headset came off or the system's menu is up: the player is not looking
 * and the controllers are not ours. The game pauses the way it does when its window loses focus,
 * sound included, and goes on by itself when focus is back. The option that governs the one
 * governs the other.
 */
void UpdateFocusPause()
{
	// Debug runs with nobody in the headset never get focus, and have to play.
	static const bool unattended = Knob("DEVILUTIONX_VR_FORCE_RENDER", 0) != 0 || SDL_getenv("DEVILUTIONX_VR_AUTOSTART") != nullptr || VrBenchLevel >= 0;
	// Only once it has been focused: a session comes up unfocused, which is no loss.
	bool away = sessionRunning && sessionWasFocused && !sessionFocused && !unattended && *GetOptions().Gameplay.pauseOnFocusLoss;
	// Debug aid (DEVILUTIONX_VR_DEBUG_FOCUS=<seconds>): focus comes and goes by the clock, that
	// long each, so the pause can be watched with nobody there to take the headset off.
	static const float debugFocus = Knob("DEVILUTIONX_VR_DEBUG_FOCUS", 0);
	if (debugFocus > 0)
		away = sessionRunning && gbRunGame && (SDL_GetTicks() / static_cast<Uint32>(debugFocus * 1000)) % 2 == 1;
	if (away && !focusPaused) {
		diablo_focus_pause();
		focusPaused = true;
		LogVerbose("VR: out of focus, pausing (pause mode {})", PauseMode);
	} else if (!away && focusPaused) {
		diablo_focus_unpause();
		focusPaused = false;
		LogVerbose("VR: in focus again (pause mode {})", PauseMode);
	}
}

/**
 * The first time a game is played in the headset (on this install), the help with the controls
 * comes up by itself a couple of seconds in.
 */
void ShowHelpOnce()
{
	static const bool unattended = Knob("DEVILUTIONX_VR_FORCE_RENDER", 0) != 0 || SDL_getenv("DEVILUTIONX_VR_AUTOSTART") != nullptr || VrBenchLevel >= 0;
	static bool done = unattended;
	static int frames;
	if (done || !world3d || !gbRunGame || UiOpen() || ++frames < 150)
		return;
	done = true;
	const std::string marker = paths::PrefPath() + "vr-help-shown";
	if (SDL_RWops *file = SDL_RWFromFile(marker.c_str(), "rb"); file != nullptr) {
		SDL_RWclose(file);
		return;
	}
	if (SDL_RWops *file = SDL_RWFromFile(marker.c_str(), "wb"); file != nullptr)
		SDL_RWclose(file);
	DisplayHelp();
}

/** Anything that needs the 2D frame in view: panels, dialogs, shops, menus, the automap. */
bool UiOpen()
{
	return invflag || CharFlag || SpellbookFlag || SpellSelectFlag || QuestLogIsOpen || qtextflag || HelpFlag || DropGoldFlag
	    || ChatFlag || IsStashOpen || AutomapActive || IsPlayerInStore() || gmenu_is_active() || PauseMode != 0 || MyPlayerIsDead;
}

} // namespace vr

using namespace vr;

bool VrSessionRunning()
{
	return sessionRunning;
}

bool VrOwnsFocus()
{
	return VrEnabled && sessionRunning;
}

bool VrSkipsDesktopFrame()
{
	static const bool desktopWindow = Knob("DEVILUTIONX_VR_DESKTOP_WINDOW", 1) != 0;
	if (desktopWindow || !sessionRunning || MirrorCaptureEnabled())
		return false;
	// Still a frame now and then, so that whatever watches the window sees it alive.
	static Uint32 nextDesktopFrame;
	if (SDL_TICKS_PASSED(SDL_GetTicks(), nextDesktopFrame)) {
		nextDesktopFrame = SDL_GetTicks() + 1000;
		return false;
	}
	return true;
}

bool VrShowsWorld()
{
	return sessionRunning && world3d;
}

void VrPresent(SDL_Surface *surface)
{
	if (!VrEnabled || state == State::Failed)
		return;
	// The Autosave setting: a save to the autosave slot after every level change (pfile.cpp).
	// Unattended test runs never autosave, so they leave no saves behind, unless
	// DEVILUTIONX_VR_DEBUG_AUTOSAVE=1 asks for it (to time one).
	static const bool unattendedRun = (SDL_getenv("DEVILUTIONX_VR_AUTOSTART") != nullptr || Knob("DEVILUTIONX_VR_FORCE_RENDER", 0) != 0 || VrBenchLevel >= 0)
	    && Knob("DEVILUTIONX_VR_DEBUG_AUTOSAVE", 0) == 0;
	AutosaveEnabled = *Settings().autosave && !unattendedRun;

	// Debug aid: taps Return until a game is running, so the 3D view can be tested unattended.
	static const bool autostart = SDL_getenv("DEVILUTIONX_VR_AUTOSTART") != nullptr || VrBenchLevel >= 0;
	if (autostart && !gbRunGame) {
		static Uint32 nextTap;
		static bool returnDown;
		if (SDL_TICKS_PASSED(SDL_GetTicks(), nextTap)) {
			// With no saved hero the way into a game stops at the new hero's name, which
			// Return alone does not get past. Typed text goes nowhere in the other menus.
			if (!returnDown) {
				SDL_Event text;
				SDL_zero(text);
				text.type = SDL_TEXTINPUT;
				text.text.windowID = SDL_GetWindowID(ghMainWnd);
				SDL_strlcpy(text.text.text, "V", sizeof(text.text.text));
				SDL_PushEvent(&text);
			}
			SetKey(returnDown, !returnDown, SDLK_RETURN);
			nextTap = SDL_GetTicks() + 600;
		}
	}

	// SDL_getenv returns a buffer that later calls reuse, hence the copy.
	static const std::string offscreenDirectory = SDL_getenv("DEVILUTIONX_VR_OFFSCREEN") != nullptr ? SDL_getenv("DEVILUTIONX_VR_OFFSCREEN") : "";
	if (VrBenchLevel >= 0) {
		DebugBench(VrBenchLevel, offscreenDirectory.c_str());
		return;
	}
	if (!offscreenDirectory.empty()) {
		DebugOffscreen(offscreenDirectory.c_str());
		return;
	}

	if (state == State::Uninitialized) {
		if (!SDL_TICKS_PASSED(SDL_GetTicks(), nextAttempt))
			return;
		Cleanup();
		SelfCheck();
		SelectRuntime();
		// Runtimes may block for a long time while creating the session (SteamVR
		// waits for its current scene app), so the game keeps running on the
		// desktop window in the meantime.
		initResult = -1;
		state = State::Initializing;
		SDL_DetachThread(SDL_CreateThread(
		    [](void *) -> int {
			    initResult = Init() ? 1 : 0;
			    return 0;
		    },
		    "vr-init", nullptr));
		return;
	}
	if (state == State::Initializing) {
		const int result = initResult;
		if (result < 0)
			return;
		if (result == 0 || !InitVirtualPad()) {
			static bool logged;
			if (!logged)
				LogError("VR: no headset session yet, staying on the desktop window and retrying");
			logged = true;
			runtimeIndex++;
			nextAttempt = SDL_GetTicks() + 2000;
			state = State::Uninitialized;
			return;
		}
		state = State::Ready;
	}

	PollEvents();
	UpdateFocusPause();
	if (!sessionRunning)
		return;

	XrFrameState frameState { XR_TYPE_FRAME_STATE };
	const auto lost = [](XrResult result) { return result == XR_ERROR_SESSION_LOST || result == XR_ERROR_INSTANCE_LOST; };
	// Pacing, for the log: how long the game took to come back for the next frame, how long the
	// runtime then made it wait, and how many display refreshes went by without a new frame.
	static Uint64 frameEnded;
	static Uint64 pacingSince = SDL_GetPerformanceCounter();
	static XrTime lastDisplayTime;
	static int pacedFrames;
	static int missedFrames;
	static double awayTotal;
	static double awayLongest;
	static double waitTotal;
	const double perMs = 1000.0 / static_cast<double>(SDL_GetPerformanceFrequency());
	const Uint64 waitStart = SDL_GetPerformanceCounter();
	XrResult frameResult = xrWaitFrame(session, nullptr, &frameState);
	if (XR_SUCCEEDED(frameResult)) {
		const Uint64 waitEnd = SDL_GetPerformanceCounter();
		static double lastAway;
		static double lastWork;
		static Uint64 lastWaitEnd;
		double away = 0;
		if (frameEnded != 0) {
			away = static_cast<double>(waitStart - frameEnded) * perMs;
			awayTotal += away;
			awayLongest = std::max(awayLongest, away);
		}
		const double held = static_cast<double>(waitEnd - waitStart) * perMs;
		waitTotal += held;
		if (lastDisplayTime != 0 && frameState.predictedDisplayPeriod > 0) {
			const auto steps = static_cast<int>(std::llround(static_cast<double>(frameState.predictedDisplayTime - lastDisplayTime) / static_cast<double>(frameState.predictedDisplayPeriod)));
			if (steps > 1 && steps < 100) {
				missedFrames += steps - 1;
				// DEVILUTIONX_VR_DEBUG_PACING=1: what the frames around a miss looked like.
				static const bool debugPacing = Knob("DEVILUTIONX_VR_DEBUG_PACING", 0) != 0;
				static int reported;
				if (debugPacing && reported++ < 40)
					LogVerbose("VR: missed {}: frame before worked {:.2f} ms after waking, away {:.2f} ms before that; this one away {:.2f} ms, held {:.2f} ms", steps - 1, lastWork, lastAway, away, held);
			}
		}
		lastAway = away;
		lastWork = frameEnded != 0 && lastWaitEnd != 0 ? static_cast<double>(frameEnded - lastWaitEnd) * perMs : 0;
		lastWaitEnd = waitEnd;
		lastDisplayTime = frameState.predictedDisplayTime;
		pacedFrames++;
		const double seconds = static_cast<double>(waitEnd - pacingSince) * perMs / 1000.0;
		if (seconds >= 10) {
			LogVerbose("VR: pacing: {:.1f} fps on a {:.0f} Hz display, {} refreshes missed in {:.0f} s; the game is away {:.2f} ms a frame ({:.1f} at most), the runtime holds it {:.2f} ms",
			    pacedFrames / seconds, 1e9 / static_cast<double>(frameState.predictedDisplayPeriod), missedFrames, seconds, awayTotal / pacedFrames, awayLongest, waitTotal / pacedFrames);
			pacingSince = waitEnd;
			pacedFrames = missedFrames = 0;
			awayTotal = awayLongest = waitTotal = 0;
		}
	}
	if (XR_SUCCEEDED(frameResult))
		frameResult = xrd3d::BeginFrame(session);
	if (XR_FAILED(frameResult)) {
		if (lost(frameResult))
			SessionLost("frame call failed");
		return;
	}
	ProfileBeginFrame();
	// Once per session: later the runtime stretches the period whenever the game misses frames.
	static Uint32 periodLoggedFor;
	if (periodLoggedFor != sessionStartTick) {
		periodLoggedFor = sessionStartTick;
		const auto period = static_cast<double>(frameState.predictedDisplayPeriod);
		LogInfo("VR: display period {:.2f} ms ({:.0f} Hz)", period / 1e6, 1e9 / period);
	}

	// In a game the world is drawn in 3D and the 2D frame shrinks to a panel; menus and videos get the big screen.
	const bool showWorld = VrWorldAvailable();
	// Debug aid: with the autostart, DEVILUTIONX_VR_DEBUG_LEVEL=<n> goes on to dungeon level n
	// a few seconds into the game, as it does for the offscreen renderer.
	static const int debugLevel = static_cast<int>(Knob("DEVILUTIONX_VR_DEBUG_LEVEL", 0));
	static int worldFrames;
	if (autostart && debugLevel != 0 && showWorld && ++worldFrames == 200)
		EnterLevel(debugLevel);
	// Debug aid: DEVILUTIONX_VR_DEBUG_AUTOMAP=<n> opens the automap n frames into a game, for
	// checking the map in the hand with nobody there.
	static const int debugAutomap = static_cast<int>(Knob("DEVILUTIONX_VR_DEBUG_AUTOMAP", 0));
	static int automapFrames;
	if (debugAutomap > 0 && showWorld && gbRunGame && ++automapFrames == debugAutomap && !AutomapActive)
		StartAutomap();
	if (showWorld != world3d) {
		world3d = showWorld;
		LogVerbose("VR: showing {}", world3d ? "the world" : "the screen");
		mirrorReady = false;
		screenPlaced = false;
		anchorPending = true;
		freeValid = false;
	}
	// In first person the 2D frame only comes up, in front of the current gaze, while it has something to show.
	// In first person the whole 2D frame only comes up while it has something to show;
	// otherwise the quad is just the HUD strip.
	const bool ui = !world3d || UiOpen();
	if (ui != uiMode)
		screenPlaced = false;
	uiMode = ui;
	quadVisible = true;
	const Held nowHeld = HeldPanel();
	if (nowHeld != held)
		screenPlaced = false;
	held = nowHeld;
	ShowHelpOnce();
	// A held panel and the wrists move with the body or the hand: placed every frame.
	if (!screenPlaced || held != Held::None || (world3d && !uiMode && WristHud()))
		PlaceScreen(frameState.predictedDisplayTime);

	XrCompositionLayerQuad quad { XR_TYPE_COMPOSITION_LAYER_QUAD };
	XrCompositionLayerProjection projection { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	XrCompositionLayerProjectionView projectionViews[2];
	XrCompositionLayerQuad extra { XR_TYPE_COMPOSITION_LAYER_QUAD };
	XrCompositionLayerQuad laserQuads[2];
	XrCompositionLayerQuad arrowQuad;
	XrCompositionLayerQuad card { XR_TYPE_COMPOSITION_LAYER_QUAD };
	const XrCompositionLayerBaseHeader *layers[7];
	uint32_t layerCount = 0;
	// Debug aid: a headset nobody wears is told not to render. See RenderWorld.
	static const bool forceRender = Knob("DEVILUTIONX_VR_FORCE_RENDER", 0) != 0;
	// In a game the 2D renderer leaves the world black behind panels, dialogs and shops (and the
	// see-through backing of dialogs comes out black too). Black is uploaded transparent there, so
	// only the interface floats over the world, as the desktop mirror shows it.
	const bool seeThrough = *Settings().seeThroughPanels && world3d && uiMode && held == Held::None && surface != nullptr;
	if (seeThrough)
		clearBlack = { 0, 0, surface->w, surface->h };
	else if (held != Held::Parchment)
		clearBlack = {};
	if ((frameState.shouldRender || forceRender) && screenPlaced && surface != nullptr && UploadFrame(surface)) {
		quad.space = localSpace;
		quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		quad.subImage.swapchain = swapchain;
		quadRect = { 0, 0, swapchainWidth, swapchainHeight };
		if (held != Held::None) {
			SDL_Rect clipped;
			if (SDL_IntersectRect(&heldRect, &quadRect, &clipped) == SDL_TRUE)
				quadRect = clipped;
		} else if (!uiMode) {
			const Rectangle &panel = GetMainPanel();
			const SDL_Rect panelRect { panel.position.x, panel.position.y, wristsPlaced ? WristSplit : panel.size.width, panel.size.height };
			SDL_Rect clipped;
			if (SDL_IntersectRect(&panelRect, &quadRect, &clipped) == SDL_TRUE)
				quadRect = clipped;
		}
		quad.subImage.imageRect.offset = { quadRect.x, quadRect.y };
		quad.subImage.imageRect.extent = { quadRect.w, quadRect.h };
		quad.pose = screenPose;
		quad.size = { quadWidth, quadWidth * static_cast<float>(quadRect.h) / static_cast<float>(quadRect.w) };
		if (held == Held::Parchment || seeThrough)
			quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
		// What holds the panel is drawn by the world, where the quad is.
		VrHeldPanel holder {};
		if (held != Held::None) {
			holder.kind = static_cast<int>(held);
			ToWorld(screenPose, holder.position, holder.orientation);
			holder.width = quad.size.width;
			holder.height = quad.size.height;
		}
		VrWorldSetHeldPanel(holder);
		if (world3d)
			SyncFreePosition();
		UpdateInput(frameState.predictedDisplayTime, quad.size.height);
		if (world3d && RenderWorld(frameState.predictedDisplayTime, projectionViews)) {
			projection.space = localSpace;
			projection.viewCount = 2;
			projection.views = projectionViews;
			layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader *>(&projection);
		}
		if (quadVisible)
			layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader *>(&quad);
		if (quadVisible && extraWidth > 0 && extraRect.w > 0) {
			extra = quad;
			extra.layerFlags = 0;
			extra.subImage.imageRect.offset = { extraRect.x, extraRect.y };
			extra.subImage.imageRect.extent = { extraRect.w, extraRect.h };
			extra.pose = extraPose;
			extra.size = { extraWidth, extraWidth * static_cast<float>(extraRect.h) / static_cast<float>(extraRect.w) };
			layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader *>(&extra);
		}
		if (PlaceItemCard(frameState.predictedDisplayTime, quad, card))
			layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader *>(&card);
		if (quadVisible && held == Held::Parchment && MapArrowLayer(arrowQuad))
			layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader *>(&arrowQuad);
		// The laser last, over the screen and every panel.
		const uint32_t laserCount = LaserLayers(frameState.predictedDisplayTime, laserQuads);
		for (uint32_t i = 0; i < laserCount; i++)
			layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader *>(&laserQuads[i]);
	}

	ProfileEndFrame();
	XrFrameEndInfo endInfo { XR_TYPE_FRAME_END_INFO };
	endInfo.displayTime = frameState.predictedDisplayTime;
	endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	endInfo.layerCount = layerCount;
	endInfo.layers = layers;
	const XrResult result = xrd3d::EndFrame(session, endInfo);
	frameEnded = SDL_GetPerformanceCounter();
	static XrResult lastResult = XR_SUCCESS;
	// With DEVILUTIONX_VR_DEBUG_HANDS the layers the laser and the wrists add are logged as they come and go.
	static int lastLayers = -1;
	static const bool logLayers = Knob("DEVILUTIONX_VR_DEBUG_HANDS", 0) != 0;
	const bool arrowShown = held == Held::Parchment && mapArrow.visible;
	const int layersNow = static_cast<int>(layerCount) | (laser.visible ? 16 : 0) | (laser.onQuad ? 32 : 0) | (wristsPlaced ? 64 : 0) | (arrowShown ? 128 : 0) | (static_cast<int>(held) << 8);
	if (logLayers && layersNow != lastLayers) {
		LogInfo("VR: {} layer(s), xrEndFrame -> {} (laser {}, on a panel {}, wrists {}, held {}, map arrow {})", layerCount, static_cast<int>(result), laser.visible, laser.onQuad, wristsPlaced, static_cast<int>(held), arrowShown);
		lastLayers = layersNow;
	}
	if (result != lastResult) {
		LogInfo("VR: xrEndFrame -> {} ({} layer(s), {}x{})", static_cast<int>(result), layerCount, swapchainWidth, swapchainHeight);
		lastResult = result;
	}
	if (lost(result)) {
		SessionLost("xrEndFrame failed");
		return;
	}
	// Debug aid (DEVILUTIONX_VR_DEBUG_RESTART=<seconds>): ends the session that long after it
	// started, first by asking the runtime to exit, the second time as if it had been lost.
	static const float restartAfter = Knob("DEVILUTIONX_VR_DEBUG_RESTART", 0);
	static int restarts;
	if (restartAfter > 0 && restarts < 2 && static_cast<float>(SDL_GetTicks() - sessionStartTick) > restartAfter * 1000) {
		sessionStartTick = SDL_GetTicks();
		if (restarts++ == 0) {
			LogInfo("VR: debug restart: xrRequestExitSession");
			xrRequestExitSession(session);
		} else {
			SessionLost("debug restart");
		}
	}
	static bool firstFrameLogged;
	if (layerCount >= 1 && !firstFrameLogged) {
		LogInfo("VR: first frame submitted ({}x{})", swapchainWidth, swapchainHeight);
		firstFrameLogged = true;
	}
}

void VrShutdown()
{
	// A runtime call may still be blocked on the init thread; the process is exiting anyway.
	if (state != State::Initializing)
		Cleanup();
	state = State::Failed;
}

} // namespace devilution

/**
 * @file xr_input.cpp
 *
 * Controller actions and what they do in the game.
 *
 * Nothing here talks to game logic beyond the commands the mouse sends. Controllers are
 * turned into the SDL events (or the network commands) the game already understands. In the 3D
 * world:
 *  - left stick          -> free movement over the ground, any direction, as fast as it is pushed
 *  - right stick         -> snap or smooth turn (left/right), potions (up/down)
 *  - right hand swing    -> a melee attack on the neighbouring monster the swing goes towards
 *  - right trigger       -> click on what is pointed at (attack in its direction otherwise)
 *  - bow: right trigger by the left hand, pulled back and let go -> a shot along the arrow
 *  - right grip          -> take hold of a door, chest, lever, shrine or item within reach (a click on it)
 *  - left trigger        -> cast the spell held ready, at what the left hand points at (the right button)
 *  - left grip (held)    -> the speedbook round the left hand; the left stick chooses, letting go sets it
 *  - left Y / X          -> inventory / character panel; everything else through the virtual pad
 * On the 2D screen the right hand points and clicks as before. The left hand points where the
 * right one misses a screen or panel, and its trigger clicks there.
 */
#include "platform/vr/vr_internal.h"

#include "platform/vr/input_rules.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "automap.h"
#include "control/control.hpp"
#include "controls/plrctrls.h"
#include "cursor.h"
#include "diablo.h"
#include "engine/backbuffer_state.hpp"
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
#include "monster.h"
#include "objects.h"
#include "levels/gendung.h"
#include "msg.h"
#include "options.h"
#include "panels/quest_log.hpp"
#include "platform/vr/render/world.h"
#include "player.h"
#include "qol/stash.h"
#include "quests.h"
#include "stores.h"
#include "utils/display.h"
#include "utils/log.hpp"
#include "utils/ui_fwd.h"

namespace devilution {
namespace vr {

XrActionSet actionSet;
XrPath handPath[2];
XrSpace aimSpace[2];
XrAction aimAction;
XrAction triggerAction;
XrAction squeezeAction;
XrAction stickAction;
XrAction stickClickAction;
XrAction aAction;
XrAction bAction;
XrAction xAction;
XrAction yAction;
XrAction menuAction;
XrAction hapticAction;
// Only the Steam Frame's controllers have these: they are a gamepad split over two hands.
XrAction bumperAction;
XrAction viewAction;
XrAction dpadAction[4];
/** The controllers in use are the Steam Frame's. */
bool frameControllers;
rules::ViewButton viewButton;
/** A short press of View: the game's Back (Select) for a few frames, or Escape in a game. */
int backPulse;
int escapePulse;
bool viewEscapeDown;

SDL_Joystick *virtualPad;
bool mouseLeftDown;
bool mouseRightDown;
bool menuDown;
bool recenterDown;
/** Both thumbsticks are (or just were) pressed in together: neither does its own job until both are up. */
bool stickChord;
bool automapDown;
int automapPulse;
bool swinging;
int pointerX = -1;
int pointerY = -1;
/** Last pointer position delivered to the game as mouse motion. */
int sentX = -1;
int sentY = -1;
bool turnHeld;
bool worldPointerValid;
float worldPointerHit[3];

// Smooth locomotion: where the camera stands, in world meters. The game's player follows it.
float freeX;
float freeZ;
bool freeValid;
int walkTargetX = -1;
int walkTargetY = -1;
Uint32 walkSentAt;
/** The trigger is held as a plain click on something that has to be reached (item, object, townsperson). */
bool clickingWorld;
bool pointerOnQuad;
/** The left hand is the one pointing at the screen or a panel (the right one is not on any). */
bool leftPointing;
/** The trigger would do something with what the laser is on: worth a tick in the hand when it gets there. */
bool pointerWasActive;
/** Where the right hand points, flattened to the ground plane (world x, z), unit length. */
float aimDirX;
float aimDirZ = -1;
Uint32 lastAttackAt;
/** A melee attack whose outcome the hand should feel: the monster and its life when the swing went out. */
int swingTarget = -1;
int swingTargetLife;
Uint32 swingAt;
/** The bow: drawing since the right trigger closed by the left hand. */
bool drawingBow;
bool rightTriggerWas;
/** The left hand casts; the speedbook is open while the left grip is held. */
bool castingDown;
bool speedbookOpen;
VrSpellRing ring {};
bool grabDown;
bool panelInventoryDown;
bool panelCharacterDown;
/** Seated play (the settings page): the world is raised by this much so the eyes are at a standing person's height. */
float SeatedRaise()
{
	static const float raise = Knob("DEVILUTIONX_VR_SEATED_RAISE", 0.45F);
	return *Settings().seated ? raise : 0.F;
}
/** Radians of one snap turn. */
float SnapTurnAngle()
{
	return static_cast<float>(*Settings().snapAngle) * 0.0174533F;
}
/** Radians per second of a smooth turn at full deflection. */
float SmoothTurnSpeed()
{
	return static_cast<float>(*Settings().smoothTurnSpeed) * 0.0174533F;
}
/** How dark the edge of view gets while moving or turning smoothly (0: no vignette). */
float ComfortVignette()
{
	switch (*Settings().vignette) {
	case VrVignette::Low:
		return 0.35F;
	case VrVignette::Medium:
		return 0.6F;
	case VrVignette::High:
		return 0.85F;
	default:
		return 0.F;
	}
}
/** 0: the old walk, the character stepping from tile to tile in eight directions with the view on it. */
const bool FreeMovement = Knob("DEVILUTIONX_VR_FREE_MOVE", 1) != 0;
/** Tiles per second at full stick: the game's own walk covers a tile in 8 of its 20 ticks a second. */
const float WalkSpeed = Knob("DEVILUTIONX_VR_WALK_SPEED", 2.5F);
/** How much faster the player goes in town, where the game lets the character jog. */
const float TownPace = Knob("DEVILUTIONX_VR_TOWN_PACE", 1.5F);
bool gliding;
float vignetteNow;

/**
 * Debug aid (DEVILUTIONX_VR_DEBUG_HANDS=1): with nobody holding the controllers, hands held out in
 * front at waist height, the right one pointing ahead and a little down, so the laser and the
 * wrists reach the compositor in unattended runs.
 */
const bool DebugHands = Knob("DEVILUTIONX_VR_DEBUG_HANDS", 0) != 0;

bool LocateHand(Hand hand, XrTime time, XrPosef &pose, XrSpaceVelocity *velocity)
{
	if (Locate(aimSpace[hand], time, pose, velocity))
		return true;
	XrPosef head;
	if (!DebugHands || !Locate(viewSpace, time, head))
		return false;
	const XrVector3f forward = Rotate(head.orientation, { 0, 0, -1 });
	const float yaw = std::atan2(-forward.x, -forward.z);
	const XrQuaternionf yawOnly { 0, std::sin(yaw / 2), 0, std::cos(yaw / 2) };
	const float pitch = hand == Right ? -0.12F : -0.6F;
	pose.orientation = Multiply(yawOnly, { std::sin(pitch / 2), 0, 0, std::cos(pitch / 2) });
	const XrVector3f offset = Rotate(yawOnly, { hand == Right ? 0.2F : -0.2F, -0.45F, -0.35F });
	pose.position = { head.position.x + offset.x, head.position.y + offset.y, head.position.z + offset.z };
	if (velocity != nullptr)
		velocity->velocityFlags = 0;
	return true;
}

XrAction CreateAction(const char *name, XrActionType type, bool perHand)
{
	XrActionCreateInfo info { XR_TYPE_ACTION_CREATE_INFO };
	info.actionType = type;
	SDL_strlcpy(info.actionName, name, sizeof(info.actionName));
	SDL_strlcpy(info.localizedActionName, name, sizeof(info.localizedActionName));
	if (perHand) {
		info.countSubactionPaths = 2;
		info.subactionPaths = handPath;
	}
	XrAction action = XR_NULL_HANDLE;
	Check(xrCreateAction(actionSet, &info, &action), name);
	return action;
}

void SuggestBindings(const char *profile, const std::vector<XrActionSuggestedBinding> &bindings)
{
	XrInteractionProfileSuggestedBinding suggested { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	suggested.interactionProfile = Path(profile);
	suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
	suggested.suggestedBindings = bindings.data();
	Check(xrSuggestInteractionProfileBindings(instance, &suggested), profile);
}

bool InitActions()
{
	XrActionSetCreateInfo setInfo { XR_TYPE_ACTION_SET_CREATE_INFO };
	SDL_strlcpy(setInfo.actionSetName, "gameplay", sizeof(setInfo.actionSetName));
	SDL_strlcpy(setInfo.localizedActionSetName, "Gameplay", sizeof(setInfo.localizedActionSetName));
	if (!Check(xrCreateActionSet(instance, &setInfo, &actionSet), "xrCreateActionSet"))
		return false;

	handPath[Left] = Path("/user/hand/left");
	handPath[Right] = Path("/user/hand/right");

	aimAction = CreateAction("aim", XR_ACTION_TYPE_POSE_INPUT, true);
	triggerAction = CreateAction("trigger", XR_ACTION_TYPE_FLOAT_INPUT, true);
	squeezeAction = CreateAction("squeeze", XR_ACTION_TYPE_FLOAT_INPUT, true);
	stickAction = CreateAction("stick", XR_ACTION_TYPE_VECTOR2F_INPUT, true);
	stickClickAction = CreateAction("stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, true);
	aAction = CreateAction("a", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	bAction = CreateAction("b", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	xAction = CreateAction("x", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	yAction = CreateAction("y", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	menuAction = CreateAction("menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	hapticAction = CreateAction("haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, true);
	bumperAction = CreateAction("bumper", XR_ACTION_TYPE_BOOLEAN_INPUT, true);
	viewAction = CreateAction("view", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	dpadAction[0] = CreateAction("dpad_up", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	dpadAction[1] = CreateAction("dpad_down", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	dpadAction[2] = CreateAction("dpad_left", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
	dpadAction[3] = CreateAction("dpad_right", XR_ACTION_TYPE_BOOLEAN_INPUT, false);

	// Touch + Khronos simple profiles only (Quest, and whatever runtimes
	// remap onto Touch). Add Index / WMR / Vive binding tables here when needed.
	SuggestBindings("/interaction_profiles/oculus/touch_controller",
	    {
	        { aimAction, Path("/user/hand/left/input/aim/pose") },
	        { aimAction, Path("/user/hand/right/input/aim/pose") },
	        { triggerAction, Path("/user/hand/left/input/trigger/value") },
	        { triggerAction, Path("/user/hand/right/input/trigger/value") },
	        { squeezeAction, Path("/user/hand/left/input/squeeze/value") },
	        { squeezeAction, Path("/user/hand/right/input/squeeze/value") },
	        { stickAction, Path("/user/hand/left/input/thumbstick") },
	        { stickAction, Path("/user/hand/right/input/thumbstick") },
	        { stickClickAction, Path("/user/hand/left/input/thumbstick/click") },
	        { stickClickAction, Path("/user/hand/right/input/thumbstick/click") },
	        { aAction, Path("/user/hand/right/input/a/click") },
	        { bAction, Path("/user/hand/right/input/b/click") },
	        { xAction, Path("/user/hand/left/input/x/click") },
	        { yAction, Path("/user/hand/left/input/y/click") },
	        { menuAction, Path("/user/hand/left/input/menu/click") },
	        { hapticAction, Path("/user/hand/left/output/haptic") },
	        { hapticAction, Path("/user/hand/right/output/haptic") },
	    });
	SuggestBindings("/interaction_profiles/khr/simple_controller",
	    {
	        { aimAction, Path("/user/hand/left/input/aim/pose") },
	        { aimAction, Path("/user/hand/right/input/aim/pose") },
	        { triggerAction, Path("/user/hand/left/input/select/click") },
	        { triggerAction, Path("/user/hand/right/input/select/click") },
	        { menuAction, Path("/user/hand/left/input/menu/click") },
	        { menuAction, Path("/user/hand/right/input/menu/click") },
	        { hapticAction, Path("/user/hand/left/output/haptic") },
	        { hapticAction, Path("/user/hand/right/output/haptic") },
	    });

	// The Steam Frame's controllers: the D-pad and View are on the left, all four face buttons
	// and Menu on the right. Buttons keep the names printed on them, so A, B, X and Y do what
	// they do on Touch, and what Touch has no button for goes to the game as the gamepad's
	// own: bumpers (the belt's potions), the D-pad and View (Select).
	if (hasFrameControllers) {
		SuggestBindings("/interaction_profiles/valve/frame_controller_valve",
		    {
		        { aimAction, Path("/user/hand/left/input/aim/pose") },
		        { aimAction, Path("/user/hand/right/input/aim/pose") },
		        { triggerAction, Path("/user/hand/left/input/trigger/value") },
		        { triggerAction, Path("/user/hand/right/input/trigger/value") },
		        { squeezeAction, Path("/user/hand/left/input/squeeze/value") },
		        { squeezeAction, Path("/user/hand/right/input/squeeze/value") },
		        { stickAction, Path("/user/hand/left/input/thumbstick") },
		        { stickAction, Path("/user/hand/right/input/thumbstick") },
		        { stickClickAction, Path("/user/hand/left/input/thumbstick/click") },
		        { stickClickAction, Path("/user/hand/right/input/thumbstick/click") },
		        { aAction, Path("/user/hand/right/input/a/click") },
		        { bAction, Path("/user/hand/right/input/b/click") },
		        { xAction, Path("/user/hand/right/input/x/click") },
		        { yAction, Path("/user/hand/right/input/y/click") },
		        { menuAction, Path("/user/hand/right/input/menu/click") },
		        { viewAction, Path("/user/hand/left/input/view/click") },
		        { bumperAction, Path("/user/hand/left/input/bumper/click") },
		        { bumperAction, Path("/user/hand/right/input/bumper/click") },
		        { dpadAction[0], Path("/user/hand/left/input/dpad_up/click") },
		        { dpadAction[1], Path("/user/hand/left/input/dpad_down/click") },
		        { dpadAction[2], Path("/user/hand/left/input/dpad_left/click") },
		        { dpadAction[3], Path("/user/hand/left/input/dpad_right/click") },
		        { hapticAction, Path("/user/hand/left/output/haptic") },
		        { hapticAction, Path("/user/hand/right/output/haptic") },
		    });
	}

	for (const Hand hand : { Left, Right }) {
		XrActionSpaceCreateInfo spaceInfo { XR_TYPE_ACTION_SPACE_CREATE_INFO };
		spaceInfo.action = aimAction;
		spaceInfo.subactionPath = handPath[hand];
		spaceInfo.poseInActionSpace.orientation.w = 1;
		if (!Check(xrCreateActionSpace(session, &spaceInfo, &aimSpace[hand]), "xrCreateActionSpace"))
			return false;
	}

	XrSessionActionSetsAttachInfo attachInfo { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	attachInfo.countActionSets = 1;
	attachInfo.actionSets = &actionSet;
	return Check(xrAttachSessionActionSets(session, &attachInfo), "xrAttachSessionActionSets");
}

void ControllersChanged()
{
	XrInteractionProfileState profile { XR_TYPE_INTERACTION_PROFILE_STATE };
	char name[XR_MAX_PATH_LENGTH] = "none";
	uint32_t length = 0;
	if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(session, handPath[Right], &profile)) && profile.interactionProfile != XR_NULL_PATH)
		xrPathToString(instance, profile.interactionProfile, sizeof(name), &length, name);
	frameControllers = SDL_strcmp(name, "/interaction_profiles/valve/frame_controller_valve") == 0;
	LogInfo("VR: controllers: {}", name);
}

bool InitVirtualPad()
{
	if (virtualPad != nullptr)
		return true;

	// The desktop window rarely has focus while the headset is worn.
	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

	SDL_VirtualJoystickDesc desc;
	SDL_zero(desc);
	desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
	desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
	desc.naxes = SDL_CONTROLLER_AXIS_MAX;
	desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
	desc.name = "VR Motion Controllers";
	const int index = SDL_JoystickAttachVirtualEx(&desc);
	if (index < 0 || (virtualPad = SDL_JoystickOpen(index)) == nullptr) {
		LogError("VR: virtual controller unavailable: {}", SDL_GetError());
		return false;
	}
	return true;
}

bool RayToQuad(const XrPosef &aim, const XrPosef &screenPose, float quadWidth, float screenHeight, const SDL_Rect &quadRect, int &x, int &y, float *hitDistance);

/**
 * Intersects the aim ray with the screen quad (or the second one). Returns game-surface pixel
 * coordinates, and how far along the ray the hit is.
 */
bool RayToScreen(const XrPosef &aim, float screenHeight, int &x, int &y, float *hitDistance = nullptr)
{
	if (RayToQuad(aim, screenPose, quadWidth, screenHeight, quadRect, x, y, hitDistance))
		return true;
	return extraWidth > 0 && extraRect.w > 0
	    && RayToQuad(aim, extraPose, extraWidth, extraWidth * static_cast<float>(extraRect.h) / static_cast<float>(extraRect.w), extraRect, x, y, hitDistance);
}

bool RayToQuad(const XrPosef &aim, const XrPosef &screenPose, float quadWidth, float screenHeight, const SDL_Rect &quadRect, int &x, int &y, float *hitDistance)
{
	const XrVector3f direction = Rotate(aim.orientation, { 0, 0, -1 });
	const XrVector3f normal = Rotate(screenPose.orientation, { 0, 0, 1 });
	const float facing = Dot(direction, normal);
	if (facing >= -1e-4F)
		return false; // Parallel to the screen or pointing at its back.
	const XrVector3f toScreen {
		screenPose.position.x - aim.position.x,
		screenPose.position.y - aim.position.y,
		screenPose.position.z - aim.position.z,
	};
	const float distance = Dot(toScreen, normal) / facing;
	if (distance <= 0)
		return false;
	const XrVector3f onScreen {
		direction.x * distance - toScreen.x,
		direction.y * distance - toScreen.y,
		direction.z * distance - toScreen.z,
	};
	const float u = Dot(onScreen, Rotate(screenPose.orientation, { 1, 0, 0 })) / quadWidth + 0.5F;
	const float v = 0.5F - Dot(onScreen, Rotate(screenPose.orientation, { 0, 1, 0 })) / screenHeight;
	if (u < 0 || u >= 1 || v < 0 || v >= 1)
		return false;
	if (hitDistance != nullptr)
		*hitDistance = distance;
	if (quadRect.w > 0) {
		x = quadRect.x + static_cast<int>(u * static_cast<float>(quadRect.w));
		y = quadRect.y + static_cast<int>(v * static_cast<float>(quadRect.h));
	} else {
		x = static_cast<int>(u * static_cast<float>(swapchainWidth));
		y = static_cast<int>(v * static_cast<float>(swapchainHeight));
	}
	return true;
}

/** Debug-build sanity check of the pointer math: a level ray at the screen centre hits the middle pixel. */
void SelfCheck()
{
#ifndef NDEBUG
	const XrPosef savedPose = screenPose;
	const int savedWidth = swapchainWidth;
	const int savedHeight = swapchainHeight;
	swapchainWidth = 640;
	swapchainHeight = 480;
	const float yaw = 0.5F; // Screen placed for a head turned 0.5 rad to the left.
	screenPose.orientation = { 0, std::sin(yaw / 2), 0, std::cos(yaw / 2) };
	screenPose.position = { -std::sin(yaw) * ScreenDistance, 0, -std::cos(yaw) * ScreenDistance };
	int x = -1;
	int y = -1;
	const XrPosef aim { screenPose.orientation, { 0, 0, 0 } };
	assert(RayToScreen(aim, ScreenWidth * 0.75F, x, y) && x == 320 && y == 240);
	const XrPosef away { { 0, std::sin((yaw + 3.14159F) / 2), 0, std::cos((yaw + 3.14159F) / 2) }, { 0, 0, 0 } };
	assert(!RayToScreen(away, ScreenWidth * 0.75F, x, y));
	screenPose = savedPose;
	swapchainWidth = savedWidth;
	swapchainHeight = savedHeight;
#endif
}

/**
 * Points the game's cursor at whatever the aim ray hits in the 3D world. The game keeps
 * doing its own targeting (CheckCursMove) from a mouse position, so the hit tile is
 * converted to where that tile sits in the 2D frame.
 */
void AimAtWorld(const XrPosef &aim)
{
	float origin[3];
	float orientation[4];
	ToWorld(aim, origin, orientation);
	const XrVector3f ray = Rotate({ orientation[0], orientation[1], orientation[2], orientation[3] }, { 0, 0, -1 });
	const float direction[3] = { ray.x, ray.y, ray.z };
	const float flat = std::sqrt(ray.x * ray.x + ray.z * ray.z);
	if (flat > 1e-3F) {
		aimDirX = ray.x / flat;
		aimDirZ = ray.z / flat;
	}
	int tileX;
	int tileY;
	if (!VrWorldPick(origin, direction, tileX, tileY, worldPointerHit))
		return;
	worldPointerValid = true;
	Point screen = GetScreenPosition({ tileX, tileY }) + Displacement { TILE_WIDTH / 2, -TILE_HEIGHT / 2 };
	if (*GetOptions().Graphics.zoom) {
		screen.x *= 2;
		screen.y *= 2;
	}
	// tiles outside the 2D frame (or under an open side panel) cannot be targeted.
	// Send targets to the game directly instead of through the cursor when that gets in the way.
	if (screen.x < 0 || screen.y < 0 || screen.x >= gnScreenWidth || screen.y >= gnViewportHeight)
		return;
	pointerX = screen.x;
	pointerY = screen.y;
}

float FloatState(XrAction action, Hand hand)
{
	XrActionStateGetInfo info { XR_TYPE_ACTION_STATE_GET_INFO };
	info.action = action;
	info.subactionPath = handPath[hand];
	XrActionStateFloat value { XR_TYPE_ACTION_STATE_FLOAT };
	xrGetActionStateFloat(session, &info, &value);
	return value.isActive ? value.currentState : 0;
}

XrVector2f StickState(Hand hand)
{
	XrActionStateGetInfo info { XR_TYPE_ACTION_STATE_GET_INFO };
	info.action = stickAction;
	info.subactionPath = handPath[hand];
	XrActionStateVector2f value { XR_TYPE_ACTION_STATE_VECTOR2F };
	xrGetActionStateVector2f(session, &info, &value);
	return value.isActive ? value.currentState : XrVector2f { 0, 0 };
}

bool BoolState(XrAction action, XrPath subactionPath = XR_NULL_PATH)
{
	XrActionStateGetInfo info { XR_TYPE_ACTION_STATE_GET_INFO };
	info.action = action;
	info.subactionPath = subactionPath;
	XrActionStateBoolean value { XR_TYPE_ACTION_STATE_BOOLEAN };
	xrGetActionStateBoolean(session, &info, &value);
	return value.isActive && value.currentState;
}

/** The game reads mouse coordinates in window space and lets the SDL renderer scale them. */
void SurfaceToWindow(int &x, int &y)
{
	if (renderer != nullptr)
		SDL_RenderLogicalToWindow(renderer, static_cast<float>(x), static_cast<float>(y), &x, &y);
}

void PushMouseMotion()
{
	SDL_Event event;
	SDL_zero(event);
	event.type = SDL_MOUSEMOTION;
	event.motion.windowID = SDL_GetWindowID(ghMainWnd);
	event.motion.x = pointerX;
	event.motion.y = pointerY;
	sentX = pointerX;
	sentY = pointerY;
	SurfaceToWindow(event.motion.x, event.motion.y);
	SDL_PushEvent(&event);
}

void SetMouseButton(bool &isDown, bool down, Uint8 button)
{
	if (isDown == down)
		return;
	// Never start a click while pointing off-screen, but always deliver the release.
	if (down && pointerX < 0)
		return;
	isDown = down;
	if (down)
		PushMouseMotion(); // Make sure the click lands where the ray is right now.
	SDL_Event event;
	SDL_zero(event);
	event.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
	event.button.windowID = SDL_GetWindowID(ghMainWnd);
	event.button.button = button;
	event.button.state = down ? SDL_PRESSED : SDL_RELEASED;
	event.button.clicks = 1;
	event.button.x = std::max(pointerX, 0);
	event.button.y = std::max(pointerY, 0);
	SurfaceToWindow(event.button.x, event.button.y);
	SDL_PushEvent(&event);
}

void SetKey(bool &isDown, bool down, SDL_Keycode key)
{
	if (isDown == down)
		return;
	isDown = down;
	SDL_Event event;
	SDL_zero(event);
	event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
	event.key.windowID = SDL_GetWindowID(ghMainWnd);
	event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
	event.key.keysym.sym = key;
	event.key.keysym.scancode = SDL_GetScancodeFromKey(key);
	SDL_PushEvent(&event);
}

// Any pad event flips the game to gamepad controls and any mouse motion flips it
// back, so analog noise from a resting stick or trigger must not produce events.
Sint16 AxisValue(float value)
{
	if (std::fabs(value) < 0.2F)
		return 0;
	return static_cast<Sint16>(std::clamp(value, -1.0F, 1.0F) * 32767.0F);
}

/** Virtual triggers are full-range axes that rest at the minimum. The game only uses them as buttons. */
Sint16 TriggerValue(float value)
{
	return value > 0.5F ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN;
}

/** Tile under a world position. */
Point TileUnder(float x, float z)
{
	const float tile = VrWorldTileSize();
	return { static_cast<int>(std::floor(x / tile)), static_cast<int>(std::floor(z / tile)) };
}

/**
 * Where the player stands. In single player that is a free position the stick moves in any
 * direction at any speed (Glide), with the character kept on the tile underneath. The game takes
 * it back whenever it moves the character itself: a walk it ordered (a click on something to
 * reach), a teleport, a new level, a multiplayer game.
 */
void SyncFreePosition()
{
	const Player &player = *MyPlayer;
	const Point under = TileUnder(freeX, freeZ);
	if (!freeValid || !FreeMovement || gbIsMultiplayer || player.isWalking() || (under != player.position.tile && under != player.position.future)) {
		VrWorldPlayerPosition(freeX, freeZ);
		freeValid = true;
	}
	worldOrigin[0] = freeX;
	worldOrigin[1] = floorOffset + SeatedRaise();
	worldOrigin[2] = freeZ;
}

void Pulse(Hand hand, float amplitude, int milliseconds)
{
	// Scaled by the Haptics setting; off at 0.
	const int strength = *Settings().haptics;
	if (strength <= 0)
		return;
	XrHapticVibration vibration { XR_TYPE_HAPTIC_VIBRATION };
	vibration.amplitude = std::clamp(amplitude * static_cast<float>(strength) / 100.F, 0.F, 1.F);
	vibration.duration = static_cast<XrDuration>(milliseconds) * 1000 * 1000;
	vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
	XrHapticActionInfo hapticInfo { XR_TYPE_HAPTIC_ACTION_INFO };
	hapticInfo.action = hapticAction;
	hapticInfo.subactionPath = handPath[hand];
	xrApplyHapticFeedback(session, &hapticInfo, reinterpret_cast<XrHapticBaseHeader *>(&vibration));
}

/**
 * Walking is the character walking: the stick's direction, turned into the nearest of the
 * game's eight, tells the character to walk on; the game moves it at its own speed and the view
 * goes with it (SyncFreePosition). Letting go stops it on the next tile.
 * @param dirX, dirZ world-space direction scaled by stick deflection
 */
bool walkHeld;
void Walk(float dirX, float dirZ)
{
	const Uint32 now = SDL_GetTicks();
	const bool frozen = gmenu_is_active() || IsPlayerInStore() || qtextflag || PauseMode != 0 || MyPlayerIsDead;
	const Player &player = *MyPlayer;
	const float length = std::sqrt(dirX * dirX + dirZ * dirZ);
	const bool attacking = now - lastAttackAt < 600;
	if (frozen || length < 0.3F) {
		if (walkHeld && !frozen) {
			// Stop where the step under way ends.
			NetSendCmdLoc(MyPlayerId, true, CMD_WALKXY, player.position.future);
			walkTargetX = walkTargetY = -1;
		}
		walkHeld = false;
		return;
	}
	walkHeld = true;
	const float angle = std::atan2(dirZ, dirX);
	const Displacement step { static_cast<int>(std::lround(std::cos(angle))), static_cast<int>(std::lround(std::sin(angle))) };
	// Two steps ahead keeps the walk going without a stop at every tile; one if the second is blocked.
	Point target = player.position.future + step + step;
	if (!InDungeonBounds(target) || !IsTileWalkable(target))
		target = player.position.future + step;
	if (!InDungeonBounds(target) || !IsTileWalkable(target) || attacking)
		return;
	if (target.x != walkTargetX || target.y != walkTargetY || now - walkSentAt > 300) {
		NetSendCmdLoc(MyPlayerId, true, CMD_WALKXY, target);
		walkTargetX = target.x;
		walkTargetY = target.y;
		walkSentAt = now;
	}
}

/** The stick as QuestZDoom reads it: a dead zone, then a curve that makes small movements easy to make. */
float StickCurve(float deflection)
{
	constexpr float DeadZone = 0.1F;
	if (deflection <= DeadZone)
		return 0;
	return std::pow((std::min(deflection, 1.F) - DeadZone) / (1 - DeadZone), 2.2F);
}

/**
 * Free movement: the stick moves the player over the ground in any direction, as fast as it is
 * pushed, sliding along walls. No tiles, no steps. The character is put on whichever tile the
 * player is over (VrWorldPlacePlayer), so the game's fights, doors, stairs and light go on as
 * they do. A swing, a cast or a blow taken roots the player for as long as it roots the character.
 * @param dirX, dirZ world-space direction scaled by stick deflection
 * @return whether the player moved
 */
bool Glide(float dirX, float dirZ, float dt)
{
	const bool frozen = gmenu_is_active() || IsPlayerInStore() || qtextflag || PauseMode != 0 || MyPlayerIsDead;
	const float deflection = std::sqrt(dirX * dirX + dirZ * dirZ);
	const float pace = StickCurve(deflection);
	if (frozen || pace <= 0 || !VrWorldPlayerCanGlide())
		return false;
	if (MyPlayer->isWalking()) {
		// A walk the game had under way ends where it is; the stick has it from here.
		if (!VrWorldPlacePlayer(freeX, freeZ, dirX, dirZ))
			return false;
	}
	const float speed = WalkSpeed * VrWorldTileSize() * (leveltype == DTYPE_TOWN ? TownPace : 1.F);
	const float stepX = dirX / deflection * pace * speed * dt;
	const float stepZ = dirZ / deflection * pace * speed * dt;
	const float beforeX = freeX;
	const float beforeZ = freeZ;
	// Axis by axis, so walking into a wall at an angle slides along it.
	if (VrWorldCanStand(freeX + stepX, freeZ))
		freeX += stepX;
	if (VrWorldCanStand(freeX, freeZ + stepZ))
		freeZ += stepZ;
	if (!VrWorldPlacePlayer(freeX, freeZ, dirX, dirZ)) {
		freeX = beforeX;
		freeZ = beforeZ;
	}
	worldOrigin[0] = freeX;
	worldOrigin[2] = freeZ;
	return freeX != beforeX || freeZ != beforeZ;
}

/**
 * Attacks in a direction, without walking. Melee goes for the monster next to the character
 * that is best lined up with that direction, or swings at the tile that way; a bow shoots
 * at the aimed monster or along the direction. Hit chance, damage and speed are the game's.
 * @param dirX, dirZ unit direction on the ground (world x = tile x, world z = tile y)
 */
void Attack(float dirX, float dirZ)
{
	const Uint32 now = SDL_GetTicks();
	if (leveltype == DTYPE_TOWN || now - lastAttackAt < 250)
		return;
	lastAttackAt = now;
	const Player &player = *MyPlayer;
	const Point origin = player.position.future;
	if (player.UsesRangedWeapon()) {
		if (pcursmonst != -1 && worldPointerValid)
			NetSendCmdParam1(true, CMD_RATTACKID, static_cast<uint16_t>(pcursmonst));
		else
			NetSendCmdLoc(MyPlayerId, true, CMD_RATTACKXY, origin + Displacement { static_cast<int>(std::lround(dirX * 4)), static_cast<int>(std::lround(dirZ * 4)) });
		return;
	}

	int target = -1;
	float bestAlignment = 0.25F; // Anything in the front half-ish.
	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			const Point tile = origin + Displacement { dx, dy };
			if ((dx == 0 && dy == 0) || !InDungeonBounds(tile) || dMonster[tile.x][tile.y] == 0)
				continue;
			const float alignment = (static_cast<float>(dx) * dirX + static_cast<float>(dy) * dirZ) / std::sqrt(static_cast<float>(dx * dx + dy * dy));
			if (alignment > bestAlignment) {
				bestAlignment = alignment;
				target = std::abs(dMonster[tile.x][tile.y]) - 1;
			}
		}
	}
	if (target >= 0) {
		NetSendCmdParam1(true, CMD_ATTACKID, static_cast<uint16_t>(target));
		// The hand will feel whether it hit: the game decides, its life tells.
		swingTarget = target;
		swingTargetLife = Monsters[target].hitPoints;
		swingAt = now;
		return;
	}
	// Nothing in reach: swing at the neighbouring tile in that direction (one of 8).
	const float angle = std::atan2(dirZ, dirX);
	const Displacement step { static_cast<int>(std::lround(std::cos(angle))), static_cast<int>(std::lround(std::sin(angle))) };
	NetSendCmdLoc(MyPlayerId, true, CMD_SATTACKXY, origin + step);
}

/** Where the head looks, flattened to the ground plane, in world space. */
bool HeadForward(XrTime time, float &x, float &z)
{
	XrPosef head;
	if (!Locate(viewSpace, time, head))
		return false;
	float position[3];
	float orientation[4];
	ToWorld(head, position, orientation);
	const XrVector3f forward = Rotate({ orientation[0], orientation[1], orientation[2], orientation[3] }, { 0, 0, -1 });
	const float length = std::sqrt(forward.x * forward.x + forward.z * forward.z);
	if (length < 1e-3F)
		return false;
	x = forward.x / length;
	z = forward.z / length;
	return true;
}

/**
 * Test key, F9 on the PC's keyboard (the game's window must have the keyboard): puts the player
 * outside the Butcher's door on dungeon level 2, facing it, going to that level first when
 * somewhere else. The quest is put back to its start, so the room and the Butcher are there
 * whatever the save says.
 */
void ButcherShortcut()
{
	static bool wasDown;
	static int waiting;
	const bool down = SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_F9] != 0;
	const bool pressed = down && !wasDown;
	wasDown = down;
	if (!gbRunGame || MyPlayer == nullptr || gbIsMultiplayer)
		return;
	if (pressed) {
		waiting = 600;
		if (currlevel != 2 || setlevel || Quests[Q_BUTCHER]._qactive == QUEST_NOTAVAIL || Quests[Q_BUTCHER]._qactive == QUEST_DONE) {
			Quests[Q_BUTCHER]._qactive = QUEST_INIT;
			StartNewLvl(*MyPlayer, WM_DIABNEXTLVL, 2);
			LogInfo("VR: F9, going to level 2 for the Butcher");
			return;
		}
	}
	if (waiting == 0)
		return;
	waiting--;
	if (currlevel != 2 || setlevel || !VrWorldPlayerCanGlide())
		return;
	const Monster *butcher = nullptr;
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const Monster &monster = Monsters[ActiveMonsters[i]];
		if (monster.type().type == MT_CLEAVER)
			butcher = &monster;
	}
	if (butcher == nullptr) {
		if (waiting == 0)
			LogInfo("VR: F9, no Butcher on this level");
		return;
	}
	// His room has one door: the nearest to him.
	const Object *door = nullptr;
	int nearest = 1 << 20;
	for (int i = 0; i < ActiveObjectCount; i++) {
		const Object &object = Objects[ActiveObjects[i]];
		const int away = std::abs(object.position.x - butcher->position.tile.x) + std::abs(object.position.y - butcher->position.tile.y);
		if (object.isDoor() && away < nearest) {
			nearest = away;
			door = &object;
		}
	}
	if (door == nullptr || nearest > 12) {
		waiting = 0;
		LogInfo("VR: F9, the Butcher has no door near him");
		return;
	}
	// Outside is the side of the door away from him, along the way the door is gone through.
	const Point from = butcher->position.tile;
	const Point at = door->position;
	const int dx = at.x - from.x;
	const int dy = at.y - from.y;
	const Point steps[2] = { std::abs(dx) >= std::abs(dy) ? Point { dx > 0 ? 1 : -1, 0 } : Point { 0, dy > 0 ? 1 : -1 },
		std::abs(dx) >= std::abs(dy) ? Point { 0, dy >= 0 ? 1 : -1 } : Point { dx >= 0 ? 1 : -1, 0 } };
	const float tile = VrWorldTileSize();
	for (const Point step : steps) {
		for (int reach = 1; reach <= 3; reach++) {
			const Point spot { at.x + step.x * reach, at.y + step.y * reach };
			if (!PosOkPlayer(*MyPlayer, spot))
				continue;
			if (VrWorldPlacePlayer((static_cast<float>(spot.x) + 0.5F) * tile, (static_cast<float>(spot.y) + 0.5F) * tile, static_cast<float>(-step.x), static_cast<float>(-step.y))) {
				LogInfo("VR: F9, outside the Butcher's door at tile {}, {} (door {}, {})", spot.x, spot.y, at.x, at.y);
				waiting = 0;
				return;
			}
		}
	}
	if (waiting == 0)
		LogInfo("VR: F9, no free tile outside the Butcher's door");
}

/** Belt Grab: the belt slot held in a hand (-1: none) and which hand. */
int beltSlot = -1;
Hand beltHand = Right;
bool beltGrip[2];

bool IsPotion(const Item &item)
{
	return IsAnyOf(item._iMiscId, IMISC_FULLHEAL, IMISC_HEAL, IMISC_MANA, IMISC_FULLMANA, IMISC_REJUV, IMISC_FULLREJUV);
}

/**
 * Belt Grab: a grip closing by a potion on the belt (worn round the waist, AddBelt) takes it in
 * that hand; brought to the mouth, it is drunk, as clicking it on the belt would; let go anywhere
 * else, it goes back. Returns, per hand, whether its grip is busy with the belt, so that it does
 * not also grab in the world or open the speedbook.
 */
std::array<bool, 2> UpdateBeltGrab(XrTime time)
{
	std::array<bool, 2> busy {};
	XrPosef head;
	const bool usable = *Settings().beltGrab && world3d && !UiOpen() && MyPlayer != nullptr && Locate(viewSpace, time, head);
	if (!usable) {
		beltSlot = -1;
		VrWorldSetBeltHold({ -1, 0 });
		return busy;
	}
	// The belt as AddBelt draws it, in tracking space: round the front of the waist.
	const XrVector3f look = Rotate(head.orientation, { 0, 0, -1 });
	const float flat = std::max(std::sqrt(look.x * look.x + look.z * look.z), 1e-3F);
	const XrVector3f forward { look.x / flat, 0, look.z / flat };
	// The mouth: a little below and in front of the eyes.
	const XrVector3f down = Rotate(head.orientation, { 0, -1, 0 });
	const XrVector3f mouth { head.position.x + look.x * 0.07F + down.x * 0.1F, head.position.y + look.y * 0.07F + down.y * 0.1F, head.position.z + look.z * 0.07F + down.z * 0.1F };
	const auto point = [](const XrVector3f &v) { return rules::Point3 { v.x, v.y, v.z }; };
	for (const Hand hand : { Left, Right }) {
		XrPosef pose;
		const bool tracked = LocateHand(hand, time, pose);
		const bool holding = beltSlot >= 0 && beltHand == hand;
		const bool grip = tracked && FloatState(squeezeAction, hand) > (holding || beltGrip[hand] ? 0.3F : 0.6F);
		const bool closed = grip && !beltGrip[hand];
		beltGrip[hand] = grip;
		if (holding) {
			busy[hand] = true;
			const Item &item = MyPlayer->SpdList[beltSlot];
			if (item.isEmpty()) {
				beltSlot = -1;
			} else if (tracked && rules::Distance(point(pose.position), point(mouth)) < rules::MouthReach) {
				UseInvItem(INVITEM_BELT_FIRST + beltSlot);
				beltSlot = -1;
			} else if (!grip) {
				beltSlot = -1; // Back on the belt.
				Pulse(hand, 0.15F, 15);
			}
			continue;
		}
		if (!closed || beltSlot >= 0)
			continue;
		uint32_t potions = 0;
		for (int slot = 0; slot < MaxBeltItems; slot++) {
			const Item &item = MyPlayer->SpdList[slot];
			if (!item.isEmpty() && IsPotion(item))
				potions |= 1U << slot;
		}
		const int nearest = rules::NearestBeltSlot(point(pose.position), point(head.position), forward.x, forward.z, potions);
		if (nearest >= 0) {
			beltSlot = nearest;
			beltHand = hand;
			busy[hand] = true;
			Pulse(hand, 0.4F, 30);
		}
	}
	VrWorldSetBeltHold({ beltSlot, beltHand == Left ? 0 : 1 });
	return busy;
}

/** Spell Gestures: casting waits for a flick of the casting hand while its trigger is held. */
int castClick;
Uint32 lastCast;
Uint32 lastCharge;
Uint32 aimingSince;

void UpdateInput(XrTime time, float screenHeight)
{
	// The step from one frame to the next, from the times the frames are shown at. The game's
	// own clock counts whole milliseconds, which at 72 frames a second makes every step 13 or
	// 14 of them, and walking and turning shake by that much.
	static XrTime lastTime;
	const float frameStep = std::clamp(static_cast<float>(time - lastTime) * 1e-9F, 0.F, 0.05F);
	lastTime = time;

	ButcherShortcut();
	XrActiveActionSet activeSet { actionSet, XR_NULL_PATH };
	XrActionsSyncInfo syncInfo { XR_TYPE_ACTIONS_SYNC_INFO };
	syncInfo.countActiveActionSets = 1;
	syncInfo.activeActionSets = &activeSet;
	if (xrSyncActions(session, &syncInfo) != XR_SUCCESS && !DebugHands)
		return; // Not focused (e.g. the system menu is open).

	const XrVector2f leftStick = StickState(Left);
	const XrVector2f rightStick = StickState(Right);
	// The sticks and buttons stay where they are; what a hand does follows the Dominant Hand
	// setting. Below, "right" and "left" for a hand's pose, trigger and grip mean the dominant
	// hand and the other one.
	const Hand main = LeftHanded() ? Left : Right;
	const Hand off = LeftHanded() ? Right : Left;

	// Right thumbstick click: bring the screen back in front of the player.
	// Left thumbstick click: automap. Both pressed in together: a screenshot of what the left eye
	// sees. A stick therefore does its own job when it comes up again, and only if the other one
	// stayed up meanwhile.
	const bool recenter = BoolState(stickClickAction, handPath[Right]);
	const bool automap = BoolState(stickClickAction, handPath[Left]);
	if (recenter && automap && !stickChord) {
		stickChord = true;
		screenshotRequested = true;
		Pulse(Left, 0.5F, 120);
		Pulse(Right, 0.5F, 120);
	}
	if (!recenter && recenterDown && !stickChord) {
		screenPlaced = false;
		anchorPending = true;
	}
	if (!automap && automapDown && !stickChord)
		automapPulse = 6; // Frames the game's automap button is held down for.
	if (!recenter && !automap)
		stickChord = false;
	recenterDown = recenter;
	automapDown = automap;

	// The Frame's View button. Held for a second it re-centres as the right stick's click does,
	// with a buzz in both hands to say so. A short press is the gamepad's Select in the menus
	// (back); in a game it closes what is open, or brings up the help with the controls.
	const rules::ViewEvent viewEvent = rules::StepView(viewButton, BoolState(viewAction), SDL_GetTicks());
	if (viewEvent == rules::ViewEvent::Recentre) {
		screenPlaced = false;
		anchorPending = true;
		Pulse(Left, 0.6F, 80);
		Pulse(Right, 0.6F, 80);
	}
	if (viewEvent == rules::ViewEvent::Tap) {
		if (!world3d)
			backPulse = 6;
		else if (HelpFlag)
			HelpFlag = false;
		else if (!UiOpen() && gbRunGame)
			DisplayHelp();
		else
			escapePulse = 2;
	}

	// Right hand: pointer and weapon swing. While the left trigger casts, the left hand aims
	// instead. The left hand also points where the right one misses: at the right wrist, or at
	// a screen or panel the right hand is not on.
	XrPosef aim;
	XrPosef leftAim;
	XrSpaceVelocity velocity;
	float speed = 0;
	XrVector3f swingDirection { 0, 0, 0 };
	worldPointerValid = false;
	laser = {};
	XrSpaceVelocity offVelocity {};
	const bool leftValid = LocateHand(off, time, leftAim, &offVelocity);
	const bool rightValid = LocateHand(main, time, aim, &velocity);
	float hitDistance = 0;
	// A bow being drawn keeps both hands for the bow, whatever panel they cross.
	const bool panelsReachable = quadVisible && !drawingBow;
	pointerOnQuad = rightValid && panelsReachable && RayToScreen(aim, screenHeight, pointerX, pointerY, &hitDistance);
	leftPointing = !pointerOnQuad && leftValid && panelsReachable && (!world3d || uiMode || WristHud())
	    && RayToScreen(leftAim, screenHeight, pointerX, pointerY, &hitDistance);
	pointerOnQuad = pointerOnQuad || leftPointing;
	const bool casting = world3d && leftValid && !leftPointing && FloatState(triggerAction, off) > (castingDown ? 0.3F : 0.6F);
	if (!pointerOnQuad) {
		pointerX = pointerY = -1;
		if (world3d && rightValid)
			AimAtWorld(casting ? leftAim : aim);
	}
	if (rightValid && (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0) {
		speed = std::sqrt(Dot(velocity.linearVelocity, velocity.linearVelocity));
		swingDirection = velocity.linearVelocity;
	}
	// The laser: to the spot on the screen or panel, or to what the hand aims at in the world.
	// Over the screen and the panels, with nothing hit, it still goes a little way out.
	const XrPosef &pointing = leftPointing || casting ? leftAim : aim;
	const XrVector3f pointingAlong = Rotate(pointing.orientation, { 0, 0, -1 });
	float laserLength = 0;
	if (pointerOnQuad) {
		laserLength = hitDistance;
		laser.onQuad = true;
	} else if (worldPointerValid) {
		// The world is the tracking space turned and moved, never scaled, so the distance carries over.
		float hand[3];
		float handOrientation[4];
		ToWorld(pointing, hand, handOrientation);
		const float dx = worldPointerHit[0] - hand[0];
		const float dy = worldPointerHit[1] - hand[1];
		const float dz = worldPointerHit[2] - hand[2];
		laserLength = std::sqrt(dx * dx + dy * dy + dz * dz);
	} else if (rightValid && quadVisible && (!world3d || uiMode)) {
		laserLength = 1.5F;
	}
	if (laserLength > 0 && (rightValid || leftPointing)) {
		laser.visible = true;
		laser.from = pointing.position;
		laser.to = { pointing.position.x + pointingAlong.x * laserLength, pointing.position.y + pointingAlong.y * laserLength, pointing.position.z + pointingAlong.z * laserLength };
	}

	// Mouse motion switches the game to mouse controls, so hold it back while the
	// player walks with the stick, and ignore hand tremor.
	const bool walking = std::fabs(leftStick.x) > 0.2F || std::fabs(leftStick.y) > 0.2F;
	if (pointerX >= 0 && (world3d || !walking) && (std::abs(pointerX - sentX) > 1 || std::abs(pointerY - sentY) > 1))
		PushMouseMotion();

	// No click-to-move in first person. Pointing at the 2D frame clicks it. Pointing at
	// something that has to be reached (item, object, townsperson, another player) is a plain
	// click, and the character steps up to it. Anything else is an attack along the hand's
	// aim, repeated while the trigger is held, which never moves the character.
	const bool triggerDown = FloatState(triggerAction, leftPointing ? off : main) > (mouseLeftDown || clickingWorld || drawingBow ? 0.3F : 0.6F);
	// A bow is drawn with both hands: the right trigger closes by the left hand, the right hand
	// pulls back and lets go. The arrow flies from the left hand along the line from the right one.
	const bool bow = world3d && MyPlayer->UsesRangedWeapon() && leveltype != DTYPE_TOWN;
	float handsApart = 1e9F;
	float leftWorld[3];
	float rightWorld[3];
	float scratch[4];
	if (bow && leftValid) {
		ToWorld(leftAim, leftWorld, scratch);
		ToWorld(aim, rightWorld, scratch);
		const float dx = leftWorld[0] - rightWorld[0];
		const float dy = leftWorld[1] - rightWorld[1];
		const float dz = leftWorld[2] - rightWorld[2];
		handsApart = std::sqrt(dx * dx + dy * dy + dz * dz);
	}
	if (bow && !pointerOnQuad) {
		if (triggerDown && !rightTriggerWas && handsApart < 0.2F) {
			drawingBow = true;
			Pulse(main, 0.2F, 30);
		}
		if (drawingBow && !triggerDown) {
			drawingBow = false;
			if (handsApart > 0.3F) {
				const float direction[3] = { (leftWorld[0] - rightWorld[0]) / handsApart, (leftWorld[1] - rightWorld[1]) / handsApart, (leftWorld[2] - rightWorld[2]) / handsApart };
				int tileX;
				int tileY;
				float hit[3];
				const Point origin = MyPlayer->position.future;
				if (VrWorldPick(leftWorld, direction, tileX, tileY, hit) && dMonster[tileX][tileY] != 0)
					NetSendCmdParam1(true, CMD_RATTACKID, static_cast<uint16_t>(std::abs(dMonster[tileX][tileY]) - 1));
				else
					NetSendCmdLoc(MyPlayerId, true, CMD_RATTACKXY, origin + Displacement { static_cast<int>(std::lround(direction[0] * 6)), static_cast<int>(std::lround(direction[2] * 6)) });
				lastAttackAt = SDL_GetTicks();
				Pulse(main, 0.5F, 50);
				Pulse(off, 0.35F, 50);
			}
		} else if (drawingBow) {
			// The string's pull grows as the hands part.
			static Uint32 lastCreak;
			if (SDL_GetTicks() - lastCreak > 120) {
				Pulse(main, std::clamp(handsApart, 0.F, 0.7F) * 0.25F, 20);
				lastCreak = SDL_GetTicks();
			}
		}
	}
	rightTriggerWas = triggerDown;
	if (bow && !pointerOnQuad) {
		// The bow's trigger does nothing else.
	} else if (world3d && !pointerOnQuad && !mouseLeftDown) {
		const bool interact = worldPointerValid && pointerX >= 0
		    && (pcursitem != -1 || ObjectUnderCursor != nullptr || PlayerUnderCursor != nullptr || (leveltype == DTYPE_TOWN && pcursmonst != -1));
		// The beam turns gold on what the trigger picks up, opens or talks to, with a tick in the hand.
		laser.active = interact;
		if (interact && !pointerWasActive)
			Pulse(main, 0.2F, 10);
		if (triggerDown && interact && !clickingWorld) {
			clickingWorld = true;
			SetMouseButton(mouseLeftDown, true, SDL_BUTTON_LEFT);
		} else if (triggerDown && !clickingWorld) {
			Attack(aimDirX, aimDirZ);
		}
	} else if (!clickingWorld || !triggerDown) {
		SetMouseButton(mouseLeftDown, triggerDown, SDL_BUTTON_LEFT);
	}
	if (!triggerDown)
		clickingWorld = false;
	pointerWasActive = laser.active;
	if (world3d) {
		// The left hand casts the spell held ready: the right mouse button, aimed by the left hand.
		// On a panel the right grip is the right button, as on the 2D screen: drink, read, use.
		castingDown = casting;
		const std::array<bool, 2> beltBusy = UpdateBeltGrab(time);
		const bool panelRightClick = pointerOnQuad && !leftPointing && FloatState(squeezeAction, main) > (mouseRightDown ? 0.3F : 0.6F);
		bool castNow = casting && !speedbookOpen;
		if (*Settings().spellGestures) {
			// Holding the trigger aims and gathers the spell, with a quickening tick in the hand; a
			// flick of that hand along where it points lets it go.
			const Uint32 tick = SDL_GetTicks();
			if (castNow && aimingSince == 0)
				aimingSince = tick;
			if (!castNow)
				aimingSince = 0;
			float flick = 0;
			if (castNow && (offVelocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0)
				flick = Dot(offVelocity.linearVelocity, Rotate(leftAim.orientation, { 0, 0, -1 }));
			if (castNow && flick > 1.4F && tick - lastCast > 350) {
				castClick = 2;
				lastCast = tick;
				Pulse(off, 0.6F, 60);
			} else if (castNow && tick - lastCharge > 150) {
				const float gathered = std::min(static_cast<float>(tick - aimingSince) / 1000.F, 1.5F);
				Pulse(off, 0.1F + 0.08F * gathered, 15);
				lastCharge = tick;
			}
			castNow = castClick > 0;
			if (castClick > 0)
				castClick--;
		}
		SetMouseButton(mouseRightDown, castNow || panelRightClick, SDL_BUTTON_RIGHT);
		// The right grip takes hold of what is within reach: a click on it, as the mouse would.
		const bool grab = !beltBusy[main] && FloatState(squeezeAction, main) > (grabDown ? 0.3F : 0.6F);
		if (grab && !grabDown && !pointerOnQuad) {
			float hand[3];
			float handOrientation[4];
			ToWorld(aim, hand, handOrientation);
			int tileX;
			int tileY;
			if (VrWorldGrabTarget(hand, tileX, tileY)) {
				Point screen = GetScreenPosition({ tileX, tileY }) + Displacement { TILE_WIDTH / 2, -TILE_HEIGHT / 2 };
				if (*GetOptions().Graphics.zoom) {
					screen.x *= 2;
					screen.y *= 2;
				}
				if (screen.x >= 0 && screen.y >= 0 && screen.x < gnScreenWidth && screen.y < gnViewportHeight) {
					pointerX = screen.x;
					pointerY = screen.y;
					PushMouseMotion();
					bool click = false;
					SetMouseButton(click, true, SDL_BUTTON_LEFT);
					SetMouseButton(click, false, SDL_BUTTON_LEFT);
					Pulse(main, 0.4F, 40);
				}
			}
		}
		grabDown = grab;
		// The speedbook: held open with the left grip, the left stick picks, letting go sets it.
		const bool book = !leftPointing && !beltBusy[off] && FloatState(squeezeAction, off) > (speedbookOpen ? 0.3F : 0.6F);
		if (book && !speedbookOpen) {
			ring = {};
			const Player &me = *MyPlayer;
			const uint64_t sets[4] = { me._pAblSpells, me._pMemSpells, me._pScrlSpells, me._pISpells };
			const SpellType kinds[4] = { SpellType::Skill, SpellType::Spell, SpellType::Scroll, SpellType::Charges };
			for (int k = 0; k < 4; k++) {
				for (int spell = 1; spell < 64 && ring.count < 32; spell++) {
					if ((sets[k] & GetSpellBitmask(static_cast<SpellID>(spell))) == 0)
						continue;
					ring.spells[ring.count] = spell;
					ring.types[ring.count] = static_cast<int>(kinds[k]);
					if (me._pRSpell == static_cast<SpellID>(spell) && me._pRSplType == kinds[k])
						ring.selected = ring.count;
					ring.count++;
				}
			}
			Pulse(off, 0.2F, 30);
		}
		if (book && ring.count > 0 && (std::fabs(leftStick.x) > 0.5F || std::fabs(leftStick.y) > 0.5F)) {
			// Clockwise from the top, as the ring stands round the hand.
			float angle = std::atan2(leftStick.x, leftStick.y);
			if (angle < 0)
				angle += 6.2831853F;
			const int chosen = static_cast<int>(std::lround(angle / 6.2831853F * static_cast<float>(ring.count))) % ring.count;
			if (chosen != ring.selected)
				Pulse(off, 0.1F, 15);
			ring.selected = chosen;
		}
		if (!book && speedbookOpen && ring.count > 0 && ring.selected >= 0 && ring.selected < ring.count) {
			// What the speedbook does when a spell in it is clicked.
			Player &me = *MyPlayer;
			me._pRSpell = static_cast<SpellID>(ring.spells[ring.selected]);
			me._pRSplType = static_cast<SpellType>(ring.types[ring.selected]);
			RedrawEverything();
		}
		speedbookOpen = book;
		ring.open = book;
		VrWorldSetSpellRing(ring);
		// The hand feels what the swing did: a hard knock when the monster's life dropped, nothing on a miss.
		if (swingTarget >= 0) {
			if (Monsters[swingTarget].hitPoints < swingTargetLife) {
				Pulse(main, 0.95F, 90);
				swingTarget = -1;
			} else if (SDL_GetTicks() - swingAt > 900) {
				swingTarget = -1;
			}
		}
	} else {
		SetMouseButton(mouseRightDown, FloatState(squeezeAction, main) > (mouseRightDown ? 0.3F : 0.6F), SDL_BUTTON_RIGHT);
	}
	SetKey(menuDown, BoolState(menuAction), SDLK_ESCAPE);
	SetKey(viewEscapeDown, escapePulse > 0, SDLK_ESCAPE);
	if (escapePulse > 0)
		escapePulse--;

	// A swing only asks for the game's own primary action; hit chance, damage and
	// attack speed stay entirely with the original combat code.
	const bool canSwing = gbRunGame && !gmenu_is_active() && leveltype != DTYPE_TOWN && !bow && !speedbookOpen;
	const bool wasSwinging = swinging;
	swinging = canSwing && speed > (swinging ? SwingSpeed / 2 : SwingSpeed);
	if (swinging && !wasSwinging) {
		// Only a light tick for the swing itself; the knock comes if it hits.
		Pulse(main, 0.15F, 25);
		// The swing's own direction picks the neighbouring monster; a swing straight up or down
		// (no direction on the ground) goes where the player looks.
		const XrQuaternionf turn { 0, std::sin(turnYaw / 2), 0, std::cos(turnYaw / 2) };
		const XrVector3f inWorld = Rotate(turn, swingDirection);
		const float flat = std::sqrt(inWorld.x * inWorld.x + inWorld.z * inWorld.z);
		float lookX;
		float lookZ;
		if (world3d && flat > 0.6F * speed && flat > 1e-3F)
			Attack(inWorld.x / flat, inWorld.z / flat);
		else if (world3d && HeadForward(time, lookX, lookZ))
			Attack(lookX, lookZ);
	}

	// On the 2D screen the stick drives the game's own gamepad movement and menu navigation.
	// In first person it is smooth locomotion relative to where the player looks.
	float moveX = leftStick.x;
	float moveY = -leftStick.y;
	XrPosef head;
	if (world3d && Locate(viewSpace, time, head)) {
		float position[3];
		float orientation[4];
		ToWorld(head, position, orientation);
		const XrVector3f forward = Rotate({ orientation[0], orientation[1], orientation[2], orientation[3] }, { 0, 0, -1 });
		const float length = std::sqrt(forward.x * forward.x + forward.z * forward.z);
		if (length > 1e-3F) {
			const float fx = forward.x / length;
			const float fz = forward.z / length;
			const float goX = speedbookOpen ? 0 : fx * leftStick.y - fz * leftStick.x;
			const float goZ = speedbookOpen ? 0 : fz * leftStick.y + fx * leftStick.x;
			if (FreeMovement && !gbIsMultiplayer) {
				gliding = Glide(goX, goZ, frameStep);
			} else {
				Walk(goX, goZ);
			}
		}
	}
	if (world3d)
		moveX = moveY = 0; // The gamepad's tile-stepping walk is not used in first person.

	// Right stick: snap turn and potions in first person; potions and menu modifiers on the 2D screen.
	bool healthPotion = rightStick.x < -0.7F;
	bool manaPotion = rightStick.x > 0.7F;
	bool hotspellModifier = rightStick.y < -0.7F;
	bool menuNavigator = rightStick.y > 0.7F;
	if (world3d) {
		const float dt = frameStep;
		bool smoothTurning = false;
		if (*Settings().turnMode == VrTurnMode::Smooth) {
			if (std::fabs(rightStick.x) > 0.25F) {
				turnYaw -= rightStick.x * SmoothTurnSpeed() * dt;
				smoothTurning = true;
			}
		} else {
			const bool turn = std::fabs(rightStick.x) > 0.7F;
			if (turn && !turnHeld)
				turnYaw += rightStick.x > 0 ? -SnapTurnAngle() : SnapTurnAngle();
			turnHeld = turn;
		}
		// With bumpers for the potions the stick is left to turning: a potion is not something
		// to drink by accident.
		healthPotion = !frameControllers && rightStick.y > 0.7F;
		manaPotion = !frameControllers && rightStick.y < -0.7F;
		hotspellModifier = menuNavigator = false;
		// Comfort: the edge of view darkens while the view moves without the head moving it.
		const bool moving = MyPlayer->isWalking() || gliding || smoothTurning;
		const float wanted = moving ? ComfortVignette() : 0.F;
		vignetteNow += std::clamp(wanted - vignetteNow, -dt * 3, dt * 3);
		VrWorldSetVignette(vignetteNow);
	}

	// Everything else goes through the stock gamepad scheme (see padmapper defaults in diablo.cpp).
	SDL_JoystickSetVirtualAxis(virtualPad, SDL_CONTROLLER_AXIS_LEFTX, AxisValue(moveX));
	SDL_JoystickSetVirtualAxis(virtualPad, SDL_CONTROLLER_AXIS_LEFTY, AxisValue(moveY));
	// On the Frame the menus, the game menu, shops and dialogs take A to confirm and B to go
	// back, as the gamepad's A and B do there (and every other game on it). In play A and B
	// stay the primary and secondary actions. The switch waits for both to be up: an A that
	// opens a shop must not also buy the first thing in it.
	static bool menuButtonsLatch;
	const bool menuButtons = rules::LatchMenuButtons(menuButtonsLatch, BoolState(aAction), BoolState(bAction),
	    rules::MenuButtonsWanted(frameControllers, world3d, InGameMenu(), QuestLogIsOpen));
	if (menuButtons) {
		SDL_JoystickSetVirtualAxis(virtualPad, SDL_CONTROLLER_AXIS_TRIGGERLEFT, TriggerValue(0));
		SDL_JoystickSetVirtualAxis(virtualPad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, TriggerValue(0));
		SDL_JoystickSetVirtualButton(virtualPad, SDL_CONTROLLER_BUTTON_A, BoolState(aAction) ? 1 : 0); // Confirm
		SDL_JoystickSetVirtualButton(virtualPad, SDL_CONTROLLER_BUTTON_B, BoolState(bAction) ? 1 : 0); // Back
		SDL_JoystickSetVirtualButton(virtualPad, SDL_CONTROLLER_BUTTON_X, !world3d && BoolState(xAction) ? 1 : 0); // Delete (a hero)
		SDL_JoystickSetVirtualButton(virtualPad, SDL_CONTROLLER_BUTTON_Y, 0);
	} else if (world3d) {
		// In the world the left trigger casts and the left grip holds the speedbook (above); the
		// panels come up on the left hand's buttons: Y the inventory, X the character.
		SDL_JoystickSetVirtualAxis(virtualPad, SDL_CONTROLLER_AXIS_TRIGGERLEFT, TriggerValue(BoolState(xAction) ? 1.F : 0.F));  // Character
		SDL_JoystickSetVirtualAxis(virtualPad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, TriggerValue(BoolState(yAction) ? 1.F : 0.F)); // Inventory
		SDL_JoystickSetVirtualButton(virtualPad, SDL_CONTROLLER_BUTTON_X, 0);
		SDL_JoystickSetVirtualButton(virtualPad, SDL_CONTROLLER_BUTTON_A, 0);
	} else {
		SDL_JoystickSetVirtualAxis(virtualPad, SDL_CONTROLLER_AXIS_TRIGGERLEFT, TriggerValue(FloatState(squeezeAction, off)));    // Character
		SDL_JoystickSetVirtualAxis(virtualPad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, TriggerValue(FloatState(triggerAction, off)));  // Inventory
		SDL_JoystickSetVirtualButton(virtualPad, SDL_CONTROLLER_BUTTON_X, BoolState(xAction) ? 1 : 0);                            // Spell action
		SDL_JoystickSetVirtualButton(virtualPad, SDL_CONTROLLER_BUTTON_A, BoolState(yAction) ? 1 : 0);                            // Cancel / spell list
	}
	if (!menuButtons) {
		SDL_JoystickSetVirtualButton(virtualPad, SDL_CONTROLLER_BUTTON_B, (BoolState(aAction) || (swinging && !world3d)) ? 1 : 0); // Primary action
		SDL_JoystickSetVirtualButton(virtualPad, SDL_CONTROLLER_BUTTON_Y, BoolState(bAction) ? 1 : 0);                             // Secondary action
	}
	SDL_JoystickSetVirtualButton(virtualPad, SDL_CONTROLLER_BUTTON_LEFTSTICK, automapPulse > 0 ? 1 : 0); // Automap
	if (automapPulse > 0)
		automapPulse--;
	healthPotion = healthPotion || BoolState(bumperAction, handPath[Left]);
	manaPotion = manaPotion || BoolState(bumperAction, handPath[Right]);
	SDL_JoystickSetVirtualButton(virtualPad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, healthPotion ? 1 : 0);
	SDL_JoystickSetVirtualButton(virtualPad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, manaPotion ? 1 : 0);
	UpdateHaptics(main, off, pointerOnQuad, leftPointing ? off : main, mouseLeftDown);
	// View's short press reaches the game as Select only on release, so holding it to re-centre
	// does not open the game menu as well.
	SDL_JoystickSetVirtualButton(virtualPad, SDL_CONTROLLER_BUTTON_BACK, (hotspellModifier || backPulse > 0) ? 1 : 0);
	if (backPulse > 0)
		backPulse--;
	// The D-pad is the gamepad's wherever the game shows its own interface: menus, the inventory's
	// slots, shops. Out in the world it would walk the character a tile at a time under the player.
	const bool padNavigates = !world3d || UiOpen();
	const SDL_GameControllerButton padButtons[4] = { SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT };
	for (int i = 0; i < 4; i++)
		SDL_JoystickSetVirtualButton(virtualPad, padButtons[i], padNavigates && BoolState(dpadAction[i]) ? 1 : 0);
	SDL_JoystickSetVirtualButton(virtualPad, SDL_CONTROLLER_BUTTON_START, menuNavigator ? 1 : 0);
}

} // namespace vr

const std::vector<const char *> &VrControlsHelp()
{
	static const std::vector<const char *> frame {
		"$Steam Frame controls:",
		"Left stick: walk. Right stick: turn.",
		"Right trigger: point and click. The beam turns gold on anything the trigger picks up, opens or talks to.",
		"Right grip: take hold of a door, chest, lever or item within reach. On a panel it is the right mouse button: drink a potion, read a scroll.",
		"A: attack or talk. B: pick up, open, use what is next to you. Swing the right hand to hit a monster next to you.",
		"Left trigger: cast the readied spell where the left hand points. Hold the left grip for the speedbook, choose with the left stick.",
		"Y: inventory. X: character. Bumpers: health and mana potion. Left stick click: automap.",
		"Close a grip by a potion on your belt to take it, and bring it to your mouth to drink it.",
		"View: this help, or close what is open. Hold View: recentre. Menu: game menu.",
		"Turning, hand, haptics and the rest: Menu, Options, VR Settings.",
		"",
		"$Menus:",
		"Point and pull the trigger, or use the D-pad. A confirms, B goes back.",
		"",
		"$Saving:",
		"Press Menu and choose Save Game. Diablo only saves when you do, so save before you quit.",
		"",
	};
	static const std::vector<const char *> frameWithWrists = [] {
		std::vector<const char *> text = frame;
		text.insert(text.end() - 3, {
		                                "$Life, mana and the belt:",
		                                "Life, the menu buttons and belt slots 1 to 4 are on the left wrist; mana, belt slots 5 to 8 and the spell buttons on the right. Point at one wrist with the other hand and pull the trigger to press what is on it.",
		                                "",
		                            });
		return text;
	}();
	static const std::vector<const char *> touch {
		"$VR controls:",
		"Left stick: walk. Right stick: turn, up and down for potions.",
		"Right trigger: point and click. The beam turns gold on anything the trigger picks up, opens or talks to.",
		"Right grip: take hold of a door, chest, lever or item within reach. On a panel it is the right mouse button.",
		"A: attack or talk. B: pick up, open, use what is next to you. Swing the right hand to hit a monster next to you.",
		"Left trigger: cast the readied spell where the left hand points. Hold the left grip for the speedbook.",
		"Y: inventory. X: character. Left stick click: automap. Menu: game menu.",
		"Close a grip by a potion on your belt to take it, and bring it to your mouth to drink it.",
		"Turning, hand, haptics and the rest: Menu, Options, VR Settings.",
		"",
		"$Saving:",
		"Press Menu and choose Save Game. Diablo only saves when you do, so save before you quit.",
		"",
	};
	if (!vr::frameControllers)
		return touch;
	return vr::WristHud() ? frameWithWrists : frame;
}

} // namespace devilution

/**
 * @file vr_internal.h
 *
 * State and helpers shared by the VR modules (session, input, frame, debug).
 * Not for use outside platform/vr.
 */
#pragma once

#include "platform/vr/vr.h"

#include <atomic>
#include <cstdint>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif
#include <d3d11.h>
#include <dxgi.h>
#ifdef _WIN32
#include <windows.h>

#define XR_USE_PLATFORM_WIN32
#endif

#include "platform/vr/knob.h"
#include "platform/vr/xr_d3d11.h"

namespace devilution {
namespace vr {

// Meters. The quad is placed this far in front of the head when (re)centered.
inline const float ScreenDistance = Knob("DEVILUTIONX_VR_SCREEN_DISTANCE", 2.0F);
inline const float ScreenWidth = Knob("DEVILUTIONX_VR_SCREEN_WIDTH", 2.6F);
// The 2D frame while the 3D world is shown. It only appears while a panel, dialog or menu is open.
inline const float PanelDistance = Knob("DEVILUTIONX_VR_PANEL_DISTANCE", 1.1F);
inline const float PanelWidth = Knob("DEVILUTIONX_VR_PANEL_WIDTH", 1.4F);
inline const float HudWidth = Knob("DEVILUTIONX_VR_HUD_WIDTH", 0.6F);
// Eye height used when the runtime has no floor-level tracking space.
inline const float SeatedEyeHeight = Knob("DEVILUTIONX_VR_EYE_HEIGHT", 1.65F);
// Meters per second of right-hand speed that counts as a weapon swing.
inline const float SwingSpeed = Knob("DEVILUTIONX_VR_SWING_SPEED", 2.0F);
constexpr float SnapTurn = 0.7853982F; // 45 degrees

enum Hand : uint8_t {
	Left,
	Right,
};

enum class State : uint8_t {
	Uninitialized,
	Initializing,
	Ready,
	Failed,
};

struct EyeTarget {
	XrSwapchain swapchain;
	std::vector<ID3D11Texture2D *> images;
	std::vector<ID3D11RenderTargetView *> views;
	int width;
	int height;
	/** Depth submitted with the projection layer; null without XR_KHR_composition_layer_depth. */
	XrSwapchain depthSwapchain;
	std::vector<ID3D11DepthStencilView *> depthViews;
	/** The part of the image the lenses never show, as triangles in view space at z = -1; empty without XR_KHR_visibility_mask. */
	ID3D11Buffer *maskVertices;
	uint32_t maskVertexCount;
};

// xr_session.cpp
extern State state;
extern bool sessionRunning;
/** The session has the wearer's attention: the headset is on and the system shows nothing over the game. */
extern bool sessionFocused;
/** It has had it at least once since it began. */
extern bool sessionWasFocused;
extern std::atomic<int> initResult;
extern size_t runtimeIndex;
extern XrInstance instance;
extern XrSystemId systemId;
extern XrSession session;
extern XrSpace localSpace;
extern XrSpace viewSpace;
extern ID3D11Device *device;
extern ID3D11DeviceContext *context;
extern XrSwapchain swapchain;
extern std::vector<ID3D11Texture2D *> swapchainImages;
extern int swapchainWidth;
extern int swapchainHeight;
extern int64_t swapchainFormat;
extern Uint32 swapchainSdlFormat;
extern EyeTarget eyes[2];
extern float floorOffset;
/** The runtime knows the Steam Frame's controllers (XR_VALVE_frame_controller_interaction). */
extern bool hasFrameControllers;
/** SDL tick at which the running session began. */
extern Uint32 sessionStartTick;

bool Check(XrResult result, const char *what);
XrVector3f Rotate(const XrQuaternionf &q, const XrVector3f &v);
float Dot(const XrVector3f &a, const XrVector3f &b);
XrPath Path(const char *path);
XrQuaternionf Multiply(const XrQuaternionf &a, const XrQuaternionf &b);
bool Locate(XrSpace space, XrTime time, XrPosef &pose, XrSpaceVelocity *velocity = nullptr);
void ToWorld(const XrPosef &pose, float position[3], float orientation[4]);
bool Init();
void SelectRuntime();
void PollEvents();
/** The session or instance is gone (or going): tear down and reconnect from the next frame on. */
void SessionLost(const char *reason);
bool UploadFrame(SDL_Surface *surface);
void Cleanup();

// vr.cpp
/** Pauses the game while the session is out of focus, and lets it go on when focus is back. */
void UpdateFocusPause();

// vr.cpp: where the 2D frame and the world sit relative to the tracking space.
extern XrPosef screenPose;
extern bool screenPlaced;
extern bool world3d;
extern float quadWidth;
extern float turnYaw;
extern float worldOrigin[3];
extern XrVector3f headAnchor;
extern bool anchorPending;
extern bool quadVisible;
extern bool uiMode;
extern SDL_Rect quadRect;
/** A second quad (the inventory's equipment while its backpack is at the hip): pose, width, part of the frame. Width 0: none. */
extern XrPosef extraPose;
extern float extraWidth;
extern SDL_Rect extraRect;
/** Black in this part of the frame is uploaded transparent (the automap over its parchment). Width 0: none. */
extern SDL_Rect clearBlack;

// xr_input.cpp
extern XrPath handPath[2];
extern XrSpace aimSpace[2];
extern bool worldPointerValid;
extern float worldPointerHit[3];
extern bool freeValid;

bool InitActions();
/** The runtime bound another kind of controller: note which. */
void ControllersChanged();
/** Anything that needs the 2D frame in view: panels, dialogs, shops, menus, the automap. */
bool UiOpen();
bool InitVirtualPad();
void SelfCheck();
void SetKey(bool &isDown, bool down, SDL_Keycode key);
void SyncFreePosition();
void UpdateInput(XrTime time, float screenHeight);

// xr_frame.cpp
bool RenderWorld(XrTime time, XrCompositionLayerProjectionView projectionViews[2]);

// debug/mirror.cpp
extern bool mirrorReady;
void DumpTexture(ID3D11Texture2D *texture, const char *path);
/** Set by the controllers (both thumbsticks pressed in together): the next frame's left eye is saved as a picture. */
extern bool screenshotRequested;
/**
 * Saves a finished eye image as `diablo-vr-<date>-<time>.png` in the screenshot folder: the
 * `screenshots` folder beside `vrassets`, or DEVILUTIONX_VR_SCREENSHOTS.
 */
void SaveScreenshot(ID3D11Texture2D *texture);
/** Whether the mirror is also written to vr-view.bmp (DEVILUTIONX_VR_MIRROR). */
bool MirrorCaptureEnabled();
/** Copies the left eye's finished picture to the desktop mirror. Call after the world was rendered. */
void RenderMirror();
void ReleaseMirror();

// debug/offscreen.cpp
void EnterLevel(int level);
void DebugOffscreen(const char *directory);
void DebugBench(int level, const char *directory);

} // namespace vr
} // namespace devilution

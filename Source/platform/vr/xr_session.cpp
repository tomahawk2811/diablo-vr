/**
 * @file xr_session.cpp
 *
 * OpenXR instance, session, swapchains and the pose math between tracking and world space.
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

State state = State::Uninitialized;
bool sessionRunning;
bool sessionFocused;
bool sessionWasFocused;
/** Result of the background Init(): -1 while running, then 0 (failed) or 1 (ready). */
std::atomic<int> initResult { -1 };
/** Which installed OpenXR runtime to try next; advanced whenever one has no headset. */
size_t runtimeIndex;

XrInstance instance;
XrSystemId systemId;
XrSession session;
XrSpace localSpace;
XrSpace viewSpace;
ID3D11Device *device;
ID3D11DeviceContext *context;

XrSwapchain swapchain;
std::vector<ID3D11Texture2D *> swapchainImages;
int swapchainWidth;
int swapchainHeight;
int64_t swapchainFormat;
Uint32 swapchainSdlFormat;
std::vector<uint8_t> pixelBuffer;

EyeTarget eyes[2];
Uint32 sessionStartTick;

// Optional extensions. Each has a fallback: own depth buffer, no mask, the runtime's refresh rate.
bool hasDepthLayer;
bool hasVisibilityMask;
bool hasRefreshRate;
bool hasFrameControllers;
int64_t depthFormat;
/** Height of the tracking origin above the real floor; 0 when the runtime gives us a floor-level space. */
float floorOffset;

bool Check(XrResult result, const char *what)
{
	if (XR_SUCCEEDED(result))
		return true;
	// Headset polling repeats every few seconds; the first rounds tell the whole story.
	static int logged;
	if (logged++ < 16)
		LogError("VR: {} failed ({})", what, static_cast<int>(result));
	return false;
}

XrVector3f Rotate(const XrQuaternionf &q, const XrVector3f &v)
{
	// v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
	const XrVector3f t {
		q.y * v.z - q.z * v.y + q.w * v.x,
		q.z * v.x - q.x * v.z + q.w * v.y,
		q.x * v.y - q.y * v.x + q.w * v.z,
	};
	return {
		v.x + 2 * (q.y * t.z - q.z * t.y),
		v.y + 2 * (q.z * t.x - q.x * t.z),
		v.z + 2 * (q.x * t.y - q.y * t.x),
	};
}

float Dot(const XrVector3f &a, const XrVector3f &b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

XrPath Path(const char *path)
{
	XrPath result = XR_NULL_PATH;
	xrStringToPath(instance, path, &result);
	return result;
}

XrQuaternionf Multiply(const XrQuaternionf &a, const XrQuaternionf &b)
{
	return {
		a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
		a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
		a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
	};
}

bool Locate(XrSpace space, XrTime time, XrPosef &pose, XrSpaceVelocity *velocity)
{
	XrSpaceLocation location { XR_TYPE_SPACE_LOCATION };
	if (velocity != nullptr) {
		*velocity = { XR_TYPE_SPACE_VELOCITY };
		location.next = velocity;
	}
	const XrSpaceLocationFlags required = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	if (XR_FAILED(xrLocateSpace(space, localSpace, time, &location)) || (location.locationFlags & required) != required)
		return false;
	pose = location.pose;
	return true;
}

/** OpenXR tracking space to world space: the player's tile position plus the snap-turn yaw. */
void ToWorld(const XrPosef &pose, float position[3], float orientation[4])
{
	const XrQuaternionf turn { 0, std::sin(turnYaw / 2), 0, std::cos(turnYaw / 2) };
	const XrVector3f rotated = Rotate(turn, { pose.position.x - headAnchor.x, pose.position.y, pose.position.z - headAnchor.z });
	position[0] = worldOrigin[0] + rotated.x;
	position[1] = worldOrigin[1] + rotated.y;
	position[2] = worldOrigin[2] + rotated.z;
	const XrQuaternionf q = Multiply(turn, pose.orientation);
	orientation[0] = q.x;
	orientation[1] = q.y;
	orientation[2] = q.z;
	orientation[3] = q.w;
}

/** The optional extensions the runtime offers, plus the one we cannot do without. */
std::vector<const char *> NegotiateExtensions()
{
	// DEVILUTIONX_VR_NO_EXTENSIONS=1 runs the fallbacks on a runtime that has everything.
	const bool optional = SDL_getenv("DEVILUTIONX_VR_NO_EXTENSIONS") == nullptr;
	uint32_t count = 0;
	xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
	std::vector<XrExtensionProperties> offered(count, XrExtensionProperties { XR_TYPE_EXTENSION_PROPERTIES });
	xrEnumerateInstanceExtensionProperties(nullptr, count, &count, offered.data());
	std::vector<const char *> enabled { xrd3d::BindingExtension() };
	const auto negotiate = [&](const char *name, bool &has) {
		has = optional && std::any_of(offered.begin(), offered.end(), [&](const XrExtensionProperties &extension) { return std::strcmp(extension.extensionName, name) == 0; });
		if (has)
			enabled.push_back(name);
	};
	negotiate(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME, hasDepthLayer);
	// DEVILUTIONX_VR_DEPTH_LAYER=0: no depth is handed to the compositor, which saves two
	// swapchains and a depth write per eye where the compositor does nothing with it.
	hasDepthLayer = hasDepthLayer && Knob("DEVILUTIONX_VR_DEPTH_LAYER", 1) != 0;
	negotiate(XR_KHR_VISIBILITY_MASK_EXTENSION_NAME, hasVisibilityMask);
	negotiate(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME, hasRefreshRate);
	negotiate("XR_VALVE_frame_controller_interaction", hasFrameControllers);
	return enabled;
}

template <typename Function>
bool ExtensionFunction(const char *name, Function &function)
{
	return Check(xrGetInstanceProcAddr(instance, name, reinterpret_cast<PFN_xrVoidFunction *>(&function)), name);
}

/** Fetches the hidden-area mesh of one eye. The runtime announces changes with an event. */
void UpdateVisibilityMask(uint32_t index)
{
	if (!hasVisibilityMask || index >= 2)
		return;
	EyeTarget &eye = eyes[index];
	if (eye.maskVertices != nullptr)
		eye.maskVertices->Release();
	eye.maskVertices = nullptr;
	eye.maskVertexCount = 0;

	PFN_xrGetVisibilityMaskKHR getMask = nullptr;
	XrVisibilityMaskKHR mask { XR_TYPE_VISIBILITY_MASK_KHR };
	const XrViewConfigurationType stereo = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	const XrVisibilityMaskTypeKHR hidden = XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR;
	if (!ExtensionFunction("xrGetVisibilityMaskKHR", getMask) || !Check(getMask(session, stereo, index, hidden, &mask), "xrGetVisibilityMaskKHR"))
		return;
	std::vector<XrVector2f> meshVertices(mask.vertexCountOutput);
	std::vector<uint32_t> indices(mask.indexCountOutput);
	mask.vertexCapacityInput = mask.vertexCountOutput;
	mask.vertices = meshVertices.data();
	mask.indexCapacityInput = mask.indexCountOutput;
	mask.indices = indices.data();
	if (indices.empty() || !Check(getMask(session, stereo, index, hidden, &mask), "xrGetVisibilityMaskKHR")) {
		LogInfo("VR: eye {} has no hidden-area mesh", index);
		return;
	}
	std::vector<XrVector2f> triangles;
	for (const uint32_t vertex : indices)
		triangles.push_back(meshVertices[vertex]);
	D3D11_BUFFER_DESC desc {};
	desc.ByteWidth = static_cast<UINT>(triangles.size() * sizeof(XrVector2f));
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	const D3D11_SUBRESOURCE_DATA data { triangles.data(), 0, 0 };
	if (FAILED(device->CreateBuffer(&desc, &data, &eye.maskVertices)))
		return;
	eye.maskVertexCount = static_cast<uint32_t>(triangles.size());
	LogInfo("VR: eye {} hidden-area mesh: {} triangles", index, triangles.size() / 3);
}

/** Lists the display's refresh rates and asks for DEVILUTIONX_VR_REFRESH_RATE (90) when it is one of them. */
void RequestRefreshRate()
{
	PFN_xrEnumerateDisplayRefreshRatesFB enumerateRates = nullptr;
	PFN_xrGetDisplayRefreshRateFB getRate = nullptr;
	PFN_xrRequestDisplayRefreshRateFB requestRate = nullptr;
	if (!hasRefreshRate || !ExtensionFunction("xrEnumerateDisplayRefreshRatesFB", enumerateRates)
	    || !ExtensionFunction("xrGetDisplayRefreshRateFB", getRate) || !ExtensionFunction("xrRequestDisplayRefreshRateFB", requestRate))
		return;
	uint32_t count = 0;
	enumerateRates(session, 0, &count, nullptr);
	std::vector<float> rates(count);
	enumerateRates(session, count, &count, rates.data());
	float current = 0;
	getRate(session, &current);
	std::string list;
	for (const float rate : rates)
		list += std::to_string(static_cast<int>(rate + 0.5F)) + " ";
	LogInfo("VR: refresh rates offered: {}Hz, current {:.0f} Hz", list, current);
	const float wanted = Knob("DEVILUTIONX_VR_REFRESH_RATE", 90);
	for (const float rate : rates) {
		if (std::fabs(rate - wanted) < 0.5F && std::fabs(rate - current) >= 0.5F)
			LogInfo("VR: requested {:.0f} Hz: {}", rate, XR_SUCCEEDED(requestRate(session, rate)) ? "accepted" : "refused");
	}
}

bool CreateDepthSwapchain(EyeTarget &eye)
{
	XrSwapchainCreateInfo info { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	info.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	info.format = depthFormat;
	info.sampleCount = 1;
	info.width = eye.width;
	info.height = eye.height;
	info.faceCount = 1;
	info.arraySize = 1;
	info.mipCount = 1;
	if (!Check(xrd3d::CreateSwapchain(session, info, &eye.depthSwapchain), "xrCreateSwapchain(depth)"))
		return false;
	for (ID3D11Texture2D *image : xrd3d::SwapchainImages(eye.depthSwapchain)) {
		D3D11_DEPTH_STENCIL_VIEW_DESC viewDesc {};
		viewDesc.Format = static_cast<DXGI_FORMAT>(depthFormat);
		viewDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
		ID3D11DepthStencilView *view = nullptr;
		if (FAILED(device->CreateDepthStencilView(image, &viewDesc, &view)))
			return false;
		eye.depthViews.push_back(view);
	}
	return true;
}

bool CreateEyeTargets()
{
	uint32_t viewCount = 0;
	XrViewConfigurationView configViews[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
	if (!Check(xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &viewCount, configViews), "xrEnumerateViewConfigurationViews"))
		return false;
	for (int i = 0; i < 2; i++) {
		EyeTarget &eye = eyes[i];
		eye.width = static_cast<int>(configViews[i].recommendedImageRectWidth);
		eye.height = static_cast<int>(configViews[i].recommendedImageRectHeight);
		// DEVILUTIONX_VR_EYE_SIZE=<pixels>: the width of the eye images, whatever the runtime
		// recommends. A standalone headset recommends fewer pixels than its panels have, and
		// what it recommends moves with its per-application settings, so this is a size and
		// not a factor.
		static const float eyeSize = Knob("DEVILUTIONX_VR_EYE_SIZE", 0);
		if (eyeSize > 0) {
			const float scale = eyeSize / static_cast<float>(eye.width);
			eye.width = std::min(static_cast<int>(eyeSize), static_cast<int>(configViews[i].maxImageRectWidth));
			eye.height = std::min(static_cast<int>(std::lround(static_cast<float>(eye.height) * scale)), static_cast<int>(configViews[i].maxImageRectHeight));
		}
		LogInfo("VR: eye {} target {}x{} (recommended {}x{})", i, eye.width, eye.height, configViews[i].recommendedImageRectWidth, configViews[i].recommendedImageRectHeight);
		XrSwapchainCreateInfo info { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
		info.format = swapchainFormat;
		info.sampleCount = 1;
		info.width = eye.width;
		info.height = eye.height;
		info.faceCount = 1;
		info.arraySize = 1;
		info.mipCount = 1;
		if (!Check(xrd3d::CreateSwapchain(session, info, &eye.swapchain), "xrCreateSwapchain(eye)"))
			return false;
		eye.images = xrd3d::SwapchainImages(eye.swapchain);
		eye.views.clear();
		for (ID3D11Texture2D *image : eye.images) {
			// Runtimes may hand out typeless textures, so name the format explicitly.
			D3D11_RENDER_TARGET_VIEW_DESC viewDesc {};
			viewDesc.Format = static_cast<DXGI_FORMAT>(swapchainFormat);
			viewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			ID3D11RenderTargetView *view = nullptr;
			if (FAILED(device->CreateRenderTargetView(image, &viewDesc, &view))) {
				LogError("VR: CreateRenderTargetView failed");
				return false;
			}
			eye.views.push_back(view);
		}
	}
	for (EyeTarget &eye : eyes) {
		if (hasDepthLayer && !CreateDepthSwapchain(eye)) {
			LogError("VR: depth swapchain creation failed, depth is not submitted");
			hasDepthLayer = false;
		}
	}
	if (!hasDepthLayer) {
		for (EyeTarget &eye : eyes)
			eye.depthSwapchain = XR_NULL_HANDLE; // Left to the session to destroy.
	}
	for (uint32_t i = 0; i < 2; i++)
		UpdateVisibilityMask(i);

	if (eyes[0].width != eyes[1].width || eyes[0].height != eyes[1].height)
		LogError("VR: the eyes' image sizes differ; both are drawn at the left eye's size");
	return VrWorldInit(device, context);
}

bool Init()
{
	const std::vector<const char *> extensions = NegotiateExtensions();
	XrInstanceCreateInfo instanceInfo { XR_TYPE_INSTANCE_CREATE_INFO };
	SDL_strlcpy(instanceInfo.applicationInfo.applicationName, "DevilutionX", sizeof(instanceInfo.applicationInfo.applicationName));
	instanceInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	instanceInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
	instanceInfo.enabledExtensionNames = extensions.data();
	if (!Check(xrCreateInstance(&instanceInfo, &instance), "xrCreateInstance"))
		return false;

	XrSystemGetInfo systemInfo { XR_TYPE_SYSTEM_GET_INFO };
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if (!Check(xrGetSystem(instance, &systemInfo, &systemId), "xrGetSystem (is the headset connected?)"))
		return false;

	xrd3d::log = [](const char *message) { LogInfo("VR: {}", message); };
	if (!xrd3d::CreateDevice(instance, systemId, &device, &context)) {
		LogError("VR: no Direct3D 11 device on the headset's graphics adapter");
		return false;
	}
	if (!Check(xrd3d::CreateSession(instance, systemId, device, &session), "xrCreateSession"))
		return false;

	XrReferenceSpaceCreateInfo spaceInfo { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	spaceInfo.poseInReferenceSpace.orientation.w = 1;
	// Prefer a floor-level space so the dungeon floor is the real floor.
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
	floorOffset = 0;
	if (XR_FAILED(xrCreateReferenceSpace(session, &spaceInfo, &localSpace))) {
		spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		floorOffset = SeatedEyeHeight;
		if (!Check(xrCreateReferenceSpace(session, &spaceInfo, &localSpace), "xrCreateReferenceSpace(LOCAL)"))
			return false;
	}
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	if (!Check(xrCreateReferenceSpace(session, &spaceInfo, &viewSpace), "xrCreateReferenceSpace(VIEW)"))
		return false;

	// Game pixels are gamma encoded, so only sRGB formats give correct brightness.
	const std::vector<int64_t> formats = xrd3d::SwapchainFormats(session);
	for (const int64_t format : formats) {
		if (format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
			swapchainFormat = format;
			swapchainSdlFormat = SDL_PIXELFORMAT_ARGB8888;
			break;
		}
		if (format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
			swapchainFormat = format;
			swapchainSdlFormat = SDL_PIXELFORMAT_ABGR8888;
			break;
		}
	}
	if (swapchainFormat == 0) {
		LogError("VR: runtime offers no 8-bit sRGB swapchain format");
		return false;
	}
	depthFormat = 0;
	for (const int64_t wanted : { DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D16_UNORM }) {
		if (depthFormat == 0 && std::find(formats.begin(), formats.end(), wanted) != formats.end())
			depthFormat = wanted;
	}
	hasDepthLayer = hasDepthLayer && depthFormat != 0;
	const auto status = [](bool has) { return has ? "enabled" : "absent, using fallback"; };
	LogInfo("VR: {}: {} (depth format {})", XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME, status(hasDepthLayer), depthFormat);
	LogInfo("VR: {}: {}", XR_KHR_VISIBILITY_MASK_EXTENSION_NAME, status(hasVisibilityMask));
	LogInfo("VR: {}: {}", XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME, status(hasRefreshRate));
	RequestRefreshRate();

	if (!InitActions() || !CreateEyeTargets())
		return false;

	LogInfo("VR: OpenXR session created");
	return true;
}

/**
 * The system's active OpenXR runtime is not necessarily the one the headset is
 * connected through (e.g. Virtual Desktop active, headset on SteamVR), so walk
 * the installed runtimes: first the active one, then each registered one.
 */
void SelectRuntime()
{
#ifdef _WIN32
	// An inherited XR_RUNTIME_JSON is the user's explicit choice, unless we run elevated: then
	// it may come from a less privileged parent and is not trusted (see Source/CMakeLists.txt).
	static const bool elevated = [] {
		HANDLE token = nullptr;
		TOKEN_ELEVATION elevation {};
		DWORD size = 0;
		const bool result = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != 0
		    && GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != 0
		    && elevation.TokenIsElevated != 0;
		if (token != nullptr)
			CloseHandle(token);
		if (result)
			LogInfo("VR: running elevated");
		return result;
	}();
	static const bool userOverride = !elevated && GetEnvironmentVariableA("XR_RUNTIME_JSON", nullptr, 0) != 0;
	if (userOverride)
		return;
	std::vector<std::string> runtimes;
	HKEY key;
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1\\AvailableRuntimes", 0, KEY_READ, &key) == ERROR_SUCCESS) {
		char name[MAX_PATH];
		DWORD length = sizeof(name);
		for (DWORD i = 0; RegEnumValueA(key, i, name, &length, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS; i++) {
			if (GetFileAttributesA(name) != INVALID_FILE_ATTRIBUTES)
				runtimes.emplace_back(name);
			length = sizeof(name);
		}
		RegCloseKey(key);
	}
	const size_t choice = runtimeIndex % (runtimes.size() + 1);
	SetEnvironmentVariableA("XR_RUNTIME_JSON", choice == 0 ? nullptr : runtimes[choice - 1].c_str());
	if (runtimeIndex < 2 * (runtimes.size() + 1))
		LogInfo("VR: trying OpenXR runtime: {}", choice == 0 ? "system default" : runtimes[choice - 1].c_str());
#endif
}

void SessionLost(const char *reason)
{
	LogInfo("VR: session lost ({}), reconnecting", reason);
	sessionRunning = false;
	sessionFocused = false;
	sessionWasFocused = false;
	UpdateFocusPause(); // The game goes on in the desktop window.
	state = State::Uninitialized; // VrPresent tears down and reconnects.
}

void PollEvents()
{
	XrEventDataBuffer event { XR_TYPE_EVENT_DATA_BUFFER };
	while (xrPollEvent(instance, &event) == XR_SUCCESS) {
		if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			const XrSessionState newState = reinterpret_cast<XrEventDataSessionStateChanged *>(&event)->state;
			// Focused: worn, and nothing of the system's (its menu, another overlay) in front.
			sessionFocused = newState == XR_SESSION_STATE_FOCUSED;
			sessionWasFocused = sessionWasFocused || sessionFocused;
			LogVerbose("VR: session state {}{}", static_cast<int>(newState), sessionFocused ? " (focused)" : "");
			if (newState == XR_SESSION_STATE_READY) {
				XrSessionBeginInfo beginInfo { XR_TYPE_SESSION_BEGIN_INFO };
				beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				sessionRunning = Check(xrBeginSession(session, &beginInfo), "xrBeginSession");
				// The headset paces frames from here on (xrWaitFrame).
				if (sessionRunning && renderer != nullptr)
					SDL_RenderSetVSync(renderer, 0);
				sessionStartTick = SDL_GetTicks();
				// SteamVR has no hidden-area mesh to give before the session runs.
				for (uint32_t i = 0; i < 2; i++) {
					if (eyes[i].maskVertexCount == 0)
						UpdateVisibilityMask(i);
				}
				LogInfo("VR: session running: {}", sessionRunning);
			} else if (newState == XR_SESSION_STATE_STOPPING) {
				xrEndSession(session);
				sessionRunning = false;
			} else if (newState == XR_SESSION_STATE_EXITING || newState == XR_SESSION_STATE_LOSS_PENDING) {
				SessionLost(newState == XR_SESSION_STATE_EXITING ? "exiting" : "loss pending");
				return; // The instance is torn down next; its remaining events no longer matter.
			}
		} else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
			SessionLost("instance loss pending");
			return;
		} else if (event.type == XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR) {
			UpdateVisibilityMask(reinterpret_cast<XrEventDataVisibilityMaskChangedKHR *>(&event)->viewIndex);
		} else if (event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
			ControllersChanged();
		} else if (event.type == XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB) {
			LogInfo("VR: refresh rate now {:.0f} Hz", reinterpret_cast<XrEventDataDisplayRefreshRateChangedFB *>(&event)->toDisplayRefreshRate);
		}
		event = { XR_TYPE_EVENT_DATA_BUFFER };
	}
}

bool EnsureSwapchain(int width, int height)
{
	if (swapchain != XR_NULL_HANDLE && width == swapchainWidth && height == swapchainHeight)
		return true;
	if (swapchain != XR_NULL_HANDLE) {
		xrd3d::DestroySwapchain(swapchain);
		swapchain = XR_NULL_HANDLE;
	}

	XrSwapchainCreateInfo info { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	info.format = swapchainFormat;
	info.sampleCount = 1;
	info.width = width;
	info.height = height;
	info.faceCount = 1;
	info.arraySize = 1;
	info.mipCount = 1;
	if (!Check(xrd3d::CreateSwapchain(session, info, &swapchain), "xrCreateSwapchain"))
		return false;
	swapchainImages = xrd3d::SwapchainImages(swapchain);
	swapchainWidth = width;
	swapchainHeight = height;
	pixelBuffer.resize(static_cast<size_t>(width) * height * 4);
	return true;
}

bool UploadFrame(SDL_Surface *surface)
{
	if (!EnsureSwapchain(surface->w, surface->h))
		return false;
	if (SDL_ConvertPixels(surface->w, surface->h, surface->format->format, surface->pixels, surface->pitch,
	        swapchainSdlFormat, pixelBuffer.data(), surface->w * 4)
	    < 0)
		return false;
	// Both swapchain formats keep alpha in the fourth byte.
	for (int y = std::max(clearBlack.y, 0); y < std::min(clearBlack.y + clearBlack.h, surface->h); y++) {
		uint8_t *row = reinterpret_cast<uint8_t *>(pixelBuffer.data()) + static_cast<size_t>(y) * surface->w * 4;
		for (int x = std::max(clearBlack.x, 0); x < std::min(clearBlack.x + clearBlack.w, surface->w); x++) {
			uint8_t *pixel = row + static_cast<size_t>(x) * 4;
			if (pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0)
				pixel[3] = 0;
		}
	}

	uint32_t imageIndex = 0;
	if (XR_FAILED(xrd3d::AcquireSwapchainImage(swapchain, &imageIndex)))
		return false;
	const bool ok = XR_SUCCEEDED(xrd3d::WaitSwapchainImage(swapchain));
	if (ok)
		context->UpdateSubresource(swapchainImages[imageIndex], 0, nullptr, pixelBuffer.data(), surface->w * 4, 0);
	xrd3d::ReleaseSwapchainImage(swapchain);
	return ok;
}

void Cleanup()
{
	// What was made from the session's images goes first, then the session, then the device under it.
	VrWorldRelease();
	for (EyeTarget &eye : eyes) {
		for (ID3D11RenderTargetView *view : eye.views)
			view->Release();
		for (ID3D11DepthStencilView *view : eye.depthViews)
			view->Release();
		if (eye.maskVertices != nullptr)
			eye.maskVertices->Release();
		eye = {};
	}
	swapchainImages.clear();
	ReleaseMirror();
	ProfileRelease();
	if (session != XR_NULL_HANDLE)
		xrd3d::DestroySession(session); // Destroys the spaces and swapchains with it.
	if (instance != XR_NULL_HANDLE)
		xrDestroyInstance(instance); // And the actions.
	instance = XR_NULL_HANDLE;
	session = XR_NULL_HANDLE;
	swapchain = XR_NULL_HANDLE;
	sessionRunning = false;
	screenPlaced = false;
	if (context != nullptr)
		context->Release();
	if (device != nullptr)
		device->Release();
	context = nullptr;
	device = nullptr;
}

} // namespace vr
} // namespace devilution

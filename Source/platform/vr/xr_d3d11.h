/**
 * @file xr_d3d11.h
 *
 * The OpenXR calls that depend on the graphics binding.
 *
 * On Windows the runtime speaks Direct3D 11 itself (XR_KHR_D3D11_enable) and these are the
 * OpenXR functions under another name. Elsewhere the runtime only speaks Vulkan, the renderer's
 * Direct3D 11 runs on DXVK, and these hand the runtime DXVK's Vulkan device and turn the
 * runtime's Vulkan swapchain images into D3D11 textures. Either way the caller deals in
 * DXGI formats and ID3D11Texture2D.
 */
#pragma once

#include <cstdint>
#include <vector>

#include <d3d11.h>

#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace devilution {
namespace vr {
namespace xrd3d {

/** Receives the binding's diagnostics; null (the default) discards them. */
extern void (*log)(const char *message);

/** The instance extension that carries the graphics binding. */
const char *BindingExtension();

/**
 * Creates the D3D11 device on the GPU the headset is attached to.
 * No DXGI or D3D11 object may exist before this is called.
 */
bool CreateDevice(XrInstance instance, XrSystemId systemId, ID3D11Device **device, ID3D11DeviceContext **context);

XrResult CreateSession(XrInstance instance, XrSystemId systemId, ID3D11Device *device, XrSession *session);
/** To be called before the device is released. */
void DestroySession(XrSession session);

/** The runtime's swapchain formats as DXGI formats, in the runtime's order of preference. */
std::vector<int64_t> SwapchainFormats(XrSession session);

/** `info.format` is a DXGI format. */
XrResult CreateSwapchain(XrSession session, const XrSwapchainCreateInfo &info, XrSwapchain *swapchain);
void DestroySwapchain(XrSwapchain swapchain);
/** The textures belong to the swapchain: they are not to be released, and die with it. */
std::vector<ID3D11Texture2D *> SwapchainImages(XrSwapchain swapchain);

XrResult AcquireSwapchainImage(XrSwapchain swapchain, uint32_t *index);
XrResult WaitSwapchainImage(XrSwapchain swapchain);
/** Everything drawn to the image so far is handed to the GPU first. */
XrResult ReleaseSwapchainImage(XrSwapchain swapchain);
XrResult BeginFrame(XrSession session);
XrResult EndFrame(XrSession session, const XrFrameEndInfo &info);

} // namespace xrd3d
} // namespace vr
} // namespace devilution

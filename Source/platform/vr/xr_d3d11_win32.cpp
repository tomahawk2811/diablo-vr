/**
 * @file xr_d3d11_win32.cpp
 *
 * The Direct3D 11 graphics binding where the OpenXR runtime speaks it itself.
 */
#include "platform/vr/xr_d3d11.h"

#include <cstring>

#include <dxgi.h>

namespace devilution {
namespace vr {
namespace xrd3d {

void (*log)(const char *message);

const char *BindingExtension()
{
	return XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
}

bool CreateDevice(XrInstance instance, XrSystemId systemId, ID3D11Device **device, ID3D11DeviceContext **context)
{
	XrGraphicsRequirementsD3D11KHR requirements { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
	// Extension functions are not exported by the loader.
	PFN_xrGetD3D11GraphicsRequirementsKHR getRequirements = nullptr;
	if (XR_FAILED(xrGetInstanceProcAddr(instance, "xrGetD3D11GraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction *>(&getRequirements)))
	    || XR_FAILED(getRequirements(instance, systemId, &requirements))) {
		if (log != nullptr)
			log("xrGetD3D11GraphicsRequirementsKHR failed");
		return false;
	}

	// The device must live on the adapter the headset is attached to.
	IDXGIFactory1 *factory = nullptr;
	IDXGIAdapter1 *adapter = nullptr;
	if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory)))) {
		for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
			DXGI_ADAPTER_DESC1 adapterDesc;
			adapter->GetDesc1(&adapterDesc);
			if (std::memcmp(&adapterDesc.AdapterLuid, &requirements.adapterLuid, sizeof(LUID)) == 0)
				break;
			adapter->Release();
			adapter = nullptr;
		}
		factory->Release();
	}
	const HRESULT hr = D3D11CreateDevice(adapter, adapter != nullptr ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
	    nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, device, nullptr, context);
	if (adapter != nullptr)
		adapter->Release();
	if (FAILED(hr) && log != nullptr)
		log("D3D11CreateDevice failed");
	return SUCCEEDED(hr);
}

XrResult CreateSession(XrInstance instance, XrSystemId systemId, ID3D11Device *device, XrSession *session)
{
	XrGraphicsBindingD3D11KHR binding { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
	binding.device = device;
	XrSessionCreateInfo info { XR_TYPE_SESSION_CREATE_INFO };
	info.next = &binding;
	info.systemId = systemId;
	return xrCreateSession(instance, &info, session);
}

void DestroySession(XrSession session)
{
	xrDestroySession(session);
}

std::vector<int64_t> SwapchainFormats(XrSession session)
{
	uint32_t count = 0;
	xrEnumerateSwapchainFormats(session, 0, &count, nullptr);
	std::vector<int64_t> formats(count);
	xrEnumerateSwapchainFormats(session, count, &count, formats.data());
	return formats;
}

XrResult CreateSwapchain(XrSession session, const XrSwapchainCreateInfo &info, XrSwapchain *swapchain)
{
	return xrCreateSwapchain(session, &info, swapchain);
}

void DestroySwapchain(XrSwapchain swapchain)
{
	xrDestroySwapchain(swapchain);
}

std::vector<ID3D11Texture2D *> SwapchainImages(XrSwapchain swapchain)
{
	uint32_t count = 0;
	xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr);
	std::vector<XrSwapchainImageD3D11KHR> images(count, XrSwapchainImageD3D11KHR { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
	xrEnumerateSwapchainImages(swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data()));
	std::vector<ID3D11Texture2D *> textures;
	for (const XrSwapchainImageD3D11KHR &image : images)
		textures.push_back(image.texture);
	return textures;
}

XrResult AcquireSwapchainImage(XrSwapchain swapchain, uint32_t *index)
{
	const XrSwapchainImageAcquireInfo info { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	return xrAcquireSwapchainImage(swapchain, &info, index);
}

XrResult WaitSwapchainImage(XrSwapchain swapchain)
{
	XrSwapchainImageWaitInfo info { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	info.timeout = XR_INFINITE_DURATION;
	return xrWaitSwapchainImage(swapchain, &info);
}

XrResult ReleaseSwapchainImage(XrSwapchain swapchain)
{
	const XrSwapchainImageReleaseInfo info { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	return xrReleaseSwapchainImage(swapchain, &info);
}

XrResult BeginFrame(XrSession session)
{
	return xrBeginFrame(session, nullptr);
}

XrResult EndFrame(XrSession session, const XrFrameEndInfo &info)
{
	return xrEndFrame(session, &info);
}

} // namespace xrd3d
} // namespace vr
} // namespace devilution

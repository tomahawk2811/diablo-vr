/**
 * @file xr_d3d11_dxvk.cpp
 *
 * The Direct3D 11 graphics binding on an OpenXR runtime that only speaks Vulkan.
 *
 * The renderer's Direct3D 11 is DXVK here, which is Vulkan underneath, so the runtime gets
 * DXVK's Vulkan device (XR_KHR_vulkan_enable) and its swapchain images come back wrapped as
 * D3D11 textures. This is what Proton's wineopenxr does for Windows games.
 */
#include <vulkan/vulkan.h>

// Before xr_d3d11.h, which includes OpenXR's platform header for D3D11 alone.
#define XR_USE_GRAPHICS_API_VULKAN
#include "platform/vr/xr_d3d11.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

#include "platform/vr/dxvk/dxvk_interop.h"

namespace devilution {
namespace vr {
namespace xrd3d {

void (*log)(const char *message);

namespace {

// DXVK has no default window system off Windows and refuses to make a device without one. Set
// when the program starts, so it also holds for devices made elsewhere (the offscreen renderer).
[[maybe_unused]] const int wsiDriverSet = setenv("DXVK_WSI_DRIVER", "SDL2", 0);

IDXGIVkInteropDevice1 *interop;

struct Swapchain {
	XrSwapchainCreateInfo info;
	std::vector<ID3D11Texture2D *> images;
};
std::unordered_map<XrSwapchain, Swapchain> swapchains;

struct FormatPair {
	DXGI_FORMAT dxgi;
	VkFormat vulkan;
};

// DXVK must pick the same Vulkan format for the DXGI format as the runtime's image has.
// D24_UNORM_S8_UINT is left out for that reason: DXVK falls back to D32_SFLOAT_S8_UINT for it
// where the GPU has no 24-bit depth.
const FormatPair Formats[] = {
	{ DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, VK_FORMAT_R8G8B8A8_SRGB },
	{ DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, VK_FORMAT_B8G8R8A8_SRGB },
	{ DXGI_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM },
	{ DXGI_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM },
	{ DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT },
	{ DXGI_FORMAT_R10G10B10A2_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32 },
	{ DXGI_FORMAT_D32_FLOAT_S8X24_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT },
	{ DXGI_FORMAT_D32_FLOAT, VK_FORMAT_D32_SFLOAT },
	{ DXGI_FORMAT_D16_UNORM, VK_FORMAT_D16_UNORM },
};

void Log(const char *format, ...) __attribute__((format(printf, 1, 2)));
void Log(const char *format, ...)
{
	if (log == nullptr)
		return;
	char message[1024];
	va_list args;
	va_start(args, format);
	std::vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	log(message);
}

template <typename Function>
bool ExtensionFunction(XrInstance instance, const char *name, Function &function)
{
	if (XR_SUCCEEDED(xrGetInstanceProcAddr(instance, name, reinterpret_cast<PFN_xrVoidFunction *>(&function))))
		return true;
	Log("%s is missing", name);
	return false;
}

/** The space-separated extension list of xrGetVulkanInstanceExtensionsKHR or xrGetVulkanDeviceExtensionsKHR. */
bool ExtensionList(XrInstance instance, XrSystemId systemId, const char *name, std::string &list)
{
	XrResult(XRAPI_PTR * get)(XrInstance, XrSystemId, uint32_t, uint32_t *, char *) = nullptr;
	uint32_t size = 0;
	if (!ExtensionFunction(instance, name, get) || XR_FAILED(get(instance, systemId, 0, &size, nullptr)))
		return false;
	list.assign(size, '\0');
	if (size != 0 && XR_FAILED(get(instance, systemId, size, &size, list.data())))
		return false;
	list.resize(std::strlen(list.c_str()));
	Log("%s: %s", name, list.c_str());
	return true;
}

/** Keeps DXVK off the Vulkan queue while the runtime uses it. */
struct QueueLock {
	explicit QueueLock(bool flush = false)
	{
		if (interop == nullptr)
			return;
		if (flush)
			interop->FlushRenderingCommands();
		interop->LockSubmissionQueue();
	}
	~QueueLock()
	{
		if (interop != nullptr)
			interop->ReleaseSubmissionQueue();
	}
};

void ReleaseImages(Swapchain &swapchain)
{
	for (ID3D11Texture2D *image : swapchain.images)
		image->Release();
	swapchain.images.clear();
}

} // namespace

const char *BindingExtension()
{
	return XR_KHR_VULKAN_ENABLE_EXTENSION_NAME;
}

bool CreateDevice(XrInstance instance, XrSystemId systemId, ID3D11Device **device, ID3D11DeviceContext **context)
{
	// The runtime wants to be asked, although DXVK decides the Vulkan version.
	PFN_xrGetVulkanGraphicsRequirementsKHR getRequirements = nullptr;
	XrGraphicsRequirementsVulkanKHR requirements { XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };
	if (!ExtensionFunction(instance, "xrGetVulkanGraphicsRequirementsKHR", getRequirements)
	    || XR_FAILED(getRequirements(instance, systemId, &requirements)))
		return false;

	// DXVK creates the Vulkan instance and device, and learns here what the runtime needs of them.
	std::string instanceExtensions;
	std::string deviceExtensions;
	if (!ExtensionList(instance, systemId, "xrGetVulkanInstanceExtensionsKHR", instanceExtensions)
	    || !ExtensionList(instance, systemId, "xrGetVulkanDeviceExtensionsKHR", deviceExtensions))
		return false;
	setenv("DXVK_OPENXR_INSTANCE_EXTENSIONS", instanceExtensions.c_str(), 1);
	setenv("DXVK_OPENXR_DEVICE_EXTENSIONS", deviceExtensions.c_str(), 1);

	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory)))) {
		Log("CreateDXGIFactory1 failed");
		return false;
	}
	IDXGIVkInteropFactory *interopFactory = nullptr;
	VkInstance vulkanInstance = VK_NULL_HANDLE;
	if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIVkInteropFactory), reinterpret_cast<void **>(&interopFactory)))) {
		PFN_vkGetInstanceProcAddr getProcAddr = nullptr;
		interopFactory->GetVulkanInstance(&vulkanInstance, &getProcAddr);
		interopFactory->Release();
	}

	// The device must live on the GPU the headset is attached to.
	PFN_xrGetVulkanGraphicsDeviceKHR getGraphicsDevice = nullptr;
	VkPhysicalDevice wanted = VK_NULL_HANDLE;
	if (vulkanInstance == VK_NULL_HANDLE || !ExtensionFunction(instance, "xrGetVulkanGraphicsDeviceKHR", getGraphicsDevice)
	    || XR_FAILED(getGraphicsDevice(instance, systemId, vulkanInstance, &wanted))) {
		Log("xrGetVulkanGraphicsDeviceKHR failed");
		factory->Release();
		return false;
	}
	IDXGIAdapter1 *adapter = nullptr;
	for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
		IDXGIVkInteropAdapter *interopAdapter = nullptr;
		VkInstance adapterInstance = VK_NULL_HANDLE;
		VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
		if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIVkInteropAdapter), reinterpret_cast<void **>(&interopAdapter)))) {
			interopAdapter->GetVulkanHandles(&adapterInstance, &physicalDevice);
			interopAdapter->Release();
		}
		if (physicalDevice == wanted)
			break;
		adapter->Release();
		adapter = nullptr;
	}
	factory->Release();
	if (adapter == nullptr) {
		Log("the headset's GPU is not among DXVK's adapters");
		return false;
	}

	const HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, device, nullptr, context);
	adapter->Release();
	if (FAILED(hr)) {
		Log("D3D11CreateDevice failed (%#x)", static_cast<unsigned>(hr));
		return false;
	}
	return true;
}

XrResult CreateSession(XrInstance instance, XrSystemId systemId, ID3D11Device *device, XrSession *session)
{
	if (FAILED(device->QueryInterface(__uuidof(IDXGIVkInteropDevice1), reinterpret_cast<void **>(&interop)))) {
		Log("the D3D11 device is not DXVK's");
		return XR_ERROR_GRAPHICS_DEVICE_INVALID;
	}
	XrGraphicsBindingVulkanKHR binding { XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR };
	interop->GetVulkanHandles(&binding.instance, &binding.physicalDevice, &binding.device);
	interop->GetSubmissionQueue1(nullptr, &binding.queueIndex, &binding.queueFamilyIndex);
	XrSessionCreateInfo info { XR_TYPE_SESSION_CREATE_INFO };
	info.next = &binding;
	info.systemId = systemId;
	const QueueLock lock;
	const XrResult result = xrCreateSession(instance, &info, session);
	if (XR_FAILED(result)) {
		interop->Release();
		interop = nullptr;
	}
	return result;
}

void DestroySession(XrSession session)
{
	if (interop != nullptr)
		interop->FlushRenderingCommands();
	for (auto &entry : swapchains)
		ReleaseImages(entry.second);
	swapchains.clear();
	{
		const QueueLock lock;
		xrDestroySession(session);
	}
	if (interop != nullptr)
		interop->Release();
	interop = nullptr;
}

std::vector<int64_t> SwapchainFormats(XrSession session)
{
	uint32_t count = 0;
	xrEnumerateSwapchainFormats(session, 0, &count, nullptr);
	std::vector<int64_t> offered(count);
	xrEnumerateSwapchainFormats(session, count, &count, offered.data());
	std::vector<int64_t> formats;
	for (const int64_t format : offered) {
		for (const FormatPair &pair : Formats) {
			if (pair.vulkan == format)
				formats.push_back(pair.dxgi);
		}
	}
	return formats;
}

XrResult CreateSwapchain(XrSession session, const XrSwapchainCreateInfo &info, XrSwapchain *swapchain)
{
	XrSwapchainCreateInfo vulkanInfo = info;
	vulkanInfo.format = VK_FORMAT_UNDEFINED;
	for (const FormatPair &pair : Formats) {
		if (pair.dxgi == info.format)
			vulkanInfo.format = pair.vulkan;
	}
	if (vulkanInfo.format == VK_FORMAT_UNDEFINED)
		return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	// DXVK clears, copies and resolves with transfers.
	vulkanInfo.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;

	XrResult result;
	{
		const QueueLock lock;
		result = xrCreateSwapchain(session, &vulkanInfo, swapchain);
	}
	if (XR_FAILED(result))
		return result;

	uint32_t count = 0;
	xrEnumerateSwapchainImages(*swapchain, 0, &count, nullptr);
	std::vector<XrSwapchainImageVulkanKHR> vulkanImages(count, XrSwapchainImageVulkanKHR { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR });
	result = xrEnumerateSwapchainImages(*swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader *>(vulkanImages.data()));

	// The runtime expects its images back in the attachment layout. DXVK leaves a texture in that
	// layout when attaching is all it may do with it, so the textures are attachments only, whatever
	// else the caller asked for; copying from them works regardless.
	const bool depth = (info.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
	D3D11_TEXTURE2D_DESC1 desc {};
	desc.Width = info.width;
	desc.Height = info.height;
	desc.MipLevels = info.mipCount;
	desc.ArraySize = info.arraySize;
	desc.Format = static_cast<DXGI_FORMAT>(info.format);
	desc.SampleDesc.Count = info.sampleCount;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET;
	desc.TextureLayout = D3D11_TEXTURE_LAYOUT_UNDEFINED;
	Swapchain entry { info, {} };
	for (const XrSwapchainImageVulkanKHR &vulkanImage : vulkanImages) {
		ID3D11Texture2D *texture = nullptr;
		if (XR_FAILED(result) || FAILED(interop->CreateTexture2DFromVkImage(&desc, vulkanImage.image, &texture))) {
			Log("no D3D11 texture for a %ux%u swapchain image of format %d", info.width, info.height, static_cast<int>(info.format));
			ReleaseImages(entry);
			const QueueLock lock;
			xrDestroySwapchain(*swapchain);
			*swapchain = XR_NULL_HANDLE;
			return XR_ERROR_RUNTIME_FAILURE;
		}
		entry.images.push_back(texture);
	}
	swapchains[*swapchain] = std::move(entry);
	return XR_SUCCESS;
}

void DestroySwapchain(XrSwapchain swapchain)
{
	const auto it = swapchains.find(swapchain);
	if (it != swapchains.end()) {
		interop->FlushRenderingCommands();
		ReleaseImages(it->second);
		swapchains.erase(it);
	}
	const QueueLock lock;
	xrDestroySwapchain(swapchain);
}

std::vector<ID3D11Texture2D *> SwapchainImages(XrSwapchain swapchain)
{
	const auto it = swapchains.find(swapchain);
	return it != swapchains.end() ? it->second.images : std::vector<ID3D11Texture2D *> {};
}

XrResult AcquireSwapchainImage(XrSwapchain swapchain, uint32_t *index)
{
	const XrSwapchainImageAcquireInfo info { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	const QueueLock lock;
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
	const QueueLock lock(true);
	return xrReleaseSwapchainImage(swapchain, &info);
}

XrResult BeginFrame(XrSession session)
{
	const XrFrameBeginInfo info { XR_TYPE_FRAME_BEGIN_INFO };
	const QueueLock lock;
	return xrBeginFrame(session, &info);
}

XrResult EndFrame(XrSession session, const XrFrameEndInfo &info)
{
	const QueueLock lock(true);
	return xrEndFrame(session, &info);
}

} // namespace xrd3d
} // namespace vr
} // namespace devilution

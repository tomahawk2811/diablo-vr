/**
 * @file xr_probe.cpp
 *
 * Falsification probe for the Steam Frame build: before any game code runs on the headset, it
 * proves the route the renderer depends on. It opens an OpenXR session through the game's own
 * graphics binding (xr_d3d11.h: Direct3D 11 on DXVK, on the runtime's Vulkan device), compiles
 * HLSL with the game's D3DCompile, and each frame clears the left eye red and the right eye
 * blue, draws a green triangle into both and hands them to the compositor. Along the way it
 * logs what the runtime offers and creates one of each kind of texture the renderer needs.
 *
 *   xr_probe [--seconds N] [--force] [--out DIR]
 *
 * --force draws while the headset is in standby (nobody wearing it). --out names the folder
 * for probe-eye0.bmp and probe-eye1.bmp, read back from the runtime's swapchain images.
 */
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <dlfcn.h>

#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include "platform/vr/xr_d3d11.h"

using namespace devilution::vr;

namespace {

void Say(const char *format, ...) __attribute__((format(printf, 1, 2)));
void Say(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	std::printf("[probe] ");
	std::vprintf(format, args);
	std::printf("\n");
	va_end(args);
	std::fflush(stdout);
}

bool Ok(XrResult result, const char *what)
{
	if (XR_SUCCEEDED(result))
		return true;
	Say("FAIL %s: %d", what, static_cast<int>(result));
	return false;
}

const char ShaderSource[] = R"hlsl(
float4 VsTriangle(uint vertex : SV_VertexID) : SV_Position
{
	float2 corner = float2(vertex == 1 ? 0.5 : (vertex == 2 ? -0.5 : 0.0), vertex == 0 ? 0.5 : -0.5);
	return float4(corner, 0.5, 1);
}

float4 PsGreen() : SV_Target
{
	return float4(0, 1, 0, 1);
}
)hlsl";

ID3DBlob *Compile(const char *entry, const char *profile)
{
	ID3DBlob *code = nullptr;
	ID3DBlob *errors = nullptr;
	const HRESULT hr = D3DCompile(ShaderSource, sizeof(ShaderSource) - 1, "probe", nullptr, nullptr, entry, profile, 0, 0, &code, &errors);
	if (FAILED(hr))
		Say("FAIL D3DCompile %s: %s", entry, errors != nullptr ? static_cast<const char *>(errors->GetBufferPointer()) : "?");
	if (errors != nullptr)
		errors->Release();
	return code;
}

void WriteBmp(const std::string &path, const uint8_t *pixels, int width, int height, int pitch, bool bgra)
{
	FILE *file = std::fopen(path.c_str(), "wb");
	if (file == nullptr)
		return;
	const uint32_t rowSize = static_cast<uint32_t>(width) * 3 + (4 - width * 3 % 4) % 4;
	const uint32_t dataSize = rowSize * height;
	uint8_t header[54] = { 'B', 'M' };
	const auto put = [&](int offset, uint32_t value) { std::memcpy(header + offset, &value, 4); };
	put(2, 54 + dataSize);
	put(10, 54);
	put(14, 40);
	put(18, width);
	put(22, height);
	header[26] = 1;
	header[28] = 24;
	put(34, dataSize);
	std::fwrite(header, 1, sizeof(header), file);
	std::vector<uint8_t> row(rowSize);
	for (int y = height - 1; y >= 0; y--) {
		const uint8_t *source = pixels + static_cast<size_t>(y) * pitch;
		for (int x = 0; x < width; x++) {
			row[x * 3 + 0] = source[x * 4 + (bgra ? 0 : 2)];
			row[x * 3 + 1] = source[x * 4 + 1];
			row[x * 3 + 2] = source[x * 4 + (bgra ? 2 : 0)];
		}
		std::fwrite(row.data(), 1, rowSize, file);
	}
	std::fclose(file);
}

struct Eye {
	XrSwapchain swapchain = XR_NULL_HANDLE;
	XrSwapchain depthSwapchain = XR_NULL_HANDLE;
	std::vector<ID3D11RenderTargetView *> views;
	std::vector<ID3D11Texture2D *> images;
	std::vector<ID3D11DepthStencilView *> depthViews;
	int width = 0;
	int height = 0;
};

/** One of each kind of texture the renderer creates, at its real size. */
void CheckRendererTextures(ID3D11Device *device, int eyeWidth, int eyeHeight)
{
	struct Case {
		const char *name;
		D3D11_TEXTURE2D_DESC desc;
	};
	const auto desc = [](int width, int height, int layers, int mips, DXGI_FORMAT format, int samples, UINT bind, UINT misc = 0) {
		D3D11_TEXTURE2D_DESC d {};
		d.Width = width;
		d.Height = height;
		d.ArraySize = layers;
		d.MipLevels = mips;
		d.Format = format;
		d.SampleDesc.Count = samples;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = bind;
		d.MiscFlags = misc;
		return d;
	};
	const UINT target = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	const UINT depth = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
	const Case cases[] = {
		{ "HDR colour, 4x MSAA, 2 slices", desc(eyeWidth, eyeHeight, 2, 1, DXGI_FORMAT_R11G11B10_FLOAT, 4, target) },
		{ "depth, 4x MSAA, 2 slices", desc(eyeWidth, eyeHeight, 2, 1, DXGI_FORMAT_R32_TYPELESS, 4, depth) },
		{ "HDR colour, 2 slices", desc(eyeWidth, eyeHeight, 2, 1, DXGI_FORMAT_R11G11B10_FLOAT, 1, target) },
		{ "surface array 64x128x2048 with mips", desc(64, 128, 2048, 0, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 1, target, D3D11_RESOURCE_MISC_GENERATE_MIPS) },
		{ "sprite atlas 8192x8192 RG8", desc(8192, 8192, 1, 1, DXGI_FORMAT_R8G8_UNORM, 1, D3D11_BIND_SHADER_RESOURCE) },
		{ "light grid R32_UINT array", desc(128, 128, 8, 1, DXGI_FORMAT_R32_UINT, 1, D3D11_BIND_SHADER_RESOURCE) },
		{ "shadow map 2048 depth", desc(2048, 2048, 1, 1, DXGI_FORMAT_R32_TYPELESS, 1, depth) },
	};
	for (const Case &c : cases) {
		ID3D11Texture2D *texture = nullptr;
		const HRESULT hr = device->CreateTexture2D(&c.desc, nullptr, &texture);
		Say("%s texture: %s", SUCCEEDED(hr) ? "ok  " : "FAIL", c.name);
		if (texture != nullptr)
			texture->Release();
	}
	for (const UINT samples : { 2U, 4U, 8U }) {
		UINT levels = 0;
		device->CheckMultisampleQualityLevels(DXGI_FORMAT_R11G11B10_FLOAT, samples, &levels);
		Say("MSAA %ux R11G11B10_FLOAT: %s", samples, levels != 0 ? "supported" : "not supported");
	}
	D3D11_FEATURE_DATA_D3D11_OPTIONS3 options {};
	const bool layered = SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS3, &options, sizeof(options)))
	    && options.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer != FALSE;
	Say("render-target array index from the vertex shader (single-pass stereo): %s", layered ? "yes" : "no");
}

} // namespace

int main(int argc, char **argv)
{
	double seconds = 10;
	bool force = false;
	std::string out = ".";
	for (int i = 1; i < argc; i++) {
		if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
			seconds = std::atof(argv[++i]);
		else if (std::strcmp(argv[i], "--force") == 0)
			force = true;
		else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc)
			out = argv[++i];
	}
	xrd3d::log = [](const char *message) { Say("binding: %s", message); };

	// DXVK asks SDL which Vulkan extensions a window needs, and SDL must have its video
	// subsystem up to answer. The game has that long before it gets here.
	if (void *sdl = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_GLOBAL); sdl != nullptr) {
		const auto init = reinterpret_cast<int (*)(uint32_t)>(dlsym(sdl, "SDL_Init"));
		const int result = init != nullptr ? init(0x20 /* SDL_INIT_VIDEO */) : -1;
		Say("SDL_Init(video): %d", result);
	} else {
		Say("FAIL no libSDL2-2.0.so.0");
	}

	uint32_t count = 0;
	xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
	std::vector<XrExtensionProperties> offered(count, XrExtensionProperties { XR_TYPE_EXTENSION_PROPERTIES });
	xrEnumerateInstanceExtensionProperties(nullptr, count, &count, offered.data());
	std::string list;
	for (const XrExtensionProperties &extension : offered)
		list += std::string(extension.extensionName) + " ";
	Say("runtime extensions: %s", list.c_str());
	const auto has = [&](const char *name) {
		for (const XrExtensionProperties &extension : offered) {
			if (std::strcmp(extension.extensionName, name) == 0)
				return true;
		}
		return false;
	};
	std::vector<const char *> extensions { xrd3d::BindingExtension() };
	const bool hasDepthLayer = has(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
	const bool hasRefreshRate = has(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
	if (hasDepthLayer)
		extensions.push_back(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
	if (hasRefreshRate)
		extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
	if (has(XR_KHR_VISIBILITY_MASK_EXTENSION_NAME))
		extensions.push_back(XR_KHR_VISIBILITY_MASK_EXTENSION_NAME);

	XrInstanceCreateInfo instanceInfo { XR_TYPE_INSTANCE_CREATE_INFO };
	std::strcpy(instanceInfo.applicationInfo.applicationName, "DevilutionX");
	instanceInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	instanceInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
	instanceInfo.enabledExtensionNames = extensions.data();
	XrInstance instance = XR_NULL_HANDLE;
	if (!Ok(xrCreateInstance(&instanceInfo, &instance), "xrCreateInstance"))
		return 1;
	XrInstanceProperties instanceProperties { XR_TYPE_INSTANCE_PROPERTIES };
	xrGetInstanceProperties(instance, &instanceProperties);
	Say("runtime: %s %u.%u.%u", instanceProperties.runtimeName, XR_VERSION_MAJOR(instanceProperties.runtimeVersion),
	    XR_VERSION_MINOR(instanceProperties.runtimeVersion), XR_VERSION_PATCH(instanceProperties.runtimeVersion));

	XrSystemGetInfo systemInfo { XR_TYPE_SYSTEM_GET_INFO };
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId systemId = XR_NULL_SYSTEM_ID;
	if (!Ok(xrGetSystem(instance, &systemInfo, &systemId), "xrGetSystem"))
		return 1;

	ID3D11Device *device = nullptr;
	ID3D11DeviceContext *context = nullptr;
	if (!xrd3d::CreateDevice(instance, systemId, &device, &context)) {
		Say("FAIL CreateDevice");
		return 1;
	}
	Say("D3D11 device created, feature level %#x", static_cast<unsigned>(device->GetFeatureLevel()));

	XrSession session = XR_NULL_HANDLE;
	if (!Ok(xrd3d::CreateSession(instance, systemId, device, &session), "CreateSession"))
		return 1;
	Say("session created");

	XrReferenceSpaceCreateInfo spaceInfo { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	spaceInfo.poseInReferenceSpace.orientation.w = 1;
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
	XrSpace space = XR_NULL_HANDLE;
	if (XR_FAILED(xrCreateReferenceSpace(session, &spaceInfo, &space))) {
		Say("no STAGE space, using LOCAL");
		spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		if (!Ok(xrCreateReferenceSpace(session, &spaceInfo, &space), "xrCreateReferenceSpace"))
			return 1;
	}

	const std::vector<int64_t> formats = xrd3d::SwapchainFormats(session);
	list.clear();
	for (const int64_t format : formats)
		list += std::to_string(format) + " ";
	Say("swapchain formats (DXGI): %s", list.c_str());
	int64_t colorFormat = 0;
	int64_t depthFormat = 0;
	for (const int64_t format : formats) {
		if (colorFormat == 0 && (format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))
			colorFormat = format;
	}
	for (const int64_t wanted : { DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D16_UNORM }) {
		for (const int64_t format : formats) {
			if (depthFormat == 0 && format == wanted)
				depthFormat = format;
		}
	}
	if (colorFormat == 0) {
		Say("FAIL no 8-bit sRGB swapchain format");
		return 1;
	}
	Say("using colour format %d, depth format %d, depth layer %s", static_cast<int>(colorFormat), static_cast<int>(depthFormat), hasDepthLayer ? "offered" : "absent");

	if (hasRefreshRate) {
		PFN_xrEnumerateDisplayRefreshRatesFB enumerateRates = nullptr;
		PFN_xrGetDisplayRefreshRateFB getRate = nullptr;
		xrGetInstanceProcAddr(instance, "xrEnumerateDisplayRefreshRatesFB", reinterpret_cast<PFN_xrVoidFunction *>(&enumerateRates));
		xrGetInstanceProcAddr(instance, "xrGetDisplayRefreshRateFB", reinterpret_cast<PFN_xrVoidFunction *>(&getRate));
		uint32_t rateCount = 0;
		enumerateRates(session, 0, &rateCount, nullptr);
		std::vector<float> rates(rateCount);
		enumerateRates(session, rateCount, &rateCount, rates.data());
		float current = 0;
		getRate(session, &current);
		list.clear();
		for (const float rate : rates)
			list += std::to_string(static_cast<int>(rate + 0.5F)) + " ";
		Say("refresh rates offered: %sHz, current %.0f Hz", list.c_str(), current);
	}

	uint32_t viewCount = 0;
	XrViewConfigurationView configViews[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
	if (!Ok(xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &viewCount, configViews), "xrEnumerateViewConfigurationViews"))
		return 1;

	Eye eyes[2];
	for (int i = 0; i < 2; i++) {
		Eye &eye = eyes[i];
		eye.width = static_cast<int>(configViews[i].recommendedImageRectWidth);
		eye.height = static_cast<int>(configViews[i].recommendedImageRectHeight);
		Say("eye %d: recommended %dx%d, max %ux%u, samples %u", i, eye.width, eye.height, configViews[i].maxImageRectWidth,
		    configViews[i].maxImageRectHeight, configViews[i].recommendedSwapchainSampleCount);
		XrSwapchainCreateInfo info { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
		info.format = colorFormat;
		info.sampleCount = 1;
		info.width = eye.width;
		info.height = eye.height;
		info.faceCount = 1;
		info.arraySize = 1;
		info.mipCount = 1;
		if (!Ok(xrd3d::CreateSwapchain(session, info, &eye.swapchain), "CreateSwapchain(colour)"))
			return 1;
		eye.images = xrd3d::SwapchainImages(eye.swapchain);
		for (ID3D11Texture2D *image : eye.images) {
			D3D11_RENDER_TARGET_VIEW_DESC viewDesc {};
			viewDesc.Format = static_cast<DXGI_FORMAT>(colorFormat);
			viewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			ID3D11RenderTargetView *view = nullptr;
			if (FAILED(device->CreateRenderTargetView(image, &viewDesc, &view))) {
				Say("FAIL CreateRenderTargetView");
				return 1;
			}
			eye.views.push_back(view);
		}
		if (hasDepthLayer && depthFormat != 0) {
			info.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
			info.format = depthFormat;
			if (Ok(xrd3d::CreateSwapchain(session, info, &eye.depthSwapchain), "CreateSwapchain(depth)")) {
				for (ID3D11Texture2D *image : xrd3d::SwapchainImages(eye.depthSwapchain)) {
					D3D11_DEPTH_STENCIL_VIEW_DESC viewDesc {};
					viewDesc.Format = static_cast<DXGI_FORMAT>(depthFormat);
					viewDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
					ID3D11DepthStencilView *view = nullptr;
					if (FAILED(device->CreateDepthStencilView(image, &viewDesc, &view)))
						Say("FAIL CreateDepthStencilView");
					eye.depthViews.push_back(view);
				}
			}
		}
		Say("eye %d: %zu colour images, %zu depth images", i, eye.images.size(), eye.depthViews.size());
	}

	CheckRendererTextures(device, eyes[0].width, eyes[0].height);

	ID3D11VertexShader *vertexShader = nullptr;
	ID3D11PixelShader *pixelShader = nullptr;
	if (ID3DBlob *code = Compile("VsTriangle", "vs_5_0"); code != nullptr) {
		device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vertexShader);
		code->Release();
	}
	if (ID3DBlob *code = Compile("PsGreen", "ps_5_0"); code != nullptr) {
		device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &pixelShader);
		code->Release();
	}
	Say("shaders: vertex %s, pixel %s", vertexShader != nullptr ? "ok" : "FAIL", pixelShader != nullptr ? "ok" : "FAIL");
	D3D11_RASTERIZER_DESC rasterDesc {};
	rasterDesc.FillMode = D3D11_FILL_SOLID;
	rasterDesc.CullMode = D3D11_CULL_NONE;
	rasterDesc.DepthClipEnable = TRUE;
	ID3D11RasterizerState *raster = nullptr;
	device->CreateRasterizerState(&rasterDesc, &raster);

	D3D11_TEXTURE2D_DESC stagingDesc {};
	stagingDesc.Width = eyes[0].width;
	stagingDesc.Height = eyes[0].height;
	stagingDesc.MipLevels = 1;
	stagingDesc.ArraySize = 1;
	stagingDesc.Format = static_cast<DXGI_FORMAT>(colorFormat);
	stagingDesc.SampleDesc.Count = 1;
	stagingDesc.Usage = D3D11_USAGE_STAGING;
	stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ID3D11Texture2D *staging = nullptr;
	device->CreateTexture2D(&stagingDesc, nullptr, &staging);

	bool running = false;
	bool quit = false;
	bool dumped = false;
	bool passed = false;
	int frames = 0;
	int rendered = 0;
	XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
	const auto start = std::chrono::steady_clock::now();
	const auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); };
	while (!quit && elapsed() < seconds) {
		XrEventDataBuffer event { XR_TYPE_EVENT_DATA_BUFFER };
		while (xrPollEvent(instance, &event) == XR_SUCCESS) {
			if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
				sessionState = reinterpret_cast<XrEventDataSessionStateChanged *>(&event)->state;
				Say("session state %d at %.2f s", static_cast<int>(sessionState), elapsed());
				if (sessionState == XR_SESSION_STATE_READY) {
					XrSessionBeginInfo beginInfo { XR_TYPE_SESSION_BEGIN_INFO };
					beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					running = Ok(xrBeginSession(session, &beginInfo), "xrBeginSession");
				} else if (sessionState == XR_SESSION_STATE_STOPPING) {
					xrEndSession(session);
					running = false;
				} else if (sessionState == XR_SESSION_STATE_EXITING || sessionState == XR_SESSION_STATE_LOSS_PENDING) {
					quit = true;
				}
			}
			event = { XR_TYPE_EVENT_DATA_BUFFER };
		}
		if (!running) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			continue;
		}

		XrFrameState frameState { XR_TYPE_FRAME_STATE };
		if (!Ok(xrWaitFrame(session, nullptr, &frameState), "xrWaitFrame") || !Ok(xrd3d::BeginFrame(session), "BeginFrame"))
			break;
		if (frames == 0)
			Say("first frame: period %.2f ms (%.1f Hz), shouldRender %d", frameState.predictedDisplayPeriod / 1e6,
			    1e9 / static_cast<double>(frameState.predictedDisplayPeriod), static_cast<int>(frameState.shouldRender));
		frames++;

		XrCompositionLayerProjectionView projectionViews[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
		XrCompositionLayerDepthInfoKHR depthInfos[2] = { { XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR }, { XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR } };
		bool drawn = false;
		if (frameState.shouldRender || force) {
			XrViewLocateInfo locateInfo { XR_TYPE_VIEW_LOCATE_INFO };
			locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			locateInfo.displayTime = frameState.predictedDisplayTime;
			locateInfo.space = space;
			XrViewState viewState { XR_TYPE_VIEW_STATE };
			XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
			uint32_t located = 0;
			xrLocateViews(session, &locateInfo, &viewState, 2, &located, views);
			if ((viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0) {
				// Standby: no pose. A synthetic head, standing at the origin and looking ahead.
				for (int i = 0; i < 2; i++) {
					views[i].pose.orientation = { 0, 0, 0, 1 };
					views[i].pose.position = { i == 0 ? -0.032F : 0.032F, 1.6F, 0 };
					if (views[i].fov.angleRight <= views[i].fov.angleLeft)
						views[i].fov = { -0.9F, 0.9F, 0.9F, -0.9F };
				}
			}
			if (rendered == 0) {
				for (int i = 0; i < 2; i++)
					Say("eye %d fov l %.3f r %.3f u %.3f d %.3f, position %.3f %.3f %.3f, pose valid %d", i, views[i].fov.angleLeft, views[i].fov.angleRight,
					    views[i].fov.angleUp, views[i].fov.angleDown, views[i].pose.position.x, views[i].pose.position.y, views[i].pose.position.z,
					    static_cast<int>((viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0));
			}

			drawn = true;
			for (int i = 0; i < 2 && drawn; i++) {
				Eye &eye = eyes[i];
				uint32_t index = 0;
				if (!Ok(xrd3d::AcquireSwapchainImage(eye.swapchain, &index), "AcquireSwapchainImage") || !Ok(xrd3d::WaitSwapchainImage(eye.swapchain), "WaitSwapchainImage")) {
					drawn = false;
					break;
				}
				ID3D11DepthStencilView *depthView = nullptr;
				if (eye.depthSwapchain != XR_NULL_HANDLE) {
					uint32_t depthIndex = 0;
					if (Ok(xrd3d::AcquireSwapchainImage(eye.depthSwapchain, &depthIndex), "AcquireSwapchainImage(depth)")) {
						xrd3d::WaitSwapchainImage(eye.depthSwapchain);
						depthView = eye.depthViews[depthIndex];
					}
				}
				// Linear values: the sRGB target stores them encoded (0.6 comes back as about 203).
				const float color[4] = { i == 0 ? 0.6F : 0.0F, 0.0F, i == 0 ? 0.0F : 0.6F, 1.0F };
				context->ClearRenderTargetView(eye.views[index], color);
				if (depthView != nullptr)
					context->ClearDepthStencilView(depthView, D3D11_CLEAR_DEPTH, 0, 0);
				const D3D11_VIEWPORT viewport { 0, 0, static_cast<float>(eye.width), static_cast<float>(eye.height), 0, 1 };
				context->RSSetViewports(1, &viewport);
				context->RSSetState(raster);
				context->OMSetRenderTargets(1, &eye.views[index], nullptr);
				context->IASetInputLayout(nullptr);
				context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				context->VSSetShader(vertexShader, nullptr, 0);
				context->PSSetShader(pixelShader, nullptr, 0);
				if (vertexShader != nullptr && pixelShader != nullptr)
					context->Draw(3, 0);
				ID3D11RenderTargetView *none = nullptr;
				context->OMSetRenderTargets(1, &none, nullptr);

				if (!dumped && rendered == 20 && staging != nullptr) {
					context->CopyResource(staging, eye.images[index]);
					D3D11_MAPPED_SUBRESOURCE mapped {};
					if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
						const uint8_t *pixels = static_cast<const uint8_t *>(mapped.pData);
						const bool bgra = colorFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
						const auto pixel = [&](int x, int y, int channel) {
							const uint8_t *p = pixels + static_cast<size_t>(y) * mapped.RowPitch + static_cast<size_t>(x) * 4;
							return static_cast<int>(p[bgra ? 2 - channel : channel]);
						};
						const int cx = eye.width / 2;
						const int cy = eye.height / 2;
						const int ex = eye.width / 16;
						const int ey = eye.height / 16;
						Say("eye %d readback: centre rgb %d %d %d (want 0 255 0), corner rgb %d %d %d (want %s)", i, pixel(cx, cy, 0), pixel(cx, cy, 1),
						    pixel(cx, cy, 2), pixel(ex, ey, 0), pixel(ex, ey, 1), pixel(ex, ey, 2), i == 0 ? "203 0 0" : "0 0 203");
						const bool centre = pixel(cx, cy, 0) < 8 && pixel(cx, cy, 1) > 247 && pixel(cx, cy, 2) < 8;
						const int strong = pixel(ex, ey, i == 0 ? 0 : 2);
						const bool corner = strong > 195 && strong < 211 && pixel(ex, ey, 1) < 8 && pixel(ex, ey, i == 0 ? 2 : 0) < 8;
						passed = (i == 0 || passed) && centre && corner;
						WriteBmp(out + "/probe-eye" + std::to_string(i) + ".bmp", pixels, eye.width, eye.height, static_cast<int>(mapped.RowPitch), bgra);
						context->Unmap(staging, 0);
					} else {
						Say("FAIL Map(staging)");
					}
					if (i == 1)
						dumped = true;
				}

				Ok(xrd3d::ReleaseSwapchainImage(eye.swapchain), "ReleaseSwapchainImage");
				projectionViews[i].pose = views[i].pose;
				projectionViews[i].fov = views[i].fov;
				projectionViews[i].subImage.swapchain = eye.swapchain;
				projectionViews[i].subImage.imageRect.extent = { eye.width, eye.height };
				if (depthView != nullptr) {
					Ok(xrd3d::ReleaseSwapchainImage(eye.depthSwapchain), "ReleaseSwapchainImage(depth)");
					depthInfos[i].subImage.swapchain = eye.depthSwapchain;
					depthInfos[i].subImage.imageRect.extent = { eye.width, eye.height };
					depthInfos[i].minDepth = 0;
					depthInfos[i].maxDepth = 1;
					depthInfos[i].nearZ = 400;
					depthInfos[i].farZ = 0.05F;
					projectionViews[i].next = &depthInfos[i];
				}
			}
			if (drawn)
				rendered++;
		}

		XrCompositionLayerProjection projection { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
		projection.space = space;
		projection.viewCount = 2;
		projection.views = projectionViews;
		const XrCompositionLayerBaseHeader *layers[] = { reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projection) };
		XrFrameEndInfo endInfo { XR_TYPE_FRAME_END_INFO };
		endInfo.displayTime = frameState.predictedDisplayTime;
		endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		endInfo.layerCount = drawn ? 1 : 0;
		endInfo.layers = layers;
		const XrResult endResult = xrd3d::EndFrame(session, endInfo);
		if (XR_FAILED(endResult)) {
			Say("FAIL EndFrame: %d", static_cast<int>(endResult));
			break;
		}
		if (frames % 300 == 0)
			Say("%d frames, %d rendered, %.1f fps", frames, rendered, frames / elapsed());
	}
	Say("done: %d frames, %d rendered in %.1f s, last session state %d", frames, rendered, elapsed(), static_cast<int>(sessionState));
	Say("%s", passed ? "PASS: both eyes read back with their colours" : "NOT PASSED: no correct readback (see above)");

	context->ClearState();
	context->Flush();
	for (Eye &eye : eyes) {
		for (ID3D11RenderTargetView *view : eye.views)
			view->Release();
		for (ID3D11DepthStencilView *view : eye.depthViews) {
			if (view != nullptr)
				view->Release();
		}
		if (eye.depthSwapchain != XR_NULL_HANDLE)
			xrd3d::DestroySwapchain(eye.depthSwapchain);
		xrd3d::DestroySwapchain(eye.swapchain);
	}
	if (running)
		xrEndSession(session);
	xrd3d::DestroySession(session);
	xrDestroyInstance(instance);
	return passed ? 0 : 2;
}

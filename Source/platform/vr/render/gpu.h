/**
 * @file gpu.h
 *
 * Thin D3D11 layer of the 3D renderer: owned resources, shader compilation and
 * memory accounting. Everything that touches the device goes through here, so the
 * passes in renderer.cpp deal in textures and buffers, not in descriptor structs.
 */
#pragma once

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wrl/client.h>

namespace devilution {
namespace vr {
namespace render {

using Microsoft::WRL::ComPtr;

extern ID3D11Device *device;
extern ID3D11DeviceContext *context;

/** What GPU memory is spent on, for the profiler. */
enum class MemoryKind : uint8_t {
	Targets,
	Textures,
	Geometry,
	Dynamic,
	Count,
};

/** Bytes currently allocated per kind (our own estimate: what we asked the device for). */
extern uint64_t memoryBytes[static_cast<size_t>(MemoryKind::Count)];

enum TextureFlags : uint32_t {
	TextureRenderTarget = 1 << 0,
	TextureDepth = 1 << 1,
	/** Full mip chain, filled by GenerateMips after an upload. */
	TextureMips = 1 << 2,
	/** A view per array slice in addition to the view of the whole array. */
	TextureSliceViews = 1 << 3,
};

struct Texture {
	ComPtr<ID3D11Texture2D> texture;
	ComPtr<ID3D11ShaderResourceView> srv;
	/** Whole array. */
	ComPtr<ID3D11RenderTargetView> rtv;
	ComPtr<ID3D11DepthStencilView> dsv;
	std::vector<ComPtr<ID3D11RenderTargetView>> sliceRtv;
	std::vector<ComPtr<ID3D11DepthStencilView>> sliceDsv;
	int width = 0;
	int height = 0;
	int layers = 0;
	uint64_t bytes = 0;
	MemoryKind kind = MemoryKind::Textures;

	Texture() = default;
	Texture(Texture &&other) noexcept { *this = std::move(other); }
	Texture &operator=(Texture &&other) noexcept;
	~Texture();

	explicit operator bool() const { return texture != nullptr; }
	void Release();
};

/**
 * Creates a 2D texture (array). Depth textures are created typeless so they can be sampled:
 * pass DXGI_FORMAT_D32_FLOAT and get an R32_FLOAT shader view.
 */
Texture CreateTexture(int width, int height, int layers, DXGI_FORMAT format, uint32_t flags = 0, int samples = 1, MemoryKind kind = MemoryKind::Textures);

/** Copies tightly packed pixels into a region of one array slice (mip 0). */
void UploadTexture(const Texture &texture, int layer, int x, int y, int width, int height, const void *pixels, int bytesPerPixel);

/**
 * A buffer whose contents are replaced every time it is used (instances, constants).
 * It grows when asked to hold more, and never shrinks, so steady state allocates nothing.
 */
struct DynamicBuffer {
	ComPtr<ID3D11Buffer> buffer;
	/** Only for structured buffers. */
	ComPtr<ID3D11ShaderResourceView> srv;
	uint32_t capacity = 0;
	uint32_t stride = 0;
	UINT bind = 0;

	~DynamicBuffer();
	/** `stride` != 0 makes a structured buffer with a shader view; `format` != UNKNOWN a typed buffer view. */
	void Upload(const void *data, uint32_t bytes, UINT bindFlags, uint32_t structureStride = 0, DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN);
	void Release();
};

/** Immutable vertex or index data. */
ComPtr<ID3D11Buffer> CreateStaticBuffer(const void *data, uint32_t bytes, UINT bindFlags);
/** To be called when a buffer made by CreateStaticBuffer is dropped. */
void ReleaseStaticBytes(uint32_t bytes);

/** Compiles one entry point. Logs the compiler's message and returns null on failure. */
ComPtr<ID3DBlob> CompileShader(std::string_view source, const char *entry, const char *profile);

/** Whether a vertex shader may pick the render-target array slice (single-pass stereo without a geometry shader). */
bool SupportsLayeredVertexShader();

} // namespace render
} // namespace vr
} // namespace devilution

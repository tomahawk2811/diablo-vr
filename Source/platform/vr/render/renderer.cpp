/**
 * @file renderer.cpp
 *
 * The render passes. See renderer.h for the order.
 */
#include "platform/vr/render/renderer.h"

#include <algorithm>
#include <cstring>
#include <string>

#include <SDL.h>
#include <sys/stat.h>

#include "platform/vr/debug/profile.h"
#include "platform/vr/knob.h"
#include "utils/log.hpp"

namespace devilution {
namespace vr {
namespace render {

namespace {

struct FrameConstants {
	Mat4 viewProj[MaxViews];
	float eyePosition[MaxViews][4];
	float headRight[4];
	float headUp[4];
	float fog[4];
	float ambient[4];
	float sunDirection[4];
	float sunColor[4];
	Mat4 shadowViewProj;
	float level[4];
	float abyss[4];
	float palette[4];
	float viewFog[4];
	uint32_t viewBase;
	uint32_t viewCount;
	uint32_t debugView;
	uint32_t lightCount;
	Light lights[MaxLights];
};

struct DrawConstants {
	uint32_t instanceBase;
	uint32_t pad[3];
	float post[4];
	float panelCenter[4];
	float panelRight[4];
	float panelUp[4];
};

constexpr int ShadowMapSize = 2048;
/** Side of the square the sun's shadow map covers, in meters. */
constexpr float ShadowExtent = 56;
constexpr int BloomLevels = 5;
constexpr int LightGridSize = 128;

/** 1 albedo, 2 normals, 3 light only, 4 lights per pixel, 5 lit grey, 6 wireframe. */
const int DebugView = static_cast<int>(Knob("DEVILUTIONX_VR_DEBUG_VIEW", 0));
/** Grit on walls and floors: square texels with grain and dirt, dithered light levels. 0 turns it off, 1 is full. */
const float Grit = Knob("DEVILUTIONX_VR_GRIT", Classic() ? 0.F : 1.F);
/** Grit texels per meter: the game's own wall art has about 45. */
const float GritTexels = Knob("DEVILUTIONX_VR_GRIT_TEXELS", 48.0F);
/** 1: no MSAA. The scene is then drawn straight into the post-processing source, with no resolve pass. */
const int Samples = std::clamp(static_cast<int>(Knob("DEVILUTIONX_VR_MSAA", 4)), 1, 8);
/** Samples a pixel takes towards each light to see whether a wall is in the way. 0: walls cast no shadows. */
const int WallShadowSteps = std::clamp(static_cast<int>(Knob("DEVILUTIONX_VR_WALL_SHADOW_STEPS", 16)), 0, 64);
/** Tiles between those samples, so near lights cost less. 0: always all of them. */
const float WallShadowSpacing = Knob("DEVILUTIONX_VR_WALL_SHADOW_SPACING", 0);
/**
 * 1: what is solid is drawn nearest first, the held things before the level, so the depth test
 * saves shading what ends up hidden. 0: in the order the scene lists it. The picture is the same.
 */
const bool NearestFirst = Knob("DEVILUTIONX_VR_NEAREST_FIRST", 1) != 0;
/** 0: no bloom, and none of its passes. */
const bool Bloom = Knob("DEVILUTIONX_VR_BLOOM", 1) != 0;
/** Filtered comparisons a pixel makes against the sun's shadow map: 9, 4 or 1. */
const int SunShadowTaps = static_cast<int>(Knob("DEVILUTIONX_VR_SUN_SHADOW_TAPS", 9));
/**
 * 1: levels without materials are drawn depth first, by a shader that only cuts the art's holes,
 * and then shaded where they are seen. Worth it where much is hidden behind cut-out walls (the
 * caves). Only without MSAA, where a cut-out has no smoothed edge to lose.
 */
const bool DepthPrepass = Knob("DEVILUTIONX_VR_DEPTH_PREPASS", 0) != 0 && Samples == 1;
const int Anisotropy = std::clamp(static_cast<int>(Knob("DEVILUTIONX_VR_ANISOTROPY", 8)), 1, 16);

ComPtr<ID3D11VertexShader> vsCarved, vsMesh, vsMeshShadow, vsSprite, vsParticle, vsFullscreen, vsPanel, vsMask;
ComPtr<ID3D11PixelShader> psCarved, psSurface, psSurfacePlain, psSurfaceDepth, psSurfaceSeen, psSprite, psParticle, psResolve, psBloomDown, psBloomUp, psFinal, psPanel;
ComPtr<ID3D11InputLayout> meshLayout, maskLayout;
ComPtr<ID3D11RasterizerState> rasterSolid, rasterShadow, rasterWire;
ComPtr<ID3D11DepthStencilState> depthScene, depthReadOnly, depthEqual, depthShadow, depthCopy, depthOff;
ComPtr<ID3D11BlendState> blendCoverage, blendPremultiplied, blendAdd, blendAlpha, blendNoColor;
ComPtr<ID3D11SamplerState> samplerSurface, samplerLinear, samplerShadow, samplerRepeat;

Texture msColor, msDepth, resolved, bloom[BloomLevels], shadowMap, lightGrid;
DynamicBuffer carvedInstanceBuffer, frameBuffer, drawBuffer, meshInstanceBuffer, spriteInstanceBuffer, particleInstanceBuffer, boneBuffer;
bool layered;
/** This frame's level has no materials, floor map or grit, so surfaces are drawn by the shader that looks none of them up. */
bool plainSurfaces;
/** Where this frame is seen from, for drawing nearest first. */
Vec3 viewerPosition;
int targetViews;
RenderStats stats;
FrameConstants frame;
float viewFov[MaxViews][4];

// Scratch lists, kept between frames so a steady frame allocates nothing.
struct Batch {
	const Mesh *mesh;
	uint32_t first;
	uint32_t count;
	/** Squared distance from the viewer to the nearest of its instances. */
	float nearest;
};
std::vector<uint32_t> order;
std::vector<MeshInstance> instances;
std::vector<CarvedInstance> carvedInstances;
std::vector<Batch> sceneBatches, shadowBatches;
std::vector<uint32_t> gridMasks;

std::string shaderFile;
time_t shaderFileTime;

time_t FileTime(const std::string &path)
{
#ifdef _WIN32
	struct _stat info;
	return _stat(path.c_str(), &info) == 0 ? info.st_mtime : 0;
#else
	struct stat info;
	return stat(path.c_str(), &info) == 0 ? info.st_mtime : 0;
#endif
}

bool CompileShaders()
{
	std::string source = layered ? "#define LAYERED 1\n" : "";
	if (Samples == 1)
		source += "#define SINGLE_SAMPLE 1\n";
	source += "#define WALL_SHADOW_STEPS " + std::to_string(WallShadowSteps) + "\n";
	source += "#define SUN_SHADOW_TAPS " + std::to_string(SunShadowTaps) + "\n";
	if (WallShadowSpacing > 0)
		source += "#define WALL_SHADOW_SPACING " + std::to_string(WallShadowSpacing) + "\n";
	bool fromFile = false;
	if (!shaderFile.empty()) {
		if (SDL_RWops *file = SDL_RWFromFile(shaderFile.c_str(), "rb"); file != nullptr) {
			std::string text(static_cast<size_t>(SDL_RWsize(file)), '\0');
			SDL_RWread(file, text.data(), 1, text.size());
			SDL_RWclose(file);
			source += text;
			fromFile = true;
		}
	}
	if (!fromFile)
		source += ShaderSource;

	const auto vertex = [&](const char *entry, ComPtr<ID3D11VertexShader> &shader, ComPtr<ID3DBlob> *keep = nullptr) {
		ComPtr<ID3DBlob> blob = CompileShader(source, entry, "vs_5_0");
		ComPtr<ID3D11VertexShader> compiled;
		if (blob == nullptr || FAILED(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &compiled)))
			return false;
		shader = compiled;
		if (keep != nullptr)
			*keep = blob;
		return true;
	};
	const auto pixel = [&](const char *entry, ComPtr<ID3D11PixelShader> &shader, const char *defines = "") {
		ComPtr<ID3DBlob> blob = CompileShader(defines + source, entry, "ps_5_0");
		ComPtr<ID3D11PixelShader> compiled;
		if (blob == nullptr || FAILED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &compiled)))
			return false;
		shader = compiled;
		return true;
	};
	ComPtr<ID3DBlob> meshBlob;
	ComPtr<ID3DBlob> maskBlob;
	if (!vertex("VsCarved", vsCarved) || !pixel("PsCarved", psCarved) || !pixel("PsSurface", psSurfacePlain, "#define PLAIN_SURFACES 1\n"))
		return false;
	if (DepthPrepass && (!pixel("PsSurfaceDepth", psSurfaceDepth) || !pixel("PsSurface", psSurfaceSeen, "#define PLAIN_SURFACES 1\n#define DEPTH_PREPASSED 1\n")))
		return false;
	if (!vertex("VsMesh", vsMesh, &meshBlob) || !vertex("VsMeshShadow", vsMeshShadow) || !vertex("VsSprite", vsSprite)
	    || !vertex("VsParticle", vsParticle) || !vertex("VsFullscreen", vsFullscreen) || !vertex("VsPanel", vsPanel)
	    || !vertex("VsMask", vsMask, &maskBlob) || !pixel("PsSurface", psSurface) || !pixel("PsSprite", psSprite)
	    || !pixel("PsParticle", psParticle) || !pixel("PsResolve", psResolve) || !pixel("PsBloomDown", psBloomDown)
	    || !pixel("PsBloomUp", psBloomUp) || !pixel("PsFinal", psFinal) || !pixel("PsPanel", psPanel))
		return false;

	const D3D11_INPUT_ELEMENT_DESC meshElements[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, position), D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, normal), D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, uv), D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, offsetof(Vertex, layer), D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(Vertex, color), D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, offsetof(Vertex, joints), D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "BLENDWEIGHT", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(Vertex, weights), D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	const D3D11_INPUT_ELEMENT_DESC maskElements[] = { { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
	return SUCCEEDED(device->CreateInputLayout(meshElements, 7, meshBlob->GetBufferPointer(), meshBlob->GetBufferSize(), &meshLayout))
	    && SUCCEEDED(device->CreateInputLayout(maskElements, 1, maskBlob->GetBufferPointer(), maskBlob->GetBufferSize(), &maskLayout));
}

bool CreateStates()
{
	D3D11_RASTERIZER_DESC raster {};
	raster.FillMode = D3D11_FILL_SOLID;
	// Cut-out slabs and cards are seen from both sides, and the shader turns normals towards the viewer.
	raster.CullMode = D3D11_CULL_NONE;
	raster.DepthClipEnable = TRUE;
	raster.MultisampleEnable = TRUE;
	bool ok = SUCCEEDED(device->CreateRasterizerState(&raster, &rasterSolid));
	raster.FillMode = D3D11_FILL_WIREFRAME;
	ok = ok && SUCCEEDED(device->CreateRasterizerState(&raster, &rasterWire));
	raster.FillMode = D3D11_FILL_SOLID;
	raster.MultisampleEnable = FALSE;
	raster.DepthBias = 200;
	raster.SlopeScaledDepthBias = 2.5F;
	raster.DepthClipEnable = FALSE; // Casters behind the shadow camera still cast.
	ok = ok && SUCCEEDED(device->CreateRasterizerState(&raster, &rasterShadow));

	D3D11_DEPTH_STENCIL_DESC depth {};
	depth.DepthEnable = TRUE;
	depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	depth.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL; // Reversed depth.
	ok = ok && SUCCEEDED(device->CreateDepthStencilState(&depth, &depthScene));
	depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	ok = ok && SUCCEEDED(device->CreateDepthStencilState(&depth, &depthReadOnly));
	depth.DepthFunc = D3D11_COMPARISON_EQUAL;
	ok = ok && SUCCEEDED(device->CreateDepthStencilState(&depth, &depthEqual));
	depth.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
	depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
	ok = ok && SUCCEEDED(device->CreateDepthStencilState(&depth, &depthShadow));
	depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
	ok = ok && SUCCEEDED(device->CreateDepthStencilState(&depth, &depthCopy));
	depth.DepthEnable = FALSE;
	depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	ok = ok && SUCCEEDED(device->CreateDepthStencilState(&depth, &depthOff));

	D3D11_BLEND_DESC blend {};
	blend.RenderTarget[0].RenderTargetWriteMask = 0;
	ok = ok && SUCCEEDED(device->CreateBlendState(&blend, &blendNoColor));
	blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	blend.AlphaToCoverageEnable = TRUE;
	ok = ok && SUCCEEDED(device->CreateBlendState(&blend, &blendCoverage));
	blend.AlphaToCoverageEnable = FALSE;
	blend.RenderTarget[0].BlendEnable = TRUE;
	blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	ok = ok && SUCCEEDED(device->CreateBlendState(&blend, &blendPremultiplied));
	blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
	ok = ok && SUCCEEDED(device->CreateBlendState(&blend, &blendAdd));
	blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	ok = ok && SUCCEEDED(device->CreateBlendState(&blend, &blendAlpha));

	D3D11_SAMPLER_DESC sampler {};
	sampler.Filter = D3D11_FILTER_ANISOTROPIC;
	sampler.MaxAnisotropy = Anisotropy;
	sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler.MaxLOD = D3D11_FLOAT32_MAX;
	ok = ok && SUCCEEDED(device->CreateSamplerState(&sampler, &samplerSurface));
	// Models tile their textures.
	sampler.AddressU = sampler.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
	ok = ok && SUCCEEDED(device->CreateSamplerState(&sampler, &samplerRepeat));
	sampler.AddressU = sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	ok = ok && SUCCEEDED(device->CreateSamplerState(&sampler, &samplerLinear));
	sampler.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
	sampler.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
	sampler.AddressU = sampler.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
	sampler.BorderColor[0] = 1;
	ok = ok && SUCCEEDED(device->CreateSamplerState(&sampler, &samplerShadow));
	return ok;
}

void EnsureTargets(int width, int height, int views)
{
	if (resolved && resolved.width == width && resolved.height == height && targetViews == views)
		return;
	targetViews = views;
	constexpr uint32_t Target = TextureRenderTarget | TextureSliceViews;
	// 11-bit float channels: HDR range at the cost of an 8-bit target, and no alpha is needed.
	if (Samples > 1)
		msColor = CreateTexture(width, height, views, DXGI_FORMAT_R11G11B10_FLOAT, Target, Samples, MemoryKind::Targets);
	msDepth = CreateTexture(width, height, views, DXGI_FORMAT_D32_FLOAT, TextureDepth | TextureSliceViews, Samples, MemoryKind::Targets);
	resolved = CreateTexture(width, height, views, DXGI_FORMAT_R11G11B10_FLOAT, Target, 1, MemoryKind::Targets);
	for (int i = 0; i < BloomLevels; i++)
		bloom[i] = CreateTexture(std::max(width >> (i + 1), 1), std::max(height >> (i + 1), 1), views, DXGI_FORMAT_R11G11B10_FLOAT, Target, 1, MemoryKind::Targets);
	// Read by the final pass even when bloom is off, where nothing else ever writes it.
	const float black[4] = {};
	context->ClearRenderTargetView(bloom[0].rtv.Get(), black);
	LogVerbose("VR: render targets {}x{} x{} views, {}x MSAA", width, height, views, Samples);
}

void SetViewport(int width, int height)
{
	const D3D11_VIEWPORT viewport { 0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1 };
	context->RSSetViewports(1, &viewport);
}

void SetDraw(const DrawConstants &constants)
{
	drawBuffer.Upload(&constants, sizeof(constants), D3D11_BIND_CONSTANT_BUFFER);
}

void UploadFrame()
{
	frameBuffer.Upload(&frame, sizeof(frame), D3D11_BIND_CONSTANT_BUFFER);
}

void CountDraw(uint32_t triangles)
{
	stats.drawCalls++;
	stats.triangles += triangles;
}

/** View and projection of the sun's shadow map: an orthographic box around the area in play. */
Mat4 ShadowMatrix(const Environment &environment)
{
	const Vec3 z = Normalize(environment.sunDirection);
	const Vec3 x = Normalize(Cross({ 0, 1, 0 }, z));
	const Vec3 y = Cross(z, x);
	// Move the box in whole texels, or the shadow edges crawl as the player walks.
	const float texel = ShadowExtent / ShadowMapSize;
	const Vec3 center = environment.shadowCenter;
	const Vec3 snapped = x * (std::floor(Dot(center, x) / texel) * texel) + y * (std::floor(Dot(center, y) / texel) * texel) + z * Dot(center, z);
	const Vec3 eye = snapped + z * 100;
	Mat4 view {};
	const Vec3 axes[3] = { x, y, z };
	for (int j = 0; j < 3; j++) {
		view.m[0][j] = axes[j].x;
		view.m[1][j] = axes[j].y;
		view.m[2][j] = axes[j].z;
		view.m[3][j] = -Dot(eye, axes[j]);
	}
	view.m[3][3] = 1;
	return view * OrthoMatrix(-ShadowExtent / 2, ShadowExtent / 2, -ShadowExtent / 2, ShadowExtent / 2, 1, 200);
}

void WorldBounds(const MeshDraw &draw, Vec3 &boundsMin, Vec3 &boundsMax)
{
	boundsMin = { 1e9F, 1e9F, 1e9F };
	boundsMax = { -1e9F, -1e9F, -1e9F };
	for (int corner = 0; corner < 8; corner++) {
		const Vec3 p = Transform(draw.instance.world, { (corner & 1) != 0 ? draw.mesh->boundsMax.x : draw.mesh->boundsMin.x,
		                                                  (corner & 2) != 0 ? draw.mesh->boundsMax.y : draw.mesh->boundsMin.y,
		                                                  (corner & 4) != 0 ? draw.mesh->boundsMax.z : draw.mesh->boundsMin.z });
		boundsMin = { std::min(boundsMin.x, p.x), std::min(boundsMin.y, p.y), std::min(boundsMin.z, p.z) };
		boundsMax = { std::max(boundsMax.x, p.x), std::max(boundsMax.y, p.y), std::max(boundsMax.z, p.z) };
	}
}

/** Squared distance from a point to a box. */
float DistanceSquared(const Vec3 &point, const Vec3 &boundsMin, const Vec3 &boundsMax)
{
	const float dx = std::max({ boundsMin.x - point.x, 0.F, point.x - boundsMax.x });
	const float dy = std::max({ boundsMin.y - point.y, 0.F, point.y - boundsMax.y });
	const float dz = std::max({ boundsMin.z - point.z, 0.F, point.z - boundsMax.z });
	return dx * dx + dy * dy + dz * dz;
}

/**
 * Appends the draws that pass `keep`, grouped by mesh, to the instance list.
 * With a `viewer` the groups are put nearest first.
 */
template <typename Keep>
void Batches(const Scene &scene, std::vector<Batch> &batches, Keep keep, const Vec3 *viewer = nullptr)
{
	batches.clear();
	order.clear();
	for (uint32_t i = 0; i < scene.meshes.size(); i++) {
		if (scene.meshes[i].mesh != nullptr && scene.meshes[i].mesh->indexCount != 0 && keep(scene.meshes[i]))
			order.push_back(i);
	}
	std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return scene.meshes[a].mesh < scene.meshes[b].mesh; });
	for (const uint32_t index : order) {
		const MeshDraw &draw = scene.meshes[index];
		if (batches.empty() || batches.back().mesh != draw.mesh)
			batches.push_back({ draw.mesh, static_cast<uint32_t>(instances.size()), 0, 1e30F });
		batches.back().count++;
		if (viewer != nullptr) {
			Vec3 boundsMin;
			Vec3 boundsMax;
			WorldBounds(draw, boundsMin, boundsMax);
			batches.back().nearest = std::min(batches.back().nearest, DistanceSquared(*viewer, boundsMin, boundsMax));
		}
		instances.push_back(draw.instance);
	}
	if (viewer != nullptr)
		std::stable_sort(batches.begin(), batches.end(), [](const Batch &a, const Batch &b) { return a.nearest < b.nearest; });
}

void DrawBatches(const std::vector<Batch> &batches, uint32_t copies)
{
	context->IASetInputLayout(meshLayout.Get());
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	for (const Batch &batch : batches) {
		const UINT stride = sizeof(Vertex);
		const UINT offset = 0;
		context->IASetVertexBuffers(0, 1, batch.mesh->vertices.GetAddressOf(), &stride, &offset);
		context->IASetIndexBuffer(batch.mesh->indices.Get(), DXGI_FORMAT_R32_UINT, 0);
		DrawConstants constants {};
		constants.instanceBase = batch.first;
		SetDraw(constants);
		context->DrawIndexedInstanced(batch.mesh->indexCount, batch.count * copies, 0, 0, 0);
		CountDraw(batch.mesh->indexCount / 3 * batch.count * copies);
	}
}

/** Which lights reach which tile, as a bit mask per tile: the pixel shader only visits those. */
void BuildLightGrid(const Scene &scene)
{
	const int tiles = std::min(static_cast<int>(scene.environment.levelTiles), LightGridSize);
	gridMasks.assign(static_cast<size_t>(LightGridSize) * LightGridSize, 0);
	for (int i = 0; i < scene.lightCount; i++) {
		const Light &light = scene.lights[i];
		const float tileSize = scene.environment.tileSize;
		const int x0 = std::max(static_cast<int>(std::floor((light.position[0] - light.range) / tileSize)), 0);
		const int x1 = std::min(static_cast<int>(std::floor((light.position[0] + light.range) / tileSize)), tiles - 1);
		const int y0 = std::max(static_cast<int>(std::floor((light.position[2] - light.range) / tileSize)), 0);
		const int y1 = std::min(static_cast<int>(std::floor((light.position[2] + light.range) / tileSize)), tiles - 1);
		for (int y = y0; y <= y1; y++) {
			for (int x = x0; x <= x1; x++)
				gridMasks[static_cast<size_t>(y) * LightGridSize + x] |= 1U << i;
		}
	}
	UploadTexture(lightGrid, 0, 0, 0, LightGridSize, LightGridSize, gridMasks.data(), 4);
}

/** `cover`: the size of a picture that keeps its shape and fills the target; what does not fit is cut off evenly on both sides. */
void DrawFullscreen(ID3D11PixelShader *shader, ID3D11RenderTargetView *target, ID3D11DepthStencilView *depth, int width, int height, const float post[4], const Texture *cover = nullptr)
{
	DrawConstants constants {};
	std::memcpy(constants.post, post, sizeof(constants.post));
	SetDraw(constants);
	context->OMSetRenderTargets(1, &target, depth);
	if (cover != nullptr) {
		const float scale = std::max(static_cast<float>(width) / static_cast<float>(cover->width), static_cast<float>(height) / static_cast<float>(cover->height));
		const float coverWidth = static_cast<float>(cover->width) * scale;
		const float coverHeight = static_cast<float>(cover->height) * scale;
		const D3D11_VIEWPORT viewport { (static_cast<float>(width) - coverWidth) / 2, (static_cast<float>(height) - coverHeight) / 2, coverWidth, coverHeight, 0, 1 };
		context->RSSetViewports(1, &viewport);
	} else {
		SetViewport(width, height);
	}
	context->VSSetShader(vsFullscreen.Get(), nullptr, 0);
	context->PSSetShader(shader, nullptr, 0);
	context->Draw(3, 0);
	CountDraw(1);
}

void BindSource(ID3D11ShaderResourceView *source, ID3D11ShaderResourceView *bloomSource = nullptr)
{
	ID3D11ShaderResourceView *views[2] = { source, bloomSource };
	context->PSSetShaderResources(13, 2, views);
}

void UnbindTargets()
{
	ID3D11RenderTargetView *none = nullptr;
	context->OMSetRenderTargets(1, &none, nullptr);
	ID3D11ShaderResourceView *nothing[4] = {};
	context->PSSetShaderResources(11, 4, nothing);
}

/** The scene's comfort vignette, for the final pass. */
float postVignette;

void Post(int view, const Output &output, int width, int height)
{
	context->RSSetState(rasterSolid.Get());
	context->OMSetDepthStencilState(depthOff.Get(), 0);
	context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	const auto slice = static_cast<float>(view);

	UnbindTargets();
	ID3D11ShaderResourceView *multisampled[2] = { msColor.srv.Get(), msDepth.srv.Get() };
	context->PSSetShaderResources(11, 2, multisampled);
	const float resolvePost[4] = { slice, static_cast<float>(Samples), 0, 0 };
	if (Samples > 1)
		DrawFullscreen(psResolve.Get(), resolved.sliceRtv[view].Get(), nullptr, width, height, resolvePost);

	// Bloom: threshold and shrink five times, then add each level back into the one above.
	const float threshold = 1.0F;
	for (int i = 0; i < BloomLevels && Bloom; i++) {
		const Texture &source = i == 0 ? resolved : bloom[i - 1];
		UnbindTargets();
		BindSource(source.srv.Get());
		const float post[4] = { slice, i == 0 ? threshold : 0, 0.5F / static_cast<float>(source.width), 0.5F / static_cast<float>(source.height) };
		DrawFullscreen(psBloomDown.Get(), bloom[i].sliceRtv[view].Get(), nullptr, bloom[i].width, bloom[i].height, post);
	}
	context->OMSetBlendState(blendAdd.Get(), nullptr, 0xFFFFFFFF);
	for (int i = BloomLevels - 1; i > 0 && Bloom; i--) {
		UnbindTargets();
		BindSource(bloom[i].srv.Get());
		const float post[4] = { slice, 0, 0.5F / static_cast<float>(bloom[i].width), 0.5F / static_cast<float>(bloom[i].height) };
		DrawFullscreen(psBloomUp.Get(), bloom[i - 1].sliceRtv[view].Get(), nullptr, bloom[i - 1].width, bloom[i - 1].height, post);
	}
	context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

	UnbindTargets();
	BindSource(resolved.srv.Get(), bloom[0].srv.Get());
	context->PSSetShaderResources(11, 2, multisampled);
	context->OMSetDepthStencilState(output.depth != nullptr ? depthCopy.Get() : depthOff.Get(), 0);
	const float finalPost[4] = { slice, postVignette, static_cast<float>(width), static_cast<float>(height) };
	DrawFullscreen(psFinal.Get(), output.color, output.depth, width, height, finalPost);
}

void DrawPanels(const Scene &scene, int view, const Output &output, int width, int height)
{
	if (scene.panels.empty())
		return;
	context->OMSetRenderTargets(1, &output.color, nullptr);
	SetViewport(width, height);
	context->OMSetDepthStencilState(depthOff.Get(), 0);
	context->OMSetBlendState(blendAlpha.Get(), nullptr, 0xFFFFFFFF);
	context->VSSetShader(vsPanel.Get(), nullptr, 0);
	context->PSSetShader(psPanel.Get(), nullptr, 0);
	for (const Panel &panel : scene.panels) {
		DrawConstants constants {};
		constants.post[0] = static_cast<float>(view);
		constants.post[1] = panel.opacity;
		std::memcpy(constants.panelCenter, &panel.center, sizeof(Vec3));
		std::memcpy(constants.panelRight, &panel.right, sizeof(Vec3));
		std::memcpy(constants.panelUp, &panel.up, sizeof(Vec3));
		SetDraw(constants);
		BindSource(panel.texture);
		context->Draw(6, 0);
		CountDraw(2);
	}
	context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
}

/** The opaque scene and the particles, for `copies` views starting at `firstView`. */
void DrawScene(const Scene &scene, int firstView, int copies, int width, int height, const Output *outputs)
{
	frame.viewBase = firstView;
	frame.viewCount = copies;
	UploadFrame();

	const bool all = copies == targetViews;
	const Texture &color = Samples > 1 ? msColor : resolved;
	ID3D11RenderTargetView *target = all ? color.rtv.Get() : color.sliceRtv[firstView].Get();
	ID3D11DepthStencilView *depth = all ? msDepth.dsv.Get() : msDepth.sliceDsv[firstView].Get();
	// Where nothing is drawn: under the sky the veil that ends the view, below ground the dark.
	context->ClearRenderTargetView(target, frame.sunColor[3] > 0 ? frame.viewFog : frame.fog);
	context->ClearDepthStencilView(depth, D3D11_CLEAR_DEPTH, 0.0F, 0);
	SetViewport(width, height);
	context->RSSetState(DebugView == 6 ? rasterWire.Get() : rasterSolid.Get());

	// The part of each eye's image the lenses never show is filled at the near plane first, so nothing is shaded there.
	context->OMSetDepthStencilState(depthCopy.Get(), 0);
	context->PSSetShader(nullptr, nullptr, 0);
	context->VSSetShader(vsMask.Get(), nullptr, 0);
	context->IASetInputLayout(maskLayout.Get());
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	for (int view = firstView; view < firstView + copies && outputs != nullptr; view++) {
		const Output &output = outputs[view];
		if (output.maskVertices == nullptr || output.maskVertexCount == 0)
			continue;
		ID3D11RenderTargetView *none = nullptr;
		context->OMSetRenderTargets(1, &none, msDepth.sliceDsv[view].Get());
		DrawConstants constants {};
		std::memcpy(constants.panelCenter, viewFov[view], sizeof(constants.panelCenter));
		SetDraw(constants);
		const UINT stride = 2 * sizeof(float);
		const UINT offset = 0;
		context->IASetVertexBuffers(0, 1, &output.maskVertices, &stride, &offset);
		context->Draw(output.maskVertexCount, 0);
	}

	context->OMSetRenderTargets(1, &target, depth);
	context->OMSetDepthStencilState(depthScene.Get(), 0);
	context->OMSetBlendState(blendCoverage.Get(), nullptr, 0xFFFFFFFF);
	const bool prepass = DepthPrepass && plainSurfaces;
	const auto drawSurfaces = [&] {
		context->VSSetShader(vsMesh.Get(), nullptr, 0);
		if (prepass) {
			// Their depth is there already: each pixel that is seen is shaded once, and no other.
			context->OMSetDepthStencilState(depthEqual.Get(), 0);
			context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
			context->PSSetShader(psSurfaceSeen.Get(), nullptr, 0);
			DrawBatches(sceneBatches, copies);
			context->OMSetDepthStencilState(depthScene.Get(), 0);
			context->OMSetBlendState(blendCoverage.Get(), nullptr, 0xFFFFFFFF);
			return;
		}
		context->PSSetShader(plainSurfaces ? psSurfacePlain.Get() : psSurface.Get(), nullptr, 0);
		DrawBatches(sceneBatches, copies);
	};
	if (prepass) {
		context->OMSetBlendState(blendNoColor.Get(), nullptr, 0xFFFFFFFF);
		context->VSSetShader(vsMesh.Get(), nullptr, 0);
		context->PSSetShader(psSurfaceDepth.Get(), nullptr, 0);
		DrawBatches(sceneBatches, copies);
		context->OMSetBlendState(blendCoverage.Get(), nullptr, 0xFFFFFFFF);
	}
	// Nearest first, the carved models go before the level: what the player holds is among
	// them, and covers a good part of the view.
	if (!NearestFirst)
		drawSurfaces();

	if (!scene.carved.empty()) {
		// One draw each: a carved model is one pose of one creature, or one object.
		context->IASetInputLayout(meshLayout.Get());
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(vsCarved.Get(), nullptr, 0);
		context->PSSetShader(psCarved.Get(), nullptr, 0);
		order.resize(scene.carved.size());
		for (uint32_t i = 0; i < order.size(); i++)
			order[i] = i;
		if (NearestFirst) {
			const auto away = [&](uint32_t index) {
				const Vec3 at = Transform(scene.carved[index].instance.world, { 0, 0, 0 });
				const Vec3 to { at.x - viewerPosition.x, at.y - viewerPosition.y, at.z - viewerPosition.z };
				return Dot(to, to);
			};
			std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return away(a) < away(b); });
		}
		for (const uint32_t i : order) {
			const Mesh &mesh = *scene.carved[i].mesh;
			const UINT stride = sizeof(Vertex);
			const UINT offset = 0;
			context->IASetVertexBuffers(0, 1, mesh.vertices.GetAddressOf(), &stride, &offset);
			context->IASetIndexBuffer(mesh.indices.Get(), DXGI_FORMAT_R32_UINT, 0);
			DrawConstants constants {};
			constants.instanceBase = i;
			SetDraw(constants);
			context->DrawIndexedInstanced(mesh.indexCount, copies, 0, 0, 0);
			CountDraw(mesh.indexCount / 3 * copies);
		}
	}
	if (NearestFirst)
		drawSurfaces();

	if (!scene.sprites.empty()) {
		context->IASetInputLayout(nullptr);
		context->VSSetShader(vsSprite.Get(), nullptr, 0);
		context->PSSetShader(psSprite.Get(), nullptr, 0);
		context->DrawInstanced(6, static_cast<UINT>(scene.sprites.size()) * copies, 0, 0);
		CountDraw(static_cast<uint32_t>(scene.sprites.size()) * 2 * copies);
	}
	if (!scene.particles.empty()) {
		context->IASetInputLayout(nullptr);
		context->OMSetDepthStencilState(depthReadOnly.Get(), 0);
		context->OMSetBlendState(blendPremultiplied.Get(), nullptr, 0xFFFFFFFF);
		context->VSSetShader(vsParticle.Get(), nullptr, 0);
		context->PSSetShader(psParticle.Get(), nullptr, 0);
		context->DrawInstanced(6, static_cast<UINT>(scene.particles.size()) * copies, 0, 0);
		CountDraw(static_cast<uint32_t>(scene.particles.size()) * 2 * copies);
	}
	context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
}

} // namespace

Mat4 ViewProjection(const View &view)
{
	return ViewMatrix(view.position, view.orientation) * ProjectionMatrix(view.fov[0], view.fov[1], view.fov[2], view.fov[3], NearPlane, FarPlane);
}

bool RendererInit(ID3D11Device *newDevice, ID3D11DeviceContext *newContext)
{
	device = newDevice;
	context = newContext;
	layered = SupportsLayeredVertexShader() && SDL_getenv("DEVILUTIONX_VR_NO_SINGLE_PASS") == nullptr;
	if (const char *file = SDL_getenv("DEVILUTIONX_VR_SHADERS"); file != nullptr) {
		shaderFile = file;
		shaderFileTime = FileTime(shaderFile);
	}
	if (!CompileShaders() || !CreateStates()) {
		LogError("VR: renderer initialisation failed");
		return false;
	}
	shadowMap = CreateTexture(ShadowMapSize, ShadowMapSize, 1, DXGI_FORMAT_D32_FLOAT, TextureDepth, 1, MemoryKind::Targets);
	lightGrid = CreateTexture(LightGridSize, LightGridSize, 1, DXGI_FORMAT_R32_UINT);
	LogInfo("VR: renderer ready, stereo in {}", layered ? "one pass" : "two passes");
	return shadowMap && lightGrid;
}

void RendererRelease()
{
	const auto reset = [](auto &...pointers) { (pointers.Reset(), ...); };
	reset(vsCarved, psCarved);
	reset(vsMesh, vsMeshShadow, vsSprite, vsParticle, vsFullscreen, vsPanel, vsMask);
	reset(psSurface, psSurfacePlain, psSurfaceDepth, psSurfaceSeen, psSprite, psParticle, psResolve, psBloomDown, psBloomUp, psFinal, psPanel);
	reset(meshLayout, maskLayout, rasterSolid, rasterShadow, rasterWire);
	reset(depthScene, depthReadOnly, depthEqual, depthShadow, depthCopy, depthOff);
	reset(blendCoverage, blendPremultiplied, blendAdd, blendAlpha, blendNoColor, samplerSurface, samplerLinear, samplerShadow, samplerRepeat);
	for (Texture *texture : { &msColor, &msDepth, &resolved, &shadowMap, &lightGrid })
		texture->Release();
	for (Texture &texture : bloom)
		texture.Release();
	for (DynamicBuffer *buffer : { &carvedInstanceBuffer, &frameBuffer, &drawBuffer, &meshInstanceBuffer, &spriteInstanceBuffer, &particleInstanceBuffer, &boneBuffer })
		buffer->Release();
	device = nullptr;
	context = nullptr;
}

void RendererRender(const Scene &scene, const View *views, int viewCount, int width, int height, const Output *outputs)
{
	// Shader hot reload: a failed compile keeps the shaders that work.
	static int frameNumber;
	if (!shaderFile.empty() && ++frameNumber % 30 == 0 && FileTime(shaderFile) != shaderFileTime) {
		shaderFileTime = FileTime(shaderFile);
		LogInfo("VR: shaders reloaded from {}: {}", shaderFile, CompileShaders() ? "ok" : "failed, keeping the old ones");
	}

	const ScopedTimer timer(ProfileScope::Render);
	viewerPosition = views[0].position;
	EnsureTargets(width, height, viewCount);
	stats = {};
	const Environment &environment = scene.environment;
	postVignette = std::clamp(environment.vignette, 0.F, 1.F);
	const bool sun = environment.sunColor[0] + environment.sunColor[1] + environment.sunColor[2] > 0;

	Mat4 viewProjections[MaxViews];
	for (int i = 0; i < viewCount; i++) {
		viewProjections[i] = ViewProjection(views[i]);
		frame.viewProj[i] = viewProjections[i];
		std::memcpy(frame.eyePosition[i], &views[i].position, sizeof(Vec3));
		std::memcpy(viewFov[i], views[i].fov, sizeof(viewFov[i]));
	}
	const auto set = [](float target[4], Vec3 v, float w) {
		target[0] = v.x;
		target[1] = v.y;
		target[2] = v.z;
		target[3] = w;
	};
	set(frame.headRight, scene.headRight, scene.time);
	set(frame.headUp, scene.headUp, environment.exposure);
	set(frame.fog, { environment.fogColor[0], environment.fogColor[1], environment.fogColor[2] }, environment.fogDensity);
	set(frame.ambient, { environment.ambient[0], environment.ambient[1], environment.ambient[2] }, Bloom ? environment.bloom : 0);
	set(frame.sunDirection, Normalize(environment.sunDirection), 1.0F / ShadowMapSize);
	set(frame.sunColor, { environment.sunColor[0], environment.sunColor[1], environment.sunColor[2] }, sun ? 1.F : 0.F);
	frame.shadowViewProj = sun ? ShadowMatrix(environment) : Mat4 {};
	frame.level[0] = environment.tileSize;
	frame.level[1] = environment.levelTiles;
	frame.level[2] = 1 / (environment.tileSize * environment.levelTiles);
	frame.level[3] = 0;
	frame.palette[0] = environment.paletteLight;
	frame.palette[1] = environment.paletteGain;
	frame.palette[2] = 1 / std::max(environment.materialSize, 0.1F);
	frame.palette[3] = environment.floorMap != nullptr && environment.floorSize > 0 ? 1 / environment.floorSize : 0;
	// The veil's colour is given as the display shows it; the scene value is what tone mapping turns into it.
	for (int c = 0; c < 3; c++) {
		const float y = std::min(environment.viewFogColor[c], 0.97F);
		const float a = 2.51F - 2.43F * y;
		const float b = 0.03F - 0.59F * y;
		frame.viewFog[c] = (-b + std::sqrt(b * b + 0.56F * a * y)) / (2 * a) / std::max(environment.exposure, 1e-3F);
	}
	frame.viewFog[3] = environment.viewDistance > 0 ? environment.viewDistance : 1e9F;
	frame.abyss[0] = environment.abyssStart;
	frame.abyss[1] = 1 / std::max(environment.abyssFade, 0.01F);
	frame.abyss[2] = Grit;
	frame.abyss[3] = GritTexels;
	frame.debugView = DebugView;
	frame.lightCount = scene.lightCount;
	std::memcpy(frame.lights, scene.lights, sizeof(frame.lights));
	stats.lights = scene.lightCount;

	// What is drawn: frustum culling against every view, then grouping by mesh.
	instances.clear();
	Batches(scene, sceneBatches, [&](const MeshDraw &draw) {
		Vec3 boundsMin;
		Vec3 boundsMax;
		WorldBounds(draw, boundsMin, boundsMax);
		for (int i = 0; i < viewCount; i++) {
			if (!OutsideFrustum(viewProjections[i], boundsMin, boundsMax))
				return true;
		}
		return false;
	}, NearestFirst ? &views[0].position : nullptr);
	stats.meshInstances = static_cast<uint32_t>(instances.size());
	if (sun) {
		Batches(scene, shadowBatches, [&](const MeshDraw &draw) {
			if (!draw.castsShadow)
				return false;
			Vec3 boundsMin;
			Vec3 boundsMax;
			WorldBounds(draw, boundsMin, boundsMax);
			return boundsMax.x > environment.shadowCenter.x - ShadowExtent && boundsMin.x < environment.shadowCenter.x + ShadowExtent
			    && boundsMax.z > environment.shadowCenter.z - ShadowExtent && boundsMin.z < environment.shadowCenter.z + ShadowExtent;
		});
	}
	stats.sprites = static_cast<uint32_t>(scene.sprites.size());
	stats.particles = static_cast<uint32_t>(scene.particles.size());

	constexpr UINT Structured = D3D11_BIND_SHADER_RESOURCE;
	meshInstanceBuffer.Upload(instances.data(), static_cast<uint32_t>(instances.size() * sizeof(MeshInstance)), Structured, sizeof(MeshInstance));
	spriteInstanceBuffer.Upload(scene.sprites.data(), static_cast<uint32_t>(scene.sprites.size() * sizeof(SpriteInstance)), Structured, sizeof(SpriteInstance));
	particleInstanceBuffer.Upload(scene.particles.data(), static_cast<uint32_t>(scene.particles.size() * sizeof(ParticleInstance)), Structured, sizeof(ParticleInstance));
	carvedInstances.clear();
	for (const CarvedDraw &draw : scene.carved)
		carvedInstances.push_back(draw.instance);
	carvedInstanceBuffer.Upload(carvedInstances.data(), static_cast<uint32_t>(carvedInstances.size() * sizeof(CarvedInstance)), Structured, sizeof(CarvedInstance));
	boneBuffer.Upload(scene.bones.data(), static_cast<uint32_t>(scene.bones.size() * sizeof(Mat34)), Structured, 16, DXGI_FORMAT_R32G32B32A32_FLOAT);
	BuildLightGrid(scene);

	// Bindings shared by all passes.
	ID3D11Buffer *constantBuffers[2] = {};
	frame.viewBase = 0;
	frame.viewCount = 1;
	UploadFrame();
	SetDraw({});
	constantBuffers[0] = frameBuffer.buffer.Get();
	constantBuffers[1] = drawBuffer.buffer.Get();
	context->VSSetConstantBuffers(0, 2, constantBuffers);
	context->PSSetConstantBuffers(0, 2, constantBuffers);
	ID3D11SamplerState *samplers[4] = { samplerSurface.Get(), samplerLinear.Get(), samplerShadow.Get(), samplerRepeat.Get() };
	context->PSSetSamplers(0, 4, samplers);
	ID3D11ShaderResourceView *vertexViews[4] = { boneBuffer.srv.Get(), meshInstanceBuffer.srv.Get(), spriteInstanceBuffer.srv.Get(), particleInstanceBuffer.srv.Get() };
	context->VSSetShaderResources(7, 4, vertexViews);
	ID3D11ShaderResourceView *carvedView = carvedInstanceBuffer.srv.Get();
	context->VSSetShaderResources(20, 1, &carvedView);
	context->PSSetShaderResources(20, 1, &carvedView);

	if (sun) {
		const ScopedGpuTimer gpuTimer(GpuPass::Shadow);
		ID3D11ShaderResourceView *none = nullptr;
		context->PSSetShaderResources(6, 1, &none);
		ID3D11RenderTargetView *noTarget = nullptr;
		context->OMSetRenderTargets(1, &noTarget, shadowMap.dsv.Get());
		context->ClearDepthStencilView(shadowMap.dsv.Get(), D3D11_CLEAR_DEPTH, 1.0F, 0);
		SetViewport(ShadowMapSize, ShadowMapSize);
		context->RSSetState(rasterShadow.Get());
		context->OMSetDepthStencilState(depthShadow.Get(), 0);
		context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
		context->VSSetShader(vsMeshShadow.Get(), nullptr, 0);
		context->PSSetShader(nullptr, nullptr, 0);
		DrawBatches(shadowBatches, 1);
		context->OMSetRenderTargets(1, &noTarget, nullptr);
	}

	{
		const ScopedGpuTimer gpuTimer(GpuPass::Scene);
		ID3D11ShaderResourceView *sceneViews[7] = {
			environment.surfaces, environment.surfaceNormals, environment.sprites, environment.lightmap,
			environment.occupancy, lightGrid.srv.Get(), shadowMap.srv.Get()
		};
		context->PSSetShaderResources(0, 7, sceneViews);
		ID3D11ShaderResourceView *more[5] = { environment.models, environment.paletteLit, environment.paletteCube, environment.materials, environment.materialNormals };
		context->PSSetShaderResources(15, 5, more);
		ID3D11ShaderResourceView *floorMap = environment.floorMap;
		context->PSSetShaderResources(21, 1, &floorMap);
		plainSurfaces = environment.materials == nullptr && environment.floorMap == nullptr && frame.abyss[2] <= 0;
		if (layered) {
			DrawScene(scene, 0, viewCount, width, height, outputs);
		} else {
			for (int i = 0; i < viewCount; i++)
				DrawScene(scene, i, 1, width, height, outputs);
		}
		UnbindTargets();
	}

	if (outputs != nullptr) {
		const ScopedGpuTimer gpuTimer(GpuPass::Post);
		for (int i = 0; i < viewCount; i++) {
			Post(i, outputs[i], width, height);
			DrawPanels(scene, i, outputs[i], width, height);
		}
		UnbindTargets();
	}
}

void RendererBlitView(ID3D11RenderTargetView *target, int width, int height)
{
	if (!resolved)
		return;
	context->RSSetState(rasterSolid.Get());
	context->OMSetDepthStencilState(depthOff.Get(), 0);
	context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
	context->IASetInputLayout(nullptr);
	BindSource(resolved.srv.Get(), bloom[0].srv.Get());
	ID3D11ShaderResourceView *multisampled[2] = { msColor.srv.Get(), msDepth.srv.Get() };
	context->PSSetShaderResources(11, 2, multisampled);
	const float post[4] = { 0, 0, static_cast<float>(resolved.width), static_cast<float>(resolved.height) };
	DrawFullscreen(psFinal.Get(), target, nullptr, width, height, post, &resolved);
	UnbindTargets();
}

const RenderStats &RendererStats()
{
	return stats;
}

} // namespace render
} // namespace vr
} // namespace devilution

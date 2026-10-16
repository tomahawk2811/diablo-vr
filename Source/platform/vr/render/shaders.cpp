/**
 * @file shaders.cpp
 *
 * HLSL of the 3D renderer. One source, compiled per entry point by renderer.cpp.
 * DEVILUTIONX_VR_SHADERS=<file> replaces it with a file that is reloaded when it changes.
 */
#include "platform/vr/render/renderer.h"

namespace devilution {
namespace vr {
namespace render {

const char ShaderSource[] = R"hlsl(
#define MAX_LIGHTS 32

struct Light {
	float3 position;
	float range;
	float3 color;
	float shadowed;
};

cbuffer Frame : register(b0)
{
	row_major float4x4 viewProj[2];
	float4 eyePosition[2];
	float4 headRight;      // w = time in seconds
	float4 headUp;         // w = exposure
	float4 fog;            // rgb, a = density per meter
	float4 ambient;        // rgb, a = bloom strength
	float4 sunDirection;   // xyz towards the sun, w = 1 / shadow map size
	float4 sunColor;       // rgb, a = 1 when the sun is on
	row_major float4x4 shadowViewProj;
	float4 level;          // x = meters per tile, y = tiles per side, z = 1 / (x * y), w = 1 / sprite atlas size
	float4 abyss;          // x = height where the dark above begins, y = 1 / meters it takes to turn black, z = grit strength (0: none), w = grit texels per meter
	float4 palette;        // x = 1 when shading goes through the game's light tables, y = light gain, z = material repeats per meter, w = floor material repeats per meter (0: no floor map)
	float4 viewFog;        // rgb = what lies beyond the view distance (scene values), a = that distance in meters
	uint viewBase;
	uint viewCount;
	uint debugView;
	uint lightCount;
	Light lights[MAX_LIGHTS];
};

cbuffer Draw : register(b1)
{
	uint instanceBase;
	uint3 drawPad;
	float4 post;
	float4 panelCenter;
	float4 panelRight;
	float4 panelUp;
};

struct MeshInstance {
	float4 world0;
	float4 world1;
	float4 world2;
	float4 color;
	float4 params; // bone base, specular, lit
};

struct CarvedInstance {
	float4 world0;
	float4 world1;
	float4 world2;
	float4 params; // unused, self-lit, unused, unused
	float4 rect;
	float4 map;
};

struct SpriteInstance {
	float3 position;
	float emissive;
	float3 axisU;
	float lying;
	float3 axisV;
	float relief;
	float4 uv;
	float4 tint;
};

struct ParticleInstance {
	float3 position;
	float size;
	float4 color;
	float lying;
	float3 pad;
};

Texture2DArray surfaces : register(t0);
Texture2DArray surfaceNormals : register(t1);
Texture2DArray atlas : register(t2);
Texture2DArray lightmap : register(t3);
Texture2DArray occupancy : register(t4);
Texture2DArray<uint> lightGrid : register(t5);
Texture2DArray shadowMap : register(t6);
Buffer<float4> bones : register(t7);
StructuredBuffer<MeshInstance> meshInstances : register(t8);
StructuredBuffer<SpriteInstance> spriteInstances : register(t9);
StructuredBuffer<ParticleInstance> particleInstances : register(t10);
#if SINGLE_SAMPLE
// Without MSAA the scene is drawn straight into the post-processing source, and its depth is a plain texture.
Texture2DArray<float> msDepth : register(t12);
#else
Texture2DMSArray<float4> msColor : register(t11);
Texture2DMSArray<float> msDepth : register(t12);
#endif
Texture2DArray postSource : register(t13);
Texture2DArray postBloom : register(t14);
Texture2DArray modelTextures : register(t15);
Texture2DArray paletteLit : register(t16);
Texture2DArray paletteCube : register(t17);
Texture2DArray materials : register(t18);
Texture2DArray materialNormals : register(t19);
StructuredBuffer<CarvedInstance> carvedInstances : register(t20);
// Floors: x = first material + 1 (0: none), y = second material + 1, z = how far the second shows, w = distance to the nearest wall.
Texture2DArray floorMap : register(t21);

SamplerState surfaceSampler : register(s0);
SamplerState linearClamp : register(s1);
SamplerComparisonState shadowSampler : register(s2);
SamplerState repeatSampler : register(s3);

struct SurfaceOut {
	float4 clip : SV_Position;
	float3 world : TEXCOORD0;
	float3 normal : TEXCOORD1;
	float3 uvLayer : TEXCOORD2;
	float4 color : COLOR0;
	float4 material : TEXCOORD3; // emissive, specular, lit, flat
#ifdef LAYERED
	uint slice : SV_RenderTargetArrayIndex;
#endif
};

static const float2 Corners[6] = { float2(0, 0), float2(1, 0), float2(1, 1), float2(0, 0), float2(1, 1), float2(0, 1) };

// ---------------------------------------------------------------- geometry

struct MeshIn {
	float3 position : POSITION;
	float3 normal : NORMAL;
	float2 uv : TEXCOORD0;
	float layer : TEXCOORD1;
	float4 color : COLOR;
	uint4 joints : BLENDINDICES;
	float4 weights : BLENDWEIGHT;
};

float3x4 Bone(uint index)
{
	return float3x4(bones.Load(index * 3), bones.Load(index * 3 + 1), bones.Load(index * 3 + 2));
}

void MeshWorld(MeshIn input, MeshInstance instance, out float3 world, out float3 normal)
{
	float3 position = input.position;
	normal = input.normal;
	if (instance.params.x >= 0) {
		uint base = (uint)instance.params.x;
		float3x4 skin = Bone(base + input.joints.x) * input.weights.x + Bone(base + input.joints.y) * input.weights.y
		    + Bone(base + input.joints.z) * input.weights.z + Bone(base + input.joints.w) * input.weights.w;
		position = mul(skin, float4(position, 1));
		normal = mul((float3x3)skin, normal);
	}
	float3x4 toWorld = float3x4(instance.world0, instance.world1, instance.world2);
	world = mul(toWorld, float4(position, 1));
	normal = normalize(mul((float3x3)toWorld, normal));
}

SurfaceOut VsMesh(MeshIn input, uint instanceId : SV_InstanceID)
{
	// One draw covers both eyes: instances alternate between them.
	uint eye = viewBase + instanceId % viewCount;
	MeshInstance instance = meshInstances[instanceBase + instanceId / viewCount];
	SurfaceOut output;
	MeshWorld(input, instance, output.world, output.normal);
	output.clip = mul(float4(output.world, 1), viewProj[eye]);
	output.uvLayer = float3(input.uv, input.layer);
	output.color = float4(pow(abs(input.color.rgb), 2.2) * instance.color.rgb, 1);
	output.material = float4(instance.color.a + input.color.a * 6, instance.params.y, instance.params.z, 0);
#ifdef LAYERED
	output.slice = eye;
#endif
	return output;
}

float4 VsMeshShadow(MeshIn input, uint instanceId : SV_InstanceID) : SV_Position
{
	float3 world;
	float3 normal;
	MeshWorld(input, meshInstances[instanceBase + instanceId], world, normal);
	return mul(float4(world, 1), shadowViewProj);
}

SurfaceOut VsSprite(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
	uint eye = viewBase + instanceId % viewCount;
	SpriteInstance sprite = spriteInstances[instanceId / viewCount];
	float2 corner = Corners[vertexId];
	SurfaceOut output;
	output.world = sprite.position + sprite.axisU * (corner.x * 2 - 1) + sprite.axisV * corner.y;
	output.clip = mul(float4(output.world, 1), viewProj[eye]);
	// A standing sprite is lit as a surface facing the viewer; a lying one as the floor it lies on.
	float3 facing = normalize(cross(sprite.axisU, sprite.axisV));
	facing *= dot(facing, eyePosition[eye].xyz - sprite.position) < 0 ? -1 : 1;
	output.normal = sprite.lying > 0.5 ? float3(0, 1, 0) : facing;
	// Atlas texels, not 0..1: the pixel shader reads single texels.
	// z: meters of relief per art pixel of distance to the outline (0 = flat).
	output.uvLayer = float3(lerp(sprite.uv.xy, sprite.uv.zw, float2(corner.x, 1 - corner.y)), sprite.relief);
	output.color = sprite.tint;
	output.material = float4(sprite.emissive, 0, 1, sprite.lying);
#ifdef LAYERED
	output.slice = eye;
#endif
	return output;
}

struct ParticleOut {
	float4 clip : SV_Position;
	float2 uv : TEXCOORD0;
	float4 color : COLOR0;
#ifdef LAYERED
	uint slice : SV_RenderTargetArrayIndex;
#endif
};

ParticleOut VsParticle(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
	uint eye = viewBase + instanceId % viewCount;
	ParticleInstance particle = particleInstances[instanceId / viewCount];
	float2 corner = Corners[vertexId] * 2 - 1;
	float3 right = particle.lying > 0.5 ? float3(1, 0, 0) : headRight.xyz;
	float3 up = particle.lying > 0.5 ? float3(0, 0, 1) : headUp.xyz;
	float3 world = particle.position + (right * corner.x + up * corner.y) * particle.size;
	ParticleOut output;
	output.clip = mul(float4(world, 1), viewProj[eye]);
	output.uv = corner;
	// Glow is swallowed by the dark like everything else.
	float away = distance(world, eyePosition[eye].xyz);
	output.color = particle.color * float4((exp(-away * fog.a) * (1 - smoothstep(0.55 * viewFog.a, viewFog.a, away))).xxx, 1);
#ifdef LAYERED
	output.slice = eye;
#endif
	return output;
}

// Hidden-area mesh: view-space points at z = -1, written at the near plane so nothing behind is shaded.
float4 VsMask(float2 position : POSITION) : SV_Position
{
	float4 tangents = panelCenter; // left, right, up, down
	return float4((2 * position.x - tangents.y - tangents.x) / (tangents.y - tangents.x), (2 * position.y - tangents.z - tangents.w) / (tangents.z - tangents.w), 1, 1);
}

// ---------------------------------------------------------------- lighting

)hlsl"
    // MSVC caps one string literal at 16 KB; adjacent literals are joined.
    R"hlsl(
// Samples along the line from a surface point to a light that look for a wall in between
// (DEVILUTIONX_VR_WALL_SHADOW_STEPS). 0: walls cast no shadows. With WALL_SHADOW_SPACING
// (DEVILUTIONX_VR_WALL_SHADOW_SPACING, in tiles) that is the most, and a pixel takes one
// sample per that length of the way.
#ifndef WALL_SHADOW_STEPS
#define WALL_SHADOW_STEPS 16
#endif
// Filtered comparisons a pixel makes against the sun's shadow map: 9, 4 or 1 (DEVILUTIONX_VR_SUN_SHADOW_TAPS).
#ifndef SUN_SHADOW_TAPS
#define SUN_SHADOW_TAPS 9
#endif

float Noise(float2 pixel)
{
	return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// Walls are whole tiles standing floor to ceiling, so their shadows are a 2D question:
// is anything solid on the straight line across the floor plan?
float WallShadow(float3 from, float3 to, float jitter)
{
	float2 a = from.xz * level.z;
	float2 b = to.xz * level.z;
	float visibility = 1;
#ifdef WALL_SHADOW_SPACING
	// A light close by needs fewer samples to look at the same share of every tile on the way.
	int steps = clamp((int)ceil(length(b - a) * level.y / WALL_SHADOW_SPACING), 1, max(WALL_SHADOW_STEPS, 1));
#else
	const int steps = max(WALL_SHADOW_STEPS, 1);
#endif
	for (int i = 0; i < steps; i++) {
		float t = (i + jitter) / steps;
		float solid = occupancy.SampleLevel(linearClamp, float3(lerp(a, b, t), 0), 0).r;
		visibility = min(visibility, 1 - smoothstep(0.45, 0.8, solid));
	}
	return visibility;
}

float SunShadow(float3 world)
{
	float4 clip = mul(float4(world, 1), shadowViewProj);
	float2 uv = clip.xy * float2(0.5, -0.5) + 0.5;
	if (any(uv < 0) || any(uv > 1))
		return 1;
#if SUN_SHADOW_TAPS >= 9
	float sum = 0;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++)
			sum += shadowMap.SampleCmpLevelZero(shadowSampler, float3(uv + float2(x, y) * sunDirection.w, 0), clip.z);
	}
	return sum / 9;
#elif SUN_SHADOW_TAPS >= 4
	// Each comparison is filtered over two texels a side already: four of them half a texel
	// out cover three texels across, where the nine cover four.
	float sum = 0;
	for (int i = 0; i < 4; i++)
		sum += shadowMap.SampleCmpLevelZero(shadowSampler, float3(uv + (float2(i & 1, i >> 1) - 0.5) * sunDirection.w, 0), clip.z);
	return sum / 4;
#else
	return shadowMap.SampleCmpLevelZero(shadowSampler, float3(uv, 0), clip.z);
#endif
}

/**
 * Light arriving at a surface point. `wrap` softens the angle term for sprites, which have no
 * real normals. Returns diffuse light in rgb; specular highlights are added to `highlight`.
 */
float3 Lighting(float3 world, float3 normal, float3 toEye, float specular, float wrap, float2 pixel, out float3 highlight, out float lightsHere)
{
	highlight = 0;
	// Sample the floor-plan textures a little way out of the wall the point may sit on.
	float3 open = world + normal * (0.3 * level.x);
	float4 baked = lightmap.SampleLevel(linearClamp, float3(open.xz * level.z, 0), 0);
	float time = headRight.w;
	float flicker = 1 + 0.07 * sin(time * 11 + world.x * 1.3) * sin(time * 7.3 + world.z * 1.7);
	// Baked light is stored at half scale to leave headroom in eight bits.
	float3 light = (ambient.rgb + baked.rgb * (2 * flicker)) * baked.a;

	if (sunColor.a > 0) {
		float facing = lerp(saturate(dot(normal, sunDirection.xyz)), 1, wrap * 0.5);
		if (facing > 0)
			light += sunColor.rgb * facing * SunShadow(world + normal * 0.06);
	}

	int2 tile = clamp(int2(open.xz / level.x), 0, int(level.y) - 1);
	uint mask = lightGrid.Load(int4(tile, 0, 0));
	lightsHere = countbits(mask);
	float jitter = Noise(pixel);
	while (mask != 0) {
		uint index = firstbitlow(mask);
		mask &= mask - 1;
		Light source = lights[index];
		float3 toLight = source.position - world;
		float away = length(toLight);
		if (away >= source.range)
			continue;
		toLight /= away;
		float falloff = 1 - away / source.range;
		falloff *= falloff;
		float facing = lerp(saturate(dot(normal, toLight)), 1, wrap);
		if (facing * falloff <= 0.0005)
			continue;
#if WALL_SHADOW_STEPS > 0
		float visible = source.shadowed > 0.5 ? WallShadow(open, source.position, jitter) : 1;
#else
		float visible = 1;
#endif
		light += source.color * (facing * falloff * visible);
		float3 halfway = normalize(toLight + toEye);
		highlight += source.color * (pow(saturate(dot(normal, halfway)), 48) * specular * falloff * visible);
	}
	return light;
}

float3 Fog(float3 color, float3 world, float3 eye)
{
	float away = distance(world, eye);
	color = lerp(fog.rgb, color, exp(-away * fog.a));
	// The view ends in a grey veil: clear up close, closed at the view distance.
	return lerp(color, viewFog.rgb, smoothstep(0.55 * viewFog.a, viewFog.a, away));
}

// Stochastic hex tiling (after Mikkelsen 2022): three copies of a texture laid on a triangle grid,
// each shifted at random, blended by where the point lies in its triangle. No repeat shows.
float2 HexHash(float2 p)
{
	return frac(sin(mul(float2x2(127.1, 311.7, 269.5, 183.3), p)) * 43758.5453);
}

float4 HexSample(Texture2DArray source, float2 uv, float layer, float2 gx, float2 gy)
{
	float2 skewed = mul(float2x2(1, 0, -0.57735027, 1.15470054), uv * 3.4641016);
	float2 base = floor(skewed);
	float3 inside = float3(frac(skewed), 0);
	inside.z = 1 - inside.x - inside.y;
	float2 v1, v2, v3;
	float3 w;
	if (inside.z > 0) {
		w = float3(inside.z, inside.y, inside.x);
		v1 = base;
		v2 = base + float2(0, 1);
		v3 = base + float2(1, 0);
	} else {
		w = float3(-inside.z, 1 - inside.y, 1 - inside.x);
		v1 = base + 1;
		v2 = base + float2(1, 0);
		v3 = base + float2(0, 1);
	}
	float4 a = source.SampleGrad(repeatSampler, float3(uv + HexHash(v1), layer), gx, gy);
	float4 b = source.SampleGrad(repeatSampler, float3(uv + HexHash(v2), layer), gx, gy);
	float4 c = source.SampleGrad(repeatSampler, float3(uv + HexHash(v3), layer), gx, gy);
	// Sharpened weights keep the art's texels crisp instead of averaging three copies.
	w = pow(w, 6);
	w /= max(w.x + w.y + w.z, 1e-5);
	return a * w.x + b * w.y + c * w.z;
}

// Grit: walls and floors are made of square texels of a fixed size in the world, as the game's
// own art is, each with its own grain; dirt lies over them in blotches, and the colours are a
// little duller. `at` is the place on the surface in meters.
float2 GritCell(float2 at)
{
	return floor(at * abyss.w);
}

// A place on a surface moved to the middle of its grit texel, with one screen pixel of blend
// between texels so their edges do not crawl.
float2 GritSnap(float2 at)
{
	float2 texels = at * abyss.w - 0.5;
	return (floor(texels) + 0.5 + saturate((frac(texels) - 0.5) / max(fwidth(texels), 1e-4) + 0.5)) / abyss.w;
}

float GritNoise(float2 p)
{
	float2 cell = floor(p);
	float2 f = frac(p);
	f = f * f * (3 - 2 * f);
	float a = HexHash(cell).x;
	float b = HexHash(cell + float2(1, 0)).x;
	float c = HexHash(cell + float2(0, 1)).x;
	float d = HexHash(cell + float2(1, 1)).x;
	return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float3 Grit(float3 color, float2 at)
{
	float2 cell = GritCell(at);
	float grain = HexHash(cell).y;
	// Blotches from a hand's width to a stride across, read per texel so they are as coarse as the rest.
	float2 middle = (cell + 0.5) / abyss.w;
	float stain = GritNoise(middle * 0.8) * 0.55 + GritNoise(middle * 2.9 + 17) * 0.3 + GritNoise(middle * 9.1 + 41) * 0.15;
	float luma = dot(color, float3(0.3, 0.59, 0.11));
	color = lerp(color, luma, 0.18 * abyss.z);
	color *= 1 + (grain - 0.5) * 0.34 * abyss.z;
	return color * lerp(1, 0.5 + stain, 0.75 * abyss.z);
}

// Tangent frame from screen-space derivatives: no tangents have to be stored or skinned.
float3 BumpedNormal(float3 normal, float3 world, float3 uvLayer)
{
	float3 dp1 = ddx(world);
	float3 dp2 = ddy(world);
	float2 duv1 = ddx(uvLayer.xy);
	float2 duv2 = ddy(uvLayer.xy);
	float3 dp2perp = cross(dp2, normal);
	float3 dp1perp = cross(normal, dp1);
	float3 tangent = dp2perp * duv1.x + dp1perp * duv2.x;
	float3 bitangent = dp2perp * duv1.y + dp1perp * duv2.y;
	float scale = rsqrt(max(dot(tangent, tangent), dot(bitangent, bitangent)) + 1e-12);
	float2 bump = surfaceNormals.Sample(surfaceSampler, uvLayer).xy * 2 - 1;
	return normalize(tangent * (scale * bump.x) + bitangent * (scale * bump.y) + normal * sqrt(saturate(1 - dot(bump, bump))));
}

float3 Heat(float value)
{
	return saturate(float3(value * 3 - 1, 1 - abs(value * 2 - 1), 1 - value * 3));
}

struct Lit {
	float3 light;
	float3 highlight;
	float lightsHere;
};

Lit LightAt(SurfaceOut input, float3 normal, float wrap, uint eye)
{
	Lit lit;
	float3 toEye = normalize(eyePosition[eye].xyz - input.world);
	lit.light = Lighting(input.world, normal, toEye, input.material.y, wrap, input.clip.xy, lit.highlight, lit.lightsHere);
	lit.light = lerp(1, lit.light, input.material.z);
	return lit;
}

// The game's light level for an amount of light: 0 is fully lit, 15 is black.
float ShadeLevel(float3 light)
{
	float brightness = saturate(max(light.r, max(light.g, light.b)) * palette.y);
	return 15 * (1 - pow(brightness, 1 / 2.2));
}

// What is left of a light's colour once its brightness has gone into the light level.
float3 LightTint(float3 light)
{
	return lerp(1, light / max(max(light.r, max(light.g, light.b)), 1e-4), 0.6);
}

// A palette entry at a light level, as the game's light tables have it.
float3 PaletteColor(uint index, float shade)
{
	int low = min((int)shade, 15);
	int high = min(low + 1, 15);
	return lerp(paletteLit.Load(int4(index, low, 0, 0)).rgb, paletteLit.Load(int4(index, high, 0, 0)).rgb, shade - low);
}

// The same for a colour: the nearest palette colour at that light level.
float3 PaletteShade(float3 albedo, float shade)
{
	int3 cell = min(int3(pow(saturate(albedo), 1 / 2.2) * 32), 31);
	int low = min((int)shade, 15);
	int high = min(low + 1, 15);
	return lerp(paletteCube.Load(int4(cell.rg, cell.b + 32 * low, 0)).rgb, paletteCube.Load(int4(cell.rg, cell.b + 32 * high, 0)).rgb, shade - low);
}

// A display colour as the scene value that tone mapping (Aces in PsFinal) turns back into it.
float3 ToScene(float3 display)
{
	float3 y = min(display, 0.97);
	float3 a = 2.51 - 2.43 * y;
	float3 b = 0.03 - 0.59 * y;
	return (-b + sqrt(b * b + 0.56 * a * y)) / (2 * a) / headUp.w;
}

)hlsl"
    R"hlsl(
// `color` is the lit surface before fog; `albedo` is only shown by the debug views.
float4 Finish(SurfaceOut input, float3 color, float4 albedo, float3 normal, Lit lit, uint eye)
{
	color += lit.highlight;
	color = Fog(color, input.world, eyePosition[eye].xyz);
	// Dungeons have no roof: what rises above the walls' painted part sinks into black.
	float dark = saturate(1 - (input.world.y - abyss.x) * abyss.y);
	color *= dark * dark;
	if (debugView == 1)
		color = albedo.rgb;
	else if (debugView == 2)
		color = normal * 0.5 + 0.5;
	else if (debugView == 3)
		color = lit.light;
	else if (debugView == 4)
		color = Heat(lit.lightsHere / 8.0);
	else if (debugView == 5)
		color = 0.18 * lit.light;
	return float4(color, albedo.a);
}

uint EyeOf(SurfaceOut input)
{
#ifdef LAYERED
	return input.slice;
#else
	return viewBase;
#endif
}

// Alpha goes to MSAA coverage, so cut-out edges are as smooth as geometry edges.
float Coverage(float alpha)
{
	return saturate((alpha - 0.5) / max(fwidth(alpha), 1e-4) + 0.5);
}

float4 PsSurface(SurfaceOut input) : SV_Target
{
	// Thin slabs and cards are seen from both sides: the normal is the one facing the viewer.
	float3 normal = normalize(input.normal);
	normal *= dot(normal, eyePosition[EyeOf(input)].xyz - input.world) < 0 ? -1 : 1;
	// A surface that wears a material carries it in the layer value (see MaterialLayerStep).
	float layer = input.uvLayer.z;
#if PLAIN_SURFACES
	// The level has no materials, no floor map and no grit: none of that is looked up.
	float material = -1;
#else
	float material = layer >= 8192 ? floor(layer / 8192) - 1 : -1;
	layer -= (material + 1) * 8192;
#endif
	// Sampled unconditionally: texture lookups need derivatives, which a branch would not have.
	bool textured = layer >= 0;
	float3 uvLayer = float3(input.uvLayer.xy, max(layer, 0));
	// Negative v: the wall above its painting, which repeats the painting's plain masonry band back and forth.
	bool above = uvLayer.y < 0;
	if (above)
		uvLayer.y = lerp(0.6, 0.25, 1 - abs(frac(-uvLayer.y * 0.5) * 2 - 1));
	// The art's texels stay crisp when magnified: the lookup moves from one texel centre to the
	// next within one screen pixel. Mips and anisotropy still come from the true gradients.
	float2 size = float2(64, 128);
	float2 at = uvLayer.xy * size - 0.5;
	float2 crisp = (floor(at) + 0.5 + saturate((frac(at) - 0.5) / max(fwidth(at), 1e-4) + 0.5)) / size;
	float4 texel = surfaces.SampleGrad(surfaceSampler, float3(crisp, uvLayer.z), ddx(uvLayer.xy), ddy(uvLayer.xy));
	float3 bumped = BumpedNormal(normal, input.world, uvLayer);
	// Layers from 4096 up are a model's own texture, which repeats and has no relief map.
	bool model = layer >= 4096;
	float4 modelTexel = modelTextures.Sample(repeatSampler, float3(input.uvLayer.xy, layer - 4096));
	if (model) {
		texel = float4(modelTexel.rgb, 1);
		bumped = normal;
	}
#if PLAIN_SURFACES
	bool gritty = false;
	float2 surfaceAt = 0;
#else
	// Materials are laid out by world position, so they run on across tiles: floors by x and z,
	// walls by height and whichever of x and z they run along.
	float3 facing = abs(normal);
	float3 tangent = facing.y > 0.7 || facing.z >= facing.x ? float3(1, 0, 0) : float3(0, 0, 1);
	float3 bitangent = facing.y > 0.7 ? float3(0, 0, 1) : float3(0, -1, 0);
	float2 surfaceAt = float2(dot(input.world, tangent), dot(input.world, bitangent));
	bool gritty = abyss.z > 0;
	float3 materialAt = float3((gritty ? GritSnap(surfaceAt) : surfaceAt) * palette.z, max(material, 0));
	// A grit texel shows the material as it looks from far enough away to be one texel.
	float2 materialDx = ddx(surfaceAt) * palette.z;
	float2 materialDy = ddy(surfaceAt) * palette.z;
	if (gritty) {
		float least = palette.z / abyss.w;
		materialDx = normalize(materialDx + 1e-9) * max(length(materialDx), least);
		materialDy = normalize(materialDy + 1e-9) * max(length(materialDy), least);
	}
	float4 materialTexel = materials.SampleGrad(repeatSampler, materialAt, materialDx, materialDy);
	float3 materialMean = materials.SampleLevel(repeatSampler, materialAt, 16).rgb;
	float2 relief = materialNormals.Sample(repeatSampler, materialAt).xy * 2 - 1;
	// Floors: one surface over the whole level. The floor map says which two materials meet at
	// this point; they meet along the stones (the higher one wins), each laid out by world
	// position with a hex tiling, and the floor darkens towards the walls.
	float grime = 1;
	bool floorMapped = false;
	if (palette.w > 0 && facing.y > 0.7) {
		float4 meet = floorMap.Load(int4(int2(floor(input.world.xz / level.x * 8)), 0, 0));
		int first = (int)(meet.x * 255 + 0.5) - 1;
		int second = (int)(meet.y * 255 + 0.5) - 1;
		if (first >= 0) {
			float2 floorUv = (gritty ? GritSnap(input.world.xz) : input.world.xz) * palette.w;
			float2 floorDx = ddx(input.world.xz) * palette.w;
			float2 floorDy = ddy(input.world.xz) * palette.w;
			if (gritty) {
				float least = palette.w / abyss.w;
				floorDx = normalize(floorDx + 1e-9) * max(length(floorDx), least);
				floorDy = normalize(floorDy + 1e-9) * max(length(floorDy), least);
			}
			float4 a = HexSample(materials, floorUv, first, floorDx, floorDy);
			float4 b = second >= 0 ? HexSample(materials, floorUv, second, floorDx, floorDy) : a;
			float heightA = dot(a.rgb, float3(0.3, 0.59, 0.11)) + (1 - meet.z);
			float heightB = dot(b.rgb, float3(0.3, 0.59, 0.11)) + meet.z;
			float pick = smoothstep(-0.04, 0.04, heightB - heightA);
			materialTexel = lerp(a, b, pick);
			materialMean = lerp(materials.SampleLevel(repeatSampler, float3(floorUv, first), 16).rgb, materials.SampleLevel(repeatSampler, float3(floorUv, max(second, first)), 16).rgb, pick);
			relief = lerp(HexSample(materialNormals, floorUv, first, floorDx, floorDy).xy, HexSample(materialNormals, floorUv, max(second, first), floorDx, floorDy).xy, pick) * 2 - 1;
			material = first;
			floorMapped = true;
		}
		// Dark where the floor meets the walls and in corners, a little lighter down the middle of a passage.
		grime = lerp(0.62, 1.05, smoothstep(0, 1, meet.w));
	}
	if (material >= 0) {
		// The layer's own picture shows only where it differs from the material, and takes the material's grain there too.
		float own = above ? 0 : texel.a;
		float3 grain = materialTexel.rgb / max(materialMean, 0.004);
		texel = float4(lerp(materialTexel.rgb, texel.rgb * lerp(1, grain, 0.7), own) * grime, 1);
		float3 relieved = normalize(tangent * relief.x + bitangent * relief.y + normal * sqrt(saturate(1 - dot(relief, relief))));
		bumped = normalize(lerp(relieved, bumped, own * 0.5));
	}
	if (floorMapped && material < 0)
		texel.rgb *= grime;
#endif
	// Lava and water: the picture keeps the palette index of the colours the game cycles, and they
	// take the colour that index has this frame, so they flow as they do in the game.
	uint cycling = (uint)(surfaceNormals.Load(int4(int2(saturate(crisp) * size * 0.9999), uvLayer.z, 0)).b * 255 + 0.5);
	if (textured && !model && cycling != 0)
		texel.rgb = paletteLit.Load(int4(cycling, 0, 0, 0)).rgb;
#if !DEPTH_PREPASSED
	clip(textured ? texel.a - 0.3 : 1);
#endif
	// Walls, floors and everything else built of the level's art wear the grit; what glows does not.
	bool worn = gritty && textured && input.material.x <= 0 && (model || cycling == 0);
	if (worn)
		texel.rgb = Grit(texel.rgb, surfaceAt);
	float4 albedo = textured ? float4(texel.rgb * input.color.rgb, Coverage(texel.a)) : input.color;
	float3 shadingNormal = textured ? bumped : normal;
	uint eye = EyeOf(input);
	Lit lit = LightAt(input, shadingNormal, 0, eye);
	float3 color;
	if (palette.x > 0.5 && textured && input.material.z > 0.5) {
		// What glows is drawn fully lit, as the game draws it.
		float shade = input.material.x > 0 ? 0 : ShadeLevel(lit.light);
		// The game has sixteen light levels and no steps between them: each texel takes one or
		// the other, so the light falls off in a dither instead of a smooth blend.
		if (worn)
			shade = min(floor(shade + HexHash(GritCell(surfaceAt) + 7).x), 15);
		color = ToScene(PaletteShade(albedo.rgb, shade) * LightTint(lit.light)) * (1 + input.material.x);
	} else {
		color = albedo.rgb * (lit.light + input.material.x);
	}
	return Finish(input, color, albedo, shadingNormal, lit, eye);
}

// The depth pre-pass of levels without materials (DEVILUTIONX_VR_DEPTH_PREPASS): only what
// decides whether the pixel is there at all, which is the art's cut-out. PsSurface then runs once
// for each pixel that is seen, compiled with DEPTH_PREPASSED and drawn where the depth is equal.
void PsSurfaceDepth(SurfaceOut input)
{
	float layer = input.uvLayer.z;
	// Models (layers from 4096 up) and plain colours have no holes.
	bool cutOut = layer >= 0 && layer < 4096;
	float3 uvLayer = float3(input.uvLayer.xy, max(layer, 0));
	if (uvLayer.y < 0)
		uvLayer.y = lerp(0.6, 0.25, 1 - abs(frac(-uvLayer.y * 0.5) * 2 - 1));
	float2 size = float2(64, 128);
	float2 at = uvLayer.xy * size - 0.5;
	float2 crisp = (floor(at) + 0.5 + saturate((frac(at) - 0.5) / max(fwidth(at), 1e-4) + 0.5)) / size;
	float alpha = surfaces.SampleGrad(surfaceSampler, float3(crisp, uvLayer.z), ddx(uvLayer.xy), ddy(uvLayer.xy)).a;
	// Half, where alpha to coverage puts the edge when there is one sample a pixel.
	clip(cutOut ? alpha - 0.5 : 1);
}

float4 PsSprite(SurfaceOut input, out float depth : SV_Depth) : SV_Target
{
	uint eye = EyeOf(input);
	float3 normal = normalize(input.normal);
	// The atlas's coverage byte holds the distance to the outline (EncodeRelief): read it smoothly.
	float2 coverageAt = input.uvLayer.xy / 8192; // the atlas is 8192 texels square (sprites.cpp AtlasSize)
	float stored = atlas.SampleLevel(linearClamp, float3(coverageAt, 0), 0).g;
	float reliefPixels = saturate(stored * 2 - 1) * (255.0 / 2 / 4);
	float bulge = input.uvLayer.z * reliefPixels;
	float3 world = input.world;
	if (input.uvLayer.z > 0) {
		// Rounded towards the viewer by the distance to its outline, and lit as that rounded surface.
		float3 toEye = normalize(eyePosition[eye].xyz - input.world);
		world = input.world + toEye * bulge;
		float3 dpdx = ddx(input.world);
		float3 dpdy = ddy(input.world);
		float dhx = ddx(bulge);
		float dhy = ddy(bulge);
		float3 r1 = cross(dpdy, normal);
		float3 r2 = cross(normal, dpdx);
		float det = dot(dpdx, r1);
		float3 slope = sign(det) * (dhx * r1 + dhy * r2);
		normal = normalize(abs(det) * normal - slope);
	}
	float4 clipped = mul(float4(world, 1), viewProj[eye]);
	depth = input.uvLayer.z > 0 ? clipped.z / clipped.w : input.clip.z;
	SurfaceOut lifted = input;
	lifted.world = world;
	Lit lit = LightAt(lifted, normal, input.material.w > 0.5 ? 0 : 0.6, eye);
	bool tables = palette.x > 0.5;
	float shade = tables && input.material.x <= 0 ? ShadeLevel(lit.light) : 0;
	// The atlas holds palette indices, which cannot be filtered: four texels are looked up and
	// blended as colours. The blend is one screen pixel wide, so magnified texels stay crisp.
	float2 at = input.uvLayer.xy - 0.5;
	float2 corner = floor(at);
	float2 blend = saturate((at - corner - 0.5) / max(fwidth(input.uvLayer.xy), 1e-4) + 0.5);
	float4 texel = 0;
	for (int i = 0; i < 4; i++) {
		float2 offset = float2(i & 1, i >> 1);
		float2 texelStored = atlas.Load(int4(corner + offset, 0, 0)).rg;
		float2 weights = lerp(1 - blend, blend, offset);
		texel += float4(PaletteColor((uint)(texelStored.r * 255 + 0.5), shade), 1) * (step(0.5, texelStored.g) * weights.x * weights.y);
	}
	clip(texel.a - 0.1);
	float4 albedo = float4(texel.rgb / texel.a * input.color.rgb, Coverage(texel.a) * input.color.a);
	float3 color = tables ? ToScene(albedo.rgb * LightTint(lit.light)) * (1 + input.material.x) : albedo.rgb * (lit.light + input.material.x);
	return Finish(input, color, albedo, normal, lit, eye);
}

)hlsl"
    R"hlsl(
// ---------------------------------------------------------------- carved models

struct CarvedOut {
	float4 clip : SV_Position;
	float3 world : TEXCOORD0;
	float3 normal : TEXCOORD1;
	float3 model : TEXCOORD2;
	float2 uv : TEXCOORD3;
	nointerpolation uint instance : TEXCOORD4;
#ifdef LAYERED
	uint slice : SV_RenderTargetArrayIndex;
#endif
};

CarvedOut VsCarved(MeshIn input, uint instanceId : SV_InstanceID)
{
	uint eye = viewBase + instanceId % viewCount;
	CarvedOut output;
	output.instance = instanceBase + instanceId / viewCount;
	CarvedInstance instance = carvedInstances[output.instance];
	float3x4 toWorld = float3x4(instance.world0, instance.world1, instance.world2);
	output.world = mul(toWorld, float4(input.position, 1));
	output.normal = normalize(mul((float3x3)toWorld, input.normal));
	output.clip = mul(float4(output.world, 1), viewProj[eye]);
	output.model = input.position;
	output.uv = input.uv;
#ifdef LAYERED
	output.slice = eye;
#endif
	return output;
}

// Index and coverage of one atlas texel, nothing outside the picture's own rectangle.
float2 CarvedLoad(float2 texel, float4 rect)
{
	float2 inside = floor(texel);
	if (any(inside < rect.xy) || any(inside >= rect.xy + rect.zw))
		return 0;
	return atlas.Load(int4(inside, 0, 0)).rg;
}

// Colour of the picture at `at` (atlas texels): four texels blended over one screen pixel, as sprites are.
float4 CarvedColor(float2 at, float4 rect, float2 perPixel, float shade)
{
	float2 from = at - 0.5;
	float2 corner = floor(from);
	float2 blend = saturate((from - corner - 0.5) / max(perPixel, 1e-4) + 0.5);
	float4 texel = 0;
	for (int i = 0; i < 4; i++) {
		float2 offset = float2(i & 1, i >> 1);
		float2 stored = CarvedLoad(corner + offset + 0.5, rect);
		float2 weights = lerp(1 - blend, blend, offset);
		texel += float4(PaletteColor((uint)(stored.r * 255 + 0.5), shade), 1) * (step(0.5, stored.g) * weights.x * weights.y);
	}
	return texel;
}

float4 PsCarved(CarvedOut input) : SV_Target
{
#ifdef LAYERED
	uint eye = input.slice;
#else
	uint eye = viewBase;
#endif
	CarvedInstance instance = carvedInstances[input.instance];
	float3 normal = normalize(input.normal);
	normal *= dot(normal, eyePosition[eye].xyz - input.world) < 0 ? -1 : 1;
	SurfaceOut surface;
	surface.clip = input.clip;
	surface.world = input.world;
	surface.normal = normal;
	surface.uvLayer = 0;
	surface.color = 1;
	surface.material = float4(instance.params.y, 0, 1, 0);
#ifdef LAYERED
	surface.slice = input.slice;
#endif
	Lit lit = LightAt(surface, normal, 0.35, eye);
	bool tables = palette.x > 0.5;
	float shade = tables && instance.params.y <= 0 ? ShadeLevel(lit.light) : 0;
	// Texels per screen pixel, for the one-pixel blend between texels.
	float2 uvPerPixel = fwidth(input.uv);
	float4 map = instance.map;
	float4 texel = CarvedColor(map.xy + input.uv * map.z, instance.rect, uvPerPixel * map.z, shade);
	clip(texel.a - 0.1);
	float4 albedo = float4(texel.rgb / texel.a, 1);
	float3 color = tables ? ToScene(albedo.rgb * LightTint(lit.light)) * (1 + instance.params.y) : albedo.rgb * (lit.light + instance.params.y);
	return Finish(surface, color, albedo, normal, lit, eye);
}
)hlsl"
    R"hlsl(
float4 PsParticle(ParticleOut input) : SV_Target
{
	float falloff = saturate(1 - length(input.uv));
	return input.color * (falloff * falloff);
}

// ---------------------------------------------------------------- post

struct FullOut {
	float4 clip : SV_Position;
	float2 uv : TEXCOORD0;
};

FullOut VsFullscreen(uint vertexId : SV_VertexID)
{
	FullOut output;
	output.uv = float2((vertexId << 1) & 2, vertexId & 2);
	output.clip = float4(output.uv * float2(2, -2) + float2(-1, 1), 0, 1);
	return output;
}

// MSAA resolve of HDR colour. Samples are weighted by their inverse brightness, otherwise one
// very bright sample (a flame against the dark) wins the pixel and the edge stays jagged.
float4 PsResolve(FullOut input) : SV_Target
{
#if SINGLE_SAMPLE
	return 0;
#else
	int3 pixel = int3(input.clip.xy, post.x);
	float3 sum = 0;
	float weights = 0;
	for (int i = 0; i < (int)post.y; i++) {
		float3 color = msColor.Load(pixel, i).rgb;
		float weight = 1 / (1 + max(color.r, max(color.g, color.b)));
		sum += color * weight;
		weights += weight;
	}
	return float4(sum / weights, 1);
#endif
}

float3 Box(float2 uv, float2 texel)
{
	return (postSource.SampleLevel(linearClamp, float3(uv + texel * float2(-1, -1), post.x), 0).rgb
	    + postSource.SampleLevel(linearClamp, float3(uv + texel * float2(1, -1), post.x), 0).rgb
	    + postSource.SampleLevel(linearClamp, float3(uv + texel * float2(-1, 1), post.x), 0).rgb
	    + postSource.SampleLevel(linearClamp, float3(uv + texel * float2(1, 1), post.x), 0).rgb) / 4;
}

// post: slice, threshold, source texel size.
float4 PsBloomDown(FullOut input) : SV_Target
{
	float3 color = Box(input.uv, post.zw);
	float brightness = max(color.r, max(color.g, color.b));
	return float4(color * saturate((brightness - post.y) / max(brightness, 1e-4)), 1);
}

float4 PsBloomUp(FullOut input) : SV_Target
{
	return float4(Box(input.uv, post.zw), 1);
}

float3 Aces(float3 x)
{
	return saturate(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14));
}

struct FinalOut {
	float4 color : SV_Target;
	float depth : SV_Depth;
};

// post: slice, unused, internal target size.
FinalOut PsFinal(FullOut input)
{
	float3 hdr = postSource.SampleLevel(linearClamp, float3(input.uv, post.x), 0).rgb;
	hdr += postBloom.SampleLevel(linearClamp, float3(input.uv, post.x), 0).rgb * ambient.a;
	float3 color = Aces(hdr * headUp.w);
	// Comfort while moving or turning (post.y): the edge of view sinks into black.
	float edge = length((input.uv - 0.5) * float2(1.3, 1.0));
	color *= 1 - post.y * smoothstep(0.62 - 0.32 * post.y, 0.7, edge);
	// Dither in display space: eight bits cannot hold a dungeon's gradients without banding.
	color = pow(color, 1 / 2.2) + (Noise(input.clip.xy) - 0.5) / 255;
	FinalOut output;
	output.color = float4(pow(max(color, 0), 2.2), 1);
#if SINGLE_SAMPLE
	output.depth = msDepth.Load(int4(input.uv * post.zw, post.x, 0));
#else
	output.depth = msDepth.Load(int3(input.uv * post.zw, post.x), 0);
#endif
	return output;
}

FullOut VsPanel(uint vertexId : SV_VertexID)
{
	float2 corner = Corners[vertexId];
	float3 world = panelCenter.xyz + panelRight.xyz * (corner.x * 2 - 1) + panelUp.xyz * (corner.y * 2 - 1);
	FullOut output;
	output.clip = mul(float4(world, 1), viewProj[(uint)post.x]);
	output.uv = float2(corner.x, 1 - corner.y);
	return output;
}

float4 PsPanel(FullOut input) : SV_Target
{
	float4 color = postSource.Sample(linearClamp, float3(input.uv, 0));
	return float4(color.rgb, color.a * post.y);
}
)hlsl";

} // namespace render
} // namespace vr
} // namespace devilution

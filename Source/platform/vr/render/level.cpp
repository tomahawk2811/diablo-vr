/**
 * @file level.cpp
 *
 * The loaded level as renderer data: static meshes in chunks, surface textures cut from
 * the tile art, a wall-occupancy map for shadows and culling, and a baked lightmap.
 *
 * Geometry comes from what the game knows, not from the paintings: a tile that is solid
 * and blocks light is a wall, standing on its whole footprint and rising out of sight (dungeons
 * have no ceiling, only darkness); every
 * other tile is floor. The paintings only supply the surface: an isometric tile image is
 * an affine picture of the faces it shows (floor diamond, the two far wall faces), so
 * each face is cut out and straightened into its own texture layer.
 *
 * Everything is built when a level is entered and then only patched: a chunk is rebuilt
 * when its tiles change (doors), and so are the occupancy map and the lightmap.
 */
#include "platform/vr/render/bridge.h"

#include <array>
#include <cstring>

#include "engine/palette.h"
#include "engine/render/dun_render.hpp"
#include "engine/render/light_render.hpp"
#include "engine/surface.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/tile_properties.hpp"
#include "levels/trigs.h"
#include "lighting.h"
#include "objects.h"
#include "utils/log.hpp"

namespace devilution {
namespace vr {
namespace render {

namespace {

constexpr int LayerWidth = 64;
constexpr int LayerHeight = 128;
constexpr int MaxLayers = 2048;
constexpr int MaxMaterials = 32;
/** A layer texel this far (0..1, colour distance) from its material's colour starts to show through it, and twice as far shows fully. */
const float DecalThreshold = Knob("DEVILUTIONX_VR_DECAL_THRESHOLD", 0.06F);
/** Meters one repeat of a material covers. */
const float MaterialSize = Knob("DEVILUTIONX_VR_MATERIAL_SIZE", 2.8F);
constexpr int ChunkTiles = 8;
constexpr int ChunksPerSide = MAXDUNX / ChunkTiles;
constexpr int LightmapScale = 4;
constexpr int LightmapSize = MAXDUNX * LightmapScale;
constexpr int OccupancyScale = 2;
constexpr int OccupancySize = MAXDUNX * OccupancyScale;
/** A wall face is recognised when at least this fraction of its place in the art is painted. */
constexpr float FaceCoverage = 0.6F;
/** How far the occlusion test looks, in tiles. Beyond it nothing is lit anyway. */
constexpr int ViewTiles = 48;
/** Dungeons have no roof: walls rise this far, long past where the dark has swallowed them. */
const float AbyssHeight = Knob("DEVILUTIONX_VR_ABYSS_HEIGHT", 48.0F);
/** Meters above the painted wall over which everything fades to black. */
const float AbyssFade = Knob("DEVILUTIONX_VR_ABYSS_FADE", 12.0F);
/** The part of a wall painting (v from, v to) that is plain masonry: the shader repeats it up the wall. */
constexpr float MasonryFrom = 0.6F;
constexpr float MasonryTo = 0.25F;

enum Face : uint8_t {
	FaceNW = 1 << 0,
	FaceNE = 1 << 1,
	FaceFloor = 1 << 2,
	FaceCard = 1 << 3,
	/** Town only: part of a tall painting (house, tree). */
	FaceTall = 1 << 4,
};

struct PieceInfo {
	bool known;
	uint8_t faces;
	bool glows;
	int16_t floorLayer;
	int16_t nwLayer;
	int16_t neLayer;
	int16_t cardLayer;
	float color[3];
	/**
	 * Hell's flesh blocks (HellBlock): the red sides and the bone ridge along the top, for faces
	 * across x ([0]) and across y ([1]). -1 when the piece is no such block.
	 */
	int16_t blockSide[2];
	int16_t blockRidge[2];
};

/** An image as palette index + alpha pairs. */
struct Image {
	const uint8_t *pixels;
	int width;
	int height;
};

/** A town painting (tree, fence, well): a generated model where there is one, otherwise one upright cut-out. */
struct Standee {
	/** "s_<x>_<y>": the model is `vrassets/models/town/<key>.glb`. */
	std::string key;
	const GeneratedModel *model;
	/** Meters from the ground line through `position` to the painting's top. */
	float top;
	SpriteSlot slot;
	Vec3 position;
	/** Meters from `position` to the cut-out's centre line, along the viewer's right. */
	float shift;
	float halfWidth;
	float height;
	/** Meters from the ground to the cut-out's lower edge. */
	float lift;
};

Texture surfaces, surfaceNormals, lightmapTexture, occupancyTexture;
/** 8 texels per tile: which floor materials meet, how far the second shows, how far the nearest wall is. */
Texture floorMapTexture;
constexpr int FloorMapPerTile = 8;
/** Meters one repeat of a floor material covers (manifest `floorsize`, art pixels); 0 = the walls' size. */
float floorSize;
Texture materials, materialNormals;
int materialCount;
float materialColor[MaxMaterials][3];
/** Which material a piece's floor, north-west face and north-east face wear; -1 for none. */
int8_t pieceMaterial[MAXTILES][3];
int8_t layerMaterial[MaxLayers];
/**
 * How much light and dark a layer has at large scale (the spread of its 8 x 16 texel block
 * brightnesses, against their mean): high for a face with an arch or an alcove painted on it,
 * low for plain masonry.
 */
float layerBlotch[MaxLayers];
/** A layer's mean brightness, 0..255. */
float layerBrightness[MaxLayers];
/** The wall piece and face a wall layer was taken from. */
uint16_t layerPiece[MaxLayers];
bool layerNorthEast[MaxLayers];
bool layerIsWall[MaxLayers];
/** Wall art only ever found inside a block of wall, never towards open ground: the block's top seen from above. */
bool layerIsTop[MaxLayers];
/** A layer's left (s = 0) and right (s = 1) edge columns, brightness in 64 steps down: for matching seams. */
constexpr int EdgeSteps = LayerHeight / 2;
float layerEdge[MaxLayers][2][EdgeSteps];
/**
 * Wall faces drawn flipped left to right so their edges meet their neighbours' (FlipForSeams).
 * Bits: 1 the face towards +x, 2 towards -x, 4 towards +y, 8 towards -y.
 */
uint8_t faceFlipped[MAXDUNX][MAXDUNY];
/** 0 puts the art's thin arch and door-frame cut-outs back instead of built doorways. */
const bool Doorways = Knob("DEVILUTIONX_VR_DOORWAYS", 1) != 0;
/** Where a doorway's arch starts and how far it rises, in meters. */
const float DoorwaySpring = Knob("DEVILUTIONX_VR_DOORWAY_SPRING", 2.15F);
const float DoorwayRise = Knob("DEVILUTIONX_VR_DOORWAY_RISE", 0.7F);
/** The one material every wall of the level wears, or -1. */
int wallMaterial = -1;
/** 1 lets a wall piece's own picture (half an arch, a shadow) show through its masonry again. */
const bool WallDecals = Knob("DEVILUTIONX_VR_WALL_DECALS", 0) != 0;
int layerCount;
bool layersDirty;
PieceInfo pieces[MAXTILES];
std::vector<std::array<int16_t, 2>> specialLayers;
int16_t genericWallLayer;
/** A common wall face with the least painted on it (layerBlotch): beside built doorways. */
int16_t plainWallLayer = -1;

Mesh chunks[ChunksPerSide][ChunksPerSide];
uint32_t chunkHash[ChunksPerSide][ChunksPerSide];
bool chunkBuilt[ChunksPerSide][ChunksPerSide];
uint32_t chunkSeen[ChunksPerSide][ChunksPerSide];
uint32_t seenStamp;
Mesh townMesh;
bool townBuilt;
std::vector<Standee> standees;
/** Palette entries the town's open ground is painted with. */
bool groundIndex[256];

/** Solid and light-blocking: a wall prism. */
bool wall[MAXDUNX][MAXDUNY];
/** Light-blocking, solid or not (closed doors): shadows and occlusion. */
bool blocks[MAXDUNX][MAXDUNY];
/** A door stands on the tile. Shut, the game makes the tile a wall; here it is a doorway with a leaf in it. */
bool door[MAXDUNX][MAXDUNY];
/** Town: 1 on the footprint of a modelled building, 2 right around one. */
uint8_t townMask[MAXDUNX][MAXDUNY];
/** Town: the plain grass most of Tristram's open ground is painted with, or -1. */
int16_t townGrassLayer = -1;
/** The standing piece on the tile is part of a carved model: it gets no cut-out. */
bool carvedTile[MAXDUNX][MAXDUNY];
/** A model carved out of the tile set's art or a town painting, and where its origin stands. */
struct CarvedPlacement {
	std::string key;
	Vec3 foot;
};
std::vector<CarvedPlacement> carvedPlacements;
/** A building modelled in Blender: `vrassets/models/houses/<key>.glb`, in metres about its lowest tile's middle. */
struct BuiltPlacement {
	const GeneratedModel *model;
	Vec3 foot;
};
std::vector<BuiltPlacement> builtPlacements;

// What the built data belongs to.
const std::byte *builtCels;
int builtLevel = -1;
bool builtSetLevel;
int builtSetLevelNumber;
int builtType = -99;
float ceilingHeight;
float wallPixels;

std::vector<uint8_t> column;
int columnHeight;
MeshBuilder builder;
std::vector<float> bake;
std::vector<uint8_t> packed;

Surface Scratch(int index)
{
	static OwnedSurface scratch[2] = { OwnedSurface { 128, 512 }, OwnedSurface { 128, 512 } };
	return scratch[index];
}

bool InLevel(int x, int y)
{
	return x >= 0 && y >= 0 && x < MAXDUNX && y < MAXDUNY;
}

bool IsWall(int x, int y)
{
	return InLevel(x, y) && wall[x][y];
}

/** Renders a level piece's whole column image (floor diamond plus everything standing on it), unlit. */
Image RenderColumn(uint16_t piece)
{
	const int height = columnHeight;
	// With the per-pixel lighting option on, the tile renderer reads a light level per pixel
	// from the lightmap, so hand it one that says "fully lit" everywhere.
	static const std::vector<uint8_t> fullyLitLightmap(512 * 512, 0);
	const uint8_t *fullyLit = LightTables[0].data();
	const MICROS &micros = DPieceMicros[piece];
	for (int pass = 0; pass < 2; pass++) {
		const Surface out = Scratch(pass).subregion(0, 0, TILE_WIDTH, height);
		const Lightmap lightmap(Scratch(pass).at(0, 0), fullyLitLightmap, Scratch(pass).pitch(),
		    LightTables, FullyLitLightTable, FullyDarkLightTable);
		for (int y = 0; y < height; y++)
			std::memset(out.at(0, y), pass, TILE_WIDTH);
		Point position { 0, height - 1 };
		for (uint_fast8_t i = 0; i < MicroTileLen; i += 2) {
			for (int side = 0; side < 2; side++) {
				const LevelCelBlock block { micros.mt[i + side] };
				if (!block.hasValue())
					continue;
				const Point target = position + Displacement { side * TILE_WIDTH / 2, 0 };
				if (i == 0 && block.type() == TileType::TransparentSquare && !HasAnyOf(SOLData[piece], TileProperties::Solid | TileProperties::BlockMissile)) {
					// Floor piece with foliage on top: the floor half is a triangle, like DrawFloorTile does it.
					RenderTileFrame(out, lightmap, target, side == 0 ? TileType::LeftTriangle : TileType::RightTriangle,
					    GetDunFrame(pDungeonCels.get(), block.frame()), DunFrameTriangleHeight, MaskType::Solid, fullyLit);
					RenderTileFoliage(out, lightmap, target, pDungeonCels.get(), block, fullyLit);
				} else {
					RenderTile(out, lightmap, target, pDungeonCels.get(), block, MaskType::Solid, fullyLit);
				}
			}
			position.y -= TILE_HEIGHT;
		}
	}
	column.resize(static_cast<size_t>(TILE_WIDTH) * height * 2);
	for (int y = 0; y < height; y++) {
		const uint8_t *a = Scratch(0).at(0, y);
		const uint8_t *b = Scratch(1).at(0, y);
		for (int x = 0; x < TILE_WIDTH; x++) {
			column[(static_cast<size_t>(y) * TILE_WIDTH + x) * 2] = a[x];
			column[(static_cast<size_t>(y) * TILE_WIDTH + x) * 2 + 1] = a[x] == b[x] ? 255 : 0;
		}
	}
	return { column.data(), TILE_WIDTH, height };
}

bool Opaque(const Image &image, int x, int y)
{
	return x >= 0 && y >= 0 && x < image.width && y < image.height && image.pixels[(static_cast<size_t>(y) * image.width + x) * 2 + 1] != 0;
}

/**
 * The highest palette index whose colour the game cycles on this level (lava, water; the hive's
 * pools): 1..this rotate through the palette or the light tables every frame. 0 when none do.
 */
int CyclingColors()
{
	switch (leveltype) {
	case DTYPE_CAVES:
	case DTYPE_HELL:
	case DTYPE_CRYPT:
		return 31;
	case DTYPE_NEST:
		return 15;
	default:
		return 0;
	}
}

/** Bilinear sample as premultiplied sRGB, 0..255 per channel. `index` gets the nearest painted pixel's palette index (0 for none). */
void Sample(const Image &image, float px, float py, float out[4], uint8_t *index = nullptr)
{
	px -= 0.5F;
	py -= 0.5F;
	const int x0 = static_cast<int>(std::floor(px));
	const int y0 = static_cast<int>(std::floor(py));
	const float fx = px - static_cast<float>(x0);
	const float fy = py - static_cast<float>(y0);
	out[0] = out[1] = out[2] = out[3] = 0;
	float heaviest = 0;
	if (index != nullptr)
		*index = 0;
	for (int dy = 0; dy < 2; dy++) {
		for (int dx = 0; dx < 2; dx++) {
			if (!Opaque(image, x0 + dx, y0 + dy))
				continue;
			const float weight = (dx == 0 ? 1 - fx : fx) * (dy == 0 ? 1 - fy : fy);
			if (index != nullptr && weight > heaviest) {
				heaviest = weight;
				*index = image.pixels[(static_cast<size_t>(y0 + dy) * image.width + x0 + dx) * 2];
			}
			const SDL_Color color = logical_palette[image.pixels[(static_cast<size_t>(y0 + dy) * image.width + x0 + dx) * 2]];
			out[0] += static_cast<float>(color.r) * weight;
			out[1] += static_cast<float>(color.g) * weight;
			out[2] += static_cast<float>(color.b) * weight;
			out[3] += 255 * weight;
		}
	}
}

/**
 * Straightens a face of an image into a new texture layer. `map` gives the image position of
 * a layer position (s, t in 0..1, t downwards). Opaque layers get their unpainted texels filled
 * in from their surroundings; cut-out layers keep them transparent.
 * Returns the layer, or -1 when the array is full.
 */
template <typename Map>
int16_t ExtractLayer(const Image &image, bool opaque, Map map, float *averageColor = nullptr, int material = -1, bool decal = true)
{
	if (layerCount >= MaxLayers) {
		static bool logged;
		if (!logged)
			LogError("VR: more than {} surface layers in one level, the rest are left untextured", MaxLayers);
		logged = true;
		return -1;
	}
	constexpr int Count = LayerWidth * LayerHeight;
	static float texels[Count][4];
	// Palette indices the game cycles, kept so the shader can give them their colour of the moment.
	static uint8_t cycling[Count];
	const int cyclingColors = CyclingColors();
	float sum[3] = {};
	float covered = 0;
	for (int j = 0; j < LayerHeight; j++) {
		for (int i = 0; i < LayerWidth; i++) {
			float px;
			float py;
			map((static_cast<float>(i) + 0.5F) / LayerWidth, (static_cast<float>(j) + 0.5F) / LayerHeight, px, py);
			float *texel = texels[j * LayerWidth + i];
			uint8_t index;
			Sample(image, px, py, texel, &index);
			cycling[j * LayerWidth + i] = index >= 1 && index <= cyclingColors ? index : 0;
			if (texel[3] > 0) {
				// Un-premultiply: colour stays full strength, alpha carries the coverage.
				for (int c = 0; c < 3; c++) {
					texel[c] *= 255 / texel[3];
					sum[c] += texel[c];
				}
				covered++;
			}
		}
	}
	float average[3] = { 40, 40, 40 };
	if (covered > 0) {
		for (int c = 0; c < 3; c++)
			average[c] = sum[c] / covered;
	}
	if (averageColor != nullptr) {
		for (int c = 0; c < 3; c++)
			averageColor[c] = std::pow(average[c] / 255, 2.2F);
	}

	// Colour spreads into unpainted texels: a few steps for cut-outs (so filtering finds no
	// black fringe), until everything is covered for opaque layers.
	static bool painted[Count];
	static bool paintedBefore[Count];
	for (int i = 0; i < Count; i++)
		painted[i] = texels[i][3] > 0;
	for (int pass = 0; pass < (opaque ? 24 : 3); pass++) {
		std::memcpy(paintedBefore, painted, sizeof(painted));
		bool changed = false;
		for (int j = 0; j < LayerHeight; j++) {
			for (int i = 0; i < LayerWidth; i++) {
				const int index = j * LayerWidth + i;
				if (paintedBefore[index])
					continue;
				const int neighbours[4] = { i > 0 ? index - 1 : -1, i < LayerWidth - 1 ? index + 1 : -1, j > 0 ? index - LayerWidth : -1, j < LayerHeight - 1 ? index + LayerWidth : -1 };
				for (const int neighbour : neighbours) {
					if (neighbour < 0 || !paintedBefore[neighbour])
						continue;
					std::memcpy(texels[index], texels[neighbour], 3 * sizeof(float));
					cycling[index] = cycling[neighbour];
					painted[index] = true;
					changed = true;
					break;
				}
			}
		}
		if (!changed)
			break;
	}

	static uint32_t albedo[Count];
	static uint32_t normals[Count];
	static float heights[Count];
	for (int i = 0; i < Count; i++) {
		float *texel = texels[i];
		if (!painted[i]) {
			// Far from any paint: the layer's own average, darkened, reads as shadowed stone.
			for (int c = 0; c < 3; c++)
				texel[c] = average[c] * 0.6F;
		}
		if (opaque)
			texel[3] = 255;
		if (opaque && material >= 0) {
			// Alpha becomes how much of this picture shows through the material: nothing where it
			// is the material's own colour, all of it where something else is painted (a stain,
			// a shadow, an arch). Judged on a blurred copy, so single stones do not count.
			float blurred[3] = {};
			int count = 0;
			const int x = i % LayerWidth;
			const int y = i / LayerWidth;
			for (int dy = -2; dy <= 2; dy++) {
				for (int dx = -2; dx <= 2; dx++) {
					const int nx = x + dx;
					const int ny = y + dy;
					if (nx < 0 || ny < 0 || nx >= LayerWidth || ny >= LayerHeight || !painted[ny * LayerWidth + nx])
						continue;
					for (int c = 0; c < 3; c++)
						blurred[c] += texels[ny * LayerWidth + nx][c];
					count++;
				}
			}
			float distance = 0;
			static const float weight[3] = { 0.3F, 0.6F, 0.1F };
			for (int c = 0; c < 3; c++) {
				const float difference = (count != 0 ? blurred[c] / static_cast<float>(count) : texel[c]) / 255 - materialColor[material][c];
				distance += weight[c] * difference * difference;
			}
			distance = std::sqrt(distance);
			float show = std::clamp((distance - DecalThreshold) / DecalThreshold, 0.F, 1.F);
			// A painted shadow (darker than the stone and no other colour) is not a decal: the floor's
			// own light makes its shadows now. Blood, rugs and pentagrams keep their colour.
			if (count != 0) {
				const float r = blurred[0] / static_cast<float>(count) / 255;
				const float g = blurred[1] / static_cast<float>(count) / 255;
				const float b = blurred[2] / static_cast<float>(count) / 255;
				const float luminance = 0.3F * r + 0.59F * g + 0.11F * b;
				const float stone = 0.3F * materialColor[material][0] + 0.59F * materialColor[material][1] + 0.11F * materialColor[material][2];
				const float chroma = std::max(r, std::max(g, b)) - std::min(r, std::min(g, b));
				if (luminance < stone * 0.85F && chroma < 0.08F)
					show = 0;
			}
			texel[3] = decal ? 255 * show * show * (3 - 2 * show) : 0;
		}
		const auto byte = [](float value) { return static_cast<uint32_t>(std::clamp(value, 0.F, 255.F)); };
		albedo[i] = byte(texel[0]) | byte(texel[1]) << 8 | byte(texel[2]) << 16 | byte(texel[3]) << 24;
		heights[i] = (texel[0] * 0.3F + texel[1] * 0.59F + texel[2] * 0.11F) / 255;
	}
	// Relief from brightness: mortar lines and cracks are darker than the stone around them.
	constexpr float Strength = 2.2F;
	for (int j = 0; j < LayerHeight; j++) {
		for (int i = 0; i < LayerWidth; i++) {
			const auto height = [&](int x, int y) { return heights[std::clamp(y, 0, LayerHeight - 1) * LayerWidth + std::clamp(x, 0, LayerWidth - 1)]; };
			const float dx = (height(i + 1, j) - height(i - 1, j)) * Strength;
			const float dy = (height(i, j + 1) - height(i, j - 1)) * Strength;
			const float length = std::sqrt(dx * dx + dy * dy + 1);
			const auto pack = [&](float value) { return static_cast<uint32_t>((-value / length * 0.5F + 0.5F) * 255); };
			// Blue: the palette index of a cycling colour (0 for any other), read by PsSurface.
			const int at = j * LayerWidth + i;
			normals[at] = pack(dx) | pack(dy) << 8 | static_cast<uint32_t>(painted[at] ? cycling[at] : 0) << 16 | 0xFF000000;
		}
	}
	{
		constexpr int BlockW = 8;
		constexpr int BlockH = 16;
		float blocks[(LayerWidth / BlockW) * (LayerHeight / BlockH)] = {};
		for (int j = 0; j < LayerHeight; j++) {
			for (int i = 0; i < LayerWidth; i++) {
				const uint32_t c = albedo[j * LayerWidth + i];
				blocks[(j / BlockH) * (LayerWidth / BlockW) + i / BlockW] += 0.3F * static_cast<float>(c & 0xFF) + 0.59F * static_cast<float>((c >> 8) & 0xFF) + 0.11F * static_cast<float>((c >> 16) & 0xFF);
			}
		}
		float mean = 0;
		for (const float block : blocks)
			mean += block;
		mean /= static_cast<float>(std::size(blocks));
		float spread = 0;
		for (const float block : blocks)
			spread += (block - mean) * (block - mean);
		layerBlotch[layerCount] = mean > 0 ? std::sqrt(spread / static_cast<float>(std::size(blocks))) / mean : 1;
		for (int side = 0; side < 2; side++) {
			for (int step = 0; step < EdgeSteps; step++) {
				float sum = 0;
				for (int j = step * 2; j < step * 2 + 2; j++) {
					for (int i = side == 0 ? 0 : LayerWidth - 2; i < (side == 0 ? 2 : LayerWidth); i++) {
						const uint32_t c = albedo[j * LayerWidth + i];
						sum += 0.3F * static_cast<float>(c & 0xFF) + 0.59F * static_cast<float>((c >> 8) & 0xFF) + 0.11F * static_cast<float>((c >> 16) & 0xFF);
					}
				}
				layerEdge[layerCount][side][step] = sum / 4;
			}
		}
		layerBrightness[layerCount] = mean / (BlockW * BlockH);
	}
	// Debug aid: DEVILUTIONX_VR_DEBUG_WALLS=<folder> saves every layer there as wall-<n>.bmp.
	if (const char *folder = SDL_getenv("DEVILUTIONX_VR_DEBUG_WALLS"); folder != nullptr && folder[0] == '/') {
		if (SDL_Surface *picture = SDL_CreateRGBSurfaceWithFormatFrom(albedo, LayerWidth, LayerHeight, 32, LayerWidth * 4, SDL_PIXELFORMAT_ABGR8888); picture != nullptr) {
			char path[512];
			SDL_snprintf(path, sizeof(path), "%s/wall-%d.bmp", folder, layerCount);
			SDL_SaveBMP(picture, path);
			SDL_FreeSurface(picture);
		}
	}
	UploadTexture(surfaces, layerCount, 0, 0, LayerWidth, LayerHeight, albedo, 4);
	UploadTexture(surfaceNormals, layerCount, 0, 0, LayerWidth, LayerHeight, normals, 4);
	layerMaterial[layerCount] = static_cast<int8_t>(opaque ? material : -1);
	layersDirty = true;
	return static_cast<int16_t>(layerCount++);
}

/** Painted fraction of a wall face's place in a column image: columns [x0, x0 + 32), bottom edge from yLeft to yRight. */
float Coverage(const Image &image, int x0, int yLeft, int yRight, int height)
{
	int opaque = 0;
	for (int i = 0; i < TILE_WIDTH / 2; i++) {
		const int bottom = yLeft + (yRight - yLeft) * i / (TILE_WIDTH / 2);
		for (int y = bottom - height; y < bottom; y++)
			opaque += Opaque(image, x0 + i, y) ? 1 : 0;
	}
	return static_cast<float>(opaque) / static_cast<float>(TILE_WIDTH / 2 * height);
}

int16_t ExtractWallFace(const Image &image, bool northEast, bool opaque, int material = -1, bool decal = true)
{
	const auto h = static_cast<float>(image.height);
	return ExtractLayer(image, opaque, [&](float s, float t, float &px, float &py) {
		// The face's bottom edge runs along the diamond's far side; up the wall is up the image.
		px = (northEast ? 32 : 0) + 32 * s;
		py = (northEast ? h - 32 + 16 * s : h - 16 - 16 * s) - (1 - t) * wallPixels;
	}, nullptr, material, decal);
}

/**
 * A cathedral wall face with no arch on it. The cathedral's walls are painted as a blind arcade,
 * so every face carries half an arch; beside a doorway built in 3D that half has nothing to meet.
 * This takes the face as ExtractWallFace does, but below the ledge every row repeats the two brick
 * courses under it (layer rows 41 to 52), and on the right, where the arch starts in those rows,
 * from 32 texels to the left.
 */
int16_t ExtractPlainWallFace(const Image &image, bool northEast, int material, bool decal)
{
	const auto h = static_cast<float>(image.height);
	return ExtractLayer(image, true, [&](float s, float t, float &px, float &py) {
		auto i = static_cast<int>(s * LayerWidth);
		auto j = static_cast<int>(t * LayerHeight);
		if (j >= 47) {
			j = 41 + (j - 47) % 12;
			if (i >= 44)
				i -= 32;
		}
		s = (static_cast<float>(i) + 0.5F) / LayerWidth;
		t = (static_cast<float>(j) + 0.5F) / LayerHeight;
		px = (northEast ? 32 : 0) + 32 * s;
		py = (northEast ? h - 32 + 16 * s : h - 16 - 16 * s) - (1 - t) * wallPixels;
	}, nullptr, material, decal);
}

/*
 * Hell's flesh blocks. The art draws them as a box, its top and two front sides red flesh, with a
 * horned ridge of bone along the top's back edges: from above, red ground walled in by bone. In a
 * 64 x H column the top's left and right corners are at H - 86, its front corner at H - 70; the
 * sides show 45 px below the top's front edges, the ridge rises up to 40 px over its back edges.
 * Taking such a piece as a wall face stretches sides, top and ridge up one face. Instead the block
 * is built with the sides as its faces, as high as the top stands (70 px), and the ridge as a
 * cut-out on top of each.
 */
constexpr float HellBlockTop = 70;
constexpr float HellBlockSide = 45;
constexpr float HellBlockRidge = 40;

bool IsFlesh(const Image &image, int x, int y)
{
	float texel[4];
	Sample(image, static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F, texel);
	return texel[3] > 128 && texel[0] > 2 * texel[1] + 20 * texel[3] / 255;
}

/** Whether a hell wall piece is a flesh block: red on both of its sides. */
bool HellBlock(const Image &image)
{
	const int h = image.height;
	return leveltype == DTYPE_HELL && image.width == TILE_WIDTH && h >= 120
	    && IsFlesh(image, 16, h - 52) && IsFlesh(image, 48, h - 52) && IsFlesh(image, 8, h - 62) && IsFlesh(image, 56, h - 62);
}

/** A flesh block's side. `right`: the top's front-right edge (a face across x), else front-left (across y). */
int16_t ExtractHellSide(const Image &image, bool right)
{
	const auto h = static_cast<float>(image.height);
	return ExtractLayer(image, true, [&](float s, float t, float &px, float &py) {
		px = (right ? 32 : 0) + 32 * s;
		py = (right ? h - 70 - 16 * s : h - 86 + 16 * s) + t * HellBlockSide;
	});
}

/** The ridge over a flesh block's top edge, a cut-out; -1 when there is hardly any. `right`: the back-right edge. */
int16_t ExtractHellRidge(const Image &image, bool right)
{
	const int h = image.height;
	int painted = 0;
	for (int i = 0; i < TILE_WIDTH / 2; i++) {
		const int edge = right ? h - 102 + i / 2 : h - 86 - i / 2;
		for (int y = edge - static_cast<int>(HellBlockRidge); y < edge - 2; y++)
			painted += Opaque(image, (right ? 32 : 0) + i, y) ? 1 : 0;
	}
	if (painted < TILE_WIDTH / 2 * 2)
		return -1;
	const auto fh = static_cast<float>(h);
	return ExtractLayer(image, false, [&](float s, float t, float &px, float &py) {
		px = (right ? 32 : 0) + 32 * s;
		py = (right ? fh - 102 + 16 * s : fh - 86 - 16 * s) - 2 - (1 - t) * (HellBlockRidge - 2);
	});
}

/** A layer as Vertex::layer has it: with its material, if it wears one. */
float Worn(int16_t layer)
{
	if (layer < 0 || layerMaterial[layer] < 0)
		return layer;
	return static_cast<float>(layer) + static_cast<float>(layerMaterial[layer] + 1) * MaterialLayerStep;
}

const PieceInfo &Classify(uint16_t piece)
{
	PieceInfo &info = pieces[piece];
	if (info.known)
		return info;
	info.known = true;
	info.floorLayer = info.nwLayer = info.neLayer = info.cardLayer = -1;
	info.blockSide[0] = info.blockSide[1] = info.blockRidge[0] = info.blockRidge[1] = -1;
	const Image image = RenderColumn(piece);
	// Debug aid: with DEVILUTIONX_VR_DEBUG_WALLS=<folder>, every piece's art as col-<piece>.bmp.
	if (const char *folder = SDL_getenv("DEVILUTIONX_VR_DEBUG_WALLS"); folder != nullptr && folder[0] == '/' && image.width > 0 && image.height > 0) {
		std::vector<uint32_t> pixels(static_cast<size_t>(image.width) * image.height);
		for (int y = 0; y < image.height; y++) {
			for (int x = 0; x < image.width; x++) {
				float texel[4];
				uint8_t index;
				Sample(image, static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F, texel, &index);
				const auto byte = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.F, 255.F)); };
				pixels[static_cast<size_t>(y) * image.width + x] = byte(texel[0]) | byte(texel[1]) << 8 | byte(texel[2]) << 16 | byte(texel[3]) << 24;
			}
		}
		if (SDL_Surface *picture = SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(), image.width, image.height, 32, image.width * 4, SDL_PIXELFORMAT_ABGR8888); picture != nullptr) {
			char path[512];
			SDL_snprintf(path, sizeof(path), "%s/col-%d.bmp", folder, piece);
			SDL_SaveBMP(picture, path);
			SDL_FreeSurface(picture);
		}
	}
	const int h = image.height;

	// Diablo draws walls as slabs standing on the tile's two far edges (north-west, north-east).
	const int faceHeight = std::min(static_cast<int>(wallPixels), h - TILE_HEIGHT);
	if (Coverage(image, 0, h - 16, h - 32, faceHeight) >= FaceCoverage)
		info.faces |= FaceNW;
	if (Coverage(image, 32, h - 32, h - 16, faceHeight) >= FaceCoverage)
		info.faces |= FaceNE;
	int diamond = 0;
	int above = 0;
	int tall = 0;
	for (int y = 0; y < h; y++) {
		const int row = y - (h - TILE_HEIGHT);
		const int halfWidth = row < 0 ? 0 : (row < 16 ? 2 * (row + 1) : 2 * (TILE_HEIGHT - row));
		for (int x = 0; x < TILE_WIDTH; x++) {
			if (!Opaque(image, x, y))
				continue;
			if (x >= 32 - halfWidth && x < 32 + halfWidth)
				diamond++;
			else if (y < h - TILE_HEIGHT / 2)
				above++;
			if (y < h - 3 * TILE_HEIGHT)
				tall++;
		}
	}
	if (diamond >= TILE_WIDTH * TILE_HEIGHT / 4)
		info.faces |= FaceFloor;
	const TileProperties properties = SOLData[piece];
	const bool isWall = leveltype != DTYPE_TOWN && HasAnyOf(properties, TileProperties::Solid) && HasAnyOf(properties, TileProperties::BlockLight);
	if (leveltype == DTYPE_TOWN) {
		// Houses and trees are large paintings cut into tile columns; they are put back together (BuildTown).
		info.faces &= ~(FaceNW | FaceNE);
		if (tall >= 48)
			info.faces |= FaceTall;
		else if (above >= 48)
			info.faces |= FaceCard;
	} else if (!isWall && (info.faces & (FaceNW | FaceNE)) == 0 && above >= 48) {
		// Something stands on the tile that is not a wall (pillar base, bones, a stalagmite): an upright card.
		info.faces |= FaceCard;
	}

	if (isWall && HellBlock(image) && Knob("DEVILUTIONX_VR_HELL_BLOCKS", 1) != 0) {
		// Across x: the front-right side and the back-left ridge; across y the other two.
		info.blockSide[0] = ExtractHellSide(image, true);
		info.blockSide[1] = ExtractHellSide(image, false);
		info.blockRidge[0] = ExtractHellRidge(image, false);
		info.blockRidge[1] = ExtractHellRidge(image, true);
	}
	if (isWall) {
		if ((info.faces & FaceNW) != 0)
			info.nwLayer = ExtractWallFace(image, false, true, wallMaterial >= 0 ? wallMaterial : pieceMaterial[piece][1], WallDecals);
		if ((info.faces & FaceNE) != 0)
			info.neLayer = ExtractWallFace(image, true, true, wallMaterial >= 0 ? wallMaterial : pieceMaterial[piece][2], WallDecals);
		for (const int16_t layer : { info.nwLayer, info.neLayer }) {
			if (layer >= 0) {
				layerPiece[layer] = piece;
				layerNorthEast[layer] = layer == info.neLayer;
				layerIsWall[layer] = true;
			}
		}
		return info;
	}
	if ((info.faces & FaceFloor) != 0) {
		info.floorLayer = ExtractLayer(image, true, [&](float s, float t, float &px, float &py) {
			// Tile x runs from the diamond's north corner to its east corner, tile y to its west corner.
			px = 32 + 32 * s - 32 * t;
			py = static_cast<float>(h) - 32 + 16 * s + 16 * t;
		}, info.color, pieceMaterial[piece][0]);
		// Lava and other self-lit ground: bright and warm where everything else is dark stone.
		const float brightness = std::max(info.color[0], std::max(info.color[1], info.color[2]));
		info.glows = leveltype != DTYPE_TOWN && brightness > 0.16F && info.color[0] > 2.2F * info.color[2];
	}
	if ((info.faces & FaceNW) != 0)
		info.nwLayer = ExtractWallFace(image, false, false);
	if ((info.faces & FaceNE) != 0)
		info.neLayer = ExtractWallFace(image, true, false);
	if ((info.faces & FaceCard) != 0) {
		// Everything above the middle of the floor diamond.
		const float cardPixels = static_cast<float>(h) - TILE_HEIGHT / 2;
		info.cardLayer = ExtractLayer(image, false, [&](float s, float t, float &px, float &py) {
			px = 64 * s;
			py = t * cardPixels;
		});
	}
	return info;
}

/** Arches over doorways and the like: sprites laid out like a tile column, drawn over the tile. */
const std::array<int16_t, 2> &SpecialLayers(int index)
{
	static const std::array<int16_t, 2> none { -1, -1 };
	if (!pSpecialCels || index < 0 || static_cast<uint32_t>(index) >= ClxSpriteList { *pSpecialCels }.numSprites())
		return none;
	if (specialLayers.size() <= static_cast<size_t>(index))
		specialLayers.resize(index + 1, { -2, -2 });
	std::array<int16_t, 2> &layers = specialLayers[index];
	if (layers[0] == -2) {
		const ClxSprite sprite = (*pSpecialCels)[index];
		const uint8_t *pixels = RasterizeSprite(sprite, nullptr);
		layers = { -1, -1 };
		if (pixels != nullptr && sprite.width() == TILE_WIDTH) {
			const Image image { pixels, sprite.width(), sprite.height() };
			const int faceHeight = std::min(static_cast<int>(wallPixels), image.height - TILE_HEIGHT);
			if (faceHeight > 0 && Coverage(image, 0, image.height - 16, image.height - 32, faceHeight) > 0.02F)
				layers[0] = ExtractWallFace(image, false, false);
			if (faceHeight > 0 && Coverage(image, 32, image.height - 32, image.height - 16, faceHeight) > 0.02F)
				layers[1] = ExtractWallFace(image, true, false);
		}
	}
	return layers;
}

Vec3 At(float x, float y, float z)
{
	return { x * TileSize, y, z * TileSize };
}

void AddQuad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, float layer, uint32_t color = 0x00FFFFFF, bool mirrored = false)
{
	const Vec3 corners[4] = { a, b, c, d };
	// Bottom-left, bottom-right, top-right, top-left of an upright face; or N, E, S, W of a floor.
	static const float wallUv[4][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
	static const float mirroredWallUv[4][2] = { { 1, 1 }, { 0, 1 }, { 0, 0 }, { 1, 0 } };
	static const float floorUv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	const bool flat = a.y == c.y;
	builder.Quad(corners, flat ? floorUv : (mirrored ? mirroredWallUv : wallUv), Worn(static_cast<int16_t>(layer)), color);
}

/** An upright face from (x0, z0) to (x1, z1), floor to ceiling; `mirrored` flips the picture left to right. */
void AddWallFace(float x0, float z0, float x1, float z1, int16_t layer, float height = 0, bool mirrored = false)
{
	if (height == 0)
		height = ceilingHeight;
	// A face without art of its own wears the level's most ordinary wall.
	const uint32_t color = layer >= 0 ? 0x00FFFFFF : PackColor(0.35F, 0.35F, 0.35F);
	AddQuad(At(x0, 0, z0), At(x1, 0, z1), At(x1, height, z1), At(x0, height, z0), layer, color, mirrored);
}

/**
 * The wall above the painted face from (x0, z0) to (x1, z1), up into the dark. Negative v tells
 * the shader to repeat the painting's masonry band, one unit per band.
 */
void AddWallAbove(float x0, float z0, float x1, float z1, int16_t layer)
{
	// The plain look has the wall as high as its painting and nothing but the dark above it.
	if (Classic())
		return;
	const float bands = (AbyssHeight - ceilingHeight) / ((MasonryFrom - MasonryTo) * ceilingHeight);
	const Vec3 corners[4] = { At(x0, ceilingHeight, z0), At(x1, ceilingHeight, z1), At(x1, AbyssHeight, z1), At(x0, AbyssHeight, z0) };
	const float uv[4][2] = { { 0, -1e-4F }, { 1, -1e-4F }, { 1, -bands }, { 0, -bands } };
	builder.Quad(corners, uv, Worn(layer), layer >= 0 ? 0x00FFFFFF : PackColor(0.35F, 0.35F, 0.35F));
}

/** Smooth value noise in 0..1 over a 3D lattice. */
float LatticeNoise(float x, float y, float z)
{
	const auto hash = [](int i, int j, int k) {
		uint32_t h = static_cast<uint32_t>(i) * 73856093U ^ static_cast<uint32_t>(j) * 19349663U ^ static_cast<uint32_t>(k) * 83492791U;
		h ^= h >> 13;
		h *= 0x5bd1e995U;
		h ^= h >> 15;
		return static_cast<float>(h & 0xFFFF) / 65535.F;
	};
	const int i = static_cast<int>(std::floor(x));
	const int j = static_cast<int>(std::floor(y));
	const int k = static_cast<int>(std::floor(z));
	const auto fade = [](float t) { return t * t * (3 - 2 * t); };
	const float fx = fade(x - static_cast<float>(i));
	const float fy = fade(y - static_cast<float>(j));
	const float fz = fade(z - static_cast<float>(k));
	const auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
	return lerp(lerp(lerp(hash(i, j, k), hash(i + 1, j, k), fx), lerp(hash(i, j + 1, k), hash(i + 1, j + 1, k), fx), fy),
	    lerp(lerp(hash(i, j, k + 1), hash(i + 1, j, k + 1), fx), lerp(hash(i, j + 1, k + 1), hash(i + 1, j + 1, k + 1), fx), fy), fz);
}

/**
 * Where a point of a cave wall really lies. Corners of the tile grid are rounded off (a wall
 * corner sticking into a passage is cut back into the rock, a pocket between walls is filled),
 * and the rock is pushed in and out by a few hand-widths of noise. It depends on the position
 * alone, so neighbouring faces stay joined.
 */
Vec3 CaveDisplace(Vec3 p)
{
	if (p.y >= ceilingHeight + 3)
		return p;
	const float u = p.x / TileSize;
	const float v = p.z / TileSize;
	const int cx = static_cast<int>(std::lround(u));
	const int cy = static_cast<int>(std::lround(v));
	const float du = u - static_cast<float>(cx);
	const float dv = v - static_cast<float>(cy);
	const float fromCorner = std::sqrt(du * du + dv * dv);
	Vec3 out = p;
	if (fromCorner < 0.6F) {
		// The four tiles around the grid corner: how many are rock.
		int rock = 0;
		float wx = 0;
		float wz = 0;
		float ox = 0;
		float oz = 0;
		for (int ty = cy - 1; ty <= cy; ty++) {
			for (int tx = cx - 1; tx <= cx; tx++) {
				const float mx = static_cast<float>(tx) + 0.5F - static_cast<float>(cx);
				const float mz = static_cast<float>(ty) + 0.5F - static_cast<float>(cy);
				if (IsWall(tx, ty)) {
					rock++;
					wx += mx;
					wz += mz;
				} else {
					ox += mx;
					oz += mz;
				}
			}
		}
		const float pull = 0.3F * TileSize * (1 - fromCorner / 0.6F) * (1 - fromCorner / 0.6F);
		if (rock == 1) {
			out.x += wx * pull;
			out.z += wz * pull;
		} else if (rock == 3) {
			out.x += ox * pull;
			out.z += oz * pull;
		}
	}
	const float f = 1.35F;
	const float amplitude = 0.16F * std::clamp(1.4F - p.y / (ceilingHeight + 2), 0.3F, 1.F);
	out.x += (LatticeNoise(p.x * f, p.y * f * 0.7F, p.z * f) - 0.5F) * 2 * amplitude;
	out.z += (LatticeNoise(p.x * f + 17.3F, p.y * f * 0.7F, p.z * f + 5.1F) - 0.5F) * 2 * amplitude;
	return out;
}

/**
 * A cave wall face: the same face cut into a grid of small quads and moved by CaveDisplace, so
 * the rock rounds off at corners and is uneven everywhere. Texture coordinates stay the face's.
 */
void AddCaveWallFace(float x0, float z0, float x1, float z1, int16_t layer)
{
	constexpr int Along = 4;
	constexpr int Up = 6;
	const uint32_t color = layer >= 0 ? 0x00FFFFFF : PackColor(0.35F, 0.35F, 0.35F);
	const float bands = (AbyssHeight - ceilingHeight) / ((MasonryFrom - MasonryTo) * ceilingHeight);
	for (int i = 0; i < Along; i++) {
		const float s0 = static_cast<float>(i) / Along;
		const float s1 = static_cast<float>(i + 1) / Along;
		const float ax = x0 + (x1 - x0) * s0;
		const float az = z0 + (z1 - z0) * s0;
		const float bx = x0 + (x1 - x0) * s1;
		const float bz = z0 + (z1 - z0) * s1;
		for (int j = 0; j < Up; j++) {
			const float t0 = static_cast<float>(j) / Up;
			const float t1 = static_cast<float>(j + 1) / Up;
			const Vec3 corners[4] = { CaveDisplace(At(ax, ceilingHeight * t0, az)), CaveDisplace(At(bx, ceilingHeight * t0, bz)),
				CaveDisplace(At(bx, ceilingHeight * t1, bz)), CaveDisplace(At(ax, ceilingHeight * t1, az)) };
			const float uv[4][2] = { { s0, 1 - t0 }, { s1, 1 - t0 }, { s1, 1 - t1 }, { s0, 1 - t1 } };
			builder.Quad(corners, uv, Worn(layer), color);
		}
		// Above the painted part: two rows, the first still uneven, then straight up into the dark.
		const float h1 = ceilingHeight + 3;
		const float b1 = -(h1 - ceilingHeight) / ((MasonryFrom - MasonryTo) * ceilingHeight);
		const Vec3 lower[4] = { CaveDisplace(At(ax, ceilingHeight, az)), CaveDisplace(At(bx, ceilingHeight, bz)), At(bx, h1, bz), At(ax, h1, az) };
		const float uvLower[4][2] = { { s0, -1e-4F }, { s1, -1e-4F }, { s1, b1 }, { s0, b1 } };
		builder.Quad(lower, uvLower, Worn(layer), color);
		const Vec3 upper[4] = { At(ax, h1, az), At(bx, h1, bz), At(bx, AbyssHeight, bz), At(ax, AbyssHeight, az) };
		const float uvUpper[4][2] = { { s0, b1 }, { s1, b1 }, { s1, -bands }, { s0, -bands } };
		builder.Quad(upper, uvUpper, Worn(layer), color);
	}
}

/** A wall tile's face: the painting, and the wall going on above it. */
void AddTallWallFace(float x0, float z0, float x1, float z1, int16_t layer, bool mirrored = false)
{
	if (leveltype == DTYPE_CAVES) {
		AddCaveWallFace(x0, z0, x1, z1, layer);
		return;
	}
	AddWallFace(x0, z0, x1, z1, layer, 0, mirrored);
	AddWallAbove(x0, z0, x1, z1, layer);
}

/**
 * A way through a wall, built as what it is when seen from inside the level: the wall carries on
 * over the opening as an arch as thick as the wall itself. The art only has the arch's front,
 * drawn for the one camera it was painted for (and in two halves, one of them an overlay sprite),
 * which seen from anywhere else is a paper-thin cut-out hanging in the gap.
 * `alongX`: the passage runs along x, so the opening spans the tile's y.
 */
void AddDoorway(int x, int y, bool alongX)
{
	const auto fx = static_cast<float>(x);
	const auto fy = static_cast<float>(y);
	// Across the opening (0..1), height in meters, through the wall (0..1).
	const auto point = [&](float across, float height, float through) {
		return alongX ? At(fx + through, height, fy + across) : At(fx + across, height, fy + through);
	};
	const float spring = std::min(DoorwaySpring, ceilingHeight * 0.65F);
	const float rise = std::min(DoorwayRise, ceilingHeight - spring - 0.3F);
	constexpr int Segments = 10;
	const float layer = Worn(genericWallLayer);
	const uint32_t color = genericWallLayer >= 0 ? 0x00FFFFFF : PackColor(0.35F, 0.35F, 0.35F);
	for (int i = 0; i < Segments; i++) {
		const float t0 = static_cast<float>(i) / Segments;
		const float t1 = static_cast<float>(i + 1) / Segments;
		const float h0 = spring + rise * std::sin(3.14159265F * t0);
		const float h1 = spring + rise * std::sin(3.14159265F * t1);
		const float uv[4][2] = { { t0, 1 - h0 / ceilingHeight }, { t1, 1 - h1 / ceilingHeight }, { t1, 0 }, { t0, 0 } };
		for (const float through : { 0.F, 1.F }) {
			const Vec3 face[4] = { point(t0, h0, through), point(t1, h1, through), point(t1, ceilingHeight, through), point(t0, ceilingHeight, through) };
			builder.Quad(face, uv, layer, color);
		}
		const Vec3 soffit[4] = { point(t0, h0, 0), point(t1, h1, 0), point(t1, h1, 1), point(t0, h0, 1) };
		const float soffitUv[4][2] = { { t0, 1 }, { t1, 1 }, { t1, 0 }, { t0, 0 } };
		builder.Quad(soffit, soffitUv, layer, color);
	}
	if (alongX) {
		AddWallAbove(fx, fy + 1, fx, fy, genericWallLayer);
		AddWallAbove(fx + 1, fy + 1, fx + 1, fy, genericWallLayer);
	} else {
		AddWallAbove(fx, fy, fx + 1, fy, genericWallLayer);
		AddWallAbove(fx, fy + 1, fx + 1, fy + 1, genericWallLayer);
	}
}

// ---------------------------------------------------------------- stairwells

/**
 * The stairs are built, not painted: a flight of steps going down into a pit or up into the rock,
 * where the art only shows the picture of one. Each tile set has its own. Hell keeps its rib cage, the crypt and the hive their art.
 */
enum class StairKind : uint8_t {
	CathedralDown, // a stone-walled pit, the steps going down away from its open side
	CathedralUp,   // a half-round landing and a flight up into the block behind it
	CatacombsDown, // a straight flight down beside a round pillar
	CatacombsUp,   // steps winding up round a round pillar
	CavesDown,     // a tunnel going down into the rock
	CavesUp,       // a flight cut up into the rock
};
struct Stairwell {
	StairKind kind;
	Point at;
};
std::vector<Stairwell> stairwells;
/** Index + 1 of the stairwell built with the tile (its trigger). */
uint8_t stairAt[MAXDUNX][MAXDUNY];
/** No floor: the stairs go down through the tile. */
bool stairHole[MAXDUNX][MAXDUNY];
/** Built over: the tile's painted faces and cut-outs are left out. */
bool stairTile[MAXDUNX][MAXDUNY];
/** A wall tile a flight runs into: no faces towards the stairwell's tiles. */
bool stairCut[MAXDUNX][MAXDUNY];
/** A wall tile that is a round pillar here (built with the stairwell). */
bool stairPillar[MAXDUNX][MAXDUNY];

void MarkStairs(bool (&flags)[MAXDUNX][MAXDUNY], int x0, int y0, int x1, int y1)
{
	for (int y = y0; y <= y1; y++) {
		for (int x = x0; x <= x1; x++) {
			if (InLevel(x, y))
				flags[x][y] = true;
		}
	}
}

/** Finds the level's stairs (the game's triggers) and the tiles they are built over. After `wall` is known. */
void PlanStairwells()
{
	stairwells.clear();
	std::memset(stairAt, 0, sizeof(stairAt));
	std::memset(stairHole, 0, sizeof(stairHole));
	std::memset(stairTile, 0, sizeof(stairTile));
	std::memset(stairCut, 0, sizeof(stairCut));
	std::memset(stairPillar, 0, sizeof(stairPillar));
	if (leveltype != DTYPE_CATHEDRAL && leveltype != DTYPE_CATACOMBS && leveltype != DTYPE_CAVES)
		return;
	struct Shape {
		dungeon_type type;
		uint16_t trigger;
		StairKind kind;
		// The pieces around the trigger the shape needs, as (dx, dy, piece): another layout (a set
		// level's own stairs) is left as painted.
		std::array<std::array<int, 3>, 5> around;
	};
	static const Shape shapes[] = {
		{ DTYPE_CATHEDRAL, 114, StairKind::CathedralDown, { { { 0, -1, 108 }, { -1, 0, 113 }, { 1, 0, 117 }, { 0, -2, 106 }, { 0, -1, 108 } } } },
		{ DTYPE_CATHEDRAL, 128, StairKind::CathedralUp, { { { 1, 0, 129 }, { 0, -1, 138 }, { 1, -1, 139 }, { 0, -2, 136 }, { 1, -2, 137 } } } },
		{ DTYPE_CATACOMBS, 270, StairKind::CatacombsDown, { { { 0, -1, 268 }, { 1, 0, 271 }, { 1, -1, 269 }, { 1, -2, 248 }, { 0, -1, 268 } } } },
		{ DTYPE_CATACOMBS, 266, StairKind::CatacombsUp, { { { 0, -1, 265 }, { 0, -1, 265 }, { 0, -1, 265 }, { 0, -1, 265 }, { 0, -1, 265 } } } },
		{ DTYPE_CAVES, 167, StairKind::CavesDown, { { { 0, -1, 165 }, { 0, 1, 161 }, { -1, 0, 30 }, { -2, 0, 29 }, { 0, -1, 165 } } } },
		{ DTYPE_CAVES, 170, StairKind::CavesUp, { { { 0, -1, 30 }, { 0, -2, 181 }, { -1, 0, 169 }, { 1, 0, 173 }, { 0, -1, 30 } } } },
	};
	for (int i = 0; i < numtrigs && stairwells.size() < 250; i++) {
		const Point t = trigs[i].position;
		if (t.x < 3 || t.y < 3 || t.x + 3 >= MAXDUNX || t.y + 3 >= MAXDUNY)
			continue;
		for (const Shape &shape : shapes) {
			if (shape.type != leveltype || dPiece[t.x][t.y] != shape.trigger)
				continue;
			bool fits = true;
			for (const auto &[dx, dy, piece] : shape.around)
				fits = fits && dPiece[t.x + dx][t.y + dy] == piece;
			if (!fits)
				continue;
			stairwells.push_back({ shape.kind, t });
			stairAt[t.x][t.y] = static_cast<uint8_t>(stairwells.size());
			const int x = t.x;
			const int y = t.y;
			switch (shape.kind) {
			case StairKind::CathedralDown:
				// The pit is wider than a tile: the floor round it is built with it (FloorBesides).
				MarkStairs(stairTile, x - 1, y - 2, x + 1, y);
				MarkStairs(stairHole, x - 1, y - 2, x + 1, y);
				break;
			case StairKind::CathedralUp:
				MarkStairs(stairCut, x, y - 2, x + 1, y - 1);
				MarkStairs(stairTile, x, y, x + 1, y + 2);
				break;
			case StairKind::CatacombsDown:
				MarkStairs(stairPillar, x, y - 1, x, y - 1);
				MarkStairs(stairTile, x - 1, y - 3, x + 2, y + 1);
				MarkStairs(stairHole, x + 1, y - 2, x + 1, y);
				break;
			case StairKind::CatacombsUp:
				MarkStairs(stairPillar, x, y - 1, x, y - 1);
				MarkStairs(stairTile, x - 1, y - 2, x + 1, y + 1);
				break;
			case StairKind::CavesDown:
				// The tunnel is cut out of the rock: the rock round it gets its faces.
				wall[x - 1][y] = wall[x - 2][y] = false;
				MarkStairs(stairTile, x - 2, y, x, y);
				MarkStairs(stairHole, x - 2, y, x - 1, y);
				break;
			case StairKind::CavesUp:
				wall[x][y - 1] = wall[x][y - 2] = false;
				MarkStairs(stairTile, x, y - 2, x, y);
				MarkStairs(stairHole, x, y - 2, x, y - 1);
				break;
			}
			break;
		}
	}
	if (!stairwells.empty())
		LogVerbose("VR: {} stairwells built", stairwells.size());
}

/** Texture coordinates that tell the shader to show only the layer's material (no picture). */
constexpr float PlainUv[4][2] = { { 0, -0.2F }, { 1, -0.2F }, { 1, -0.8F }, { 0, -0.8F } };

void Plain(Vec3 a, Vec3 b, Vec3 c, Vec3 d, int16_t layer)
{
	const Vec3 corners[4] = { a, b, c, d };
	builder.Quad(corners, PlainUv, Worn(layer), layer >= 0 ? 0x00FFFFFF : PackColor(0.35F, 0.35F, 0.35F));
}

/** An upright face from (x0, z0) to (x1, z1) (tiles), from height y0 to y1 (meters). */
void PlainFace(float x0, float z0, float x1, float z1, float y0, float y1, int16_t layer)
{
	Plain(At(x0, y0, z0), At(x1, y0, z1), At(x1, y1, z1), At(x0, y1, z0), layer);
}

void PlainFlat(float x0, float z0, float x1, float z1, float y, int16_t layer)
{
	Plain(At(x0, y, z0), At(x1, y, z0), At(x1, y, z1), At(x0, y, z1), layer);
}

void PlainBox(float x0, float z0, float x1, float z1, float y0, float y1, int16_t side, int16_t top)
{
	PlainFace(x0, z0, x1, z0, y0, y1, side);
	PlainFace(x1, z0, x1, z1, y0, y1, side);
	PlainFace(x1, z1, x0, z1, y0, y1, side);
	PlainFace(x0, z1, x0, z0, y0, y1, side);
	PlainFlat(x0, z0, x1, z1, y1, top);
}

/**
 * A straight flight over [x0, x1] x [z0, z1] (tiles), along x (`alongX`) or z, entered from the
 * low-coordinate edge (`forward` +1) or the high one (-1) at height `from`, climbing (or going
 * down) in `steps` equal steps to `to` at the far edge: each step's riser and tread.
 */
void Flight(float x0, float z0, float x1, float z1, bool alongX, int forward, float from, float to, int steps, int16_t tread, int16_t riser)
{
	const float a0 = alongX ? x0 : z0;
	const float a1 = alongX ? x1 : z1;
	const float c0 = alongX ? z0 : x0;
	const float c1 = alongX ? z1 : x1;
	const float start = forward > 0 ? a0 : a1;
	const float length = (a1 - a0) * static_cast<float>(forward);
	const auto point = [&](float along, float across, float y) { return alongX ? At(along, y, across) : At(across, y, along); };
	float previous = from;
	for (int i = 0; i < steps; i++) {
		const float s0 = start + length * static_cast<float>(i) / static_cast<float>(steps);
		const float s1 = start + length * static_cast<float>(i + 1) / static_cast<float>(steps);
		const float h = from + (to - from) * static_cast<float>(i + 1) / static_cast<float>(steps);
		Plain(point(s0, c0, previous), point(s0, c1, previous), point(s0, c1, h), point(s0, c0, h), riser);
		Plain(point(s0, c0, h), point(s0, c1, h), point(s1, c1, h), point(s1, c0, h), tread);
		previous = h;
	}
}

/** The sides and bottom of a pit under [x0, x1] x [z0, z1] (tiles), `depth` meters deep. */
void Pit(float x0, float z0, float x1, float z1, float depth, int16_t side)
{
	PlainFace(x0, z0, x1, z0, -depth, 0, side);
	PlainFace(x1, z0, x1, z1, -depth, 0, side);
	PlainFace(x1, z1, x0, z1, -depth, 0, side);
	PlainFace(x0, z1, x0, z0, -depth, 0, side);
	PlainFlat(x0, z0, x1, z1, -depth, side);
}

/** The floor of tiles [tx0, tx1] x [ty0, ty1] except where the pit [px0, px1] x [pz0, pz1] (tiles) opens. */
void FloorBesides(int tx0, int ty0, int tx1, int ty1, float px0, float pz0, float px1, float pz1, int16_t layer)
{
	for (int j = ty0; j <= ty1; j++) {
		for (int i = tx0; i <= tx1; i++) {
			const auto x0 = static_cast<float>(i);
			const auto z0 = static_cast<float>(j);
			const float x1 = x0 + 1;
			const float z1 = z0 + 1;
			const float cz0 = std::clamp(pz0, z0, z1);
			const float cz1 = std::clamp(pz1, z0, z1);
			const float cx0 = std::clamp(px0, x0, x1);
			const float cx1 = std::clamp(px1, x0, x1);
			if (cz1 <= cz0 || cx1 <= cx0) {
				PlainFlat(x0, z0, x1, z1, 0, layer);
				continue;
			}
			if (cz0 > z0)
				PlainFlat(x0, z0, x1, cz0, 0, layer);
			if (cz1 < z1)
				PlainFlat(x0, cz1, x1, z1, 0, layer);
			if (cx0 > x0)
				PlainFlat(x0, cz0, cx0, cz1, 0, layer);
			if (cx1 < x1)
				PlainFlat(cx1, cz0, x1, cz1, 0, layer);
		}
	}
}

/** A round pillar on the tile (tiles), from the floor up into the dark. */
void Pillar(float cx, float cz, float radius, int16_t layer)
{
	constexpr int Sides = 16;
	for (int i = 0; i < Sides; i++) {
		const float a0 = 6.2831853F * static_cast<float>(i) / Sides;
		const float a1 = 6.2831853F * static_cast<float>(i + 1) / Sides;
		PlainFace(cx + radius * std::cos(a0), cz + radius * std::sin(a0), cx + radius * std::cos(a1), cz + radius * std::sin(a1), 0, AbyssHeight, layer);
	}
}

/**
 * Steps winding round a pillar at (cx, cz): wedges between radius `inner` and `outer` (tiles),
 * from angle `start` through `sweep` (radians; angle 0 is +x, a quarter turn +z), each a slab
 * `thickness` meters thick, climbing from `from` to `to`.
 */
void Winding(float cx, float cz, float inner, float outer, float start, float sweep, float from, float to, int steps, float thickness, int16_t tread, int16_t side)
{
	const auto point = [&](float r, float a, float y) { return At(cx + r * std::cos(a), y, cz + r * std::sin(a)); };
	for (int i = 0; i < steps; i++) {
		const float a0 = start + sweep * static_cast<float>(i) / static_cast<float>(steps);
		const float a1 = start + sweep * static_cast<float>(i + 1) / static_cast<float>(steps);
		const float h = from + (to - from) * static_cast<float>(i + 1) / static_cast<float>(steps);
		const float b = h - thickness;
		Plain(point(inner, a0, h), point(outer, a0, h), point(outer, a1, h), point(inner, a1, h), tread);
		Plain(point(inner, a0, b), point(outer, a0, b), point(outer, a1, b), point(inner, a1, b), side);
		Plain(point(outer, a0, b), point(outer, a1, b), point(outer, a1, h), point(outer, a0, h), side);
		Plain(point(inner, a0, b), point(outer, a0, b), point(outer, a0, h), point(inner, a0, h), side);
		Plain(point(inner, a1, b), point(outer, a1, b), point(outer, a1, h), point(inner, a1, h), side);
	}
}

/**
 * A D-shaped step on the floor: half round towards +z from z = cz, and straight back from there
 * for `straight` tiles.
 */
void HalfRound(float cx, float cz, float radius, float straight, float y0, float y1, int16_t tread, int16_t side)
{
	if (straight > 0) {
		PlainFlat(cx - radius, cz - straight, cx + radius, cz, y1, tread);
		PlainFace(cx - radius, cz - straight, cx - radius, cz, y0, y1, side);
		PlainFace(cx + radius, cz - straight, cx + radius, cz, y0, y1, side);
	}
	constexpr int Segments = 12;
	const float uv[3][2] = { { 0.5F, -0.2F }, { 0, -0.8F }, { 1, -0.8F } };
	for (int i = 0; i < Segments; i++) {
		const float a0 = 3.1415927F * static_cast<float>(i) / Segments;
		const float a1 = 3.1415927F * static_cast<float>(i + 1) / Segments;
		const float x0 = cx + radius * std::cos(a0);
		const float z0 = cz + radius * std::sin(a0);
		const float x1 = cx + radius * std::cos(a1);
		const float z1 = cz + radius * std::sin(a1);
		const Vec3 fan[3] = { At(cx, y1, cz), At(x0, y1, z0), At(x1, y1, z1) };
		builder.Triangle(fan, uv, Worn(tread), tread >= 0 ? 0x00FFFFFF : PackColor(0.35F, 0.35F, 0.35F));
		PlainFace(x0, z0, x1, z1, y0, y1, side);
	}
}

/** The floor layer of an ordinary floor tile near `at`, to dress the steps in the floor's own stone. */
int16_t NearbyFloorLayer(Point at)
{
	for (int ring = 1; ring <= 8; ring++) {
		for (int dy = -ring; dy <= ring; dy++) {
			for (int dx = -ring; dx <= ring; dx++) {
				if (std::max(std::abs(dx), std::abs(dy)) != ring)
					continue;
				const int x = at.x + dx;
				const int y = at.y + dy;
				if (!InLevel(x, y) || wall[x][y] || stairTile[x][y] || stairHole[x][y] || dPiece[x][y] >= MAXTILES)
					continue;
				const PieceInfo &info = Classify(dPiece[x][y]);
				if (info.floorLayer >= 0 && info.cardLayer < 0 && info.nwLayer < 0 && info.neLayer < 0)
					return info.floorLayer;
			}
		}
	}
	return genericWallLayer;
}

void BuildStairwell(const Stairwell &stairs)
{
	const int16_t tread = NearbyFloorLayer(stairs.at);
	const int16_t side = genericWallLayer;
	const auto x = static_cast<float>(stairs.at.x);
	const auto y = static_cast<float>(stairs.at.y);
	switch (stairs.kind) {
	case StairKind::CathedralDown: {
		// The walled pit as the art has it: about 1.8 tiles across, open
		// on the side towards the trigger. The steps go down from its open side, on under the
		// back wall into the dark.
		const float px0 = x - 0.45F;
		const float px1 = x + 1.3F;
		const float front = y + 0.55F;
		const float back = y - 1.45F;
		FloorBesides(stairs.at.x - 1, stairs.at.y - 2, stairs.at.x + 1, stairs.at.y, px0, back, px1, front, tread);
		Flight(px0, back - 0.55F, px1, front, false, -1, 0, -2.2F, 12, tread, side);
		Pit(px0, back - 0.55F, px1, front, 2.8F, side);
		PlainFace(px0, back, px1, back, -1.1F, 0, side);
		PlainFlat(px0, back - 0.55F, px1, back, -0.02F, side);
		// The walls round three sides, capped, with a post at each end of the open side.
		PlainBox(px0 - 0.4F, back - 0.4F, px0, front + 0.2F, 0, 1.45F, side, side);
		PlainBox(px1, back - 0.4F, px1 + 0.4F, front + 0.2F, 0, 1.45F, side, side);
		PlainBox(px0, back - 0.4F, px1, back, 0, 1.45F, side, side);
		PlainBox(px0 - 0.46F, back - 0.46F, px0 + 0.06F, front + 0.26F, 1.45F, 1.6F, side, tread);
		PlainBox(px1 - 0.06F, back - 0.46F, px1 + 0.46F, front + 0.26F, 1.45F, 1.6F, side, tread);
		PlainBox(px0 + 0.06F, back - 0.46F, px1 - 0.06F, back + 0.06F, 1.45F, 1.6F, side, tread);
		PlainBox(px0 - 0.5F, front - 0.1F, px0 + 0.1F, front + 0.5F, 0, 1.75F, side, tread);
		PlainBox(px1 - 0.1F, front - 0.1F, px1 + 0.5F, front + 0.5F, 0, 1.75F, side, tread);
		break;
	}
	case StairKind::CathedralUp:
		// Half-round steps in the opening, then the flight up through the block behind it.
		// (Round a point half a tile out from the flight, straight back to it.)
		HalfRound(x + 1, y + 0.5F, 1.6F, 0.5F, 0, 0.05F, tread, side);
		HalfRound(x + 1, y + 0.5F, 1.4F, 0.5F, 0.05F, 0.1F, tread, side);
		HalfRound(x + 1, y + 0.5F, 1.2F, 0.5F, 0.1F, 0.15F, tread, side);
		HalfRound(x + 1, y + 0.5F, 1.0F, 0.5F, 0.15F, 0.2F, tread, side);
		Flight(x, y - 2, x + 2, y, false, -1, 0.2F, 2.6F, 12, tread, side);
		// The walls of the cut, seen from inside it (the block's own faces are its outside).
		PlainFace(x + 0.02F, y, x + 0.02F, y - 2, 0, AbyssHeight, side);
		PlainFace(x + 1.98F, y, x + 1.98F, y - 2, 0, AbyssHeight, side);
		PlainFace(x, y - 1.98F, x + 2, y - 1.98F, 0, AbyssHeight, side);
		break;
	case StairKind::CatacombsDown:
		Pillar(x + 0.5F, y - 0.5F, 0.4F, side);
		Flight(x + 1, y - 2, x + 2, y + 1, false, -1, 0, -2.4F, 12, tread, side);
		Pit(x + 1, y - 2, x + 2, y + 1, 3.0F, side);
		break;
	case StairKind::CatacombsUp:
		Pillar(x + 0.5F, y - 0.5F, 0.4F, side);
		// From the trigger's side of the pillar, winding up round its far side as the art has it.
		Winding(x + 0.5F, y - 0.5F, 0.4F, 1.0F, 1.5707963F, 3.6F, 0.18F, 2.4F, 12, 0.14F, tread, side);
		break;
	case StairKind::CavesDown:
		Flight(x - 2, y, x, y + 1, true, -1, 0, -2.0F, 10, tread, side);
		Pit(x - 2, y, x, y + 1, 2.6F, side);
		// Its roof, and the rock over the mouth.
		PlainFlat(x - 2, y, x, y + 1, 2.1F, side);
		PlainFace(x, y, x, y + 1, 2.1F, AbyssHeight, side);
		if (!IsWall(stairs.at.x - 3, stairs.at.y))
			PlainFace(x - 2, y, x - 2, y + 1, 0, AbyssHeight, side);
		break;
	case StairKind::CavesUp:
		Flight(x, y - 2, x + 1, y, false, -1, 0, 2.6F, 12, tread, side);
		if (!IsWall(stairs.at.x, stairs.at.y - 3))
			PlainFace(x, y - 2, x + 1, y - 2, 0, AbyssHeight, side);
		break;
	}
}

/**
 * The plain look: the floor piece most of the level's open ground is laid with. A floor piece
 * that is the same stone with a shadow painted into it (the art's one light, baked in) is laid
 * as this one instead: the shadow belongs to a wall or pillar seen from the art's camera and
 * lies wrong under any other.
 */
int16_t plainFloorLayer = -1;
float plainFloorColor[3];
bool plainFloorKnown;

void FindPlainFloor()
{
	plainFloorKnown = true;
	plainFloorLayer = -1;
	if (!Classic() || leveltype == DTYPE_TOWN)
		return;
	static int uses[MAXTILES];
	std::memset(uses, 0, sizeof(uses));
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			if (!wall[x][y] && dPiece[x][y] < MAXTILES)
				uses[dPiece[x][y]]++;
		}
	}
	int best = 0;
	for (int piece = 0; piece < MAXTILES; piece++) {
		if (uses[piece] <= best)
			continue;
		const PieceInfo &info = Classify(static_cast<uint16_t>(piece));
		if (info.floorLayer < 0 || info.glows || (info.faces & (FaceNW | FaceNE | FaceCard)) != 0)
			continue;
		best = uses[piece];
		plainFloorLayer = info.floorLayer;
		std::memcpy(plainFloorColor, info.color, sizeof(plainFloorColor));
	}
}

/** Whether a floor piece is the plain floor's stone, darkened: a baked shadow. */
bool ShadowedFloor(const PieceInfo &info)
{
	if (plainFloorLayer < 0 || info.floorLayer < 0 || info.floorLayer == plainFloorLayer || info.glows)
		return false;
	const auto luminance = [](const float color[3]) { return 0.3F * color[0] + 0.59F * color[1] + 0.11F * color[2]; };
	const float plain = luminance(plainFloorColor);
	const float own = luminance(info.color);
	if (plain <= 0 || own <= 0 || own > plain * 0.78F)
		return false;
	// The same hue, only darker: blood, rugs and lava have a colour of their own.
	float difference = 0;
	for (int c = 0; c < 3; c++)
		difference += std::fabs(info.color[c] / own - plainFloorColor[c] / plain);
	return difference < 0.25F;
}

/**
 * Whether a doorway is built on the tile (AddTile): a marked opening (door frame, grate, arch) in
 * a wall that runs on to both sides of it. `alongX`: the way through runs along x.
 */
bool BuiltDoorway(int x, int y, bool &alongX)
{
	if (!Doorways || !InLevel(x, y) || wall[x][y] || leveltype == DTYPE_TOWN || dPiece[x][y] >= MAXTILES)
		return false;
	const PieceInfo &info = Classify(dPiece[x][y]);
	const std::array<int16_t, 2> &arch = SpecialLayers(dSpecial[x][y] - 1);
	const bool marked = info.nwLayer >= 0 || info.neLayer >= 0 || arch[0] >= 0 || arch[1] >= 0 || door[x][y];
	alongX = IsWall(x, y - 1) && IsWall(x, y + 1) && !IsWall(x - 1, y) && !IsWall(x + 1, y);
	const bool alongY = IsWall(x - 1, y) && IsWall(x + 1, y) && !IsWall(x, y - 1) && !IsWall(x, y + 1);
	return marked && (alongX || alongY);
}

/**
 * The faces of a wall tile: across x (`nw`, the art's left face) and across y (`ne`). A side the
 * art has no face for borrows the other; one with a block top's art, or beside a built doorway,
 * gets a plain wall.
 */
void WallFaceLayers(int x, int y, int16_t &nw, int16_t &ne)
{
	const PieceInfo &info = Classify(dPiece[x][y]);
	nw = info.nwLayer >= 0 ? info.nwLayer : (info.neLayer >= 0 ? info.neLayer : genericWallLayer);
	ne = info.neLayer >= 0 ? info.neLayer : (info.nwLayer >= 0 ? info.nwLayer : genericWallLayer);
	// A side shown with the art of a block's top (hell's red flesh, borrowed from the other side)
	// wears the common wall face instead.
	if (nw >= 0 && layerIsTop[nw])
		nw = genericWallLayer;
	if (ne >= 0 && layerIsTop[ne])
		ne = genericWallLayer;
	// Beside a built doorway the wall's picture has the other half of the arch painted on it,
	// which the doorway now has for real: plain wall there instead.
	bool alongX;
	const int16_t plain = plainWallLayer >= 0 ? plainWallLayer : genericWallLayer;
	if ((BuiltDoorway(x, y - 1, alongX) && alongX) || (BuiltDoorway(x, y + 1, alongX) && alongX))
		nw = plain;
	if ((BuiltDoorway(x - 1, y, alongX) && !alongX) || (BuiltDoorway(x + 1, y, alongX) && !alongX))
		ne = plain;
}

/**
 * Many wall faces are one half of a pattern: half an arch in the cathedral, half a rib cage in
 * hell. Where the art pairs two pieces, neighbours along a wall make the whole; where one half
 * repeats, they do not. In the cathedral, walking each run of faces, a face is flipped left to
 * right when that makes its edge meet the one before it clearly better, so arches carry on across
 * the seams. Hell's rib cages simply alternate, every other face flipped, in mirrored pairs.
 */
void FlipForSeams()
{
	std::memset(faceFlipped, 0, sizeof(faceFlipped));
	static const bool enabled = Knob("DEVILUTIONX_VR_FLIP_SEAMS", 1) != 0;
	if (!enabled || (leveltype != DTYPE_CATHEDRAL && leveltype != DTYPE_HELL))
		return;
	const auto open = [](int x, int y) { return InLevel(x, y) && !wall[x][y]; };
	const auto difference = [](const float *a, const float *b) {
		float sum = 0;
		// Below the ledge, where the arches and ribs are.
		for (int step = EdgeSteps / 4; step < EdgeSteps; step++)
			sum += std::fabs(a[step] - b[step]);
		return sum;
	};
	// Faces across x run along y; across y along x. `bit` marks the face, `towards` is the open
	// tile in front. The edge a face shares with the next tile along the run is s = 0 for faces
	// across x (s runs from y + 1 to y) and s = 1 for faces across y.
	struct Side {
		uint8_t bit;
		int dx, dy;
		bool acrossX;
	};
	const Side sides[4] = { { 1, 1, 0, true }, { 2, -1, 0, true }, { 4, 0, 1, false }, { 8, 0, -1, false } };
	for (const Side &side : sides) {
		for (int line = 0; line < (side.acrossX ? MAXDUNX : MAXDUNY); line++) {
			int16_t before = -1;
			bool beforeFlipped = false;
			for (int along = 0; along < (side.acrossX ? MAXDUNY : MAXDUNX); along++) {
				const int x = side.acrossX ? line : along;
				const int y = side.acrossX ? along : line;
				if (!InLevel(x, y) || !wall[x][y] || dPiece[x][y] >= MAXTILES || !open(x + side.dx, y + side.dy)
				    || Classify(dPiece[x][y]).blockSide[0] >= 0) {
					before = -1;
					continue;
				}
				int16_t nw;
				int16_t ne;
				WallFaceLayers(x, y, nw, ne);
				const int16_t layer = side.acrossX ? nw : ne;
				if (layer < 0) {
					before = -1;
					continue;
				}
				bool flip = false;
				if (leveltype == DTYPE_HELL) {
					// Hell's rib cages go in mirrored pairs all along: every other face flipped.
					flip = (along & 1) != 0;
				} else if (before >= 0) {
					// The previous face's edge towards this tile, and this face's edge towards it, either way round.
					const int sharedBefore = side.acrossX ? 0 : 1;
					const float *edge = layerEdge[before][beforeFlipped ? 1 - sharedBefore : sharedBefore];
					const int sharedHere = 1 - sharedBefore;
					const float straight = difference(edge, layerEdge[layer][sharedHere]);
					const float turned = difference(edge, layerEdge[layer][1 - sharedHere]);
					flip = turned < straight * 0.8F;
				}
				if (flip)
					faceFlipped[x][y] |= side.bit;
				before = layer;
				beforeFlipped = flip;
			}
		}
	}
}

void AddTile(int x, int y)
{
	if (!plainFloorKnown)
		FindPlainFloor();
	const uint16_t piece = dPiece[x][y];
	if (piece >= MAXTILES)
		return;
	const PieceInfo &info = Classify(piece);
	const auto fx = static_cast<float>(x);
	const auto fy = static_cast<float>(y);

	if (wall[x][y] && leveltype == DTYPE_CAVES) {
		// Rounded corners cut the rock back past the tile's edge: there is floor under it.
		for (const Point next : { Point { x + 1, y }, Point { x - 1, y }, Point { x, y + 1 }, Point { x, y - 1 } }) {
			if (!InLevel(next.x, next.y) || wall[next.x][next.y] || dPiece[next.x][next.y] >= MAXTILES)
				continue;
			const int16_t floorLayer = Classify(dPiece[next.x][next.y]).floorLayer;
			if (floorLayer >= 0) {
				AddQuad(At(fx, 0, fy), At(fx + 1, 0, fy), At(fx + 1, 0, fy + 1), At(fx, 0, fy + 1), floorLayer);
				break;
			}
		}
	}
	if (wall[x][y]) {
		// A stairwell's round pillar is built with the stairwell.
		if (stairPillar[x][y])
			return;
		int16_t nw;
		int16_t ne;
		WallFaceLayers(x, y, nw, ne);
		// Where a flight runs into the wall, it has no face towards the stairs.
		const auto open = [&](int nx, int ny) { return InLevel(nx, ny) && !wall[nx][ny] && !(stairCut[x][y] && stairTile[nx][ny]); };
		if (info.blockSide[0] >= 0 && info.blockSide[1] >= 0) {
			// A flesh block: the sides as high as its top stands, the bone ridge standing on them.
			const float metersPerPixel = ceilingHeight / wallPixels;
			const float top = HellBlockTop * metersPerPixel;
			const float ridge = HellBlockRidge * metersPerPixel;
			const auto face = [&](float x0, float z0, float x1, float z1, int side) {
				AddWallFace(x0, z0, x1, z1, info.blockSide[side], top);
				if (info.blockRidge[side] >= 0)
					AddQuad(At(x0, top, z0), At(x1, top, z1), At(x1, top + ridge, z1), At(x0, top + ridge, z0), info.blockRidge[side]);
			};
			if (open(x + 1, y))
				face(fx + 1, fy + 1, fx + 1, fy, 0);
			if (open(x - 1, y))
				face(fx, fy + 1, fx, fy, 0);
			if (open(x, y + 1))
				face(fx, fy + 1, fx + 1, fy + 1, 1);
			if (open(x, y - 1))
				face(fx, fy, fx + 1, fy, 1);
			return;
		}
		// Faces flipped so their edges meet the neighbours' (FlipForSeams).
		const uint8_t flipped = faceFlipped[x][y];
		if (open(x + 1, y))
			AddTallWallFace(fx + 1, fy + 1, fx + 1, fy, nw, (flipped & 1) != 0);
		if (open(x - 1, y))
			AddTallWallFace(fx, fy + 1, fx, fy, nw, (flipped & 2) != 0);
		if (open(x, y + 1))
			AddTallWallFace(fx, fy + 1, fx + 1, fy + 1, ne, (flipped & 4) != 0);
		if (open(x, y - 1))
			AddTallWallFace(fx, fy, fx + 1, fy, ne, (flipped & 8) != 0);
		return;
	}

	int16_t floorLayer = info.floorLayer;
	// Town: a tile whose picture is a house, a tree's foot or a boulder has no ground of its own
	// (or ground with that thing painted into it, which would show again under its model). It
	// gets plain grass: no holes in the map, no walls and paths painted on the ground under a house.
	if (leveltype == DTYPE_TOWN && townGrassLayer >= 0 && (floorLayer < 0 || townMask[x][y] == 1 || (info.faces & (FaceTall | FaceCard)) != 0))
		floorLayer = townGrassLayer;
	if (floorLayer < 0 && door[x][y]) {
		// A shut door's piece is painted as a wall; the floor runs on under it from next door.
		for (const Point next : { Point { x + 1, y }, Point { x - 1, y }, Point { x, y + 1 }, Point { x, y - 1 } }) {
			if (floorLayer < 0 && InLevel(next.x, next.y) && !wall[next.x][next.y] && dPiece[next.x][next.y] < MAXTILES)
				floorLayer = Classify(dPiece[next.x][next.y]).floorLayer;
		}
	}
	if (floorLayer >= 0 && floorLayer == info.floorLayer && ShadowedFloor(info))
		floorLayer = plainFloorLayer;
	if (floorLayer >= 0 && !stairHole[x][y]) {
		AddQuad(At(fx, 0, fy), At(fx + 1, 0, fy), At(fx + 1, 0, fy + 1), At(fx, 0, fy + 1), floorLayer,
		    info.glows ? PackColor(1, 1, 1, 0.5F) : 0x00FFFFFF);
	}
	if (stairAt[x][y] != 0)
		BuildStairwell(stairwells[stairAt[x][y] - 1]);
	if (stairTile[x][y])
		return;
	if (leveltype == DTYPE_TOWN) {
		if (townMask[x][y] != 0)
			return;
	} else {
		// Thin cut-out slabs: door frames, grates, the open sides of arches. Where the tile beyond
		// is a wall, that wall's own face is already there.
		const std::array<int16_t, 2> &arch = SpecialLayers(dSpecial[x][y] - 1);
		// A marked opening (door frame, grate, arch) in a wall that runs on to both sides of it.
		const bool marked = info.nwLayer >= 0 || info.neLayer >= 0 || arch[0] >= 0 || arch[1] >= 0 || door[x][y];
		const bool alongX = IsWall(x, y - 1) && IsWall(x, y + 1) && !IsWall(x - 1, y) && !IsWall(x + 1, y);
		const bool alongY = IsWall(x - 1, y) && IsWall(x + 1, y) && !IsWall(x, y - 1) && !IsWall(x, y + 1);
		if (Doorways && marked && (alongX || alongY)) {
			AddDoorway(x, y, alongX);
			// What fills the opening (a door's frame, a grate's bars) stands in the middle of the wall's thickness.
			// A door's leaf is the door object's own picture (actors.cpp).
			if (alongX && info.nwLayer >= 0 && !door[x][y])
				AddWallFace(fx + 0.5F, fy + 1, fx + 0.5F, fy, info.nwLayer);
			if (alongY && info.neLayer >= 0 && !door[x][y])
				AddWallFace(fx, fy + 0.5F, fx + 1, fy + 0.5F, info.neLayer);
			return;
		}
		if (info.nwLayer >= 0 && !IsWall(x - 1, y))
			AddWallFace(fx, fy + 1, fx, fy, info.nwLayer);
		if (info.neLayer >= 0 && !IsWall(x, y - 1))
			AddWallFace(fx, fy, fx + 1, fy, info.neLayer);
		if (arch[0] >= 0 && !IsWall(x - 1, y))
			AddWallFace(fx - 0.002F, fy + 1, fx - 0.002F, fy, arch[0]);
		if (arch[1] >= 0 && !IsWall(x, y - 1))
			AddWallFace(fx, fy - 0.002F, fx + 1, fy - 0.002F, arch[1]);
		// Over a door frame or an arch the wall goes on like everywhere else.
		if ((info.nwLayer >= 0 || arch[0] >= 0) && !IsWall(x - 1, y))
			AddWallAbove(fx, fy + 1, fx, fy, genericWallLayer);
		if ((info.neLayer >= 0 || arch[1] >= 0) && !IsWall(x, y - 1))
			AddWallAbove(fx, fy, fx + 1, fy, genericWallLayer);
	}
	if (info.cardLayer >= 0 && !carvedTile[x][y]) {
		// Two crossed cut-outs: one as painted (facing the original camera), one across it.
		const float cardHeight = (static_cast<float>(columnHeight) - TILE_HEIGHT / 2) / PixelsPerTileV * TileSize;
		const float half = TILE_WIDTH / PixelsPerTileH / 2 * 0.7071F;
		const float cx = fx + 0.5F;
		const float cy = fy + 0.5F;
		AddWallFace(cx - half, cy + half, cx + half, cy - half, info.cardLayer, cardHeight);
		AddWallFace(cx - half, cy - half, cx + half, cy + half, info.cardLayer, cardHeight);
	}
}

uint32_t HashChunk(int cx, int cy)
{
	uint32_t hash = 2166136261U;
	for (int y = cy * ChunkTiles; y < (cy + 1) * ChunkTiles; y++) {
		for (int x = cx * ChunkTiles; x < (cx + 1) * ChunkTiles; x++) {
			hash = (hash ^ dPiece[x][y]) * 16777619U;
			hash = (hash ^ static_cast<uint8_t>(dSpecial[x][y])) * 16777619U;
		}
	}
	return hash;
}

void BuildChunk(int cx, int cy)
{
	builder.Clear();
	for (int y = cy * ChunkTiles; y < (cy + 1) * ChunkTiles; y++) {
		for (int x = cx * ChunkTiles; x < (cx + 1) * ChunkTiles; x++)
			AddTile(x, y);
	}
	chunks[cx][cy] = builder.Build();
	chunkBuilt[cx][cy] = true;
}

/** True when no light-blocking tile lies on the straight line between two points (tile units). */
bool LineOfSight(float x0, float y0, float x1, float y1)
{
	const float length = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
	const int steps = static_cast<int>(length * 4) + 1;
	const int startX = static_cast<int>(x0);
	const int startY = static_cast<int>(y0);
	for (int i = 1; i < steps; i++) {
		const float t = static_cast<float>(i) / static_cast<float>(steps);
		const int x = static_cast<int>(x0 + (x1 - x0) * t);
		const int y = static_cast<int>(y0 + (y1 - y0) * t);
		if ((x != startX || y != startY) && InLevel(x, y) && blocks[x][y])
			return false;
	}
	return true;
}

/**
 * Bakes the light that does not move into the lightmap: the game's own pre-lit areas, objects
 * that burn, glowing ground, and the darkening of corners. Also refreshes the occupancy map.
 */
void BakeLight()
{
	packed.assign(static_cast<size_t>(OccupancySize) * OccupancySize, 0);
	for (int y = 0; y < OccupancySize; y++) {
		for (int x = 0; x < OccupancySize; x++)
			packed[static_cast<size_t>(y) * OccupancySize + x] = blocks[x / OccupancyScale][y / OccupancyScale] ? 255 : 0;
	}
	UploadTexture(occupancyTexture, 0, 0, 0, OccupancySize, OccupancySize, packed.data(), 1);

	constexpr size_t Texels = static_cast<size_t>(LightmapSize) * LightmapSize;
	bake.assign(Texels * 4, 0);
	const auto splat = [&](float cx, float cy, float radius, const float color[3], bool shadowed) {
		const int x0 = std::max(static_cast<int>((cx - radius) * LightmapScale), 0);
		const int x1 = std::min(static_cast<int>((cx + radius) * LightmapScale), LightmapSize - 1);
		const int y0 = std::max(static_cast<int>((cy - radius) * LightmapScale), 0);
		const int y1 = std::min(static_cast<int>((cy + radius) * LightmapScale), LightmapSize - 1);
		for (int y = y0; y <= y1; y++) {
			for (int x = x0; x <= x1; x++) {
				const float tx = (static_cast<float>(x) + 0.5F) / LightmapScale;
				const float ty = (static_cast<float>(y) + 0.5F) / LightmapScale;
				const float distance = std::sqrt((tx - cx) * (tx - cx) + (ty - cy) * (ty - cy));
				if (distance >= radius || blocks[x / LightmapScale][y / LightmapScale])
					continue;
				if (shadowed && !LineOfSight(cx, cy, tx, ty))
					continue;
				const float falloff = (1 - distance / radius) * (1 - distance / radius);
				float *texel = &bake[(static_cast<size_t>(y) * LightmapSize + x) * 4];
				for (int c = 0; c < 3; c++)
					texel[c] += color[c] * falloff;
			}
		}
	};

	int emitters = 0;
	if (leveltype != DTYPE_TOWN) {
		// Objects the game draws at full brightness are the ones that burn: torches, braziers, candles.
		for (int i = 0; i < ActiveObjectCount; i++) {
			const Object &object = Objects[ActiveObjects[i]];
			if (object.applyLighting || !object._oAnimData || object._oAnimFrame < 1)
				continue;
			const SpriteSlot *slot = CacheSprite(object.currentSprite());
			float color[3] = { 1.0F, 0.6F, 0.3F };
			if (slot != nullptr) {
				const float peak = std::max(std::max(slot->color[0], slot->color[1]), std::max(slot->color[2], 0.01F));
				for (int c = 0; c < 3; c++)
					color[c] = slot->color[c] / peak * 0.9F;
			}
			splat(static_cast<float>(object.position.x) + 0.5F, static_cast<float>(object.position.y) + 0.5F, 6.5F, color, true);
			emitters++;
		}
		for (int y = 0; y < MAXDUNY; y++) {
			for (int x = 0; x < MAXDUNX; x++) {
				const uint16_t piece = dPiece[x][y];
				if (piece < MAXTILES && pieces[piece].known && pieces[piece].glows) {
					const float color[3] = { pieces[piece].color[0] * 0.5F, pieces[piece].color[1] * 0.5F, pieces[piece].color[2] * 0.5F };
					splat(static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F, 2.2F, color, false);
				}
			}
		}
	}

	for (int y = 0; y < LightmapSize; y++) {
		for (int x = 0; x < LightmapSize; x++) {
			const int tileX = x / LightmapScale;
			const int tileY = y / LightmapScale;
			float *texel = &bake[(static_cast<size_t>(y) * LightmapSize + x) * 4];
			if (leveltype != DTYPE_TOWN) {
				// Areas the level itself lights (quest rooms), as the game's light level 0 (bright) to 15 (dark).
				const float preLit = 1 - static_cast<float>(std::min<uint8_t>(dPreLight[tileX][tileY], LightsMax)) / LightsMax;
				texel[0] += preLit * preLit * 0.30F;
				texel[1] += preLit * preLit * 0.26F;
				texel[2] += preLit * preLit * 0.20F;
			}
			// Corners gather less light: darken by the distance to the nearest wall.
			const float tx = (static_cast<float>(x) + 0.5F) / LightmapScale;
			const float ty = (static_cast<float>(y) + 0.5F) / LightmapScale;
			float nearest = 1;
			for (int dy = -1; dy <= 1; dy++) {
				for (int dx = -1; dx <= 1; dx++) {
					if (!IsWall(tileX + dx, tileY + dy))
						continue;
					const float px = std::clamp(tx, static_cast<float>(tileX + dx), static_cast<float>(tileX + dx + 1));
					const float py = std::clamp(ty, static_cast<float>(tileY + dy), static_cast<float>(tileY + dy + 1));
					nearest = std::min(nearest, std::sqrt((tx - px) * (tx - px) + (ty - py) * (ty - py)));
				}
			}
			texel[3] = 0.5F + 0.5F * std::min(nearest / 0.7F, 1.F);
		}
	}

	// Texels inside walls take the light of the open texels next to them, so that filtering at
	// the foot of a wall does not mix in black.
	for (int y = 1; y < LightmapSize - 1; y++) {
		for (int x = 1; x < LightmapSize - 1; x++) {
			if (!blocks[x / LightmapScale][y / LightmapScale])
				continue;
			float sum[4] = {};
			int open = 0;
			for (int dy = -1; dy <= 1; dy++) {
				for (int dx = -1; dx <= 1; dx++) {
					if (blocks[(x + dx) / LightmapScale][(y + dy) / LightmapScale])
						continue;
					const float *neighbour = &bake[(static_cast<size_t>(y + dy) * LightmapSize + x + dx) * 4];
					for (int c = 0; c < 4; c++)
						sum[c] += neighbour[c];
					open++;
				}
			}
			if (open == 0)
				continue;
			float *texel = &bake[(static_cast<size_t>(y) * LightmapSize + x) * 4];
			for (int c = 0; c < 4; c++)
				texel[c] = sum[c] / static_cast<float>(open);
		}
	}

	packed.resize(Texels * 4);
	for (size_t i = 0; i < Texels; i++) {
		for (int c = 0; c < 3; c++)
			packed[i * 4 + c] = static_cast<uint8_t>(std::min(bake[i * 4 + c] / 2, 1.F) * 255);
		packed[i * 4 + 3] = static_cast<uint8_t>(bake[i * 4 + 3] * 255);
	}
	UploadTexture(lightmapTexture, 0, 0, 0, LightmapSize, LightmapSize, packed.data(), 4);
	LogVerbose("VR: lightmap baked, {} burning objects", emitters);
}

// ---------------------------------------------------------------- town

/** Painted fraction of a rectangle of a column image. */
float PatchCoverage(const Image &image, int left, int top, int width, int height)
{
	int opaque = 0;
	for (int y = top; y < top + height; y++) {
		for (int x = left; x < left + width; x++)
			opaque += Opaque(image, x, y) ? 1 : 0;
	}
	return static_cast<float>(opaque) / static_cast<float>(width * height);
}

/**
 * A town building as real geometry. The paintings cannot be turned into 3D, so a building is
 * rebuilt from what the game does know, its solid footprint: straight walls around the
 * footprint and a gabled roof over it, surfaced with swatches cut from the building's own
 * painting (low on its front for the walls, near its top for the roof).
 */
void AddBuilding(const std::vector<Point> &footprint)
{
	constexpr int PatchWidth = 32;
	constexpr int WallPatchHeight = 48;
	constexpr int RoofPatchHeight = 32;
	const int h = columnHeight;
	struct Swatch {
		uint16_t piece;
		int top;
		float score = -1;
	};
	Swatch wallSwatch;
	Swatch roofSwatch;
	int minX = MAXDUNX;
	int minY = MAXDUNY;
	int maxX = 0;
	int maxY = 0;
	for (const Point tile : footprint) {
		minX = std::min(minX, tile.x);
		minY = std::min(minY, tile.y);
		maxX = std::max(maxX, tile.x);
		maxY = std::max(maxY, tile.y);
		const uint16_t piece = dPiece[tile.x][tile.y];
		const Image image = RenderColumn(piece);
		// Walls are painted just above the diamond of the front (southern) tiles.
		const int wallTop = h - TILE_HEIGHT - WallPatchHeight - 8;
		// The best painted patch wins; among equals, the one nearest the front.
		const float wallCoverage = PatchCoverage(image, 16, wallTop, PatchWidth, WallPatchHeight);
		const float wallScore = wallCoverage + static_cast<float>(tile.x + tile.y) * 0.0005F;
		if (wallCoverage > 0.6F && wallScore > wallSwatch.score)
			wallSwatch = { piece, wallTop, wallScore };
		// The roof is what is painted highest up.
		for (int top = 0; top + RoofPatchHeight < h - 3 * TILE_HEIGHT; top += 8) {
			if (PatchCoverage(image, 16, top, PatchWidth, RoofPatchHeight) > 0.8F) {
				if (static_cast<float>(h - top) > roofSwatch.score)
					roofSwatch = { piece, top, static_cast<float>(h - top) };
				break;
			}
		}
	}
	const auto extract = [&](const Swatch &swatch, int height) -> int16_t {
		if (swatch.score < 0)
			return -1;
		const Image image = RenderColumn(swatch.piece);
		return ExtractLayer(image, true, [&](float s, float t, float &px, float &py) {
			px = 16 + s * PatchWidth;
			py = static_cast<float>(swatch.top) + t * static_cast<float>(height);
		});
	};
	int16_t wallLayer = extract(wallSwatch, WallPatchHeight);
	int16_t roofLayer = extract(roofSwatch, RoofPatchHeight);
	// A building whose painting offers only one usable swatch wears it everywhere.
	if (wallLayer < 0)
		wallLayer = roofLayer;
	if (roofLayer < 0)
		roofLayer = wallLayer;

	constexpr float WallHeight = 3.0F;
	constexpr float RoofHeight = 2.4F;
	constexpr float Overhang = 0.25F; // Tiles.
	const auto inFootprint = [&](int x, int y) { return InLevel(x, y) && townMask[x][y] == 1; };
	for (const Point tile : footprint) {
		const auto x0 = static_cast<float>(tile.x);
		const auto z0 = static_cast<float>(tile.y);
		if (!inFootprint(tile.x, tile.y + 1))
			AddWallFace(x0, z0 + 1, x0 + 1, z0 + 1, wallLayer, WallHeight);
		if (!inFootprint(tile.x + 1, tile.y))
			AddWallFace(x0 + 1, z0 + 1, x0 + 1, z0, wallLayer, WallHeight);
		if (!inFootprint(tile.x - 1, tile.y))
			AddWallFace(x0, z0, x0, z0 + 1, wallLayer, WallHeight);
		if (!inFootprint(tile.x, tile.y - 1))
			AddWallFace(x0 + 1, z0, x0, z0, wallLayer, WallHeight);
	}

	// Roof: a gable over the footprint's bounding box, ridge along the longer side.
	const float left = static_cast<float>(minX) - Overhang;
	const float right = static_cast<float>(maxX + 1) + Overhang;
	const float back = static_cast<float>(minY) - Overhang;
	const float front = static_cast<float>(maxY + 1) + Overhang;
	const float ridge = WallHeight + RoofHeight;
	const bool alongX = right - left >= front - back;
	const float alongStart = alongX ? left : back;
	const float alongEnd = alongX ? right : front;
	const float acrossStart = alongX ? back : left;
	const float acrossEnd = alongX ? front : right;
	const float acrossMiddle = (acrossStart + acrossEnd) / 2;
	const auto point = [&](float along, float across, float y) { return alongX ? At(along, y, across) : At(across, y, along); };
	const int segments = std::max(1, static_cast<int>(std::ceil(alongEnd - alongStart)));
	for (int i = 0; i < segments; i++) {
		const float a0 = alongStart + (alongEnd - alongStart) * static_cast<float>(i) / static_cast<float>(segments);
		const float a1 = alongStart + (alongEnd - alongStart) * static_cast<float>(i + 1) / static_cast<float>(segments);
		for (const float eave : { acrossStart, acrossEnd })
			AddQuad(point(a0, eave, WallHeight - 0.25F), point(a1, eave, WallHeight - 0.25F), point(a1, acrossMiddle, ridge), point(a0, acrossMiddle, ridge), roofLayer,
			    roofLayer >= 0 ? 0x00FFFFFF : PackColor(0.32F, 0.26F, 0.16F));
	}
	// Gable ends, wall-coloured triangles under the roof.
	for (const float end : { alongStart + Overhang, alongEnd - Overhang }) {
		const Vec3 corners[3] = { point(end, acrossStart + Overhang, WallHeight), point(end, acrossEnd - Overhang, WallHeight), point(end, acrossMiddle, ridge - 0.3F) };
		const float uv[3][2] = { { 0, 1 }, { 1, 1 }, { 0.5F, 0 } };
		builder.Triangle(corners, uv, wallLayer);
	}
}

/** Which palette entries are ground: the ones the tiles with nothing on them are painted with. */
void FindGroundIndices()
{
	uint32_t counts[256] = {};
	uint32_t total = 0;
	static bool seen[MAXTILES];
	std::memset(seen, 0, sizeof(seen));
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			const uint16_t piece = dPiece[x][y];
			if (piece >= MAXTILES || seen[piece])
				continue;
			seen[piece] = true;
			const uint8_t faces = Classify(piece).faces;
			if ((faces & FaceFloor) == 0 || (faces & (FaceTall | FaceCard)) != 0)
				continue;
			const Image image = RenderColumn(piece);
			for (int py = image.height - TILE_HEIGHT; py < image.height; py++) {
				for (int px = 0; px < image.width; px++) {
					if (!Opaque(image, px, py))
						continue;
					counts[image.pixels[(static_cast<size_t>(py) * image.width + px) * 2]]++;
					total++;
				}
			}
		}
	}
	for (int i = 0; i < 256; i++)
		groundIndex[i] = counts[i] * 400 > total;
}

/** Tile columns put back together into the one picture they were cut from. */
struct Painting {
	std::vector<uint8_t> canvas;
	int width;
	int height;
	int minDiagonal;
	int maxDiagonal;
	int maxSum;
};

/**
 * Lays the columns of a group of tiles (plus the tree crowns and roof edges of the special sprite
 * list) out exactly as the 2D renderer does, without the ground they stand on. Sorts `tiles`.
 */
bool ComposeGroup(std::vector<Point> &tiles, Painting &painting)
{
	if (tiles.empty())
		return false;
	// Far columns first, so nearer ones paint over them.
	std::sort(tiles.begin(), tiles.end(), [](Point a, Point b) { return a.x + a.y < b.x + b.y; });
	painting.minDiagonal = 1 << 20;
	painting.maxDiagonal = -(1 << 20);
	for (const Point tile : tiles) {
		painting.minDiagonal = std::min(painting.minDiagonal, tile.x - tile.y);
		painting.maxDiagonal = std::max(painting.maxDiagonal, tile.x - tile.y);
	}
	painting.maxSum = tiles.back().x + tiles.back().y;
	const int minSum = tiles.front().x + tiles.front().y;
	const int width = (painting.maxDiagonal - painting.minDiagonal) * 32 + TILE_WIDTH;
	const int height = (painting.maxSum - minSum) * 16 + columnHeight + 96; // Tree crowns stand taller than a column.
	if (width > 2048 || height > 2048)
		return false;
	painting.width = width;
	painting.height = height;
	painting.canvas.assign(static_cast<size_t>(width) * height * 2, 0);
	const auto paint = [&](const Image &image, int left, int bottom, bool column) {
		for (int y = 0; y < image.height; y++) {
			const int cy = bottom - image.height + y;
			if (cy < 0 || cy >= height)
				continue;
			// The floor diamond at the foot of a column: what is painted in ground colours there is the ground.
			const int row = y - (image.height - TILE_HEIGHT);
			const int halfWidth = !column || row < 0 ? 0 : (row < 16 ? 2 * (row + 1) : 2 * (TILE_HEIGHT - row));
			for (int x = 0; x < image.width && left + x < width; x++) {
				if (!Opaque(image, x, y))
					continue;
				const uint8_t *pixel = &image.pixels[(static_cast<size_t>(y) * image.width + x) * 2];
				if (x >= 32 - halfWidth && x < 32 + halfWidth && groundIndex[pixel[0]])
					continue;
				std::memcpy(&painting.canvas[(static_cast<size_t>(cy) * width + left + x) * 2], pixel, 2);
			}
		}
	};
	for (const Point tile : tiles) {
		const int left = (tile.x - tile.y - painting.minDiagonal) * 32;
		const int bottom = height - (painting.maxSum - (tile.x + tile.y)) * 16;
		// Under a building there is no ground to take away: what its tiles show at their foot is the building.
		if ((Classify(dPiece[tile.x][tile.y]).faces & FaceTall) != 0)
			paint(RenderColumn(dPiece[tile.x][tile.y]), left, bottom, townMask[tile.x][tile.y] != 1);
		if (dSpecial[tile.x][tile.y] > 0 && pSpecialCels) {
			const ClxSprite sprite = (*pSpecialCels)[dSpecial[tile.x][tile.y] - 1];
			if (const uint8_t *pixels = RasterizeSprite(sprite, nullptr); pixels != nullptr)
				paint({ pixels, sprite.width(), sprite.height() }, left, bottom, false);
		}
	}
	return true;
}

/** Pixels from a painting's lower edge up to the ground line through the middle of tile (x, y). */
float GroundLine(const Painting &painting, float x, float y)
{
	return (static_cast<float>(painting.maxSum) - (x + y)) * 16 + 16;
}

/**
 * DEVILUTIONX_VR_DUMP_TOWN=<dir>: writes every town painting there as `<key>.rgba` (RGBA bytes) and
 * a line `<key> <width> <height>` in `town.txt`. These are the pictures the 3D models are generated from.
 */
void DumpPainting(const std::string &key, const Painting &painting)
{
	static const std::string directory = SDL_getenv("DEVILUTIONX_VR_DUMP_TOWN") != nullptr ? SDL_getenv("DEVILUTIONX_VR_DUMP_TOWN") : "";
	if (directory.empty())
		return;
	std::vector<uint32_t> rgba(static_cast<size_t>(painting.width) * painting.height);
	for (size_t i = 0; i < rgba.size(); i++)
		rgba[i] = PaletteRgba(painting.canvas[i * 2], painting.canvas[i * 2 + 1]);
	if (SDL_RWops *file = SDL_RWFromFile((directory + "/" + key + ".rgba").c_str(), "wb"); file != nullptr) {
		SDL_RWwrite(file, rgba.data(), 4, rgba.size());
		SDL_RWclose(file);
	}
	if (SDL_RWops *file = SDL_RWFromFile((directory + "/town.txt").c_str(), "ab"); file != nullptr) {
		const std::string line = key + " " + std::to_string(painting.width) + " " + std::to_string(painting.height) + "\n";
		SDL_RWwrite(file, line.data(), 1, line.size());
		SDL_RWclose(file);
	}
}

/**
 * Finds Tristram's buildings: connected areas of solid tiles that are at least 3 tiles in
 * both directions and carry tall art. Fences, trees and rocks are solid too but fail one of
 * those tests; they become cut-outs. The town never changes, so this runs once per visit.
 */
void BuildTown()
{
	std::memset(townMask, 0, sizeof(townMask));
	FindGroundIndices();
	{
		// The grass: the floor of the plain piece (ground and nothing else) that most tiles use.
		static uint16_t uses[MAXTILES];
		std::memset(uses, 0, sizeof(uses));
		uint16_t most = 0;
		townGrassLayer = -1;
		for (int y = 0; y < MAXDUNY; y++) {
			for (int x = 0; x < MAXDUNX; x++) {
				const uint16_t piece = dPiece[x][y];
				if (piece >= MAXTILES || HasAnyOf(SOLData[piece], TileProperties::Solid))
					continue;
				const PieceInfo &info = Classify(piece);
				if (info.faces != FaceFloor || info.floorLayer < 0)
					continue;
				if (++uses[piece] > most) {
					most = uses[piece];
					townGrassLayer = info.floorLayer;
				}
			}
		}
	}
	static bool visited[MAXDUNX][MAXDUNY];
	std::memset(visited, 0, sizeof(visited));
	const auto isSolid = [](int x, int y) {
		return InLevel(x, y) && dPiece[x][y] < MAXTILES && HasAnyOf(SOLData[dPiece[x][y]], TileProperties::Solid);
	};
	std::vector<std::vector<Point>> buildings;
	std::vector<Point> pending;
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			if (visited[x][y] || !isSolid(x, y))
				continue;
			std::vector<Point> region;
			pending.assign(1, Point { x, y });
			visited[x][y] = true;
			int minX = x;
			int maxX = x;
			int minY = y;
			int maxY = y;
			int tall = 0;
			while (!pending.empty()) {
				const Point current = pending.back();
				pending.pop_back();
				region.push_back(current);
				minX = std::min(minX, current.x);
				maxX = std::max(maxX, current.x);
				minY = std::min(minY, current.y);
				maxY = std::max(maxY, current.y);
				if ((Classify(dPiece[current.x][current.y]).faces & FaceTall) != 0)
					tall++;
				for (const Point next : { Point { current.x + 1, current.y }, Point { current.x - 1, current.y }, Point { current.x, current.y + 1 }, Point { current.x, current.y - 1 } }) {
					if (isSolid(next.x, next.y) && !visited[next.x][next.y]) {
						visited[next.x][next.y] = true;
						pending.push_back(next);
					}
				}
			}
			// The map's solid border is one huge region; a building is compact.
			const int width = maxX - minX + 1;
			const int depth = maxY - minY + 1;
			if (region.size() < 12 || width < 3 || depth < 3 || width > 20 || depth > 20 || tall < 4)
				continue;
			for (const Point tile : region)
				townMask[tile.x][tile.y] = 1;
			for (int my = std::max(minY - 2, 0); my <= std::min(maxY + 1, MAXDUNY - 1); my++) {
				for (int mx = std::max(minX - 2, 0); mx <= std::min(maxX + 1, MAXDUNX - 1); mx++) {
					if (townMask[mx][my] == 0)
						townMask[mx][my] = 2;
				}
			}
			buildings.push_back(std::move(region));
		}
	}
	// The cathedral is joined to the churchyard's fences, so the search above takes it for part of
	// the map's border. Tristram is always the same map: it is the block behind the way down.
	// fixed rectangle off the stair trigger; find it by flood fill inside the fences if the town map ever varies.
	for (int i = 0; i < numtrigs; i++) {
		if (trigs[i]._tmsg != WM_DIABNEXTLVL)
			continue;
		const Point stairs = trigs[i].position;
		const Point low { std::max(stairs.x - 9, 0), std::max(stairs.y - 17, 0) };
		const Point high { stairs.x + 7, stairs.y + 1 };
		// All of it is one model (houses/b_church): its solid tiles and every tile
		// that carries a piece of its painting, the vestibule and the apse at the front included.
		std::vector<Point> church;
		for (int y = low.y; y <= high.y; y++) {
			for (int x = low.x; x <= high.x; x++) {
				if (!InLevel(x, y) || dPiece[x][y] >= MAXTILES)
					continue;
				if (isSolid(x, y) || (Classify(dPiece[x][y]).faces & (FaceTall | FaceCard)) != 0 || dSpecial[x][y] > 0) {
					townMask[x][y] = 1;
					church.push_back({ x, y });
				}
			}
		}
		if (const GeneratedModel *model = ModelsGenerated("houses/b_church"); model != nullptr)
			builtPlacements.push_back({ model, At(static_cast<float>(low.x) + 0.5F, 0, static_cast<float>(low.y) + 0.5F) });
		else if (CarvedExists("houses/b_church"))
			carvedPlacements.push_back({ "houses/b_church", At(static_cast<float>(low.x) + 0.5F, 0, static_cast<float>(low.y) + 0.5F) });
		else if (!church.empty())
			buildings.push_back(std::move(church));
		break;
	}
	// A building is its Blender model, else the older model painted with its painting, else
	// boxes over its footprint.
	builder.Clear();
	for (const std::vector<Point> &building : buildings) {
		Point low = building[0];
		for (const Point tile : building)
			low = { std::min(low.x, tile.x), std::min(low.y, tile.y) };
		const std::string key = "houses/b_" + std::to_string(low.x) + "_" + std::to_string(low.y);
		if (const GeneratedModel *model = ModelsGenerated(key); model != nullptr)
			builtPlacements.push_back({ model, At(static_cast<float>(low.x) + 0.5F, 0, static_cast<float>(low.y) + 0.5F) });
		else if (CarvedExists(key))
			carvedPlacements.push_back({ key, At(static_cast<float>(low.x) + 0.5F, 0, static_cast<float>(low.y) + 0.5F) });
		else
			AddBuilding(building);
	}
	townMesh = builder.Build();
	townBuilt = true;

	// Trees, wells, fences: every group of touching tall tiles (plus tree crowns from the
	// special sprite list) is one painting. Its columns are put back together into one image,
	// laid out exactly as the 2D renderer lays them out on screen.
	standees.clear();
	std::memset(visited, 0, sizeof(visited));
	const auto isMember = [&](int x, int y) {
		if (!InLevel(x, y) || dPiece[x][y] >= MAXTILES || townMask[x][y] != 0)
			return false;
		return (Classify(dPiece[x][y]).faces & FaceTall) != 0 || dSpecial[x][y] > 0;
	};
	std::vector<Point> tiles;
	Painting painting;
	for (int gy = 0; gy < MAXDUNY; gy++) {
		for (int gx = 0; gx < MAXDUNX; gx++) {
			if (visited[gx][gy] || !isMember(gx, gy))
				continue;
			tiles.clear();
			pending.assign(1, Point { gx, gy });
			visited[gx][gy] = true;
			float centerX = 0;
			float centerY = 0;
			while (!pending.empty()) {
				const Point current = pending.back();
				pending.pop_back();
				tiles.push_back(current);
				centerX += static_cast<float>(current.x);
				centerY += static_cast<float>(current.y);
				for (int dy = -1; dy <= 1; dy++) {
					for (int dx = -1; dx <= 1; dx++) {
						const Point next { current.x + dx, current.y + dy };
						if (isMember(next.x, next.y) && !visited[next.x][next.y]) {
							visited[next.x][next.y] = true;
							pending.push_back(next);
						}
					}
				}
			}
			centerX /= static_cast<float>(tiles.size());
			centerY /= static_cast<float>(tiles.size());
			// A tree, the well, a cathedral wall: the model carved out of its painting, standing on its
			// first tile, where it passed its gates; the upright cut-out where it did not.
			const std::string key = "s_" + std::to_string(gx) + "_" + std::to_string(gy);
			if (CarvedExists("town/" + key)) {
				carvedPlacements.push_back({ "town/" + key, At(static_cast<float>(gx) + 0.5F, 0, static_cast<float>(gy) + 0.5F) });
				continue;
			}
			if (!ComposeGroup(tiles, painting))
				continue;
			Standee standee;
			standee.key = key;
			DumpPainting(standee.key, painting);
			const SpriteSlot *slot = CacheImage(painting.canvas.data(), painting.width, painting.height);
			if (slot == nullptr)
				continue;
			standee.model = nullptr;
			standee.slot = *slot;
			standee.position = At(centerX + 0.5F, 0, centerY + 0.5F);
			// The canvas spans the group's diagonals; its middle need not be the group's centre.
			const float middle = (static_cast<float>(painting.minDiagonal + painting.maxDiagonal) / 2 - (centerX - centerY)) * 32;
			// Only the painted part of the canvas is kept.
			const float painted = slot->left + slot->artWidth / 2 - static_cast<float>(painting.width) / 2;
			standee.shift = (middle + painted) / PixelsPerTileH * TileSize;
			standee.halfWidth = slot->artWidth / PixelsPerTileH * TileSize / 2;
			standee.height = slot->artHeight / PixelsPerTileV * TileSize;
			standee.lift = (static_cast<float>(painting.height) - slot->top - slot->artHeight) / PixelsPerTileV * TileSize;
			standee.top = (static_cast<float>(painting.height) - slot->top - GroundLine(painting, centerX, centerY)) / PixelsPerTileV * TileSize;
			standees.push_back(standee);
		}
	}
	// Low things standing on the ground (rocks, low walls, fences, graves, the well): every group of
	// touching tiles with a low standing piece, carved where it passed its gates, the pieces'
	// cut-outs otherwise. Found in the same order as the asset files were made, which name
	// a group by its first tile.
	std::memset(visited, 0, sizeof(visited));
	const auto isLow = [&](int x, int y) {
		if (!InLevel(x, y) || dPiece[x][y] >= MAXTILES || townMask[x][y] != 0)
			return false;
		return (Classify(dPiece[x][y]).faces & FaceCard) != 0;
	};
	int lowCarved = 0;
	for (int gy = 0; gy < MAXDUNY; gy++) {
		for (int gx = 0; gx < MAXDUNX; gx++) {
			if (visited[gx][gy] || !isLow(gx, gy))
				continue;
			tiles.clear();
			pending.assign(1, Point { gx, gy });
			visited[gx][gy] = true;
			while (!pending.empty()) {
				const Point current = pending.back();
				pending.pop_back();
				tiles.push_back(current);
				for (int dy = -1; dy <= 1; dy++) {
					for (int dx = -1; dx <= 1; dx++) {
						const Point next { current.x + dx, current.y + dy };
						if (isLow(next.x, next.y) && !visited[next.x][next.y]) {
							visited[next.x][next.y] = true;
							pending.push_back(next);
						}
					}
				}
			}
			const std::string key = "town/c_" + std::to_string(gx) + "_" + std::to_string(gy);
			if (!CarvedExists(key))
				continue;
			carvedPlacements.push_back({ key, At(static_cast<float>(gx) + 0.5F, 0, static_cast<float>(gy) + 0.5F) });
			for (const Point tile : tiles)
				carvedTile[tile.x][tile.y] = true;
			lowCarved++;
		}
	}
	LogVerbose("VR: town has {} buildings, {} cut-outs, {} carved low groups, {} carved models in all", buildings.size(), standees.size(), lowCarved, carvedPlacements.size());
}

// ---------------------------------------------------------------- materials

/**
 * Loads the tile set's material library (`vrassets/materials/<set>`): the textures, and which piece wears which.
 */
void LoadMaterials()
{
	materials.Release();
	materialNormals.Release();
	materialCount = 0;
	wallMaterial = -1;
	floorSize = 0;
	std::memset(pieceMaterial, -1, sizeof(pieceMaterial));
	const char *set = nullptr;
	switch (leveltype) {
	case DTYPE_TOWN:
		set = "town";
		break;
	case DTYPE_CATHEDRAL:
		set = "l1";
		break;
	case DTYPE_CATACOMBS:
		set = "l2";
		break;
	case DTYPE_CAVES:
		set = "l3";
		break;
	case DTYPE_HELL:
		set = "l4";
		break;
	default:
		break;
	}
	if (set == nullptr || AssetsDirectory().empty() || Knob("DEVILUTIONX_VR_MATERIALS", Classic() ? 0 : 1) == 0)
		return;
	const std::string base = AssetsDirectory() + "/materials/" + set + "/";
	const auto read = [&](const char *name, std::vector<uint8_t> &out) {
		SDL_RWops *file = SDL_RWFromFile((base + name).c_str(), "rb");
		if (file == nullptr)
			return false;
		out.resize(static_cast<size_t>(SDL_RWsize(file)));
		const bool ok = SDL_RWread(file, out.data(), 1, out.size()) == out.size();
		SDL_RWclose(file);
		return ok;
	};
	std::vector<uint8_t> manifest;
	std::vector<uint8_t> albedo;
	std::vector<uint8_t> relief;
	int count = 0;
	int size = 0;
	if (!read("manifest.txt", manifest))
		return;
	manifest.push_back(0);
	const char *text = reinterpret_cast<const char *>(manifest.data());
	if (SDL_sscanf(text, "materials %d %d", &count, &size) != 2 || count < 1 || count > MaxMaterials || size < 16)
		return;
	const size_t layerBytes = static_cast<size_t>(size) * size * 4;
	if (!read("materials.rgba", albedo) || !read("normals.rgba", relief) || albedo.size() != layerBytes * count || relief.size() != albedo.size())
		return;
	materials = CreateTexture(size, size, count, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, TextureMips);
	materialNormals = CreateTexture(size, size, count, DXGI_FORMAT_R8G8B8A8_UNORM, TextureMips);
	if (!materials || !materialNormals)
		return;
	for (int i = 0; i < count; i++) {
		UploadTexture(materials, i, 0, 0, size, size, &albedo[layerBytes * i], 4);
		UploadTexture(materialNormals, i, 0, 0, size, size, &relief[layerBytes * i], 4);
	}
	context->GenerateMips(materials.srv.Get());
	context->GenerateMips(materialNormals.srv.Get());
	int faces = 0;
	for (const char *line = text; line != nullptr && *line != 0;) {
		int index;
		int material;
		int r;
		int g;
		int b;
		char kind[16];
		// SDL_sscanf loses track of its arguments when a number is missing, so only lines of the right kind are handed to it.
		const bool isMaterial = line[0] == 'm' && line[1] == ' ';
		// The floor materials are quilted at the art's own scale: this many art pixels repeat.
		if (int artPixels = 0; std::strncmp(line, "floorsize ", 10) == 0 && SDL_sscanf(line, "floorsize %d", &artPixels) == 1 && artPixels > 0)
			floorSize = static_cast<float>(artPixels) / PixelsPerTileH * TileSize;
		const bool isPiece = line[0] == 'p' && line[1] == ' ';
		if (isMaterial && SDL_sscanf(line, "m %d %15s %d %d %d", &index, kind, &r, &g, &b) == 5 && index >= 0 && index < count) {
			materialColor[index][0] = static_cast<float>(r) / 255;
			materialColor[index][1] = static_cast<float>(g) / 255;
			materialColor[index][2] = static_cast<float>(b) / 255;
		} else if (isPiece && SDL_sscanf(line, "p %d %15s %d", &index, kind, &material) == 3 && index >= 0 && index < MAXTILES && material >= 0 && material < count) {
			pieceMaterial[index][kind[0] == 'f' ? 0 : (kind[1] == 'w' ? 1 : 2)] = static_cast<int8_t>(material);
			faces++;
		}
		const char *next = std::strchr(line, 10);
		line = next != nullptr ? next + 1 : nullptr;
	}
	materialCount = count;
	// The art paints its walls piece by piece, some with half an arch or another stone on them. A
	// wall seen from inside the level is one masonry: the one most wall pieces have.
	int uses[MaxMaterials] = {};
	for (int piece = 0; piece < MAXTILES; piece++) {
		if (!HasAllOf(SOLData[piece], TileProperties::Solid | TileProperties::BlockLight))
			continue;
		for (const int face : { 1, 2 }) {
			const int material = pieceMaterial[piece][face];
			if (material >= 0 && ++uses[material] > (wallMaterial >= 0 ? uses[wallMaterial] : 0))
				wallMaterial = material;
		}
	}
	LogVerbose("VR: material library '{}': {} materials on {} piece faces", set, count, faces);
}

// ---------------------------------------------------------------- build and occlusion

/** The tile set a dungeon level is built from, as the asset folders name it; null for the others. */
const char *TileSetName()
{
	switch (leveltype) {
	case DTYPE_CATHEDRAL:
		return "l1";
	case DTYPE_CATACOMBS:
		return "l2";
	case DTYPE_CAVES:
		return "l3";
	case DTYPE_HELL:
		return "l4";
	default:
		return nullptr;
	}
}

/**
 * The tile set's standing props (pillars, stalagmites, tombs, fences) as the models carved out of
 * them (`vrassets/carve/tiles/<set>/til<n>`, one per megatile of the TIL file). A megatile is
 * found wherever its four pieces lie together on the map; its model stands on the corner the four
 * share, and the pieces' cut-outs are left out.
 */
void PlaceCarvedProps()
{
	const char *set = TileSetName();
	if (set == nullptr || !pMegaTiles)
		return;
	struct Entry {
		uint16_t pieces[4];
		std::string key;
	};
	std::vector<Entry> entries;
	// A file exists only for megatiles of this set's TIL, so its index is a valid one.
	for (int t = 0; t < 512; t++) {
		std::string key = std::string("tiles/") + set + "/til" + std::to_string(t + 1);
		if (!CarvedExists(key))
			continue;
		const MegaTile &mega = pMegaTiles[t];
		entries.push_back({ { Swap16LE(mega.micro1), Swap16LE(mega.micro2), Swap16LE(mega.micro3), Swap16LE(mega.micro4) }, std::move(key) });
	}
	if (entries.empty())
		return;
	for (int y = 0; y + 1 < MAXDUNY; y++) {
		for (int x = 0; x + 1 < MAXDUNX; x++) {
			for (const Entry &entry : entries) {
				if (dPiece[x][y] != entry.pieces[0] || dPiece[x + 1][y] != entry.pieces[1] || dPiece[x][y + 1] != entry.pieces[2] || dPiece[x + 1][y + 1] != entry.pieces[3])
					continue;
				carvedPlacements.push_back({ entry.key, At(static_cast<float>(x + 1), 0, static_cast<float>(y + 1)) });
				carvedTile[x][y] = carvedTile[x + 1][y] = carvedTile[x][y + 1] = carvedTile[x + 1][y + 1] = true;
				break;
			}
		}
	}
	LogVerbose("VR: {} carved props of {} kinds", carvedPlacements.size(), entries.size());
}

void Rebuild()
{
	builtCels = pDungeonCels.get();
	builtLevel = currlevel;
	builtSetLevel = setlevel;
	builtSetLevelNumber = static_cast<int>(setlvlnum);
	builtType = leveltype;
	columnHeight = MicroTileLen / 2 * TILE_HEIGHT;
	switch (leveltype) {
	case DTYPE_TOWN:
		ceilingHeight = 0;
		break;
	case DTYPE_CATACOMBS:
		ceilingHeight = 3.6F;
		break;
	case DTYPE_CAVES:
	case DTYPE_NEST:
		ceilingHeight = 4.0F;
		break;
	case DTYPE_HELL:
		ceilingHeight = 4.6F;
		break;
	default:
		ceilingHeight = 4.2F;
		break;
	}
	// Walls keep the scale of the art: as many pixels of it as the wall is high.
	wallPixels = std::clamp((ceilingHeight > 0 ? ceilingHeight : 4.2F) / TileSize * PixelsPerTileV, 32.F, static_cast<float>(columnHeight - TILE_HEIGHT));
	if (leveltype != DTYPE_TOWN) {
		// The art decides how high the walls really are: measure how far up the wall pieces are
		// painted and put the ceiling there, so no face has to be stretched or invented.
		std::vector<int> heights;
		static bool measured[MAXTILES];
		std::memset(measured, 0, sizeof(measured));
		for (int y = 0; y < MAXDUNY; y++) {
			for (int x = 0; x < MAXDUNX; x++) {
				const uint16_t piece = dPiece[x][y];
				if (piece >= MAXTILES || measured[piece] || !HasAllOf(SOLData[piece], TileProperties::Solid | TileProperties::BlockLight))
					continue;
				measured[piece] = true;
				const Image image = RenderColumn(piece);
				for (int row = 0; row < image.height - TILE_HEIGHT; row++) {
					int painted = 0;
					for (int column = 0; column < TILE_WIDTH; column++)
						painted += Opaque(image, column, row) ? 1 : 0;
					if (painted >= TILE_WIDTH / 4) {
						heights.push_back(image.height - TILE_HEIGHT / 2 - row);
						break;
					}
				}
			}
		}
		if (heights.size() >= 8) {
			std::sort(heights.begin(), heights.end());
			wallPixels = std::clamp(static_cast<float>(heights[heights.size() / 2]), 64.F, static_cast<float>(columnHeight - TILE_HEIGHT));
			ceilingHeight = wallPixels / PixelsPerTileV * TileSize;
		}
		LogVerbose("VR: walls are {:.0f} px of art, {:.2f} m high", wallPixels, ceilingHeight);
	}
	std::memset(pieces, 0, sizeof(pieces));
	specialLayers.clear();
	layerCount = 0;
	std::memset(layerIsWall, 0, sizeof(layerIsWall));
	std::memset(layerIsTop, 0, sizeof(layerIsTop));
	genericWallLayer = -1;
	plainWallLayer = -1;
	plainFloorKnown = false;
	std::memset(layerMaterial, -1, sizeof(layerMaterial));
	LoadMaterials();
	SpritesReset();
	SpritesLevelChanged();
	PaletteLevelChanged();
	ParticlesReset();
	for (auto &row : chunkBuilt) {
		for (bool &built : row)
			built = false;
	}
	townMesh = Mesh {};
	townBuilt = false;
	ModelsReset();
	standees.clear();
	std::memset(townMask, 0, sizeof(townMask));
	std::memset(carvedTile, 0, sizeof(carvedTile));
	carvedPlacements.clear();
	builtPlacements.clear();
	PlaceCarvedProps();
}

/**
 * The floor map (FloorMapPerTile texels per tile). Each texel: the two floor materials with most
 * weight among the tiles around it (a soft falloff from each tile's middle), how much the second
 * has, and the distance to the nearest wall in tiles (0 at the wall, 1 from 1.2 tiles out), from
 * which the shader darkens the floor along walls and into corners.
 */
void BuildFloorMap()
{
	if (floorSize <= 0 || !floorMapTexture)
		return;
	constexpr int Size = MAXDUNX * FloorMapPerTile;
	static std::vector<uint32_t> map;
	static std::vector<float> distance;
	map.assign(static_cast<size_t>(Size) * Size, 0);
	distance.assign(static_cast<size_t>(Size) * Size, 1e9F);
	const auto materialOf = [&](int x, int y) -> int {
		if (!InLevel(x, y) || wall[x][y] || dPiece[x][y] >= MAXTILES)
			return -1;
		return pieceMaterial[dPiece[x][y]][0];
	};
	for (int ty = 0; ty < MAXDUNY; ty++) {
		for (int tx = 0; tx < MAXDUNX; tx++) {
			for (int sy = 0; sy < FloorMapPerTile; sy++) {
				for (int sx = 0; sx < FloorMapPerTile; sx++) {
					const float px = static_cast<float>(tx) + (static_cast<float>(sx) + 0.5F) / FloorMapPerTile;
					const float py = static_cast<float>(ty) + (static_cast<float>(sy) + 0.5F) / FloorMapPerTile;
					float weight[MaxMaterials] = {};
					for (int dy = -1; dy <= 1; dy++) {
						for (int dx = -1; dx <= 1; dx++) {
							const int m = materialOf(tx + dx, ty + dy);
							if (m < 0)
								continue;
							const float ox = px - (static_cast<float>(tx + dx) + 0.5F);
							const float oy = py - (static_cast<float>(ty + dy) + 0.5F);
							weight[m] += std::exp(-(ox * ox + oy * oy) / (2 * 0.38F * 0.38F));
						}
					}
					int first = -1;
					int second = -1;
					for (int m = 0; m < materialCount; m++) {
						if (weight[m] <= 0)
							continue;
						if (first < 0 || weight[m] > weight[first]) {
							second = first;
							first = m;
						} else if (second < 0 || weight[m] > weight[second]) {
							second = m;
						}
					}
					const float share = first >= 0 && second >= 0 ? weight[second] / (weight[first] + weight[second]) : 0;
					const size_t at = static_cast<size_t>(ty * FloorMapPerTile + sy) * Size + tx * FloorMapPerTile + sx;
					map[at] = static_cast<uint32_t>(first + 1) | static_cast<uint32_t>(second + 1) << 8 | static_cast<uint32_t>(share * 255) << 16;
					if (wall[tx][ty])
						distance[at] = 0;
				}
			}
		}
	}
	// Chamfer distance to the walls, in texels (3-4 weights in thirds).
	const auto relax = [&](int x, int y, int dx, int dy, float cost) {
		const int nx = x + dx;
		const int ny = y + dy;
		if (nx < 0 || ny < 0 || nx >= Size || ny >= Size)
			return;
		float &here = distance[static_cast<size_t>(y) * Size + x];
		here = std::min(here, distance[static_cast<size_t>(ny) * Size + nx] + cost);
	};
	for (int y = 0; y < Size; y++) {
		for (int x = 0; x < Size; x++) {
			relax(x, y, -1, 0, 1);
			relax(x, y, 0, -1, 1);
			relax(x, y, -1, -1, 1.414F);
			relax(x, y, 1, -1, 1.414F);
		}
	}
	for (int y = Size - 1; y >= 0; y--) {
		for (int x = Size - 1; x >= 0; x--) {
			relax(x, y, 1, 0, 1);
			relax(x, y, 0, 1, 1);
			relax(x, y, 1, 1, 1.414F);
			relax(x, y, -1, 1, 1.414F);
		}
	}
	for (size_t i = 0; i < map.size(); i++) {
		const float tiles = distance[i] / FloorMapPerTile;
		map[i] |= static_cast<uint32_t>(std::clamp(tiles / 1.2F, 0.F, 1.F) * 255) << 24;
	}
	UploadTexture(floorMapTexture, 0, 0, 0, Size, Size, map.data(), 4);
}

/** Rebuilds what the game's level data no longer matches. Returns true when anything was rebuilt. */
bool Patch()
{
	bool dirty[ChunksPerSide][ChunksPerSide] = {};
	bool any = false;
	for (int cy = 0; cy < ChunksPerSide; cy++) {
		for (int cx = 0; cx < ChunksPerSide; cx++) {
			const uint32_t hash = HashChunk(cx, cy);
			if (chunkBuilt[cx][cy] && hash == chunkHash[cx][cy])
				continue;
			chunkHash[cx][cy] = hash;
			any = true;
			// Wall faces depend on the tiles across the chunk's border.
			for (int dy = -1; dy <= 1; dy++) {
				for (int dx = -1; dx <= 1; dx++) {
					if (cx + dx >= 0 && cy + dy >= 0 && cx + dx < ChunksPerSide && cy + dy < ChunksPerSide)
						dirty[cx + dx][cy + dy] = true;
				}
			}
		}
	}
	if (!any)
		return false;

	int walls = 0;
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			const uint16_t piece = dPiece[x][y];
			const TileProperties properties = piece < MAXTILES ? SOLData[piece] : TileProperties::None;
			const bool town = leveltype == DTYPE_TOWN;
			blocks[x][y] = !town && HasAnyOf(properties, TileProperties::BlockLight);
			const Object *object = town ? nullptr : FindObjectAtPosition({ x, y }, false);
			door[x][y] = object != nullptr && object->isDoor();
			wall[x][y] = blocks[x][y] && HasAnyOf(properties, TileProperties::Solid) && !door[x][y];
			walls += wall[x][y] ? 1 : 0;
		}
	}
	PlanStairwells();
	if (genericWallLayer < 0 && leveltype != DTYPE_TOWN) {
		// The wall face most often seen dresses the faces that have no art of their own. Only faces
		// towards open ground count: inside a block of wall the art is the block's top seen from
		// above (hell's red flesh), never a side.
		std::vector<int> uses(MaxLayers, 0);
		std::vector<int> hidden(MaxLayers, 0);
		int best = 0;
		const auto open = [](int x, int y) { return InLevel(x, y) && !wall[x][y]; };
		for (int y = 0; y < MAXDUNY; y++) {
			for (int x = 0; x < MAXDUNX; x++) {
				if (!wall[x][y])
					continue;
				const PieceInfo &info = Classify(dPiece[x][y]);
				const bool seen[2] = { open(x - 1, y) || open(x + 1, y), open(x, y - 1) || open(x, y + 1) };
				const int16_t faces[2] = { info.nwLayer, info.neLayer };
				for (int side = 0; side < 2; side++) {
					const int16_t layer = faces[side];
					if (layer < 0)
						continue;
					if (!seen[side]) {
						hidden[layer]++;
						continue;
					}
					if (++uses[layer] > best) {
						best = uses[layer];
						genericWallLayer = layer;
					}
				}
			}
		}
		for (int layer = 0; layer < MaxLayers; layer++)
			layerIsTop[layer] = hidden[layer] > 0 && uses[layer] == 0;
		// Of the faces used at least an eighth as often, and about as bright, the one with the least
		// painted on it.
		plainWallLayer = genericWallLayer;
		for (int layer = 0; layer < MaxLayers && genericWallLayer >= 0; layer++) {
			if (uses[layer] >= std::max(4, best / 8) && layerBrightness[layer] >= 0.75F * layerBrightness[genericWallLayer]
			    && layerBlotch[layer] < layerBlotch[plainWallLayer])
				plainWallLayer = static_cast<int16_t>(layer);
		}
		for (int layer = 0; layer < MaxLayers && SDL_getenv("DEVILUTIONX_VR_DEBUG_WALLS") != nullptr; layer++) {
			if (uses[layer] > 0 || hidden[layer] > 0)
				LogInfo("VR: wall face {} seen {} hidden {} blotch {:.3f} brightness {:.0f} piece {} {}", layer, uses[layer], hidden[layer], layerBlotch[layer], layerBrightness[layer], layerPiece[layer], layerNorthEast[layer] ? "ne" : "nw");
		}
		// The cathedral has no plain face at all (ExtractPlainWallFace).
		if (leveltype == DTYPE_CATHEDRAL && genericWallLayer >= 0 && layerIsWall[genericWallLayer]) {
			const int16_t plain = ExtractPlainWallFace(RenderColumn(layerPiece[genericWallLayer]), layerNorthEast[genericWallLayer], layerMaterial[genericWallLayer], WallDecals);
			if (plain >= 0)
				plainWallLayer = plain;
		}
		FlipForSeams();
		if (plainWallLayer >= 0)
			LogVerbose("VR: plain wall face {} ({:.3f}; the most used, {}, {:.3f})", plainWallLayer, layerBlotch[plainWallLayer], genericWallLayer, layerBlotch[genericWallLayer]);
	}
	if (leveltype == DTYPE_TOWN && !townBuilt)
		BuildTown();
	int rebuilt = 0;
	for (int cy = 0; cy < ChunksPerSide; cy++) {
		for (int cx = 0; cx < ChunksPerSide; cx++) {
			if (dirty[cx][cy]) {
				BuildChunk(cx, cy);
				rebuilt++;
			}
		}
	}
	BakeLight();
	BuildFloorMap();
	if (layersDirty) {
		context->GenerateMips(surfaces.srv.Get());
		context->GenerateMips(surfaceNormals.srv.Get());
		layersDirty = false;
	}
	LogVerbose("VR: level meshed: {} chunks rebuilt, {} wall tiles, {} surface layers", rebuilt, walls, layerCount);
	// DEVILUTIONX_VR_DEBUG_MAP=<file>: the level as text, to find a spot worth pointing the capture camera at.
	// '#' wall, '.' floor, 'n'/'e'/'b' a cut-out slab on the tile's north-west / north-east / both edges, 'a' an arch sprite, 'c' a card.
	if (const char *mapFile = SDL_getenv("DEVILUTIONX_VR_DEBUG_MAP"); mapFile != nullptr) {
		std::string text;
		for (int y = 0; y < MAXDUNY; y++) {
			for (int x = 0; x < MAXDUNX; x++) {
				const uint16_t piece = dPiece[x][y];
				char c = ' ';
				if (piece < MAXTILES) {
					const PieceInfo &info = Classify(piece);
					c = wall[x][y] ? '#' : (info.floorLayer >= 0 ? '.' : ' ');
					if (!wall[x][y] && leveltype != DTYPE_TOWN) {
						if (info.nwLayer >= 0 && info.neLayer >= 0)
							c = 'b';
						else if (info.nwLayer >= 0)
							c = 'n';
						else if (info.neLayer >= 0)
							c = 'e';
						else if (dSpecial[x][y] > 0)
							c = 'a';
						else if (info.cardLayer >= 0)
							c = 'c';
					}
				}
				text += c;
			}
			text += '\n';
		}
		for (int i = 0; i < ActiveObjectCount; i++) {
			const Object &object = Objects[ActiveObjects[i]];
			text += "object " + std::to_string(static_cast<int>(object._otype)) + " " + std::to_string(object.position.x) + " " + std::to_string(object.position.y) + "\n";
		}
		if (SDL_RWops *file = SDL_RWFromFile(mapFile, "wb"); file != nullptr) {
			SDL_RWwrite(file, text.data(), 1, text.size());
			SDL_RWclose(file);
		}
	}
	return true;
}

/** Marks the chunks holding tiles that can be seen from tile (cx, cy): recursive shadow casting over the occupancy grid. */
void CastOctant(int cx, int cy, int row, float start, float end, int xx, int xy, int yx, int yy)
{
	if (start < end)
		return;
	float nextStart = start;
	for (int i = row; i <= ViewTiles; i++) {
		bool blocked = false;
		for (int dx = -i, dy = -i; dx <= 0; dx++) {
			const float leftSlope = (static_cast<float>(dx) - 0.5F) / (static_cast<float>(dy) + 0.5F);
			const float rightSlope = (static_cast<float>(dx) + 0.5F) / (static_cast<float>(dy) - 0.5F);
			if (start < rightSlope)
				continue;
			if (end > leftSlope)
				break;
			const int x = cx + dx * xx + dy * xy;
			const int y = cy + dx * yx + dy * yy;
			const bool solid = !InLevel(x, y) || blocks[x][y];
			if (InLevel(x, y))
				chunkSeen[x / ChunkTiles][y / ChunkTiles] = seenStamp;
			if (blocked) {
				if (solid) {
					nextStart = rightSlope;
				} else {
					blocked = false;
					start = nextStart;
				}
			} else if (solid && i < ViewTiles) {
				blocked = true;
				CastOctant(cx, cy, i + 1, start, leftSlope, xx, xy, yx, yy);
				nextStart = rightSlope;
			}
		}
		if (blocked)
			break;
	}
}

void MarkVisibleFrom(int x, int y)
{
	if (!InLevel(x, y) || blocks[x][y])
		return;
	chunkSeen[x / ChunkTiles][y / ChunkTiles] = seenStamp;
	static const int octants[8][4] = { { 1, 0, 0, 1 }, { 0, 1, 1, 0 }, { 0, -1, 1, 0 }, { -1, 0, 0, 1 }, { -1, 0, 0, -1 }, { 0, -1, -1, 0 }, { 0, 1, -1, 0 }, { 1, 0, 0, -1 } };
	for (const auto &octant : octants)
		CastOctant(x, y, 1, 1.0F, 0.0F, octant[0], octant[1], octant[2], octant[3]);
}

} // namespace

bool LevelInit()
{
	surfaces = CreateTexture(LayerWidth, LayerHeight, MaxLayers, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, TextureMips);
	surfaceNormals = CreateTexture(LayerWidth, LayerHeight, MaxLayers, DXGI_FORMAT_R8G8B8A8_UNORM, TextureMips);
	lightmapTexture = CreateTexture(LightmapSize, LightmapSize, 1, DXGI_FORMAT_R8G8B8A8_UNORM);
	occupancyTexture = CreateTexture(OccupancySize, OccupancySize, 1, DXGI_FORMAT_R8_UNORM);
	floorMapTexture = CreateTexture(MAXDUNX * FloorMapPerTile, MAXDUNY * FloorMapPerTile, 1, DXGI_FORMAT_R8G8B8A8_UNORM);
	builtCels = nullptr;
	builtType = -99;
	return surfaces && surfaceNormals && lightmapTexture && occupancyTexture;
}

void LevelRelease()
{
	for (auto &row : chunks) {
		for (Mesh &mesh : row)
			mesh = Mesh {};
	}
	townMesh = Mesh {};
	for (Texture *texture : { &surfaces, &surfaceNormals, &lightmapTexture, &occupancyTexture, &materials, &materialNormals, &floorMapTexture })
		texture->Release();
	builtCels = nullptr;
	builtType = -99;
}

void LevelSync()
{
	// The CEL pointer alone does not identify a level: a new one's data can land on the old address.
	if (builtCels != pDungeonCels.get() || builtLevel != currlevel || builtSetLevel != setlevel
	    || builtSetLevelNumber != static_cast<int>(setlvlnum) || builtType != leveltype)
		Rebuild();
	else if (SpritesFull() && leveltype == DTYPE_TOWN)
		Rebuild(); // The town's cut-outs live in the atlas too.
	else if (SpritesFull())
		SpritesReset();
	Patch();
}

bool LevelLineOfSight(float x0, float y0, float x1, float y1)
{
	return LineOfSight(x0, y0, x1, y1);
}

int LevelDoorwayAxis(Point tile)
{
	const int x = tile.x;
	const int y = tile.y;
	if (!InLevel(x, y) || wall[x][y])
		return 0;
	if (IsWall(x, y - 1) && IsWall(x, y + 1) && !IsWall(x - 1, y) && !IsWall(x + 1, y))
		return 1;
	if (IsWall(x - 1, y) && IsWall(x + 1, y) && !IsWall(x, y - 1) && !IsWall(x, y + 1))
		return 2;
	return 0;
}

bool LevelBuiltDoorway(int index, Point &tile, bool &alongX)
{
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			if (BuiltDoorway(x, y, alongX) && index-- == 0) {
				tile = { x, y };
				return true;
			}
		}
	}
	return false;
}

float LevelCeilingHeight()
{
	return ceilingHeight;
}

void LevelAddToScene(Scene &scene)
{
	Environment &environment = scene.environment;
	environment = {};
	environment.tileSize = TileSize;
	environment.levelTiles = MAXDUNX;
	environment.surfaces = surfaces.srv.Get();
	environment.surfaceNormals = surfaceNormals.srv.Get();
	environment.lightmap = lightmapTexture.srv.Get();
	environment.occupancy = occupancyTexture.srv.Get();
	environment.sprites = SpriteAtlasView();
	environment.paletteLit = PaletteLitView();
	environment.paletteCube = PaletteCubeView();
	environment.materials = materialCount != 0 ? materials.srv.Get() : nullptr;
	environment.materialNormals = materialCount != 0 ? materialNormals.srv.Get() : nullptr;
	environment.materialSize = MaterialSize;
	environment.floorSize = floorSize;
	environment.floorMap = floorSize > 0 ? floorMapTexture.srv.Get() : nullptr;
	// Town is drawn fully lit by the game and its palette has no light tables worth the name.
	environment.paletteLight = leveltype == DTYPE_TOWN ? 0 : Knob("DEVILUTIONX_VR_PALETTE_LIGHT", 1.0F);
	environment.paletteGain = Knob("DEVILUTIONX_VR_PALETTE_GAIN", 1.0F);
	environment.exposure = Knob("DEVILUTIONX_VR_EXPOSURE", 1.0F);
	environment.bloom = 0.6F;
	const auto set = [](float target[3], float r, float g, float b) {
		target[0] = r;
		target[1] = g;
		target[2] = b;
	};
	// Outside the reach of a light a dungeon is black, as in the game: there is no fill light,
	// only a trace so that silhouettes right next to the player do not vanish completely.
	const float trace = Knob("DEVILUTIONX_VR_AMBIENT", 0.004F);
	switch (leveltype) {
	case DTYPE_TOWN:
		// Night: a black sky, mist that swallows what is far, a pale moon low over the roofs.
		set(environment.ambient, 0.07F, 0.08F, 0.11F);
		set(environment.fogColor, 0.004F, 0.005F, 0.008F);
		environment.fogDensity = 0.03F;
		// The town's art is painted dark and was shown unlit; open the exposure instead of flooding it with light.
		environment.exposure *= 3.0F;
		environment.sunDirection = Normalize({ 0.45F, 0.55F, 0.70F });
		set(environment.sunColor, 0.32F, 0.34F, 0.42F);
		break;
	case DTYPE_CATACOMBS:
		set(environment.ambient, trace * 1.2F, trace, trace * 0.8F);
		set(environment.fogColor, 0.004F, 0.003F, 0.002F);
		environment.fogDensity = 0.055F;
		break;
	case DTYPE_CAVES:
	case DTYPE_NEST:
		set(environment.ambient, trace * 1.6F, trace * 0.8F, trace * 0.5F);
		set(environment.fogColor, 0.012F, 0.004F, 0.002F);
		environment.fogDensity = 0.04F;
		break;
	case DTYPE_HELL:
		set(environment.ambient, trace * 1.8F, trace * 0.6F, trace * 0.5F);
		set(environment.fogColor, 0.010F, 0.002F, 0.002F);
		environment.fogDensity = 0.05F;
		break;
	default:
		set(environment.ambient, trace * 0.8F, trace, trace * 1.3F);
		set(environment.fogColor, 0.002F, 0.003F, 0.005F);
		environment.fogDensity = 0.05F;
		break;
	}
	// The view ends in grey fog 28 tiles out, on every level.
	const float grey = Knob("DEVILUTIONX_VR_FOG_GREY", leveltype == DTYPE_TOWN ? 0.01F : 0.03F);
	set(environment.viewFogColor, grey, grey * 1.02F, grey * 1.06F);
	environment.viewDistance = ViewDistance;
	// No roof below ground: past the painted wall everything sinks into black.
	environment.abyssStart = leveltype == DTYPE_TOWN ? 1e9F : ceilingHeight;
	environment.abyssFade = AbyssFade;
	environment.shadowCenter = scene.headPosition;
	environment.shadowCenter.y = 0;

	// Occlusion: only chunks with a tile that can be seen from where the head is. The head is
	// not a point (two eyes, leaning), so nearby tiles count as viewpoints too.
	seenStamp++;
	const float hx = scene.headPosition.x / TileSize;
	const float hy = scene.headPosition.z / TileSize;
	const int tileX = static_cast<int>(std::floor(hx));
	const int tileY = static_cast<int>(std::floor(hy));
	bool inside = false;
	for (const float dy : { -0.35F, 0.F, 0.35F }) {
		for (const float dx : { -0.35F, 0.F, 0.35F }) {
			const int x = static_cast<int>(std::floor(hx + dx));
			const int y = static_cast<int>(std::floor(hy + dy));
			if ((dx == 0 && dy == 0) || x != tileX || y != tileY)
				MarkVisibleFrom(x, y);
			inside = inside || (InLevel(x, y) && !blocks[x][y]);
		}
	}
	MeshInstance instance {};
	instance.world = IdentityMat34;
	instance.color[0] = instance.color[1] = instance.color[2] = 1;
	instance.boneBase = -1;
	// Nothing in the world shines: stone, earth and wood are matt.
	instance.specular = 0;
	instance.lit = 1;
	for (int cy = 0; cy < ChunksPerSide; cy++) {
		for (int cx = 0; cx < ChunksPerSide; cx++) {
			// With the head inside a wall nothing can be ruled out.
			// Nothing of a chunk beyond the fog can be seen (half a chunk's diagonal is 5.7 tiles).
			const float chunkX = (static_cast<float>(cx) + 0.5F) * ChunkTiles - hx;
			const float chunkY = (static_cast<float>(cy) + 0.5F) * ChunkTiles - hy;
			const float reach = ViewDistance / TileSize + 5.7F;
			if (chunkX * chunkX + chunkY * chunkY > reach * reach)
				continue;
			if (chunkBuilt[cx][cy] && (chunkSeen[cx][cy] == seenStamp || !inside))
				scene.meshes.push_back({ &chunks[cx][cy], instance, true });
		}
	}
	if (townMesh.indexCount != 0)
		scene.meshes.push_back({ &townMesh, instance, true });

	for (const BuiltPlacement &placed : builtPlacements) {
		const Vec3 toViewer { scene.headPosition.x - placed.foot.x, 0, scene.headPosition.z - placed.foot.z };
		if (Length(toViewer) <= ViewDistance + 16 * TileSize)
			ModelsAddGenerated(scene, *placed.model, Compose(placed.foot, AxisAngle({ 0, 1, 0 }, 0)));
	}
	for (const CarvedPlacement &placed : carvedPlacements) {
		const Vec3 toViewer { scene.headPosition.x - placed.foot.x, 0, scene.headPosition.z - placed.foot.z };
		// Houses reach some tiles from where they stand: keep them a little beyond the fog.
		if (Length(toViewer) > ViewDistance + 8 * TileSize)
			continue;
		CarvedAddModel(scene, placed.key, placed.foot);
	}

	for (const Standee &standee : standees) {
		const Vec3 toViewer { scene.headPosition.x - standee.position.x, 0, scene.headPosition.z - standee.position.z };
		const float away = Length(toViewer);
		if (away > ViewDistance + standee.halfWidth)
			continue;
		// A cut-out turns about where the viewer stands, never with the head: looking around moves nothing.
		const Vec3 normal = away > 0.05F ? toViewer * (1 / away) : Vec3 { 0, 0, 1 };
		const Vec3 right { normal.z, 0, -normal.x };
		SpriteInstance sprite {};
		const Vec3 position = standee.position + right * standee.shift + Vec3 { 0, standee.lift, 0 };
		const Vec3 axisU = right * standee.halfWidth;
		std::memcpy(sprite.position, &position, sizeof(sprite.position));
		std::memcpy(sprite.axisU, &axisU, sizeof(sprite.axisU));
		sprite.axisV[1] = standee.height;
		sprite.uv[0] = standee.slot.x;
		sprite.uv[1] = standee.slot.y;
		sprite.uv[2] = static_cast<float>(standee.slot.x + standee.slot.width);
		sprite.uv[3] = static_cast<float>(standee.slot.y + standee.slot.height);
		sprite.tint[0] = sprite.tint[1] = sprite.tint[2] = sprite.tint[3] = 1;
		scene.sprites.push_back(sprite);
	}
}

void LevelDumpSurfaces(const char *path)
{
	// The first layers side by side, as stored.
	const int columns = 32;
	const int rows = std::min((layerCount + columns - 1) / columns, 16);
	if (rows == 0)
		return;
	D3D11_TEXTURE2D_DESC desc {};
	desc.Width = LayerWidth;
	desc.Height = LayerHeight;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_STAGING;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ComPtr<ID3D11Texture2D> staging;
	SDL_Surface *sheet = SDL_CreateRGBSurfaceWithFormat(0, columns * LayerWidth, rows * LayerHeight, 32, SDL_PIXELFORMAT_ABGR8888);
	if (sheet == nullptr || FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
		return;
	SDL_FillRect(sheet, nullptr, SDL_MapRGBA(sheet->format, 255, 0, 255, 255));
	D3D11_TEXTURE2D_DESC arrayDesc;
	surfaces.texture->GetDesc(&arrayDesc);
	for (int layer = 0; layer < std::min(layerCount, columns * rows); layer++) {
		context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, surfaces.texture.Get(), D3D11CalcSubresource(0, layer, arrayDesc.MipLevels), nullptr);
		D3D11_MAPPED_SUBRESOURCE mapped;
		if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
			continue;
		for (int y = 0; y < LayerHeight; y++) {
			auto *target = static_cast<uint32_t *>(sheet->pixels) + static_cast<size_t>(layer / columns * LayerHeight + y) * (sheet->pitch / 4) + layer % columns * LayerWidth;
			const auto *source = reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch);
			for (int x = 0; x < LayerWidth; x++) {
				if ((source[x] >> 24) != 0)
					target[x] = source[x] | 0xFF000000;
			}
		}
		context->Unmap(staging.Get(), 0);
	}
	SDL_SaveBMP(sheet, path);
	SDL_FreeSurface(sheet);
}

} // namespace render
} // namespace vr
} // namespace devilution

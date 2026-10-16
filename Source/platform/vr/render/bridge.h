/**
 * @file bridge.h
 *
 * The game side of the renderer: reads game state and fills a Scene.
 *
 * Only the bridge (level.cpp, sprites.cpp, actors.cpp, particles.cpp, world.cpp) includes
 * game headers. It never issues draw calls; it owns the level's textures and meshes and
 * lists what exists. Not for use outside platform/vr/render.
 */
#pragma once

#include <string>

#include <SDL.h>

#include "engine/clx_sprite.hpp"
#include "engine/point.hpp"
#include "platform/vr/knob.h"
#include "platform/vr/render/mesh.h"
#include "platform/vr/render/scene.h"
#include "platform/vr/render/world.h"

namespace devilution {
namespace vr {
namespace render {

/**
 * Meters per dungeon tile. Character bodies are about 50 px tall in the art (the frames are
 * larger but mostly padding), i.e. 1.3 tiles, so this makes a person about 1.8 m.
 */
inline const float TileSize = Knob("DEVILUTIONX_VR_TILE_SIZE", 1.4F);
/** Art pixels per tile along the screen's horizontal axis: a tile diagonal (sqrt(2) tiles) is 64 px. */
constexpr float PixelsPerTileH = 45.25F;
/** Art pixels per tile of height: the art is drawn from 30 degrees above the horizon (cos 30 * 45.25). */
inline const float PixelsPerTileV = Knob("DEVILUTIONX_VR_PIXELS_PER_TILE_V", 39.2F);
/** How far the eye sees, in meters (28 tiles): beyond it lies fog and nothing is drawn. */
inline const float ViewDistance = Knob("DEVILUTIONX_VR_VIEW_TILES", 28.0F) * TileSize;
/** Sprites keep this many pixels of ground shadow below the feet. */
inline const float SpriteFootPixels = Knob("DEVILUTIONX_VR_SPRITE_FOOT_PX", 16.0F);

// sprites.cpp: the sprite atlas. Sprites are rasterised once, on first use, and stay until the level changes.
struct SpriteSlot {
	/** Where the sprite's painted part sits in the atlas, in texels. Frames are mostly empty, so only that part is stored. */
	int16_t x, y, width, height;
	/** Atlas texels per pixel of the game's art: 1 for the game's own sprites, more for a hi-res pack's. */
	float scale;
	/** Where that part sits inside the original frame and how large it is, in art pixels. */
	float left, top, artWidth, artHeight;
	/** The original frame's size, in art pixels. */
	int16_t frameWidth, frameHeight;
	/** Average colour of the opaque pixels, linear. Lights and stand-in figures take their colour from it. */
	float color[3];
};

/** The `vrassets` folder (hi-res sprite packs, material libraries), or empty when there is none. */
const std::string &AssetsDirectory();

/** Which file a sprite was loaded from ("objects/barrel") and which frame of it (from 0). False for sprites of unknown origin. */
bool SpriteOrigin(ClxSprite sprite, std::string &path, int &frame);

bool SpritesInit();
void SpritesRelease();
/** Forgets every cached sprite (new level, atlas full). */
void SpritesReset();
/** Changes whenever the atlas was emptied: slots handed out before are gone. */
uint32_t SpritesEpoch();
/** Drops the hi-res sheets of the level that was left. */
void SpritesLevelChanged();
/** Once a frame: learns the level's monster files and takes in hi-res sheets that finished loading. */
void SpritesSync();
/** True once a sprite did not fit: the caller empties the atlas at the start of the next frame. */
bool SpritesFull();
/** Null when the atlas is full. `trn` is an optional colour translation table (unique monsters). */
const SpriteSlot *CacheSprite(ClxSprite sprite, const uint8_t *trn = nullptr);
/**
 * The game's own picture of a sprite, never a hi-res pack's, without the ground shadow painted
 * into it (palette index 0 that is not closed in by the body): what carved creatures are coloured from.
 */
const SpriteSlot *CacheSpriteArt(ClxSprite sprite, const uint8_t *trn = nullptr);
/** Stores an image made elsewhere (palette index + alpha pairs); only its painted part is kept. Null when the atlas is full. */
const SpriteSlot *CacheImage(const uint8_t *pairs, int width, int height);
ID3D11ShaderResourceView *SpriteAtlasView();
/** Draws a sprite into the two scratch surfaces and returns index + alpha pairs, `width * height * 2` bytes. */
const uint8_t *RasterizeSprite(ClxSprite sprite, const uint8_t *trn);
/** sRGB palette entry to linear floats. */
void PaletteLinear(uint8_t index, float rgb[3]);
uint32_t PaletteRgba(uint8_t index, uint8_t alpha);

// palette.cpp: the game's palette and light tables as lookup textures.
bool PaletteInit();
void PaletteRelease();
/** The light tables differ from one kind of level to the next. */
void PaletteLevelChanged();
void PaletteSync();
ID3D11ShaderResourceView *PaletteLitView();
ID3D11ShaderResourceView *PaletteCubeView();

// level.cpp: geometry, textures and baked light of the loaded level.
bool LevelInit();
void LevelRelease();
/** Brings meshes and textures up to date with the game's level data. Cheap when nothing changed. */
void LevelSync();
/** Lists the level's visible pieces and fills in the environment. */
void LevelAddToScene(Scene &scene);
float LevelCeilingHeight();
/** How the opening in a wall on this tile lies: 1 = the way through runs along x (the opening spans y), 2 = along y, 0 = no such opening. */
int LevelDoorwayAxis(Point tile);
/** The level's `index`-th doorway built in 3D (scanning by rows), for debug cameras. False when it has fewer. */
bool LevelBuiltDoorway(int index, Point &tile, bool &alongX);
/** True when no light-blocking tile lies on the straight line between two points (tile units). */
bool LevelLineOfSight(float x0, float y0, float x1, float y1);
void LevelDumpSurfaces(const char *path);

// models.cpp: the model pack (3dobjects-vr), stood where the game has the same thing.
bool ModelsInit();
void ModelsRelease();
/** Takes down everything placed (new level). */
void ModelsReset();
/** Stands a model of the pack on a town building's footprint (tiles, inclusive). False when the pack has none. */
bool ModelsPlaceBuilding(int minX, int minY, int maxX, int maxY);
void ModelsAddToScene(Scene &scene);

/**
 * A model built in Blender after one of the game's own pictures: `vrassets/models/<key>.glb`.
 * Its base is centred on the origin, y is up and the side
 * the picture shows faces +Z.
 */
struct GeneratedModel {
	Mesh mesh;
	Vec3 size;
};
/** Null when there is no such model. Looked up once; a model added later shows after the next level change. */
const GeneratedModel *ModelsGenerated(const std::string &key);
/** The model of the picture a sprite shows ("<file>.f<frame>", or the nearest earlier frame that has one), or null. */
const GeneratedModel *ModelsForSprite(ClxSprite sprite);
/**
 * Stands a generated model on the ground at `foot`, turned the way the game's art looks at things,
 * as large as the game's picture shows it: `spriteHeight` is how far the picture's top stands above
 * the ground line through `foot`, in meters as an upright sprite has them.
 */
void ModelsAddGenerated(Scene &scene, const GeneratedModel &model, Vec3 foot, float spriteHeight);
/** The same with a transform of the caller's (held items). `glow` > 0 draws it at full brightness, as the game draws what is in the hand. */
void ModelsAddGenerated(Scene &scene, const GeneratedModel &model, const Mat34 &world, float glow = 0);

// carved.cpp: models built from the game's pictures (`vrassets/carve`).
void CarvedRelease();
float CarvedMetersPerPixel();
/**
 * The carved model of the picture an object's sprite shows, stood at `foot` (where the game stands
 * the sprite's bottom centre), with the frame's moving part (a flame, a liquid) as a sprite over it,
 * self-lit by `effectEmissive`. False when there is none. `size`: its extent in meters.
 */
bool CarvedAddObject(Scene &scene, ClxSprite sprite, Vec3 foot, float emissive, Vec3 *size = nullptr, float effectEmissive = 0);
/** True when `vrassets/carve/<key>.crv` has a model ("tiles/l3/til52", "town/b_26_48"). */
bool CarvedExists(const std::string &key);
/** That model stood with its origin (the middle of its reference tile, or of its megatile) at `foot`. */
bool CarvedAddModel(Scene &scene, const std::string &key, Vec3 foot, float emissive = 0, Vec3 *size = nullptr);
/**
 * The model of inventory picture `icon` (frame of objcurs, from 0);
 * `grip` places its origin, where the hand holds it. x across, y up its length, z out of its face.
 */
bool CarvedAddItem(Scene &scene, int icon, const Mat34 &grip, float metersPerPixel, float emissive);
/** Record `id` of `vrassets/carve/<key>.crv` placed by `world` (a door's leaf). */
bool CarvedAddRecord(Scene &scene, const std::string &key, int id, const Mat34 &world, float emissive = 0);

// actors.cpp: everything that moves or can be picked up.
/** Set by the input layer each frame (world.cpp). */
extern VrSpellRing spellRing;
/** Set by vr.cpp each frame (world.cpp). */
extern VrHeldPanel heldPanel;
/** Set by the input layer each frame (world.cpp). */
extern VrBeltHold beltHold;
/** What rays are tested against when aiming: the upright sprites and figures of the last scene. */
struct Pickable {
	Vec3 base;
	/** Along the rectangle's width. */
	Vec3 right;
	float halfWidth;
	float height;
	Point tile;
};
extern std::vector<Pickable> pickables;
bool ActorsInit();
void ActorsRelease();
void ActorsAddToScene(Scene &scene, const VrHandPose &leftHand, const VrHandPose &rightHand, bool pointerValid, const float pointerHit[3]);
/** World position of a point given the way the 2D renderer gives it: a tile plus a pixel offset on screen. */
Vec3 TileToWorld(Point tile, float screenX, float screenY);

// feedback.cpp: crosshair, swing arc, hit marks and the names of loot.
void FeedbackAddToScene(Scene &scene, bool pointerValid, const float pointerHit[3]);
void FeedbackRelease();

// particles.cpp
void ParticlesReset();
/** Sparks thrown off by something burning or flying; `color` is linear HDR. */
void ParticlesEmit(Vec3 position, const float color[3], int count, float speed, float life, float size);
void ParticlesUpdate(Scene &scene, float dt);

// debug_panel.cpp: the performance panel floating in the world.
void DebugPanelAddToScene(Scene &scene);
void DebugPanelRelease();

} // namespace render
} // namespace vr
} // namespace devilution

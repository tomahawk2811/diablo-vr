/**
 * @file world.h
 *
 * First-person 3D view of the current level: the interface the VR session, the desktop
 * mirror and the debug tools use. Knows nothing about OpenXR; callers supply a device,
 * poses and render targets.
 *
 * Behind it (platform/vr/render/):
 *   renderer.*            draws a Scene; no game types
 *   scene.h               what a frame contains, as plain data
 *   level/actors/...      the bridge: reads game state, fills the Scene
 *
 * World space: meters, Y up, tile (x, y) covers X in [x, x+1) * TileSize and
 * Z in [y, y+1) * TileSize.
 */
#pragma once

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;
struct ID3D11Buffer;

namespace devilution {

struct VrHandPose {
	bool valid;
	float position[3];
	/** x, y, z, w */
	float orientation[4];
};

/** One eye (or a single debug camera) in world space. */
struct VrView {
	float position[3];
	/** x, y, z, w */
	float orientation[4];
	/** Tangents of the field of view's half-angles: left, right, up, down. */
	float fov[4];
};

/** Where a view's finished picture goes. */
struct VrViewTarget {
	ID3D11RenderTargetView *color;
	/** Optional: receives the scene depth (reversed: 1 near, 0 far). */
	ID3D11DepthStencilView *depth;
	/** Optional: the part of the image the lenses never show, as a triangle list of x, y pairs in view space at z = -1. */
	ID3D11Buffer *maskVertices;
	unsigned maskVertexCount;
};

/** The speedbook held open on the left hand: the spells it offers (game SpellID / SpellType values) and the one chosen. */
struct VrSpellRing {
	bool open;
	int selected;
	int count;
	int spells[32];
	int types[32];
};

/** Distances of the clip planes, for whoever submits the depth image. */
constexpr float VrNearPlane = 0.05F;
constexpr float VrFarPlane = 400.0F;

bool VrWorldInit(ID3D11Device *device, ID3D11DeviceContext *context);
void VrWorldRelease();

/** True when a level is loaded and the local player exists. */
bool VrWorldAvailable();

/** World position of the local player's feet, including walking interpolation. */
void VrWorldPlayerPosition(float &x, float &z);

/** Meters per dungeon tile. */
float VrWorldTileSize();

/** Whether a person can stand at this world position (walkable tiles). */
bool VrWorldCanStand(float x, float z);

/** Whether the character is free to be moved (standing or walking, not in a swing, a cast or a blow). */
bool VrWorldPlayerCanGlide();

/**
 * Puts the character on the tile under this world position, facing the way it moves, without a
 * walk: free movement carries the view and the character follows tile by tile underneath.
 * @return false when the character cannot be there (the tile is taken) or is busy
 */
bool VrWorldPlacePlayer(float x, float z, float dirX, float dirZ);

/**
 * Casts a ray into the scene built by the last VrWorldUpdate.
 * Returns the tile that was hit (a monster/object's tile if the ray hits its sprite,
 * otherwise the floor tile) and the hit point.
 */
bool VrWorldPick(const float origin[3], const float direction[3], int &tileX, int &tileY, float hit[3]);

/**
 * Reads the game's state into the scene for this frame. The head pose turns sprites towards the
 * viewer and decides what can be seen at all; hands are in world space; `pointer` draws the aim beam.
 */
/**
 * The time the frame about to be built is shown at, in seconds from any fixed start. Without it
 * (no headset: the offscreen renderer) the scene runs on the game's millisecond clock.
 */
void VrWorldSetClock(float seconds);
void VrWorldUpdate(const float headPosition[3], const float headOrientation[4], const VrHandPose &leftHand, const VrHandPose &rightHand, bool pointerValid, const float pointerHit[3]);

/** Draws the scene of the last VrWorldUpdate, all views in one go. All targets are `width` x `height`. */
void VrWorldRender(const VrView *views, int viewCount, int width, int height, const VrViewTarget *targets);

/** Copies the finished picture of view 0 of the last VrWorldRender into another target, scaled. */
void VrWorldBlit(ID3D11RenderTargetView *target, int width, int height);

/** Debug: saves the level's surface textures as a BMP. */
void VrWorldDumpAtlas(const char *path);

/**
 * A game panel held as a thing: 0 none, 1 the inventory's backpack in a satchel at the hip, 2 the
 * spellbook as a tome on the left hand, 3 the automap on a parchment in the left hand. The panel
 * itself is the 2D frame's quad; this is where that quad is (world space, its picture on the +z
 * side) so the world can draw what holds it.
 */
struct VrHeldPanel {
	int kind;
	float position[3];
	/** x, y, z, w */
	float orientation[4];
	float width;
	float height;
};
void VrWorldSetHeldPanel(const VrHeldPanel &panel);

/** Comfort: how much of the edge of view to darken this frame (0 = none, 1 = only the middle stays). */
void VrWorldSetVignette(float strength);

/** The left hand's speedbook as the input layer has it this frame. */
void VrWorldSetSpellRing(const VrSpellRing &ring);

/**
 * Something the hand at `hand` (world space) can take hold of: an object (door, chest, lever,
 * shrine) or an item on the ground within reach. False when there is none.
 */
bool VrWorldGrabTarget(const float hand[3], int &tileX, int &tileY);

/** Where the townsperson the player is talking to (or trading with) stands, at head height. False when there is none. */
bool VrWorldSpeaker(float position[3]);

/** The head's position and the direction of its right, in world space, as of the last update. */
void VrWorldHead(float position[3], float right[3]);

/**
 * A sound at tile (sourceX, sourceY) as the player hears it in the headset: panned by where it lies
 * from the head, as loud as the game makes it at that distance, quieter behind walls (the game's
 * volume and pan units). False when it is too faint to play.
 */
bool VrWorldSoundPosition(int sourceX, int sourceY, int *volume, int *pan);

} // namespace devilution

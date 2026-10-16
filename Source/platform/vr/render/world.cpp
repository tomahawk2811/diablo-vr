/**
 * @file world.cpp
 *
 * The world renderer's public interface (world.h): owns the Scene, runs the bridge to fill
 * it and the renderer to draw it.
 */
#include "platform/vr/render/bridge.h"

#include <cstring>

#include "diablo.h"
#include "engine/sound_defs.hpp"
#include "interfac.h"
#include "items.h"
#include "objects.h"
#include "towners.h"
#include "levels/dun_tile_data.hpp"
#include "levels/tile_properties.hpp"
#include "platform/vr/debug/profile.h"
#include "platform/vr/render/renderer.h"
#include "player.h"

namespace devilution {

using namespace vr::render;

namespace {

Scene scene;
bool ready;

} // namespace

bool VrWorldInit(ID3D11Device *newDevice, ID3D11DeviceContext *newContext)
{
	ready = RendererInit(newDevice, newContext) && SpritesInit() && PaletteInit() && LevelInit() && ModelsInit() && ActorsInit();
	return ready;
}

void VrWorldRelease()
{
	if (device == nullptr)
		return;
	ready = false;
	DebugPanelRelease();
	ActorsRelease();
	CarvedRelease();
	ModelsRelease();
	LevelRelease();
	PaletteRelease();
	SpritesRelease();
	RendererRelease();
}

bool VrWorldAvailable()
{
	return ready && gbRunGame && !IsLoadingLevel() && MyPlayer != nullptr && pDungeonCels != nullptr;
}

float VrWorldTileSize()
{
	return TileSize;
}

void VrWorldPlayerPosition(float &x, float &z)
{
	const Player &player = *MyPlayer;
	// Same rule the 2D renderer uses: the sprite is anchored to the tile the walk started from.
	Point anchor = player.position.tile;
	if (player._pmode == PM_WALK_SOUTHWARDS || (player._pmode == PM_WALK_SIDEWAYS && player._pdir == Direction::East))
		anchor += Opposite(player._pdir);
	Displacement fromAnchor {};
	if (player.isWalking())
		fromAnchor = GetOffsetForWalking(player.AnimInfo, player._pdir);
	const Vec3 position = TileToWorld(anchor, static_cast<float>(fromAnchor.deltaX), static_cast<float>(fromAnchor.deltaY));
	x = position.x;
	z = position.z;
}

bool VrWorldCanStand(float x, float z)
{
	// A person-sized footprint, so the camera cannot be pressed into a wall.
	constexpr float Radius = 0.25F;
	for (const float dx : { -Radius, Radius }) {
		for (const float dz : { -Radius, Radius }) {
			const Point tile { static_cast<int>(std::floor((x + dx) / TileSize)), static_cast<int>(std::floor((z + dz) / TileSize)) };
			// Monsters do not block the camera, only the character: being wedged against
			// one while trying to swing at it is worse than overlapping it a little.
			if (!InDungeonBounds(tile) || !IsTileWalkable(tile))
				return false;
		}
	}
	// The character itself stands on the tile under the middle, which must be free for it
	// (a monster, a townsperson or another player on it keeps the player out).
	const Player &player = *MyPlayer;
	const Point under { static_cast<int>(std::floor(x / TileSize)), static_cast<int>(std::floor(z / TileSize)) };
	return under == player.position.tile || under == player.position.future || PosOkPlayer(player, under);
}

bool VrWorldPlayerCanGlide()
{
	const Player &player = *MyPlayer;
	return player._pmode == PM_STAND || player.isWalking();
}

bool VrWorldPlacePlayer(float x, float z, float dirX, float dirZ)
{
	Player &player = *MyPlayer;
	if (!VrWorldPlayerCanGlide())
		return false;
	const Point tile { static_cast<int>(std::floor(x / TileSize)), static_cast<int>(std::floor(z / TileSize)) };
	const bool walking = player.isWalking();
	if (tile == player.position.tile && !walking)
		return true;
	if (tile != player.position.tile && tile != player.position.future && !PosOkPlayer(player, tile))
		return false;
	// The way the character faces: the nearest of the game's eight to where it is going.
	Direction facing = player._pdir;
	if (dirX * dirX + dirZ * dirZ > 1e-6F) {
		const float angle = std::atan2(dirZ, dirX);
		const Point step { static_cast<int>(std::lround(std::cos(angle))), static_cast<int>(std::lround(std::sin(angle))) };
		if (step.x != 0 || step.y != 0)
			facing = GetDirection({ 0, 0 }, step);
	}
	// What the end of a walk does, without the walk: the character is on the new tile at once,
	// its light and sight with it, and whatever it was walking towards is forgotten.
	ClrPlrPath(player);
	player.destAction = ACTION_NONE;
	FixPlrWalkTags(player);
	player.position.tile = tile;
	StartStand(player, facing);
	if (leveltype != DTYPE_TOWN)
		PlrDoTrans(tile);
	return true;
}

bool VrWorldPick(const float origin[3], const float direction[3], int &tileX, int &tileY, float hit[3])
{
	float best = 20.0F; // Meters; nothing in Diablo is targeted further away than this.
	bool found = false;
	const Vec3 from { origin[0], origin[1], origin[2] };
	const Vec3 along { direction[0], direction[1], direction[2] };

	// Sprites and figures: an upright rectangle facing the viewer.
	for (const Pickable &pickable : pickables) {
		const Vec3 right = pickable.right;
		const Vec3 normal { -right.z, 0, right.x };
		const float facing = Dot(along, normal);
		if (std::fabs(facing) < 1e-4F)
			continue;
		const float t = Dot(pickable.base - from, normal) / facing;
		if (t <= 0 || t >= best)
			continue;
		const Vec3 point = from + along * t;
		if (std::fabs(Dot(point - pickable.base, right)) > pickable.halfWidth || point.y < 0 || point.y > pickable.base.y + pickable.height)
			continue;
		best = t;
		found = true;
		tileX = pickable.tile.x;
		tileY = pickable.tile.y;
		std::memcpy(hit, &point, sizeof(point));
	}

	// Floor.
	if (direction[1] < -1e-4F) {
		const float t = -origin[1] / direction[1];
		if (t > 0 && t < best) {
			const Vec3 point = from + along * t;
			hit[0] = point.x;
			hit[1] = 0;
			hit[2] = point.z;
			tileX = static_cast<int>(std::floor(point.x / TileSize));
			tileY = static_cast<int>(std::floor(point.z / TileSize));
			found = tileX >= 0 && tileY >= 0 && tileX < MAXDUNX && tileY < MAXDUNY;
		}
	}
	return found;
}

namespace {
float vignette;
Vec3 lastHead;
Vec3 lastRight { 1, 0, 0 };
} // namespace

void VrWorldSetVignette(float strength)
{
	vignette = strength;
}

void VrWorldSetSpellRing(const VrSpellRing &ring)
{
	spellRing = ring;
}

void VrWorldSetHeldPanel(const VrHeldPanel &panel)
{
	heldPanel = panel;
}

void VrWorldHead(float position[3], float right[3])
{
	position[0] = lastHead.x;
	position[1] = lastHead.y;
	position[2] = lastHead.z;
	right[0] = lastRight.x;
	right[1] = lastRight.y;
	right[2] = lastRight.z;
}

bool VrWorldGrabTarget(const float hand[3], int &tileX, int &tileY)
{
	// Within reach: a hand's length from the hand, and not above the head.
	constexpr float Reach = 0.55F;
	float best = Reach * Reach;
	bool found = false;
	const auto consider = [&](Point tile, float top) {
		const Vec3 at = TileToWorld(tile, 0, 0);
		const float dx = at.x - hand[0];
		const float dz = at.z - hand[2];
		const float d = dx * dx + dz * dz;
		if (d < best && hand[1] < top + 0.3F) {
			best = d;
			tileX = tile.x;
			tileY = tile.y;
			found = true;
		}
	};
	for (int i = 0; i < ActiveObjectCount; i++) {
		const Object &object = Objects[ActiveObjects[i]];
		// Things that can be opened, pulled or prayed at; not decoration.
		if (!object.canInteractWith())
			continue;
		consider(object.position, 1.8F);
	}
	for (int i = 0; i < ActiveItemCount; i++)
		consider(Items[ActiveItems[i]].position, 0.6F);
	return found;
}

bool VrWorldSpeaker(float position[3])
{
	if (leveltype != DTYPE_TOWN)
		return false;
	const Towner *nearest = nullptr;
	int best = 5;
	for (const Towner &towner : Towners) {
		const int d = MyPlayer->position.tile.ApproxDistance(towner.position);
		if (d < best) {
			best = d;
			nearest = &towner;
		}
	}
	if (nearest == nullptr)
		return false;
	const Vec3 at = TileToWorld(nearest->position, 0, 0);
	position[0] = at.x;
	position[1] = 1.55F;
	position[2] = at.z;
	return true;
}

bool VrWorldSoundPosition(int sourceX, int sourceY, int *volume, int *pan)
{
	const Point source { sourceX, sourceY };
	// Panned by where the source lies from the head, and as loud as the game makes it at that
	// distance, less what the walls between swallow. (No head-related filtering: stereo panning.)
	const Vec3 at = TileToWorld(source, 0, 0) + Vec3 { 0, 1.0F, 0 };
	Vec3 toSource { at.x - lastHead.x, 0, at.z - lastHead.z };
	const float away = Length(toSource);
	const float side = away > 0.05F ? Dot(toSource * (1 / away), Normalize(Vec3 { lastRight.x, 0, lastRight.z })) : 0;
	*pan = std::clamp(static_cast<int>(side * 0.8F * PAN_MAX), PAN_MIN, PAN_MAX);
	const Point listener = MyPlayer->position.tile;
	int loudness = listener.ApproxDistance(source) * -64;
	if (!LevelLineOfSight(lastHead.x / TileSize, lastHead.z / TileSize, static_cast<float>(source.x) + 0.5F, static_cast<float>(source.y) + 0.5F))
		loudness -= 1200;
	*volume = loudness;
	return loudness > ATTENUATION_MIN;
}

namespace {
/** Set by VrWorldSetClock for the next update; negative: none. */
float frameClock = -1;
} // namespace

void VrWorldSetClock(float seconds)
{
	frameClock = seconds;
}

void VrWorldUpdate(const float headPosition[3], const float headOrientation[4], const VrHandPose &leftHand, const VrHandPose &rightHand, bool pointerValid, const float pointerHit[3])
{
	const vr::ScopedTimer timer(vr::ProfileScope::Extract);
	// A headset frame is built for the moment it is shown, and the moments are evenly spaced:
	// what moves by the scene's time (particles, monsters gliding between tiles) moves evenly.
	const float now = frameClock >= 0 ? frameClock : static_cast<float>(SDL_GetTicks()) / 1000.0F;
	frameClock = -1;
	const float dt = std::clamp(now - scene.time, 0.F, 0.1F);
	scene.Clear();
	scene.time = now;
	const Quat orientation { headOrientation[0], headOrientation[1], headOrientation[2], headOrientation[3] };
	scene.headPosition = { headPosition[0], headPosition[1], headPosition[2] };
	scene.headRight = Rotate(orientation, { 1, 0, 0 });
	scene.headUp = Rotate(orientation, { 0, 1, 0 });

	LevelSync();
	SpritesSync();
	PaletteSync();
	lastHead = scene.headPosition;
	lastRight = scene.headRight;
	LevelAddToScene(scene);
	scene.environment.vignette = vignette;
	ModelsAddToScene(scene);
	ActorsAddToScene(scene, leftHand, rightHand, pointerValid, pointerHit);
	ParticlesUpdate(scene, dt);
	DebugPanelAddToScene(scene);
}

void VrWorldRender(const VrView *views, int viewCount, int width, int height, const VrViewTarget *targets)
{
	View renderViews[MaxViews];
	Output outputs[MaxViews];
	viewCount = std::min(viewCount, MaxViews);
	for (int i = 0; i < viewCount; i++) {
		renderViews[i].position = { views[i].position[0], views[i].position[1], views[i].position[2] };
		renderViews[i].orientation = { views[i].orientation[0], views[i].orientation[1], views[i].orientation[2], views[i].orientation[3] };
		std::memcpy(renderViews[i].fov, views[i].fov, sizeof(renderViews[i].fov));
		outputs[i] = { targets[i].color, targets[i].depth, targets[i].maskVertices, targets[i].maskVertexCount };
	}
	RendererRender(scene, renderViews, viewCount, width, height, outputs);
}

void VrWorldBlit(ID3D11RenderTargetView *target, int width, int height)
{
	RendererBlitView(target, width, height);
}

void VrWorldDumpAtlas(const char *path)
{
	LevelDumpSurfaces(path);
}

} // namespace devilution

/**
 * @file actors.cpp
 *
 * Everything in a level that is not the level: monsters, players, towners, items, objects,
 * missiles, corpses, what the player holds, and the lights these carry.
 *
 * An actor is described to the renderer by what it is doing (type, action, frame, facing),
 * read from the game every frame. By default it is drawn with the game's own sprite as a lit
 * cut-out; DEVILUTIONX_VR_FIGURES=1 draws monsters, players and towners as the jointed
 * stand-in figure instead, which is the path 3D character models will take.
 */
#include "platform/vr/render/bridge.h"

#include <cstring>
#include <string>
#include <unordered_map>

#include "cursor.h"
#include "dead.h"
#include "engine/render/scrollrt.h"
#include "items.h"
#include "levels/dun_tile_data.hpp"
#include "lighting.h"
#include "missiles.h"
#include "panels/spell_icons.hpp"
#include "monster.h"
#include "objects.h"
#include "player.h"
#include "towners.h"
#include "utils/log.hpp"
#include "utils/png.h"

namespace devilution {
namespace vr {
namespace render {

std::vector<Pickable> pickables;

namespace {

const bool UseFigures = SDL_getenv("DEVILUTIONX_VR_FIGURES") != nullptr;
/** Debug captures look at monsters the player's light has not reached. */
const bool ShowUnlitMonsters = SDL_getenv("DEVILUTIONX_VR_DEBUG_MONSTER") != nullptr;
/** Degrees a held weapon stands up from where the hand points: near 90 it is held like a sword in a fist, at 0 it is a pointer. */
const float WeaponTilt = Knob("DEVILUTIONX_VR_WEAPON_TILT", 80.0F);
/** Meters per pixel of inventory art: a three-cell sword (84 px) is 0.9 m long. */
constexpr float IconMeters = 0.9F / 84;

Figure figure;
Mesh cube;
/** Plain dark gauntlets: a closed fist on each hand. [0] left, [1] right. */
Mesh gauntlets[2];
/** A small glass vial along local y, 7 cm long, and the liquid in it (scaled by how full it is). */
Mesh vialGlass;
Mesh vialFill;
/** Spell pictures in the atlas, by spell and type: the game's large spell icons. */
struct SpellPictureSlot {
	uint32_t epoch = 0;
	SpriteSlot slot {};
};
std::unordered_map<int, SpellPictureSlot> spellPictures;
/** An arrow: shaft, head and fletching along -z, 70 cm long. */
Mesh arrow;
float lastTime;

void SetVec(float target[3], Vec3 v)
{
	target[0] = v.x;
	target[1] = v.y;
	target[2] = v.z;
}

/** Soft dark disc under something standing on the floor. */
void AddBlobShadow(Scene &scene, Vec3 position, float radius)
{
	ParticleInstance blob {};
	SetVec(blob.position, { position.x, 0.015F, position.z });
	blob.size = radius;
	blob.color[3] = 0.6F;
	blob.flat = 1;
	scene.particles.push_back(blob);
}

/**
 * The width direction of an upright sprite standing at `foot`. It depends only on where the
 * viewer stands, so turning or tilting the head leaves every sprite exactly where it is.
 */
Vec3 FacingRight(const Scene &scene, Vec3 foot)
{
	const Vec3 toViewer { scene.headPosition.x - foot.x, 0, scene.headPosition.z - foot.z };
	if (Length(toViewer) < 0.05F)
		return Normalize({ scene.headRight.x, 0, scene.headRight.z });
	const Vec3 normal = Normalize(toViewer);
	return { normal.z, 0, -normal.x };
}

struct SpriteOptions {
	const uint8_t *trn = nullptr;
	float emissive = 0;
	/** A creature: its silhouette is cast on the floor away from the nearest light. */
	bool shadow = false;
	bool pickable = true;
	/** The game's picture without the ground shadow painted into it (creatures). */
	bool art = false;
	/** Rounded towards the viewer by the distance to its outline (creatures). */
	bool round = false;
	/** Turned fully to the viewer's eyes rather than standing upright (spells, fire, explosions). */
	bool facesCamera = false;
};

/** How far an upright creature bulges towards the viewer per art pixel of distance to its outline. */
const float ReliefPerPixel = Knob("DEVILUTIONX_VR_SPRITE_RELIEF", 0.6F) * TileSize / PixelsPerTileH;

/** The light nearest to `at` that reaches it, as the game has its lights (and the player's at the head). False when none does. */
bool NearestLight(const Scene &scene, Vec3 at, Vec3 &light)
{
	float best = 1e9F;
	const int ownLight = MyPlayer != nullptr ? MyPlayer->lightId : -1;
	for (int i = 0; i < ActiveLightCount; i++) {
		const int id = ActiveLights[i];
		const devilution::Light &source = Lights[id];
		if (source.isInvalid || source.radius == 0)
			continue;
		const Vec3 position = id == ownLight
		    ? Vec3 { scene.headPosition.x, std::max(scene.headPosition.y - 0.35F, 0.6F), scene.headPosition.z }
		    : Vec3 { (static_cast<float>(source.position.tile.x) + 0.5F + static_cast<float>(source.position.offset.deltaX) / 8) * TileSize, 1.4F,
		          (static_cast<float>(source.position.tile.y) + 0.5F + static_cast<float>(source.position.offset.deltaY) / 8) * TileSize };
		const float away = Length(Vec3 { position.x - at.x, 0, position.z - at.z });
		if (away < best && away < static_cast<float>(source.radius) * TileSize) {
			best = away;
			light = position;
		}
	}
	return best < 1e9F;
}

/**
 * A creature's own silhouette lying on the floor, thrown away from the nearest light: as long as
 * the light is low and close, dark where the painted body is. Replaces the shadow painted into the
 * art (which only fits the art's one light, straight overhead).
 */
void AddCastShadow(Scene &scene, const SpriteSlot &slot, Vec3 foot, float halfWidth, float height)
{
	Vec3 light;
	if (!NearestLight(scene, foot, light))
		return;
	Vec3 away { foot.x - light.x, 0, foot.z - light.z };
	const float distance = Length(away);
	if (distance < 0.05F)
		return;
	away = away * (1 / distance);
	// Similar triangles: a body of this height under a light this high and this far away.
	const float length = std::clamp(height * distance / std::max(light.y - height * 0.5F, 0.4F), height * 0.35F, height * 2.5F);
	const Vec3 across { away.z, 0, -away.x };
	SpriteInstance instance {};
	SetVec(instance.position, foot + Vec3 { 0, 0.012F, 0 });
	SetVec(instance.axisU, across * halfWidth);
	SetVec(instance.axisV, away * length);
	instance.flat = 1;
	instance.uv[0] = slot.x;
	instance.uv[1] = slot.y;
	instance.uv[2] = static_cast<float>(slot.x + slot.width);
	instance.uv[3] = static_cast<float>(slot.y + slot.height);
	// Black, half covering: the floor shows through as much as the game's own shadows let it.
	instance.tint[3] = Knob("DEVILUTIONX_VR_SHADOW_STRENGTH", 0.55F);
	scene.sprites.push_back(instance);
}

/**
 * Adds a sprite standing upright and facing the viewer.
 * @param tile the tile the 2D renderer anchors the sprite to
 * @param offset the 2D renderer's offset of the frame's bottom-left corner from the tile's left/bottom corner
 * @return the sprite's slot, or null when it could not be drawn
 */
const SpriteSlot *AddBillboard(Scene &scene, ClxSprite sprite, Point tile, Displacement offset, const SpriteOptions &options = {}, Vec3 glide = {})
{
	const SpriteSlot *slot = options.art ? CacheSpriteArt(sprite, options.trn) : CacheSprite(sprite, options.trn);
	if (slot == nullptr)
		return nullptr;
	const float metersH = TileSize / PixelsPerTileH;
	const float metersV = TileSize / PixelsPerTileV;
	// Where the frame's bottom centre stands on the ground.
	const Vec3 foot = TileToWorld(tile, static_cast<float>(offset.deltaX) + static_cast<float>(slot->frameWidth) / 2 - TILE_WIDTH / 2, static_cast<float>(offset.deltaY)) + glide;
	const Vec3 right = FacingRight(scene, foot);
	// Only the painted part of the frame is in the atlas: place that part.
	const float shift = (slot->left + slot->artWidth / 2 - static_cast<float>(slot->frameWidth) / 2) * metersH;
	const float bottom = (static_cast<float>(slot->frameHeight) - slot->top - slot->artHeight - SpriteFootPixels) * metersV;
	const Vec3 base = foot + right * shift + Vec3 { 0, bottom, 0 };
	const float halfWidth = slot->artWidth * metersH / 2;
	const float height = slot->artHeight * metersV;

	SpriteInstance instance {};
	if (options.facesCamera) {
		// Turned to the eyes about its middle: a flash or a fireball is round from every side.
		const Vec3 middle = base + Vec3 { 0, height / 2, 0 };
		SetVec(instance.position, middle - scene.headUp * (height / 2));
		SetVec(instance.axisU, scene.headRight * halfWidth);
		SetVec(instance.axisV, scene.headUp * height);
	} else {
		SetVec(instance.position, base);
		SetVec(instance.axisU, right * halfWidth);
		SetVec(instance.axisV, { 0, height, 0 });
	}
	instance.emissive = options.emissive;
	instance.relief = options.round ? ReliefPerPixel : 0;
	instance.uv[0] = slot->x;
	instance.uv[1] = slot->y;
	instance.uv[2] = static_cast<float>(slot->x + slot->width);
	instance.uv[3] = static_cast<float>(slot->y + slot->height);
	instance.tint[0] = instance.tint[1] = instance.tint[2] = instance.tint[3] = 1;
	scene.sprites.push_back(instance);
	if (options.shadow)
		AddCastShadow(scene, *slot, foot + right * shift, halfWidth, height + bottom);
	if (options.pickable)
		pickables.push_back({ base, right, halfWidth, height, tile });
	return slot;
}

/**
 * How far each door has swung open, in radians (0 shut, a quarter turn open): doors swing to where
 * the game says they are instead of jumping there.
 */
std::unordered_map<const Object *, float> doorSwing;
/** Seconds since the scene was last updated, for what moves by itself (a swinging door). */
float frameStep;

/**
 * A door: a slab 6 cm thick with the door's picture on both faces, hanging on a hinge at one end
 * of its doorway and swinging a quarter turn to open. The art shows the leaf from the game's one
 * camera, so its lower edge runs at the 2:1 slope of a tile edge; laid on a sheared quad that
 * slope comes out level and the leaf stands upright. False when the door stands in no doorway.
 */
bool AddDoor(Scene &scene, const Object &object, ClxSprite sprite)
{
	const int axis = LevelDoorwayAxis(object.position);
	if (axis == 0)
		return false;
	// The shut picture is the one the slab wears; an open door's frame shows the leaf edge-on.
	const SpriteSlot *slot = CacheSprite(object._oAnimData ? (*object._oAnimData)[0] : sprite);
	if (slot == nullptr)
		return false;
	const bool open = object._oVar4 != 0;
	// Shut, the leaf lies along y when the way through runs along x.
	const bool alongY = axis == 1;
	// To the right in the picture is -y for such a leaf (rising 1 px per 2), +x for the other (falling).
	const Vec3 along = alongY ? Vec3 { 0, 0, -1 } : Vec3 { 1, 0, 0 };
	const float slope = alongY ? -0.5F : 0.5F;
	const float halfPixels = slot->artWidth / 2;
	const float metersV = TileSize / PixelsPerTileV;
	const float halfWidth = halfPixels * TileSize / 32;
	const float height = slot->artHeight * metersV;

	// Swing towards where the game has the door: a quarter turn in about half a second.
	const float target = open ? 1.5707963F : 0.F;
	auto [entry, added] = doorSwing.try_emplace(&object, target);
	float &swing = entry->second;
	const float step = 3.2F * frameStep;
	swing = swing < target ? std::min(swing + step, target) : std::max(swing - step, target);

	Vec3 middle = TileToWorld(object.position, 0, 0);
	// The picture's lowest corner stands on the floor.
	middle.y = 0.5F * halfPixels * metersV;
	// The hinge is at the leaf's +along end; open, the leaf has turned about it into the room on the
	// +x (or +z) side, where the game's open picture shows it lying against the jamb.
	const Vec3 hinge = middle + along * halfWidth;
	const Vec3 side = alongY ? Vec3 { 1, 0, 0 } : Vec3 { 0, 0, 1 };
	// Turning `along` by the swing about the vertical, the way that brings the free end to `side`.
	const Vec3 normal { along.z, 0, -along.x };
	const float sign = Dot(normal, side) > 0 ? 1.F : -1.F;
	const Vec3 turned = along * std::cos(swing) - normal * (sign * std::sin(swing));
	const Vec3 facing { turned.z, 0, -turned.x };
	const Vec3 base = hinge - turned * halfWidth;

	// The leaf modelled in Blender from this picture (the door sheet's `.crv`): pixels across, up and
	// through the leaf, its origin the middle of its foot.
	{
		std::string path;
		int frame;
		const ClxSprite shut = object._oAnimData ? (*object._oAnimData)[0] : sprite;
		const float yaw = std::atan2(-turned.z, turned.x);
		if (SpriteOrigin(shut, path, frame)
		    && CarvedAddRecord(scene, "doors/" + path.substr(path.find_last_of('/') + 1), frame * 2 + (alongY ? 0 : 1),
		        Compose({ base.x, 0, base.z }, AxisAngle({ 0, 1, 0 }, yaw), { TileSize / 32, metersV, 0.02F }))) {
			pickables.push_back({ { base.x, 0, base.z }, turned, halfWidth, height, object.position });
			return true;
		}
	}
	// The two faces, 3 cm either side of the slab's middle, each with the picture.
	for (const float offset : { 0.03F, -0.03F }) {
		SpriteInstance instance {};
		SetVec(instance.position, base + facing * offset);
		SetVec(instance.axisU, turned * halfWidth + Vec3 { 0, slope * halfPixels * metersV, 0 });
		SetVec(instance.axisV, { 0, height, 0 });
		instance.uv[0] = slot->x;
		instance.uv[1] = slot->y;
		instance.uv[2] = static_cast<float>(slot->x + slot->width);
		instance.uv[3] = static_cast<float>(slot->y + slot->height);
		instance.tint[0] = instance.tint[1] = instance.tint[2] = instance.tint[3] = 1;
		scene.sprites.push_back(instance);
	}
	// The slab between them: dark wood, a little inside the picture so its edges show as the leaf's thickness.
	MeshInstance slab {};
	const Vec3 centre = base + Vec3 { 0, height * 0.5F - std::fabs(slope) * halfPixels * metersV * 0.5F, 0 };
	const float yaw = std::atan2(-turned.z, turned.x);
	slab.world = Compose(centre, AxisAngle({ 0, 1, 0 }, yaw), { halfWidth * 1.94F, height * 0.92F, 0.058F });
	slab.color[0] = 0.045F;
	slab.color[1] = 0.03F;
	slab.color[2] = 0.018F;
	slab.color[3] = 1;
	slab.boneBase = -1;
	slab.specular = 0;
	slab.lit = 1;
	scene.meshes.push_back({ &cube, slab, true });
	pickables.push_back({ { base.x, 0, base.z }, turned, halfWidth, height, object.position });
	return true;
}

/** Beyond the fog that ends the view. */
bool OutOfView(const Scene &scene, Point tile)
{
	const float dx = (static_cast<float>(tile.x) + 0.5F) * TileSize - scene.headPosition.x;
	const float dz = (static_cast<float>(tile.y) + 0.5F) * TileSize - scene.headPosition.z;
	return dx * dx + dz * dz > ViewDistance * ViewDistance;
}

/**
 * Which of an actor's eight pre-rendered directions to show. The art was drawn for a camera
 * standing to the south, so "facing south" means "facing the viewer": the shown direction is
 * the actor's real one relative to where the viewer stands.
 */
Direction ApparentDirection(const Scene &scene, Direction facing, Point tile)
{
	const float dx = scene.headPosition.x / TileSize - (static_cast<float>(tile.x) + 0.5F);
	const float dy = scene.headPosition.z / TileSize - (static_cast<float>(tile.y) + 0.5F);
	// Direction from the actor to the viewer in the game's numbering (0 = south, clockwise).
	const int toViewer = static_cast<int>(std::lround(std::atan2(-(dx - dy), dx + dy) / 0.7853982F));
	return static_cast<Direction>(((static_cast<int>(facing) - toViewer) % 8 + 8) % 8);
}



/** The tile a walking actor's sprite is anchored to, mirroring the 2D renderer. */
template <typename Actor>
Point WalkAnchor(const Actor &actor, bool southwards, bool sidewaysEast, Direction direction)
{
	Point tile = actor.position.tile;
	if (southwards || sidewaysEast)
		tile += Opposite(direction);
	return tile;
}

/** The stand-in figure, posed from what the actor is doing. */
void AddFigure(Scene &scene, Vec3 position, Direction facing, Figure::Action action, float progress, float heightMeters, const float color[3])
{
	const Displacement step { facing };
	MeshInstance instance {};
	const float scale = std::clamp(heightMeters / 1.8F, 0.5F, 2.4F);
	instance.world = Compose(position, AxisAngle({ 0, 1, 0 }, std::atan2(static_cast<float>(step.deltaX), static_cast<float>(step.deltaY))), { scale, scale, scale });
	const float peak = std::max(std::max(color[0], color[1]), std::max(color[2], 0.02F));
	for (int c = 0; c < 3; c++)
		instance.color[c] = color[c] / peak * 0.55F;
	instance.boneBase = static_cast<float>(scene.bones.size());
	instance.specular = 0.3F;
	instance.lit = 1;
	const Clip &clip = figure.clips[action];
	figure.skeleton.Pose(clip, clip.loops ? scene.time : progress * clip.duration, scene.bones);
	scene.meshes.push_back({ &figure.mesh, instance, true });
	AddBlobShadow(scene, position, 0.4F * scale);
}

float Progress(const AnimationInfo &animation)
{
	return animation.numberOfFrames > 1 ? static_cast<float>(animation.currentFrame) / static_cast<float>(animation.numberOfFrames - 1) : 0;
}

/**
 * Monsters glide: the game moves them from tile to tile in eight directions, starting and stopping
 * dead at each. What is drawn follows that path a moment behind and never jumps to it, so a
 * monster eases into a walk, rounds its corners and settles to a stop.
 */
const float MonsterGlide = Knob("DEVILUTIONX_VR_MONSTER_GLIDE", 0.11F);
struct Glider {
	Vec3 at;
	float seen;
};
std::unordered_map<const Monster *, Glider> gliders;

/** How far from where the game has it the monster is drawn. */
Vec3 GlideShift(const Scene &scene, const Monster &monster, Vec3 logical)
{
	if (MonsterGlide <= 0)
		return {};
	Glider &glider = gliders[&monster];
	const float dt = scene.time - glider.seen;
	const Vec3 behind { logical.x - glider.at.x, 0, logical.z - glider.at.z };
	// Not seen for a while, or moved further than a walk can (teleport, a new level): it is simply there.
	if (glider.seen == 0 || dt < 0 || dt > 0.5F || Length(behind) > 2.5F * TileSize) {
		glider.at = logical;
	} else {
		const float follow = 1 - std::exp(-dt / MonsterGlide);
		glider.at.x += behind.x * follow;
		glider.at.z += behind.z * follow;
	}
	glider.seen = scene.time;
	return { glider.at.x - logical.x, 0, glider.at.z - logical.z };
}

void AddMonster(Scene &scene, const Monster &monster)
{
	if (OutOfView(scene, monster.position.tile))
		return;
	if ((monster.flags & MFLAG_HIDDEN) != 0 || (!IsTileLit(monster.position.tile) && !ShowUnlitMonsters) || !monster.animInfo.sprites)
		return;
	ClxSprite sprite = monster.animInfo.currentSprite();
	const Direction apparent = ApparentDirection(scene, monster.direction, monster.position.tile);
	if (apparent != monster.direction) {
		// Find which animation is playing, then take the same frame from the other direction.
		for (int graphic = 0; graphic <= static_cast<int>(MonsterGraphic::Special); graphic++) {
			const auto &animation = monster.type().getAnimData(static_cast<MonsterGraphic>(graphic));
			const OptionalClxSpriteList current = animation.spritesForDirection(monster.direction);
			if (!current || current->data() != monster.animInfo.sprites->data())
				continue;
			const OptionalClxSpriteList turned = animation.spritesForDirection(apparent);
			const auto frame = static_cast<uint32_t>(monster.animInfo.getFrameToUseForRendering());
			if (turned && frame < turned->numSprites())
				sprite = (*turned)[frame];
			break;
		}
	}
	const bool southwards = monster.mode == MonsterMode::MoveSouthwards;
	const bool sidewaysEast = monster.mode == MonsterMode::MoveSideways && monster.direction == Direction::East;
	const Point anchor = WalkAnchor(monster, southwards, sidewaysEast, monster.direction);
	const Displacement offset = monster.getRenderingOffset(sprite);
	SpriteOptions options;
	options.trn = monster.uniqueMonsterTRN.get();
	options.shadow = monster.mode != MonsterMode::Death;
	options.art = true;
	options.round = true;
	Displacement walked {};
	if (monster.isWalking())
		walked = GetOffsetForWalking(monster.animInfo, monster.direction);
	const Vec3 glide = monster.mode == MonsterMode::Death ? Vec3 {} : GlideShift(scene, monster, TileToWorld(anchor, static_cast<float>(walked.deltaX), static_cast<float>(walked.deltaY)));
	if (!UseFigures) {
		AddBillboard(scene, sprite, anchor, offset, options, glide);
		return;
	}
	const SpriteSlot *slot = CacheSprite(sprite, options.trn);
	if (slot == nullptr)
		return;
	Figure::Action action = Figure::Idle;
	switch (monster.mode) {
	case MonsterMode::MoveNorthwards:
	case MonsterMode::MoveSouthwards:
	case MonsterMode::MoveSideways:
	case MonsterMode::Charge:
		action = Figure::Walk;
		break;
	case MonsterMode::MeleeAttack:
	case MonsterMode::SpecialMeleeAttack:
	case MonsterMode::RangedAttack:
	case MonsterMode::SpecialRangedAttack:
		action = Figure::Attack;
		break;
	case MonsterMode::HitRecovery:
		action = Figure::Hit;
		break;
	case MonsterMode::Death:
		action = Figure::Death;
		break;
	default:
		break;
	}
	const Vec3 foot = TileToWorld(anchor, static_cast<float>(offset.deltaX) + static_cast<float>(slot->frameWidth) / 2 - TILE_WIDTH / 2, static_cast<float>(offset.deltaY)) + glide;
	// A monster is as tall as its standing sprite is painted; a lying one keeps a default.
	const float height = monster.mode == MonsterMode::Death ? 1.7F : slot->artHeight / PixelsPerTileV * TileSize * 0.85F;
	AddFigure(scene, foot, monster.direction, action, Progress(monster.animInfo), height, slot->color);
	pickables.push_back({ foot, FacingRight(scene, foot), 0.4F, height, monster.position.tile });
}

/** A dead monster: its last death frame, laid flat on the floor where the art shows it lying. */
void AddCorpse(Scene &scene, int x, int y)
{
	const int8_t code = dCorpse[x][y];
	const Corpse &corpse = Corpses[(code & 0x1F) - 1];
	const ClxSpriteList sprites = corpse.spritesForDirection(static_cast<Direction>((code >> 5) & 7));
	if (corpse.frame < 0 || static_cast<uint32_t>(corpse.frame) >= sprites.numSprites())
		return;
	const uint8_t *trn = corpse.translationPaletteIndex != 0 ? Monsters[corpse.translationPaletteIndex - 1].uniqueMonsterTRN.get() : nullptr;
	const SpriteSlot *slot = CacheSprite(sprites[corpse.frame], trn);
	if (slot == nullptr)
		return;
	// Ground distances in the art: 45.25 px per tile across the screen, half that into it.
	const float metersAcross = TileSize / PixelsPerTileH;
	const float metersInto = TileSize / (PixelsPerTileH / 2);
	const Vec3 across { 0.7071F, 0, -0.7071F }; // Screen right on the ground.
	const Vec3 into { -0.7071F, 0, -0.7071F };  // Screen up on the ground.
	// The frame's bottom edge passes through the tile's southern corner.
	const Vec3 corner { static_cast<float>(x + 1) * TileSize, 0.02F, static_cast<float>(y + 1) * TileSize };
	const float shift = (slot->left + slot->artWidth / 2 - static_cast<float>(slot->frameWidth) / 2) * metersAcross;
	const float nearEdge = (static_cast<float>(slot->frameHeight) - slot->top - slot->artHeight) * metersInto;
	SpriteInstance instance {};
	SetVec(instance.position, corner + across * shift + into * nearEdge);
	SetVec(instance.axisU, across * (slot->artWidth * metersAcross / 2));
	SetVec(instance.axisV, into * (slot->artHeight * metersInto));
	instance.flat = 1;
	instance.uv[0] = slot->x;
	instance.uv[1] = slot->y;
	instance.uv[2] = static_cast<float>(slot->x + slot->width);
	instance.uv[3] = static_cast<float>(slot->y + slot->height);
	instance.tint[0] = instance.tint[1] = instance.tint[2] = instance.tint[3] = 1;
	scene.sprites.push_back(instance);
}

Quat FromTo(Vec3 from, Vec3 to)
{
	const Vec3 axis = Cross(from, to);
	const float cosine = Dot(from, to);
	if (Length(axis) < 1e-5F)
		return cosine > 0 ? IdentityQuat : AxisAngle({ 0, 0, 1 }, 3.14159265F);
	return AxisAngle(Normalize(axis), std::acos(std::clamp(cosine, -1.F, 1.F)));
}

/**
 * What the player holds: the model carved from the item's inventory picture
 * (`vrassets/carve/icons/objcurs_<nnn>.crv`) when there is one, and that picture itself, flat in
 * the hand, when there is not.
 */
void AddHeldItem(Scene &scene, const Item &item, const VrHandPose &hand)
{
	if (item.isEmpty() || !hand.valid)
		return;
	switch (item._itype) {
	case ItemType::Sword:
	case ItemType::Axe:
	case ItemType::Mace:
	case ItemType::Staff:
	case ItemType::Bow:
	case ItemType::Shield:
		break;
	default:
		return;
	}
	const bool shield = item._itype == ItemType::Shield;
	const int cursor = item._iCurs + CURSOR_FIRSTITEM;
	const SpriteSlot *slot = CacheSprite(GetInvItemSprite(cursor));
	if (slot == nullptr)
		return;
	const Quat orientation { hand.orientation[0], hand.orientation[1], hand.orientation[2], hand.orientation[3] };
	const Vec3 position { hand.position[0], hand.position[1], hand.position[2] };
	// The hand's pose: x right, y up, -z along the pointing ray (away from the player).
	// A shield's painted front (the picture's +z) faces away from the player, its back lies on the
	// forearm: the picture is turned half round about its up. A weapon stands up out of the fist
	// with its edge forward and its flats to the left and right: the picture is turned a quarter
	// round about its up, so the picture's width runs along the pointing ray and its faces sideways.
	const Quat held = shield
	    ? orientation * AxisAngle({ 0, 1, 0 }, 3.14159265F)
	    : orientation * AxisAngle({ 1, 0, 0 }, -(90 - WeaponTilt) * 0.01745329F) * AxisAngle({ 0, 1, 0 }, 1.5707963F);
	const Vec3 across = Rotate(held, { 1, 0, 0 });
	const Vec3 along = Rotate(held, { 0, 1, 0 });
	const float height = slot->artHeight * IconMeters;
	// The art has the grip at the lower end; a shield is held by its middle.
	const Vec3 base = position - along * (height * (shield ? 0.5F : 0.14F));
	// Built from the inventory picture: its origin is where the hand holds it;
	// a shield's is the middle of its back, which rests a few centimetres in front of the fist.
	const Vec3 forward = Rotate(orientation, { 0, 0, -1 });
	if (CarvedAddItem(scene, cursor - 1, Compose(shield ? position + forward * 0.04F : position, held), IconMeters, 0.02F))
		return;
	SpriteInstance instance {};
	SetVec(instance.position, base);
	SetVec(instance.axisU, across * (slot->artWidth * IconMeters / 2));
	SetVec(instance.axisV, along * height);
	instance.uv[0] = slot->x;
	instance.uv[1] = slot->y;
	instance.uv[2] = static_cast<float>(slot->x + slot->width);
	instance.uv[3] = static_cast<float>(slot->y + slot->height);
	instance.tint[0] = instance.tint[1] = instance.tint[2] = instance.tint[3] = 1;
	scene.sprites.push_back(instance);
}

/**
 * A plain dark gauntlet closed round a grip, in the hand's pose (x right, y up, -z along the
 * pointing ray): the back of the hand, the curled fingers in front of it, the thumb over them and
 * a cuff behind. Low-poly boxes; the left hand is the right one mirrored.
 */
Mesh BuildGauntlet(bool left)
{
	MeshBuilder gauntlet;
	const float side = left ? -1.F : 1.F;
	const auto box = [&](Vec3 centre, Vec3 half, float yaw = 0) {
		gauntlet.Box(Compose({ centre.x * side, centre.y, centre.z }, AxisAngle({ 0, 1, 0 }, yaw * side), { 1, 1, 1 }), half, 0x00FFFFFF);
	};
	box({ 0.F, -0.012F, 0.035F }, { 0.042F, 0.042F, 0.048F });   // the hand
	box({ 0.F, -0.016F, -0.02F }, { 0.04F, 0.034F, 0.02F });     // the curled fingers
	box({ -0.03F, 0.026F, -0.005F }, { 0.014F, 0.011F, 0.03F }, 0.35F); // the thumb over them
	box({ 0.F, -0.012F, 0.11F }, { 0.046F, 0.046F, 0.04F });      // the cuff
	return gauntlet.Build();
}

void AddGauntlet(Scene &scene, const VrHandPose &hand, int index)
{
	if (!hand.valid || gauntlets[index].indexCount == 0)
		return;
	const Quat orientation { hand.orientation[0], hand.orientation[1], hand.orientation[2], hand.orientation[3] };
	MeshInstance instance {};
	instance.world = Compose({ hand.position[0], hand.position[1], hand.position[2] }, orientation);
	// Dark worn leather and iron: never brighter than what the game's light makes of it.
	instance.color[0] = 0.055F;
	instance.color[1] = 0.048F;
	instance.color[2] = 0.042F;
	instance.color[3] = 1;
	instance.boneBase = -1;
	instance.specular = 0.04F;
	instance.lit = 1;
	scene.meshes.push_back({ &gauntlets[index], instance, true });
}

/** An arrow in flight: one small mesh along its flight, coloured like its sprite. */
bool AddArrow(Scene &scene, const Missile &missile, ClxSprite sprite)
{
	if (arrow.indexCount == 0)
		return false;
	const Displacement velocity = missile.position.velocity;
	// Screen x right, y down -> tiles: one tile along x is 32 px right and 16 down, along y 32 left and 16 down.
	const float sx = static_cast<float>(velocity.deltaX);
	const float sy = static_cast<float>(velocity.deltaY);
	Vec3 along { (sx / 32 + sy / 16) / 2, 0, (sy / 16 - sx / 32) / 2 };
	if (Length(along) < 1e-6F)
		return false;
	along = Normalize(along);
	const Displacement offset = missile.position.offsetForRendering;
	Vec3 at = TileToWorld(missile.position.tile, static_cast<float>(offset.deltaX), static_cast<float>(offset.deltaY));
	at.y = 1.0F;
	MeshInstance instance {};
	instance.world = Compose(at, FromTo({ 0, 0, -1 }, along));
	const SpriteSlot *slot = CacheSprite(sprite);
	for (int c = 0; c < 3; c++)
		instance.color[c] = slot != nullptr ? std::max(slot->color[c], 0.02F) : 0.08F;
	instance.color[3] = 1;
	instance.boneBase = -1;
	instance.specular = 0.05F;
	instance.lit = 1;
	scene.meshes.push_back({ &arrow, instance, true });
	return true;
}

/** A vial strapped across the back of the wrist, filled to `fraction` with a self-lit liquid. */
void AddVial(Scene &scene, const VrHandPose &hand, float fraction, Vec3 color)
{
	if (!hand.valid || vialGlass.indexCount == 0)
		return;
	const Quat orientation { hand.orientation[0], hand.orientation[1], hand.orientation[2], hand.orientation[3] };
	// Across the wrist: local y along the hand's x, on top of the cuff.
	const Quat across = orientation * AxisAngle({ 0, 0, 1 }, -1.5707963F);
	const Vec3 at = Vec3 { hand.position[0], hand.position[1], hand.position[2] } + Rotate(orientation, { -0.035F, 0.045F, 0.1F });
	MeshInstance glass {};
	glass.world = Compose(at, across);
	glass.color[0] = 0.05F;
	glass.color[1] = 0.05F;
	glass.color[2] = 0.055F;
	glass.color[3] = 1;
	glass.boneBase = -1;
	glass.lit = 1;
	scene.meshes.push_back({ &vialGlass, glass, true });
	const float level = std::clamp(fraction, 0.F, 1.F);
	if (level <= 0.01F)
		return;
	MeshInstance fill {};
	fill.world = Compose(at + Rotate(across, { 0, 0.002F, 0 }), across, { 1.08F, 0.066F * level, 1.08F });
	fill.color[0] = color.x;
	fill.color[1] = color.y;
	fill.color[2] = color.z;
	fill.color[3] = 1;
	fill.boneBase = -1;
	// The orbs glow in the game: the liquid is self-lit.
	fill.lit = 0;
	scene.meshes.push_back({ &vialFill, fill, true });
}

void AddPanelPart(Scene &scene, Vec3 centre, Quat orientation, Vec3 size, Vec3 color, float lit = 1)
{
	MeshInstance part {};
	part.world = Compose(centre, orientation, size);
	part.color[0] = color.x;
	part.color[1] = color.y;
	part.color[2] = color.z;
	part.color[3] = 1;
	part.boneBase = -1;
	part.specular = 0;
	part.lit = lit;
	scene.meshes.push_back({ &cube, part, true });
}

/**
 * What holds a panel the player has taken out (vr.cpp HeldPanel): the leather satchel round the
 * backpack's grid, with its flap thrown back; the tome's covers, spine and the edge of its pages;
 * the parchment the automap is drawn on, rolled at both ends. The game's own panel is the quad
 * in front, drawn over all of it.
 */
void AddHeldPanel(Scene &scene)
{
	if (heldPanel.kind == 0 || heldPanel.width <= 0)
		return;
	const Quat o { heldPanel.orientation[0], heldPanel.orientation[1], heldPanel.orientation[2], heldPanel.orientation[3] };
	const Vec3 at { heldPanel.position[0], heldPanel.position[1], heldPanel.position[2] };
	const Vec3 right = Rotate(o, { 1, 0, 0 });
	const Vec3 up = Rotate(o, { 0, 1, 0 });
	const Vec3 front = Rotate(o, { 0, 0, 1 });
	const float w = heldPanel.width;
	const float h = heldPanel.height;
	switch (heldPanel.kind) {
	case 1: {
		const Vec3 leather { 0.09F, 0.05F, 0.025F };
		AddPanelPart(scene, at - front * 0.055F, o, { w + 0.05F, h + 0.05F, 0.1F }, leather);
		const Quat flap = o * AxisAngle({ 1, 0, 0 }, -0.6F);
		const Vec3 hinge = at + up * (h * 0.5F + 0.025F) - front * 0.006F;
		AddPanelPart(scene, hinge + Rotate(flap, { 0, h * 0.35F, 0 }), flap, { w + 0.05F, h * 0.7F, 0.008F }, leather * 0.8F);
		// The strap, up from the bag's back towards the other shoulder.
		AddPanelPart(scene, at - front * 0.1F + up * (h * 0.5F + 0.2F) - right * 0.05F, o * AxisAngle({ 0, 0, 1 }, 0.35F), { 0.035F, 0.4F, 0.006F }, leather * 0.7F);
		break;
	}
	case 2: {
		const Vec3 cover { 0.13F, 0.025F, 0.02F };
		AddPanelPart(scene, at - front * 0.02F, o, { w + 0.03F, h + 0.03F, 0.012F }, cover);
		AddPanelPart(scene, at - front * 0.008F, o, { w * 0.985F, h * 0.985F, 0.014F }, { 0.5F, 0.44F, 0.33F });
		AddPanelPart(scene, at - right * (w * 0.5F + 0.012F) - front * 0.012F, o, { 0.024F, h + 0.03F, 0.034F }, cover * 0.8F);
		break;
	}
	case 3: {
		// Self-lit: the map is read in the dark, as the game draws it over the dark.
		const Vec3 parchment { 0.36F, 0.29F, 0.17F };
		AddPanelPart(scene, at - front * 0.003F, o, { w + 0.03F, h + 0.03F, 0.002F }, parchment, 0);
		AddPanelPart(scene, at + up * (h * 0.5F + 0.02F) - front * 0.012F, o, { w + 0.07F, 0.035F, 0.035F }, parchment * 0.8F, 0);
		AddPanelPart(scene, at - up * (h * 0.5F + 0.02F) - front * 0.012F, o, { w + 0.07F, 0.035F, 0.035F }, parchment * 0.8F, 0);
		break;
	}
	default:
		break;
	}
}

/** The eight belt potions and scrolls, carved from their pictures, hanging round the waist in front. */
void AddBelt(Scene &scene)
{
	const Vec3 right = Normalize(Vec3 { scene.headRight.x, 0, scene.headRight.z });
	const Vec3 forward { right.z, 0, -right.x };
	const Vec3 waist = scene.headPosition + Vec3 { 0, -0.68F, 0 };
	for (int slot = 0; slot < MaxBeltItems; slot++) {
		const Item &item = MyPlayer->SpdList[slot];
		if (item.isEmpty())
			continue;
		// Left to right in the game's order, round the front of the body.
		const float angle = (static_cast<float>(slot) - 3.5F) * 0.3F;
		const Vec3 out = forward * std::cos(angle) + right * std::sin(angle);
		const Vec3 at = waist + out * 0.27F;
		const float yaw = std::atan2(-out.z, out.x) - 1.5707963F;
		const int icon = item._iCurs + CURSOR_FIRSTITEM - 1;
		CarvedAddItem(scene, icon, Compose(at, AxisAngle({ 0, 1, 0 }, yaw)), IconMeters * 0.8F, 0.02F);
	}
}

/** The game's large picture of a spell (with its type's colours), stored in the atlas once per atlas epoch. */
const SpriteSlot *SpellPicture(SpellID spell, SpellType type)
{
	const int key = static_cast<int>(spell) * 8 + static_cast<int>(type);
	SpellPictureSlot &picture = spellPictures[key];
	if (picture.epoch == SpritesEpoch() && picture.epoch != 0)
		return &picture.slot;
	static OwnedSurface surface { SPLICONLENGTH, SPLICONLENGTH };
	SetSpellTrans(type);
	DrawLargeSpellIcon(surface, { 0, SPLICONLENGTH - 1 }, spell);
	static std::vector<uint8_t> pairs;
	pairs.resize(static_cast<size_t>(SPLICONLENGTH) * SPLICONLENGTH * 2);
	for (int y = 0; y < SPLICONLENGTH; y++) {
		for (int x = 0; x < SPLICONLENGTH; x++) {
			pairs[(static_cast<size_t>(y) * SPLICONLENGTH + x) * 2] = *surface.at(x, y);
			pairs[(static_cast<size_t>(y) * SPLICONLENGTH + x) * 2 + 1] = 255;
		}
	}
	const SpriteSlot *slot = CacheImage(pairs.data(), SPLICONLENGTH, SPLICONLENGTH);
	if (slot == nullptr)
		return nullptr;
	picture.slot = *slot;
	picture.epoch = SpritesEpoch();
	return &picture.slot;
}

void AddIcon(Scene &scene, const SpriteSlot &slot, Vec3 centre, Vec3 right, Vec3 up, float size, float glow)
{
	SpriteInstance instance {};
	SetVec(instance.position, centre - up * (size / 2));
	SetVec(instance.axisU, right * (size / 2));
	SetVec(instance.axisV, up * size);
	instance.emissive = glow;
	instance.uv[0] = slot.x;
	instance.uv[1] = slot.y;
	instance.uv[2] = static_cast<float>(slot.x + slot.width);
	instance.uv[3] = static_cast<float>(slot.y + slot.height);
	instance.tint[0] = instance.tint[1] = instance.tint[2] = instance.tint[3] = 1;
	scene.sprites.push_back(instance);
}

/**
 * The casting hand: the spell it holds ready shows over its palm; while the speedbook is held open
 * the spells it offers stand in a ring round the hand, the chosen one larger and brighter.
 */
void AddSpellHand(Scene &scene, const VrHandPose &hand)
{
	if (!hand.valid)
		return;
	const Quat orientation { hand.orientation[0], hand.orientation[1], hand.orientation[2], hand.orientation[3] };
	const Vec3 at { hand.position[0], hand.position[1], hand.position[2] };
	// Turned to the eyes, whatever the hand does.
	const Vec3 right = scene.headRight;
	const Vec3 up = scene.headUp;
	const Player &me = *MyPlayer;
	if (!spellRing.open) {
		if (me._pRSpell == SpellID::Invalid)
			return;
		if (const SpriteSlot *slot = SpellPicture(me._pRSpell, me._pRSplType); slot != nullptr)
			AddIcon(scene, *slot, at + Rotate(orientation, { 0, 0.07F, 0.02F }), right, up, 0.045F, 0.6F);
		return;
	}
	const int count = std::clamp(spellRing.count, 0, 32);
	for (int i = 0; i < count; i++) {
		const float angle = 6.2831853F * static_cast<float>(i) / static_cast<float>(std::max(count, 1));
		const Vec3 centre = at + right * (std::sin(angle) * 0.16F) + up * (std::cos(angle) * 0.16F + 0.05F);
		const bool chosen = i == spellRing.selected;
		if (const SpriteSlot *slot = SpellPicture(static_cast<SpellID>(spellRing.spells[i]), static_cast<SpellType>(spellRing.types[i])); slot != nullptr)
			AddIcon(scene, *slot, centre, right, up, chosen ? 0.07F : 0.045F, chosen ? 1.2F : 0.3F);
	}
}

/** A thin beam from the hand to the aimed point and a mark on what it hits. */
void AddPointer(Scene &scene, const VrHandPose &hand, const float hit[3])
{
	const Vec3 from { hand.position[0], hand.position[1], hand.position[2] };
	const Vec3 to { hit[0], hit[1], hit[2] };
	const float length = Length(to - from);
	if (length < 0.05F)
		return;
	MeshInstance beam {};
	beam.world = Compose((from + to) * 0.5F, FromTo({ 1, 0, 0 }, Normalize(to - from)), { length, 0.004F, 0.004F });
	beam.color[0] = 1.0F;
	beam.color[1] = 0.85F;
	beam.color[2] = 0.6F;
	beam.color[3] = 0.6F;
	beam.boneBase = -1;
	scene.meshes.push_back({ &cube, beam, false });
	// What it hits is marked by the crosshair (feedback.cpp).
}

void AddLight(Scene &scene, Vec3 position, float range, const float color[3], float intensity)
{
	if (scene.lightCount >= MaxLights || range <= 0)
		return;
	Light &light = scene.lights[scene.lightCount++];
	SetVec(light.position, position);
	light.range = range;
	for (int c = 0; c < 3; c++)
		light.color[c] = color[c] * intensity;
	light.shadowed = 1;
}

/**
 * The game's light sources become point lights. Radius and position are the game's; colour is
 * ours: fire-warm by default, and for missiles the colour of the missile itself.
 */
void AddLights(Scene &scene)
{
	const float warm[3] = { 1.0F, 0.74F, 0.48F };
	float colors[MAXLIGHTS][3];
	float heights[MAXLIGHTS];
	bool skip[MAXLIGHTS] = {};
	for (int i = 0; i < MAXLIGHTS; i++) {
		std::memcpy(colors[i], warm, sizeof(warm));
		heights[i] = 1.4F;
	}
	for (const Missile &missile : Missiles) {
		if (missile._mlid < 0 || missile._mlid >= MAXLIGHTS || !missile._miAnimData || missile._miAnimFrame < 1)
			continue;
		if (const SpriteSlot *slot = CacheSprite((*missile._miAnimData)[missile._miAnimFrame - 1]); slot != nullptr) {
			const float peak = std::max(std::max(slot->color[0], slot->color[1]), std::max(slot->color[2], 0.02F));
			for (int c = 0; c < 3; c++)
				colors[missile._mlid][c] = slot->color[c] / peak * 1.3F;
			heights[missile._mlid] = 0.9F;
		}
	}
	// Burning objects are in the lightmap already.
	for (int i = 0; i < ActiveObjectCount; i++) {
		const Object &object = Objects[ActiveObjects[i]];
		// An object without a light has id 0 too, so the light must also sit on the object.
		if (!object.applyLighting && object._olid >= 0 && object._olid < MAXLIGHTS && Lights[object._olid].position.tile == object.position)
			skip[object._olid] = true;
	}
	const int ownLight = MyPlayer->lightId;
	for (int i = 0; i < ActiveLightCount; i++) {
		const int id = ActiveLights[i];
		const devilution::Light &source = Lights[id];
		if (source.isInvalid || skip[id] || source.radius == 0)
			continue;
		// The game's radius is where its light has faded to nothing.
		const float range = static_cast<float>(source.radius) * TileSize;
		if (id == ownLight) {
			// The player's own light is carried at chest height where the head is, so it moves as smoothly as the view.
			AddLight(scene, { scene.headPosition.x, std::max(scene.headPosition.y - 0.35F, 0.6F), scene.headPosition.z }, range, warm, 2.6F);
			continue;
		}
		const Vec3 position {
			(static_cast<float>(source.position.tile.x) + 0.5F + static_cast<float>(source.position.offset.deltaX) / 8) * TileSize, heights[id],
			(static_cast<float>(source.position.tile.y) + 0.5F + static_cast<float>(source.position.offset.deltaY) / 8) * TileSize
		};
		AddLight(scene, position, range, colors[id], 2.2F);
	}
}

} // namespace

VrSpellRing spellRing {};
VrHeldPanel heldPanel {};

Vec3 TileToWorld(Point tile, float screenX, float screenY)
{
	// A step of one tile along x moves 32 px right and 16 px down on screen; along y, 32 px left and 16 px down.
	return {
		(static_cast<float>(tile.x) + 0.5F + (screenX / 32 + screenY / 16) / 2) * TileSize, 0,
		(static_cast<float>(tile.y) + 0.5F + (screenY / 16 - screenX / 32) / 2) * TileSize
	};
}

bool ActorsInit()
{
	BuildFigure(figure);
	cube = BuildCube();
	gauntlets[0] = BuildGauntlet(true);
	gauntlets[1] = BuildGauntlet(false);
	{
		MeshBuilder glass;
		glass.Tube(IdentityMat34, 0.017F, 0.017F, 0.07F, 10, 0x00FFFFFF);
		vialGlass = glass.Build();
		MeshBuilder fill;
		fill.Tube(IdentityMat34, 0.014F, 0.014F, 1.0F, 10, 0x00FFFFFF);
		vialFill = fill.Build();
	}
	{
		MeshBuilder builder;
		builder.Box(Compose({ 0, 0, 0 }, IdentityQuat), { 0.005F, 0.005F, 0.35F }, 0x00FFFFFF);
		builder.Box(Compose({ 0, 0, -0.37F }, AxisAngle({ 0, 0, 1 }, 0.785398F)), { 0.012F, 0.012F, 0.03F }, 0x00FFFFFF);
		builder.Box(Compose({ 0, 0, 0.3F }, IdentityQuat), { 0.025F, 0.001F, 0.04F }, 0x00FFFFFF);
		builder.Box(Compose({ 0, 0, 0.3F }, IdentityQuat), { 0.001F, 0.025F, 0.04F }, 0x00FFFFFF);
		arrow = builder.Build();
	}
	return figure.mesh.indexCount != 0 && cube.indexCount != 0;
}

void ActorsRelease()
{
	figure.mesh = Mesh {};
	cube = Mesh {};
	gauntlets[0] = Mesh {};
	gauntlets[1] = Mesh {};
	vialGlass = Mesh {};
	vialFill = Mesh {};
	spellPictures.clear();
	arrow = Mesh {};
	pickables.clear();
	FeedbackRelease();
}

void ActorsAddToScene(Scene &scene, const VrHandPose &leftHand, const VrHandPose &rightHand, bool pointerValid, const float pointerHit[3])
{
	pickables.clear();
	const float dt = std::clamp(scene.time - lastTime, 0.F, 0.1F);
	lastTime = scene.time;
	frameStep = dt;

	// Weapons go to the right hand and shields to the left, whichever inventory hand slot they sit in.
	const Item &slotLeft = MyPlayer->InvBody[INVLOC_HAND_LEFT];
	const Item &slotRight = MyPlayer->InvBody[INVLOC_HAND_RIGHT];
	const bool leftIsShield = !slotLeft.isEmpty() && slotLeft._itype == ItemType::Shield;
	const Item &mainHand = leftIsShield || slotLeft.isEmpty() ? slotRight : slotLeft;
	const Item &offHand = &mainHand == &slotLeft ? slotRight : slotLeft;
	// DEVILUTIONX_VR_DEBUG_NO_HANDS=1: captures of what stands in the level, without the hands in the way.
	static const bool hands = SDL_getenv("DEVILUTIONX_VR_DEBUG_NO_HANDS") == nullptr;
	if (hands) {
		AddGauntlet(scene, leftHand, 0);
		AddGauntlet(scene, rightHand, 1);
		const Player &me = *MyPlayer;
		// Life on the left wrist, mana on the right, as the game's orbs are on the left and right.
		AddVial(scene, leftHand, me._pMaxHP > 0 ? static_cast<float>(me._pHitPoints) / static_cast<float>(me._pMaxHP) : 0, { 0.42F, 0.02F, 0.02F });
		AddVial(scene, rightHand, me._pMaxMana > 0 ? static_cast<float>(me._pMana) / static_cast<float>(me._pMaxMana) : 0, { 0.03F, 0.05F, 0.45F });
		AddBelt(scene);
		AddSpellHand(scene, leftHand);
		AddHeldPanel(scene);
	}
	if (hands && mainHand._itype != ItemType::Shield)
		AddHeldItem(scene, mainHand, rightHand);
	if (hands)
		AddHeldItem(scene, mainHand._itype == ItemType::Shield ? mainHand : offHand, leftHand);
	if (hands && pointerValid && rightHand.valid)
		AddPointer(scene, rightHand, pointerHit);
	FeedbackAddToScene(scene, hands && pointerValid && rightHand.valid, pointerHit);

	// Blob shadows are particles and must be listed before the sparks that glow over them.
	if (leveltype == DTYPE_TOWN) {
		for (const Towner &towner : Towners) {
			if (OutOfView(scene, towner.position))
				continue;
			SpriteOptions options;
			options.shadow = true;
			options.art = true;
			options.round = true;
			// A townsperson has one view in the art: it is turned to the player, whichever side they stand on.
			if (!UseFigures)
				AddBillboard(scene, towner.currentSprite(), towner.position, towner.getRenderingOffset(), options);
			else if (const SpriteSlot *slot = CacheSprite(towner.currentSprite()); slot != nullptr)
				AddFigure(scene, TileToWorld(towner.position, 0, 0), Direction::South, Figure::Idle, 0, 1.75F, slot->color);
		}
	} else {
		for (size_t i = 0; i < ActiveMonsterCount; i++)
			AddMonster(scene, Monsters[ActiveMonsters[i]]);
		const int reach = static_cast<int>(ViewDistance / TileSize) + 1;
		const int headX = static_cast<int>(scene.headPosition.x / TileSize);
		const int headY = static_cast<int>(scene.headPosition.z / TileSize);
		for (int y = std::max(headY - reach, 0); y < std::min(headY + reach + 1, MAXDUNY); y++) {
			for (int x = std::max(headX - reach, 0); x < std::min(headX + reach + 1, MAXDUNX); x++) {
				if (dCorpse[x][y] != 0 && IsTileLit(Point { x, y }))
					AddCorpse(scene, x, y);
			}
		}
	}

	for (const Player &player : Players) {
		if (&player == MyPlayer || !player.plractive || !player.isOnActiveLevel() || !player.AnimInfo.sprites)
			continue;
		const ClxSprite sprite = player.currentSprite();
		const bool southwards = player._pmode == PM_WALK_SOUTHWARDS;
		const bool sidewaysEast = player._pmode == PM_WALK_SIDEWAYS && player._pdir == Direction::East;
		SpriteOptions options;
		options.shadow = true;
		options.art = true;
		options.round = true;
		AddBillboard(scene, sprite, WalkAnchor(player, southwards, sidewaysEast, player._pdir), player.getRenderingOffset(sprite), options);
	}

	for (int i = 0; i < ActiveItemCount; i++) {
		const Item &item = Items[ActiveItems[i]];
		if (!item.AnimInfo.sprites || !IsTileLit(item.position) || OutOfView(scene, item.position))
			continue;
		const ClxSprite sprite = item.AnimInfo.currentSprite();
		// What lies on the floor is the thing itself: its inventory picture's model
		// on its back, turned a little differently on every tile.
		{
			const Vec3 at = TileToWorld(item.position, 0, 0);
			const float turn = static_cast<float>((item.position.x * 7 + item.position.y * 13) % 12) * 0.5236F;
			const Quat lying = AxisAngle({ 0, 1, 0 }, turn) * AxisAngle({ 1, 0, 0 }, -1.5707963F);
			if (CarvedAddItem(scene, item._iCurs + CURSOR_FIRSTITEM - 1, Compose({ at.x, 0.05F, at.z }, lying), IconMeters, 0.02F))
				continue;
		}
		AddBillboard(scene, sprite, item.position, item.getRenderingOffset(sprite));
	}

	static float emberBudget;
	emberBudget += dt * 14;
	for (int i = 0; i < ActiveObjectCount; i++) {
		const Object &object = Objects[ActiveObjects[i]];
		if (!object._oAnimData || object._oAnimFrame < 1 || OutOfView(scene, object.position))
			continue;
		const ClxSprite sprite = object.currentSprite();
		SpriteOptions options;
		// What the game draws at full brightness burns: it glows, and throws embers.
		const bool burns = !object.applyLighting && leveltype != DTYPE_TOWN;
		options.emissive = burns ? 1.6F : 0;
		if (object.isDoor() && AddDoor(scene, object, sprite))
			continue;
		// A thing carved out of its own sprite is that model; what moves in its animation (a flame,
		// a liquid) stays the game's picture, drawn over it.
		{
			const Displacement at = object.getRenderingOffset(sprite, object.position);
			const Vec3 foot = TileToWorld(object.position, static_cast<float>(at.deltaX) + static_cast<float>(sprite.width()) / 2 - TILE_WIDTH / 2, static_cast<float>(at.deltaY));
			Vec3 size;
			if (CarvedAddObject(scene, sprite, foot, 0, &size, options.emissive)) {
				pickables.push_back({ foot, FacingRight(scene, foot), std::max(size.x, size.z) / 2, size.y, object.position });
				if (burns && emberBudget >= 1 && Length(foot - scene.headPosition) < 16) {
					const float color[3] = { 2.4F, 1.0F, 0.25F };
					ParticlesEmit(foot + Vec3 { 0, size.y * 0.9F, 0 }, color, 1, 0.35F, 1.6F, 0.018F);
				}
				continue;
			}
		}
		const SpriteSlot *slot = AddBillboard(scene, sprite, object.position, object.getRenderingOffset(sprite, object.position), options);
		if (burns && slot != nullptr && emberBudget >= 1) {
			const Vec3 at = TileToWorld(object.position, 0, 0);
			if (Length(at - scene.headPosition) < 16) {
				const float color[3] = { 2.4F, 1.0F, 0.25F };
				ParticlesEmit(at + Vec3 { 0, slot->artHeight / PixelsPerTileV * TileSize * 0.7F, 0 }, color, 1, 0.35F, 1.6F, 0.018F);
			}
		}
	}
	if (emberBudget >= 1)
		emberBudget = 0;

	static float sparkBudget;
	sparkBudget += dt * 40;
	for (const Missile &missile : Missiles) {
		if (!missile._miDrawFlag || !missile._miAnimData || missile._miAnimFrame < 1 || OutOfView(scene, missile.position.tile))
			continue;
		const ClxSprite sprite = (*missile._miAnimData)[missile._miAnimFrame - 1];
		if (missile._mitype == MissileID::Arrow && AddArrow(scene, missile, sprite))
			continue;
		SpriteOptions options;
		options.emissive = 2.2F;
		options.pickable = false;
		options.facesCamera = true;
		const SpriteSlot *slot = AddBillboard(scene, sprite, missile.position.tile, missile.position.offsetForRendering - Displacement { missile._miAnimWidth2, 0 }, options);
		// Missiles that carry a light are the fiery, crackling ones: they shed sparks of their own colour.
		if (slot != nullptr && missile._mlid >= 0 && sparkBudget >= 1) {
			const SpriteInstance &placed = scene.sprites.back();
			const Vec3 center { placed.position[0], placed.position[1] + placed.axisV[1] * 0.5F, placed.position[2] };
			const float peak = std::max(std::max(slot->color[0], slot->color[1]), std::max(slot->color[2], 0.02F));
			const float color[3] = { slot->color[0] / peak * 2.5F, slot->color[1] / peak * 2.5F, slot->color[2] / peak * 2.5F };
			ParticlesEmit(center, color, 1, 0.5F, 0.7F, 0.02F);
		}
	}
	if (sparkBudget >= 1)
		sparkBudget = 0;

	// Tristram is lit by the sky; the game's light radius does nothing there either.
	if (leveltype != DTYPE_TOWN)
		AddLights(scene);
}

} // namespace render
} // namespace vr
} // namespace devilution

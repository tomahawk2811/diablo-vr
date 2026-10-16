/**
 * @file offscreen.cpp
 *
 * Headset-free rendering of the 3D world: captures for tests and the benchmark.
 */
#include "platform/vr/vr_internal.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "diablo.h"
#include "interfac.h"
#include "levels/dun_tile_data.hpp"
#include "objects.h"
#include "platform/vr/debug/profile.h"
#include "platform/vr/render/gpu.h"
#include "platform/vr/render/world.h"
#include "player.h"
#include "quests.h"
#include "utils/log.hpp"

namespace devilution {
namespace vr {

namespace {

/** Two colour targets, so a stereo frame can be captured too. */
ID3D11Texture2D *color[2];
ID3D11RenderTargetView *colorView[2];

/** A plain device with colour targets: there is no headset session here. True once a level can be drawn. */
bool EnsureOffscreen(int width, int height)
{
	if (device == nullptr) {
		swapchainFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		swapchainSdlFormat = SDL_PIXELFORMAT_ABGR8888;
		D3D11_TEXTURE2D_DESC desc {};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		desc.SampleDesc.Count = 1;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET;
		if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)))
			return false;
		for (int i = 0; i < 2; i++) {
			if (FAILED(device->CreateTexture2D(&desc, nullptr, &color[i])) || FAILED(device->CreateRenderTargetView(color[i], nullptr, &colorView[i])))
				return false;
		}
		if (!VrWorldInit(device, context))
			return false;
	}
	return VrWorldAvailable() && colorView[1] != nullptr;
}

} // namespace

/** Goes to a dungeon level whose layout is the same in every run, so two builds can be compared. */
void EnterLevel(int level)
{
	for (int i = 0; i < NUMLEVELS; i++) {
		DungeonSeeds[i] = 0xD1AB10 + static_cast<uint32_t>(i);
		LevelSeeds[i] = std::nullopt;
	}
	InitQuests(); // Which quests are in the game, and so their set pieces, comes from the seeds.
	StartNewLvl(*MyPlayer, WM_DIABNEXTLVL, level);
}

namespace {

VrView MakeView(const float position[3], const float orientation[4], const XrFovf &fov, float sideways = 0)
{
	const XrVector3f right = Rotate({ orientation[0], orientation[1], orientation[2], orientation[3] }, { 1, 0, 0 });
	VrView view {};
	view.position[0] = position[0] + right.x * sideways;
	view.position[1] = position[1] + right.y * sideways;
	view.position[2] = position[2] + right.z * sideways;
	std::copy(orientation, orientation + 4, view.orientation);
	view.fov[0] = std::tan(fov.angleLeft);
	view.fov[1] = std::tan(fov.angleRight);
	view.fov[2] = std::tan(fov.angleUp);
	view.fov[3] = std::tan(fov.angleDown);
	return view;
}

/**
 * Stands the camera on the player, turned by `yaw` and `pitch`, with a hand in front of it
 * so held items show up too, and reads the game into the scene.
 */
void UpdateScene(float yaw, float pitch, float position[3], float orientation[4])
{
	VrWorldPlayerPosition(worldOrigin[0], worldOrigin[2]);
	worldOrigin[1] = 0;
	// DEVILUTIONX_VR_DEBUG_TILE_X / _Y: look from that tile instead of from the player.
	static const float debugTileX = Knob("DEVILUTIONX_VR_DEBUG_TILE_X", -1);
	static const float debugTileY = Knob("DEVILUTIONX_VR_DEBUG_TILE_Y", -1);
	if (debugTileX >= 0 && debugTileY >= 0) {
		worldOrigin[0] = (debugTileX + 0.5F) * VrWorldTileSize();
		worldOrigin[2] = (debugTileY + 0.5F) * VrWorldTileSize();
	}
	// DEVILUTIONX_VR_DEBUG_MONSTER=<n>: look from two steps south of the level's n-th monster.
	static const int debugMonster = static_cast<int>(Knob("DEVILUTIONX_VR_DEBUG_MONSTER", 0));
	// Golems, which every level has, wait in the corner of the map until they are summoned.
	const Monster *chosen = nullptr;
	for (size_t i = 0, placed = 0; i < ActiveMonsterCount && debugMonster > 0 && leveltype != DTYPE_TOWN; i++) {
		const Monster &candidate = Monsters[ActiveMonsters[i]];
		if (candidate.position.tile.x < 8 || candidate.position.tile.y < 8 || (candidate.flags & MFLAG_HIDDEN) != 0)
			continue;
		// DEVILUTIONX_VR_DEBUG_MONSTER_TYPE=<text>: only monsters whose sprite folder contains it ("skelsd").
		static const char *wanted = SDL_getenv("DEVILUTIONX_VR_DEBUG_MONSTER_TYPE");
		static const std::string wantedType = wanted != nullptr ? wanted : "";
		if (!wantedType.empty() && std::string(candidate.data().spritePath()).find(wantedType) == std::string::npos)
			continue;
		chosen = &candidate;
		if (++placed == static_cast<size_t>(debugMonster))
			break;
	}
	if (chosen != nullptr) {
		const Monster &monster = *chosen;
		// The first open spot about two tiles away.
		bool found = false;
		for (int i = 0; i < 48 && !found; i++) {
			// DEVILUTIONX_VR_DEBUG_MONSTER_SIDE=<n>: start looking for the spot n sixteenths of a turn round (2 = south, where the art's camera stood).
			static const int side = static_cast<int>(Knob("DEVILUTIONX_VR_DEBUG_MONSTER_SIDE", 0));
			const float angle = static_cast<float>(i + side) * 0.3927F;
			const float away = 1.6F + 0.5F * static_cast<float>(i / 16);
			const float x = (static_cast<float>(monster.position.tile.x) + 0.5F + away * std::cos(angle)) * VrWorldTileSize();
			const float z = (static_cast<float>(monster.position.tile.y) + 0.5F + away * std::sin(angle)) * VrWorldTileSize();
			found = VrWorldCanStand(x, z);
			if (found) {
				worldOrigin[0] = x;
				worldOrigin[2] = z;
			}
		}
		static bool logged;
		if (!logged)
			LogInfo("VR: debug camera at monster {} ({}, {}): {}", debugMonster, monster.position.tile.x, monster.position.tile.y, found ? "placed" : "no open spot");
		logged = true;
	}
	// DEVILUTIONX_VR_DEBUG_OBJECT=doorway: look straight at the level's n-th doorway built in 3D
	// (DEVILUTIONX_VR_DEBUG_DOORWAY=<n>, from 0) from about two tiles out, to see the walls beside it.
	static const char *const objectKind = SDL_getenv("DEVILUTIONX_VR_DEBUG_OBJECT");
	if (objectKind != nullptr && std::string(objectKind) == "doorway" && leveltype != DTYPE_TOWN) {
		static const int index = static_cast<int>(Knob("DEVILUTIONX_VR_DEBUG_DOORWAY", 0));
		int tileX;
		int tileY;
		bool alongX;
		if (VrWorldBuiltDoorway(index, tileX, tileY, alongX)) {
			const float tile = VrWorldTileSize();
			const float cx = (static_cast<float>(tileX) + 0.5F) * tile;
			const float cz = (static_cast<float>(tileY) + 0.5F) * tile;
			for (const float side : { -1.F, 1.F }) {
				const float x = cx + (alongX ? side * 2.4F * tile : 0);
				const float z = cz + (alongX ? 0 : side * 2.4F * tile);
				if (VrWorldCanStand(x, z)) {
					worldOrigin[0] = x;
					worldOrigin[2] = z;
					yaw = std::atan2(-(cx - x), -(cz - z));
					break;
				}
			}
			static bool logged;
			if (!logged)
				LogInfo("VR: debug camera at doorway {} ({}, {}), level type {}", index, tileX, tileY, static_cast<int>(leveltype));
			logged = true;
		}
	}
	// DEVILUTIONX_VR_DEBUG_OBJECT=chest, sarc or door: look at the level's first one from about two
	// tiles away, and open it 200 frames in the way the game does, to see it open.
	if (objectKind != nullptr && std::string(objectKind) != "doorway") {
		static Object *watched;
		static float spot[2];
		static int frames;
		if (watched == nullptr && leveltype != DTYPE_TOWN) {
			const std::string kind = objectKind;
			for (int i = 0; i < ActiveObjectCount && watched == nullptr; i++) {
				Object &object = Objects[ActiveObjects[i]];
				const bool wanted = (kind == "chest" && object.IsChest()) || (kind == "sarc" && object._otype == OBJ_SARC) || (kind == "door" && object.isDoor());
				for (int j = 0; j < 16 && wanted && watched == nullptr; j++) {
					const float angle = static_cast<float>(j) * 0.3927F;
					const float x = (static_cast<float>(object.position.x) + 0.5F + 2.2F * std::cos(angle)) * VrWorldTileSize();
					const float z = (static_cast<float>(object.position.y) + 0.5F + 2.2F * std::sin(angle)) * VrWorldTileSize();
					if (VrWorldCanStand(x, z)) {
						watched = &object;
						spot[0] = x;
						spot[1] = z;
						LogInfo("VR: debug camera at {} ({}, {}), frame {}", kind, object.position.x, object.position.y, object._oAnimFrame);
					}
				}
			}
		}
		if (watched != nullptr) {
			worldOrigin[0] = spot[0];
			worldOrigin[2] = spot[1];
			const float dx = (static_cast<float>(watched->position.x) + 0.5F) * VrWorldTileSize() - spot[0];
			const float dz = (static_cast<float>(watched->position.y) + 0.5F) * VrWorldTileSize() - spot[1];
			yaw = std::atan2(-dx, -dz);
			if (++frames == 200) {
				if (watched->IsChest()) {
					watched->_oAnimFrame += 2;
				} else if (watched->_otype == OBJ_SARC) {
					watched->_oAnimFlag = true;
					watched->_oAnimDelay = 3;
				} else {
					watched->_oVar4 = 1; // DOOR_OPEN (objects.cpp)
				}
				LogInfo("VR: debug object opened");
			}
		}
	}
	turnYaw = yaw;
	const XrPosef local { { std::sin(pitch / 2), 0, 0, std::cos(pitch / 2) }, { 0, 1.65F, 0 } };
	headAnchor = { 0, 0, 0 };
	ToWorld(local, position, orientation);
	VrHandPose hands[2] {};
	for (int i = 0; i < 2; i++) {
		const XrPosef hand { local.orientation, { i == 0 ? -0.25F : 0.25F, 1.25F, -0.45F } };
		hands[i].valid = true;
		ToWorld(hand, hands[i].position, hands[i].orientation);
	}
	const XrVector3f ray = Rotate({ orientation[0], orientation[1], orientation[2], orientation[3] }, { 0, -0.35F, -0.94F });
	const float direction[3] = { ray.x, ray.y, ray.z };
	int tileX;
	int tileY;
	float hit[3];
	const bool picked = VrWorldPick(hands[1].position, direction, tileX, tileY, hit);
	VrWorldUpdate(position, orientation, hands[0], hands[1], picked, hit);
}

float Percentile(std::vector<float> values, float fraction)
{
	if (values.empty())
		return -1;
	std::sort(values.begin(), values.end());
	return values[std::min(values.size() - 1, static_cast<size_t>(fraction * static_cast<float>(values.size())))];
}

void WriteBench(const char *directory, int level, int width, int height, float seconds, const std::vector<FrameSample> &samples)
{
	std::vector<float> cpu;
	std::vector<float> gpu;
	std::vector<float> cpuScope[2];
	std::vector<float> gpuPass[3];
	FrameSample worst {};
	for (const FrameSample &sample : samples) {
		cpu.push_back(sample.cpuMs);
		for (int i = 0; i < 2; i++)
			cpuScope[i].push_back(sample.cpuScopeMs[i]);
		if (sample.gpuMs >= 0) {
			gpu.push_back(sample.gpuMs);
			for (int i = 0; i < 3; i++)
				gpuPass[i].push_back(sample.gpuPassMs[i]);
		}
		worst.drawCalls = std::max(worst.drawCalls, sample.drawCalls);
		worst.triangles = std::max(worst.triangles, sample.triangles);
		worst.meshInstances = std::max(worst.meshInstances, sample.meshInstances);
		worst.sprites = std::max(worst.sprites, sample.sprites);
		worst.particles = std::max(worst.particles, sample.particles);
		worst.lights = std::max(worst.lights, sample.lights);
	}
	const auto megabytes = [](render::MemoryKind kind) { return static_cast<double>(render::memoryBytes[static_cast<size_t>(kind)]) / 1048576.0; };
	char text[2048];
	SDL_snprintf(text, sizeof(text),
	    "{\n"
	    "  \"level\": %d,\n"
	    "  \"eye_width\": %d,\n"
	    "  \"eye_height\": %d,\n"
	    "  \"eyes\": 2,\n"
	    "  \"mirror\": %s,\n"
	    "  \"seconds\": %.1f,\n"
	    "  \"frames\": %d,\n"
	    "  \"gpu_frames\": %d,\n"
	    "  \"cpu_ms\": { \"p50\": %.3f, \"p95\": %.3f, \"p99\": %.3f },\n"
	    "  \"cpu_p50_ms\": { \"extract\": %.3f, \"render\": %.3f },\n"
	    "  \"gpu_ms\": { \"p50\": %.3f, \"p95\": %.3f, \"p99\": %.3f },\n"
	    "  \"gpu_p50_ms\": { \"shadow\": %.3f, \"scene\": %.3f, \"post\": %.3f },\n"
	    "  \"draw_calls\": %u,\n"
	    "  \"triangles\": %u,\n"
	    "  \"mesh_instances\": %u,\n"
	    "  \"sprites\": %u,\n"
	    "  \"particles\": %u,\n"
	    "  \"lights\": %u,\n"
	    "  \"gpu_memory_mb\": { \"targets\": %.1f, \"textures\": %.1f, \"geometry\": %.1f, \"dynamic\": %.1f },\n"
	    "  \"vram_mb\": %.1f\n"
	    "}\n",
	    level, width, height, MirrorCaptureEnabled() ? "true" : "false", seconds, static_cast<int>(cpu.size()), static_cast<int>(gpu.size()),
	    Percentile(cpu, 0.50F), Percentile(cpu, 0.95F), Percentile(cpu, 0.99F),
	    Percentile(cpuScope[0], 0.50F), Percentile(cpuScope[1], 0.50F),
	    Percentile(gpu, 0.50F), Percentile(gpu, 0.95F), Percentile(gpu, 0.99F),
	    Percentile(gpuPass[0], 0.50F), Percentile(gpuPass[1], 0.50F), Percentile(gpuPass[2], 0.50F),
	    worst.drawCalls, worst.triangles, worst.meshInstances, worst.sprites, worst.particles, worst.lights,
	    megabytes(render::MemoryKind::Targets), megabytes(render::MemoryKind::Textures), megabytes(render::MemoryKind::Geometry), megabytes(render::MemoryKind::Dynamic),
	    static_cast<double>(ProfileVramBytes()) / (1024.0 * 1024.0));
	char path[1024];
	SDL_snprintf(path, sizeof(path), "%s%sbench.json", directory, *directory != 0 ? "/" : "");
	if (SDL_RWops *file = SDL_RWFromFile(path, "wb"); file != nullptr) {
		SDL_RWwrite(file, text, 1, SDL_strlen(text));
		SDL_RWclose(file);
	}
	LogInfo("VR: bench written to {}", path);
}

} // namespace

/**
 * Debug aid (DEVILUTIONX_VR_OFFSCREEN=<dir>): renders the 3D world without any headset
 * from the player's eye height, looking in 8 directions in turn, and writes
 * eye0.bmp..eye7.bmp to the directory, then one stereo frame (stereo0.bmp, stereo1.bmp),
 * the level's surface textures (atlas.bmp) and a `done` marker.
 */
void DebugOffscreen(const char *directory)
{
	constexpr int Width = 1280;
	constexpr int Height = 960;
	if (!EnsureOffscreen(Width, Height))
		return;

	static int frame;
	static const float pitch = Knob("DEVILUTIONX_VR_DEBUG_PITCH", -0.15F);
	frame++;
	// DEVILUTIONX_VR_DEBUG_LEVEL=<n>: go to dungeon level n once the game is up.
	static const int debugLevel = static_cast<int>(Knob("DEVILUTIONX_VR_DEBUG_LEVEL", 0));
	if (debugLevel != 0 && frame == 30) {
		EnterLevel(debugLevel);
		return;
	}
	const int shot = frame / 40 % 8;
	float position[3];
	float orientation[4];
	ProfileBeginFrame();
	UpdateScene(static_cast<float>(shot) * SnapTurn, pitch, position, orientation);
	const XrFovf fov { -0.87F, 0.87F, 0.72F, -0.72F };
	// DEVILUTIONX_VR_DEBUG_MASK=1: a made-up hidden area, the four corners, to exercise the
	// mask where the runtime has no hidden-area mesh to give (some SteamVR headsets).
	static const bool debugMask = SDL_getenv("DEVILUTIONX_VR_DEBUG_MASK") != nullptr;
	static ID3D11Buffer *maskVertices;
	if (debugMask && maskVertices == nullptr) {
		const float l = std::tan(fov.angleLeft);
		const float r = std::tan(fov.angleRight);
		const float u = std::tan(fov.angleUp);
		const float d = std::tan(fov.angleDown);
		constexpr float Cut = 0.5F;
		const float corners[] = {
			l, u, l + Cut, u, l, u - Cut,
			r, u, r, u - Cut, r - Cut, u,
			l, d, l, d + Cut, l + Cut, d,
			r, d, r - Cut, d, r, d + Cut,
		};
		D3D11_BUFFER_DESC desc {};
		desc.ByteWidth = sizeof(corners);
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		const D3D11_SUBRESOURCE_DATA data { corners, 0, 0 };
		device->CreateBuffer(&desc, &data, &maskVertices);
	}
	const VrView view = MakeView(position, orientation, fov);
	const VrViewTarget target { colorView[0], nullptr, maskVertices, maskVertices != nullptr ? 12U : 0U };
	VrWorldRender(&view, 1, Width, Height, &target);
	ProfileEndFrame();
	// One set of captures, at fixed frames, then a marker file for the script that waits for them.
	// The first two turns are skipped: a level change asked for at frame 30 takes a few frames to happen.
	static int captured;
	if (captured < 8 && frame >= 80 && frame % 40 == 39) {
		char path[1024];
		SDL_snprintf(path, sizeof(path), "%s/eye%d.bmp", directory, shot);
		DumpTexture(color[0], path);
		if (++captured == 8) {
			// Both eyes in one pass, the way the headset is fed.
			const VrView views[2] = { MakeView(position, orientation, fov, -0.032F), MakeView(position, orientation, fov, 0.032F) };
			const VrViewTarget targets[2] = { { colorView[0], nullptr, nullptr, 0 }, { colorView[1], nullptr, nullptr, 0 } };
			VrWorldRender(views, 2, Width, Height, targets);
			for (int eye = 0; eye < 2; eye++) {
				SDL_snprintf(path, sizeof(path), "%s/stereo%d.bmp", directory, eye);
				DumpTexture(color[eye], path);
			}
			SDL_snprintf(path, sizeof(path), "%s/atlas.bmp", directory);
			VrWorldDumpAtlas(path);
			SDL_snprintf(path, sizeof(path), "%s/done", directory);
			if (SDL_RWops *marker = SDL_RWFromFile(path, "wb"); marker != nullptr)
				SDL_RWclose(marker);
		}
	}
}

/**
 * `--vr-bench <level>`: renders what a headset frame renders (scene update, two eyes in one
 * pass, the mirror if enabled) without a headset, for 20 seconds along a fixed camera path,
 * and writes the frame time percentiles to bench.json in `directory` (the working directory
 * when empty). Then quits the game.
 */
void DebugBench(int level, const char *directory)
{
	// A typical recommended eye size (SteamVR, Quest 2), as logged by CreateEyeTargets.
	static const int width = static_cast<int>(Knob("DEVILUTIONX_VR_BENCH_WIDTH", 1832));
	static const int height = static_cast<int>(Knob("DEVILUTIONX_VR_BENCH_HEIGHT", 1920));
	constexpr float Seconds = 20;
	constexpr int WarmupFrames = 120; // The first frames mesh the level and fill the sprite atlas.
	if (!EnsureOffscreen(width, height))
		return;

	static int frame;
	static bool done;
	static uint64_t start;
	static std::vector<FrameSample> samples;
	if (done)
		return;
	frame++;
	if (level != 0 && frame == 30) {
		EnterLevel(level);
		return;
	}
	if (frame == WarmupFrames) {
		samples.reserve(1 << 16);
		ProfileRecord(&samples);
		start = SDL_GetPerformanceCounter();
	}
	const float t = frame < WarmupFrames ? 0 : static_cast<float>(SDL_GetPerformanceCounter() - start) / static_cast<float>(SDL_GetPerformanceFrequency());

	// the camera stays on the player and looks around (two full turns, nodding),
	// because only the player's surroundings are lit. A walk through the level needs the
	// simulation to move the player, which would make runs differ.
	constexpr float Pi = 3.14159265F;
	const float yaw = t / Seconds * 4 * Pi;
	const float pitch = -0.15F + 0.25F * std::sin(t * 2 * Pi / 5);

	ProfileBeginFrame();
	float position[3];
	float orientation[4];
	UpdateScene(yaw, pitch, position, orientation);
	const VrView views[2] = {
		MakeView(position, orientation, XrFovf { -0.90F, 0.80F, 0.85F, -0.90F }, -0.0315F),
		MakeView(position, orientation, XrFovf { -0.80F, 0.90F, 0.85F, -0.90F }, 0.0315F),
	};
	const VrViewTarget targets[2] = { { colorView[0], nullptr, nullptr, 0 }, { colorView[1], nullptr, nullptr, 0 } };
	VrWorldRender(views, 2, width, height, targets);
	if (MirrorCaptureEnabled())
		RenderMirror();
	ProfileEndFrame();

	if (t >= Seconds) {
		done = true;
		ProfileRecord(nullptr);
		WriteBench(directory, level, width, height, Seconds, samples);
		SDL_Event quit;
		SDL_zero(quit);
		quit.type = SDL_QUIT;
		SDL_PushEvent(&quit);
	}
}

} // namespace vr
} // namespace devilution

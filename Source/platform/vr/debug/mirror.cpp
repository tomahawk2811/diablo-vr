/**
 * @file mirror.cpp
 *
 * Desktop mirror of the headset view and texture dumps.
 */
#include "platform/vr/vr_internal.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include "automap.h"
#include "control/control.hpp"
#include "cursor.h"
#include "diablo.h"
#include "engine/point.hpp"
#include "engine/render/scrollrt.h"
#include "game_mode.hpp"
#include "gmenu.h"
#include "help.h"
#include "interfac.h"
#include "inv.h"
#include "levels/dun_tile_data.hpp"
#include "levels/tile_properties.hpp"
#include "minitext.h"
#include "msg.h"
#include "options.h"
#include "panels/quest_log.hpp"
#include "platform/vr/render/world.h"
#include "player.h"
#include "qol/stash.h"
#include "quests.h"
#include "stores.h"
#include "utils/display.h"
#include "utils/file_util.h"
#include "utils/log.hpp"
#include "utils/png.h"
#include "utils/ui_fwd.h"

namespace devilution {
namespace vr {

// Desktop mirror of the headset view: one full-screen pass at the size of the 2D frame and a
// readback. With DEVILUTIONX_VR_MIRROR=1 it is also written to disk every two seconds.
struct MirrorTarget {
	ID3D11Texture2D *color;
	/** Readback ring: a frame is copied into one texture and read two frames later, when the GPU is done with it. */
	ID3D11Texture2D *staging[3];
	unsigned frame;
	ID3D11RenderTargetView *view;
	int width;
	int height;
};
MirrorTarget mirror;
std::vector<uint8_t> mirrorPixels;
/** The mirror with the 2D frame's panels on top, while one is open. */
std::vector<uint8_t> mirrorComposite;
int mirrorPitch;
bool mirrorReady;

/** Debug aid: writes a GPU texture to a BMP so the 3D view can be checked without a headset on. */
void DumpTexture(ID3D11Texture2D *texture, const char *path)
{
	D3D11_TEXTURE2D_DESC desc;
	texture->GetDesc(&desc);
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	desc.MiscFlags = 0;
	desc.Format = static_cast<DXGI_FORMAT>(swapchainFormat);
	ID3D11Texture2D *staging = nullptr;
	if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
		return;
	context->CopyResource(staging, texture);
	D3D11_MAPPED_SUBRESOURCE mapped;
	if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
		SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormatFrom(mapped.pData, desc.Width, desc.Height, 32, mapped.RowPitch, swapchainSdlFormat);
		if (surface != nullptr) {
			SDL_SaveBMP(surface, path);
			SDL_FreeSurface(surface);
		}
		context->Unmap(staging, 0);
	}
	staging->Release();
}

bool screenshotRequested;

std::string ScreenshotDirectory()
{
	if (const char *directory = SDL_getenv("DEVILUTIONX_VR_SCREENSHOTS"); directory != nullptr)
		return directory;
	// Beside the `vrassets` folder when the game runs from the project tree, next to the executable otherwise.
	char *base = SDL_GetBasePath();
	std::string path = base != nullptr ? base : "";
	SDL_free(base);
	const std::string beside = path;
	for (int up = 0; up < 5; up++) {
		if (SDL_RWops *probe = SDL_RWFromFile((path + "vrassets/vrassets.txt").c_str(), "rb"); probe != nullptr) {
			SDL_RWclose(probe);
			return path + "screenshots";
		}
		path += "../";
	}
	return beside + "screenshots";
}

void SaveScreenshot(ID3D11Texture2D *texture)
{
	static const std::string directory = ScreenshotDirectory();
	CreateDir(directory.c_str());
	D3D11_TEXTURE2D_DESC desc;
	texture->GetDesc(&desc);
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	desc.MiscFlags = 0;
	desc.ArraySize = 1;
	desc.Format = static_cast<DXGI_FORMAT>(swapchainFormat);
	ID3D11Texture2D *staging = nullptr;
	if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
		return;
	context->CopySubresourceRegion(staging, 0, 0, 0, 0, texture, 0, nullptr);
	D3D11_MAPPED_SUBRESOURCE mapped;
	if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
		SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormatFrom(mapped.pData, desc.Width, desc.Height, 32, mapped.RowPitch, swapchainSdlFormat);
		// The eye image's alpha means nothing; a picture has none.
		SDL_Surface *opaque = surface != nullptr ? SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_RGB24, 0) : nullptr;
		if (opaque != nullptr) {
			const std::time_t now = std::time(nullptr);
			std::tm local {};
#ifdef _WIN32
			localtime_s(&local, &now);
#else
			localtime_r(&now, &local);
#endif
			char name[64];
			static int serial;
			SDL_snprintf(name, sizeof(name), "/diablo-vr-%04d%02d%02d-%02d%02d%02d-%d.png", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec, serial++ % 10);
			const std::string path = directory + name;
			if (IMG_SavePNG(opaque, path.c_str()) == 0)
				LogInfo("VR: screenshot saved to {}", path);
			else
				LogError("VR: could not write screenshot {}: {}", path, SDL_GetError());
		}
		SDL_FreeSurface(opaque);
		SDL_FreeSurface(surface);
		context->Unmap(staging, 0);
	}
	staging->Release();
}

bool MirrorCaptureEnabled()
{
	static const bool enabled = SDL_getenv("DEVILUTIONX_VR_MIRROR") != nullptr;
	return enabled;
}

/**
 * Scales the left eye's finished picture to the size of the game's 2D frame, for the desktop
 * window. With DEVILUTIONX_VR_MIRROR=1 it is also written to vr-view.bmp next to the executable
 * every two seconds, so the headset view can be looked at from outside.
 */
void RenderMirror()
{
#ifndef _WIN32
	// A standalone headset has no desktop window to look at; the pass and the readback are
	// only worth their cost when the view is being captured.
	if (!MirrorCaptureEnabled())
		return;
#endif
	const SDL_Surface *surface = GetOutputSurface();
	if (surface == nullptr)
		return;
	if (mirror.width != surface->w || mirror.height != surface->h) {
		ReleaseMirror();
		D3D11_TEXTURE2D_DESC desc {};
		desc.Width = surface->w;
		desc.Height = surface->h;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		desc.SampleDesc.Count = 1;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET;
		D3D11_TEXTURE2D_DESC stagingDesc = desc;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.BindFlags = 0;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(device->CreateTexture2D(&desc, nullptr, &mirror.color))
		    || FAILED(device->CreateRenderTargetView(mirror.color, nullptr, &mirror.view))
		    || FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &mirror.staging[0]))
		    || FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &mirror.staging[1]))
		    || FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &mirror.staging[2])))
			return;
		mirror.width = surface->w;
		mirror.height = surface->h;
	}

	VrWorldBlit(mirror.view, mirror.width, mirror.height);

	context->CopyResource(mirror.staging[mirror.frame % 3], mirror.color);
	mirror.frame++;
	if (mirror.frame < 3)
		return;
	// The oldest copy. Never wait for the GPU: a frame that is not ready is skipped.
	ID3D11Texture2D *staging = mirror.staging[mirror.frame % 3];
	D3D11_MAPPED_SUBRESOURCE mapped;
	if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
		return;
	mirrorPitch = surface->pitch;
	mirrorPixels.resize(static_cast<size_t>(surface->pitch) * surface->h);
	mirrorReady = SDL_ConvertPixels(mirror.width, mirror.height, SDL_PIXELFORMAT_ABGR8888, mapped.pData, static_cast<int>(mapped.RowPitch),
	                  surface->format->format, mirrorPixels.data(), mirrorPitch)
	    == 0;

	static Uint32 nextCapture;
	if (MirrorCaptureEnabled() && SDL_TICKS_PASSED(SDL_GetTicks(), nextCapture)) {
		nextCapture = SDL_GetTicks() + 2000;
		static const std::string directory = [] {
			char *base = SDL_GetBasePath();
			std::string result = base != nullptr ? base : "";
			SDL_free(base);
			return result;
		}();
		SDL_Surface *capture = SDL_CreateRGBSurfaceWithFormatFrom(mapped.pData, mirror.width, mirror.height, 32, static_cast<int>(mapped.RowPitch), SDL_PIXELFORMAT_ABGR8888);
		if (capture != nullptr) {
			// Written under another name first, so a reader never sees half a file.
			const std::string temporary = directory + "vr-view.tmp";
			const std::string target = directory + "vr-view.bmp";
			if (SDL_SaveBMP(capture, temporary.c_str()) == 0) {
#ifdef _WIN32
				MoveFileExA(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
#else
				std::rename(temporary.c_str(), target.c_str());
#endif
			}
			SDL_FreeSurface(capture);
		}
	}
	context->Unmap(staging, 0);
}

void ReleaseMirror()
{
	if (mirror.color != nullptr) {
		mirror.view->Release();
		mirror.color->Release();
	}
	for (ID3D11Texture2D *staging : mirror.staging) {
		if (staging != nullptr)
			staging->Release();
	}
	mirror = {};
	mirrorReady = false;
}

} // namespace vr

using namespace vr;

const void *VrMirrorPixels(const SDL_Surface *frame, int &pitch)
{
	if (!sessionRunning || !world3d || !mirrorReady || frame->w != mirror.width || frame->h != mirror.height || frame->pitch != mirrorPitch)
		return nullptr;
	pitch = mirrorPitch;
	if (!uiMode)
		return mirrorPixels.data();
	if (frame->format->BytesPerPixel != 4)
		return nullptr;

	// A panel, dialog or menu is open. The 2D renderer left the world black, so whatever
	// is not black belongs to the interface and goes on top of the headset's view.
	const Uint32 colorMask = frame->format->Rmask | frame->format->Gmask | frame->format->Bmask;
	mirrorComposite = mirrorPixels;
	for (int y = 0; y < frame->h; y++) {
		const auto *source = reinterpret_cast<const Uint32 *>(static_cast<const uint8_t *>(frame->pixels) + static_cast<size_t>(y) * frame->pitch);
		auto *target = reinterpret_cast<Uint32 *>(mirrorComposite.data() + static_cast<size_t>(y) * mirrorPitch);
		for (int x = 0; x < frame->w; x++) {
			if ((source[x] & colorMask) != 0)
				target[x] = source[x];
		}
	}
	return mirrorComposite.data();
}

} // namespace devilution

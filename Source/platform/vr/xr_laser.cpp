/**
 * @file xr_laser.cpp
 *
 * The laser pointer as composition layers of its own: a beam from the pointing hand to what it
 * hits and a dot where it lands on a screen or panel. The screen and the panels are quad layers
 * the compositor draws over the eyes' picture, so a beam drawn in the world would vanish behind
 * the very panel it points at. These two quads are submitted last, over everything. The big
 * arrow on the automap held in the left hand is a quad of the same kind, over the map.
 */
#include "platform/vr/vr_internal.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace devilution {
namespace vr {

LaserPointer laser;
MapArrow mapArrow;

namespace {

/** Beam (idle and over something the trigger acts on) and dot pictures, made once a session. */
XrSwapchain beamSwapchain[2];
XrSwapchain dotSwapchain;
XrSwapchain arrowSwapchain;
constexpr int BeamWidth = 128;
constexpr int BeamHeight = 16;
constexpr int DotSize = 64;
constexpr int ArrowSize = 64;

/** Whether (x, y) is inside the polygon of `count` corners `xs`, `ys` (even-odd rule). */
bool Inside(float x, float y, const float *xs, const float *ys, int count)
{
	bool inside = false;
	for (int i = 0, j = count - 1; i < count; j = i++) {
		if ((ys[i] > y) != (ys[j] > y) && x < (xs[j] - xs[i]) * (y - ys[i]) / (ys[j] - ys[i]) + xs[i])
			inside = !inside;
	}
	return inside;
}

/** One picture, uploaded once: the runtime shows the image released last for as long as it is used. */
XrSwapchain MakePicture(int width, int height, const std::vector<Uint32> &pixels)
{
	XrSwapchainCreateInfo info { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	info.format = swapchainFormat;
	info.sampleCount = 1;
	info.width = width;
	info.height = height;
	info.faceCount = 1;
	info.arraySize = 1;
	info.mipCount = 1;
	XrSwapchain picture = XR_NULL_HANDLE;
	if (!Check(xrd3d::CreateSwapchain(session, info, &picture), "xrCreateSwapchain(laser)"))
		return XR_NULL_HANDLE;
	const std::vector<ID3D11Texture2D *> images = xrd3d::SwapchainImages(picture);
	uint32_t index = 0;
	if (XR_FAILED(xrd3d::AcquireSwapchainImage(picture, &index))) {
		xrd3d::DestroySwapchain(picture);
		return XR_NULL_HANDLE;
	}
	if (XR_SUCCEEDED(xrd3d::WaitSwapchainImage(picture)) && index < images.size())
		context->UpdateSubresource(images[index], 0, nullptr, pixels.data(), width * 4, 0);
	xrd3d::ReleaseSwapchainImage(picture);
	return picture;
}

bool MakePictures()
{
	if (dotSwapchain != XR_NULL_HANDLE)
		return true;
	SDL_PixelFormat *format = SDL_AllocFormat(swapchainSdlFormat);
	if (format == nullptr)
		return false;
	const auto pixel = [&](float r, float g, float b, float a) {
		const auto byte = [](float v) { return static_cast<Uint8>(std::clamp(v, 0.F, 1.F) * 255.F + 0.5F); };
		return SDL_MapRGBA(format, byte(r), byte(g), byte(b), byte(a));
	};
	// The beam: brightest along its middle, soft at the edges, faint where it leaves the hand.
	const float colors[2][3] = { { 1.F, 0.92F, 0.8F }, { 1.F, 0.72F, 0.15F } };
	for (int kind = 0; kind < 2; kind++) {
		std::vector<Uint32> pixels(static_cast<size_t>(BeamWidth) * BeamHeight);
		for (int y = 0; y < BeamHeight; y++) {
			const float across = std::fabs((static_cast<float>(y) + 0.5F) / BeamHeight * 2 - 1);
			const float edge = std::clamp((1 - across) * 2.5F, 0.F, 1.F);
			for (int x = 0; x < BeamWidth; x++) {
				const float along = (static_cast<float>(x) + 0.5F) / BeamWidth;
				const float start = std::clamp(along * 6, 0.25F, 1.F);
				pixels[static_cast<size_t>(y) * BeamWidth + x] = pixel(colors[kind][0], colors[kind][1], colors[kind][2], edge * start * 0.85F);
			}
		}
		beamSwapchain[kind] = MakePicture(BeamWidth, BeamHeight, pixels);
	}
	// The dot: white with a dark rim, so it shows on bright parchment and dark menus alike.
	std::vector<Uint32> pixels(static_cast<size_t>(DotSize) * DotSize);
	for (int y = 0; y < DotSize; y++) {
		for (int x = 0; x < DotSize; x++) {
			const float dx = (static_cast<float>(x) + 0.5F) / DotSize * 2 - 1;
			const float dy = (static_cast<float>(y) + 0.5F) / DotSize * 2 - 1;
			const float r = std::sqrt(dx * dx + dy * dy);
			const float alpha = std::clamp((1 - r) * 12, 0.F, 1.F);
			const float white = std::clamp((0.62F - r) * 12, 0.F, 1.F);
			pixels[static_cast<size_t>(y) * DotSize + x] = pixel(white, white, white * 0.92F, alpha * (0.75F + 0.25F * white));
		}
	}
	dotSwapchain = MakePicture(DotSize, DotSize, pixels);
	// The map arrow: a gold arrowhead pointing up the picture, with a dark rim, sampled four
	// times a pixel for smooth edges.
	constexpr float ArrowX[4] = { 0.F, 0.62F, 0.F, -0.62F };
	constexpr float ArrowY[4] = { 0.9F, -0.72F, -0.34F, -0.72F };
	float rimX[4];
	float rimY[4];
	for (int i = 0; i < 4; i++) {
		rimX[i] = ArrowX[i] * 1.22F;
		rimY[i] = (ArrowY[i] + 0.05F) * 1.18F - 0.05F;
	}
	std::vector<Uint32> arrow(static_cast<size_t>(ArrowSize) * ArrowSize);
	for (int y = 0; y < ArrowSize; y++) {
		for (int x = 0; x < ArrowSize; x++) {
			float fill = 0;
			float rim = 0;
			for (int sample = 0; sample < 4; sample++) {
				const float u = (static_cast<float>(x) + 0.25F + 0.5F * static_cast<float>(sample % 2)) / ArrowSize * 2 - 1;
				const float v = 1 - (static_cast<float>(y) + 0.25F + 0.5F * static_cast<float>(sample / 2)) / ArrowSize * 2;
				if (Inside(u, v, ArrowX, ArrowY, 4))
					fill += 0.25F;
				else if (Inside(u, v, rimX, rimY, 4))
					rim += 0.25F;
			}
			const float alpha = fill + rim * 0.85F;
			const float bright = alpha > 0 ? fill / alpha : 0;
			arrow[static_cast<size_t>(y) * ArrowSize + x] = pixel(bright, bright * 0.78F, bright * 0.2F, alpha);
		}
	}
	arrowSwapchain = MakePicture(ArrowSize, ArrowSize, arrow);
	SDL_FreeFormat(format);
	return beamSwapchain[0] != XR_NULL_HANDLE && beamSwapchain[1] != XR_NULL_HANDLE && dotSwapchain != XR_NULL_HANDLE && arrowSwapchain != XR_NULL_HANDLE;
}

XrVector3f Sub(const XrVector3f &a, const XrVector3f &b)
{
	return { a.x - b.x, a.y - b.y, a.z - b.z };
}

XrVector3f Scale(const XrVector3f &v, float s)
{
	return { v.x * s, v.y * s, v.z * s };
}

XrVector3f Cross(const XrVector3f &a, const XrVector3f &b)
{
	return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

XrVector3f Normalized(const XrVector3f &v)
{
	const float length = std::sqrt(Dot(v, v));
	return length > 1e-6F ? Scale(v, 1 / length) : XrVector3f { 0, 0, 1 };
}

/** The rotation whose x, y and z axes are the given orthonormal vectors. */
XrQuaternionf FromAxes(const XrVector3f &x, const XrVector3f &y, const XrVector3f &z)
{
	XrQuaternionf q;
	const float trace = x.x + y.y + z.z;
	if (trace > 0) {
		const float s = std::sqrt(trace + 1) * 2;
		q = { (y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, s / 4 };
	} else if (x.x > y.y && x.x > z.z) {
		const float s = std::sqrt(1 + x.x - y.y - z.z) * 2;
		q = { s / 4, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s };
	} else if (y.y > z.z) {
		const float s = std::sqrt(1 + y.y - x.x - z.z) * 2;
		q = { (y.x + x.y) / s, s / 4, (z.y + y.z) / s, (z.x - x.z) / s };
	} else {
		const float s = std::sqrt(1 + z.z - x.x - y.y) * 2;
		q = { (z.x + x.z) / s, (z.y + y.z) / s, s / 4, (x.y - y.x) / s };
	}
	return q;
}

XrCompositionLayerQuad Quad(XrSwapchain picture, int width, int height)
{
	XrCompositionLayerQuad quad { XR_TYPE_COMPOSITION_LAYER_QUAD };
	quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
	quad.space = localSpace;
	quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	quad.subImage.swapchain = picture;
	quad.subImage.imageRect.extent = { width, height };
	return quad;
}

} // namespace

uint32_t LaserLayers(XrTime time, XrCompositionLayerQuad layers[2])
{
	if (!LaserLayer || !laser.visible || !MakePictures())
		return 0;
	XrPosef head;
	if (!Locate(viewSpace, time, head))
		return 0;
	const XrVector3f along = Sub(laser.to, laser.from);
	const float length = std::sqrt(Dot(along, along));
	uint32_t count = 0;
	if (length > 0.03F) {
		// Turned about its own length to face the eyes, so it never shows edge-on.
		const XrVector3f x = Scale(along, 1 / length);
		const XrVector3f middle = Scale({ laser.from.x + laser.to.x, laser.from.y + laser.to.y, laser.from.z + laser.to.z }, 0.5F);
		const XrVector3f toHead = Sub(head.position, middle);
		const XrVector3f z = Normalized(Sub(toHead, Scale(x, Dot(toHead, x))));
		XrCompositionLayerQuad &beam = layers[count++];
		beam = Quad(beamSwapchain[laser.active ? 1 : 0], BeamWidth, BeamHeight);
		beam.pose.orientation = FromAxes(x, Cross(z, x), z);
		beam.pose.position = middle;
		beam.size = { length, 0.006F };
	}
	if (laser.onQuad) {
		// Facing the eyes, a hair in front of the panel, the same size to the eye wherever it lands.
		const XrVector3f toHead = Sub(head.position, laser.to);
		const float distance = std::sqrt(Dot(toHead, toHead));
		const XrVector3f z = Normalized(toHead);
		const XrVector3f x = std::fabs(z.y) < 0.99F ? Normalized(Cross({ 0, 1, 0 }, z)) : XrVector3f { 1, 0, 0 };
		XrCompositionLayerQuad &dot = layers[count++];
		dot = Quad(dotSwapchain, DotSize, DotSize);
		dot.pose.orientation = FromAxes(x, Cross(z, x), z);
		dot.pose.position = { laser.to.x + z.x * 0.004F, laser.to.y + z.y * 0.004F, laser.to.z + z.z * 0.004F };
		const float size = 0.006F + 0.011F * distance;
		dot.size = { size, size };
	}
	return count;
}

bool MapArrowLayer(XrCompositionLayerQuad &layer)
{
	if (!mapArrow.visible || !MakePictures())
		return false;
	layer = Quad(arrowSwapchain, ArrowSize, ArrowSize);
	layer.pose = mapArrow.pose;
	layer.size = { 0.04F, 0.04F };
	return true;
}

void ReleaseLaser()
{
	// The session takes its swapchains with it.
	beamSwapchain[0] = beamSwapchain[1] = dotSwapchain = arrowSwapchain = XR_NULL_HANDLE;
	laser = {};
	mapArrow = {};
}

} // namespace vr
} // namespace devilution

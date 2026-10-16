/**
 * @file xr_frame.cpp
 *
 * One headset frame of the 3D world: poses from OpenXR in, both eyes' images out.
 */
#include "platform/vr/vr_internal.h"

#include <cmath>
#include <string>

#include "platform/vr/render/world.h"
#include "utils/log.hpp"

namespace devilution {
namespace vr {

/** Chained to the projection views, so it has to outlive xrEndFrame. */
XrCompositionLayerDepthInfoKHR depthInfos[2];

/** Renders the 3D world for both eyes and fills in the projection layer. Returns false if nothing was rendered. */
bool RenderWorld(XrTime time, XrCompositionLayerProjectionView projectionViews[2])
{
	XrViewLocateInfo locateInfo { XR_TYPE_VIEW_LOCATE_INFO };
	locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locateInfo.displayTime = time;
	locateInfo.space = localSpace;
	XrViewState viewState { XR_TYPE_VIEW_STATE };
	XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
	uint32_t viewCount = 0;
	XrPosef head;
	// Debug aid (DEVILUTIONX_VR_FORCE_RENDER=1): a headset nobody wears is told not to render,
	// and lies wherever it was put down. The world is drawn anyway, from a made-up head standing
	// at the centre of the room and looking ahead, or turned by DEVILUTIONX_VR_DIAG_YAW degrees,
	// so the pictures can be checked with the headset on a desk.
	static const bool force = Knob("DEVILUTIONX_VR_FORCE_RENDER", 0) != 0;
	const bool tracked = XR_SUCCEEDED(xrLocateViews(session, &locateInfo, &viewState, 2, &viewCount, views)) && viewCount == 2
	    && (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0 && Locate(viewSpace, time, head);
	if (!tracked || force) {
		static bool logged;
		if (!logged && !tracked)
			LogInfo("VR: the head is not tracked ({} views, state {:#x}){}", viewCount, static_cast<unsigned>(viewState.viewStateFlags), force ? "" : ", the world is not drawn");
		logged = true;
		if (!force)
			return false;
		const float yaw = Knob("DEVILUTIONX_VR_DIAG_YAW", 0) * 0.0174533F;
		head.orientation = { 0, std::sin(yaw / 2), 0, std::cos(yaw / 2) };
		head.position = { 0, 1.6F, 0 };
		// The Steam Frame's own eyes, as its runtime reports them.
		const XrFovf fovs[2] = { { -1.028F, 0.885F, 0.864F, -1.048F }, { -0.877F, 1.033F, 0.866F, -1.042F } };
		for (int i = 0; i < 2; i++) {
			const XrVector3f offset = Rotate(head.orientation, { i == 0 ? -0.032F : 0.032F, 0, 0 });
			views[i].pose.orientation = head.orientation;
			views[i].pose.position = { head.position.x + offset.x, head.position.y + offset.y, head.position.z + offset.z };
			if (views[i].fov.angleRight - views[i].fov.angleLeft < 0.1F)
				views[i].fov = fovs[i];
		}
	}

	float headPosition[3];
	float headOrientation[4];
	ToWorld(head, headPosition, headOrientation);
	VrHandPose hands[2];
	for (const Hand hand : { Left, Right }) {
		XrPosef pose;
		hands[hand].valid = Locate(aimSpace[hand], time, pose);
		if (hands[hand].valid)
			ToWorld(pose, hands[hand].position, hands[hand].orientation);
	}
	// Counted from the first frame, so that a float still tells one frame's time from the next's hours in.
	static XrTime firstTime;
	if (firstTime == 0)
		firstTime = time;
	VrWorldSetClock(static_cast<float>(static_cast<double>(time - firstTime) * 1e-9));
	VrWorldUpdate(headPosition, headOrientation, hands[Left], hands[Right], worldPointerValid, worldPointerHit);

	// Both eyes are drawn in one pass, so both images are acquired before anything is drawn.
	VrView eyeViews[2];
	VrViewTarget targets[2] {};
	uint32_t imageIndex[2] = {};
	int acquired = 0;
	for (; acquired < 2; acquired++) {
		EyeTarget &eye = eyes[acquired];
		if (XR_FAILED(xrd3d::AcquireSwapchainImage(eye.swapchain, &imageIndex[acquired])))
			break;
		xrd3d::WaitSwapchainImage(eye.swapchain);
		targets[acquired].color = eye.views[imageIndex[acquired]];
		if (eye.depthSwapchain != XR_NULL_HANDLE) {
			uint32_t depthIndex = 0;
			if (XR_FAILED(xrd3d::AcquireSwapchainImage(eye.depthSwapchain, &depthIndex))) {
				xrd3d::ReleaseSwapchainImage(eye.swapchain);
				break;
			}
			xrd3d::WaitSwapchainImage(eye.depthSwapchain);
			targets[acquired].depth = eye.depthViews[depthIndex];
		}
		targets[acquired].maskVertices = eye.maskVertices;
		targets[acquired].maskVertexCount = eye.maskVertexCount;
		const XrFovf &fov = views[acquired].fov;
		ToWorld(views[acquired].pose, eyeViews[acquired].position, eyeViews[acquired].orientation);
		eyeViews[acquired].fov[0] = std::tan(fov.angleLeft);
		eyeViews[acquired].fov[1] = std::tan(fov.angleRight);
		eyeViews[acquired].fov[2] = std::tan(fov.angleUp);
		eyeViews[acquired].fov[3] = std::tan(fov.angleDown);
	}
	const bool complete = acquired == 2;
	if (complete) {
		VrWorldRender(eyeViews, 2, eyes[0].width, eyes[0].height, targets);
		RenderMirror();
		if (screenshotRequested) {
			screenshotRequested = false;
			SaveScreenshot(eyes[0].images[imageIndex[0]]);
		}

		static const std::string dumpDirectory = SDL_getenv("DEVILUTIONX_VR_DUMP") != nullptr ? SDL_getenv("DEVILUTIONX_VR_DUMP") : "";
		static int frameCounter;
		if (!dumpDirectory.empty() && ++frameCounter % 150 == 0) {
			DumpTexture(eyes[0].images[imageIndex[0]], (dumpDirectory + "/eye.bmp").c_str());
			DumpTexture(eyes[1].images[imageIndex[1]], (dumpDirectory + "/eye-right.bmp").c_str());
		}
	}

	for (int i = 0; i < acquired; i++) {
		EyeTarget &eye = eyes[i];
		xrd3d::ReleaseSwapchainImage(eye.swapchain);
		projectionViews[i] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
		projectionViews[i].pose = views[i].pose;
		projectionViews[i].fov = views[i].fov;
		projectionViews[i].subImage.swapchain = eye.swapchain;
		projectionViews[i].subImage.imageRect.extent = { eye.width, eye.height };
		if (eye.depthSwapchain != XR_NULL_HANDLE) {
			xrd3d::ReleaseSwapchainImage(eye.depthSwapchain);
			// The compositor reprojects with real depth instead of assuming everything is far away.
			// Depth is reversed (1 at the near plane), which OpenXR expresses by swapping the two distances.
			depthInfos[i] = { XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR };
			depthInfos[i].subImage.swapchain = eye.depthSwapchain;
			depthInfos[i].subImage.imageRect.extent = { eye.width, eye.height };
			depthInfos[i].minDepth = 0;
			depthInfos[i].maxDepth = 1;
			depthInfos[i].nearZ = VrFarPlane;
			depthInfos[i].farZ = VrNearPlane;
			projectionViews[i].next = &depthInfos[i];
		}
	}
	return complete;
}

} // namespace vr
} // namespace devilution

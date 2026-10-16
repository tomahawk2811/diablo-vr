/**
 * @file input_rules.hpp
 *
 * The decisions of the VR input that are easy to get wrong and hard to see from the outside, kept
 * free of OpenXR and of the game so that test/vr_input_rules_test.cpp can check them: which way A
 * and B go, what View's press means, which belt potion a hand reaches for, when a wall face is
 * flipped, how hard a blow buzzes.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace devilution {
namespace vr {
namespace rules {

/**
 * Whether A and B confirm and go back (the gamepad's A and B) instead of being the primary and
 * secondary action: on the Frame's controllers, in the main menus and in every in-game menu
 * (game menu, shops, dialogs, the quest log).
 */
inline bool MenuButtonsWanted(bool frameControllers, bool world3d, bool inGameMenu, bool questLogOpen)
{
	return frameControllers && (!world3d || inGameMenu || questLogOpen);
}

/**
 * The switch between menu and play buttons waits until A and B are both up, so that the A that
 * opens a shop does not also buy the first thing in it. Returns the buttons in force.
 */
inline bool LatchMenuButtons(bool &latched, bool aDown, bool bDown, bool wanted)
{
	if (!aDown && !bDown)
		latched = wanted;
	return latched;
}

/** View, the Frame's left system button: held for a second it re-centres, a shorter press is a tap. */
struct ViewButton {
	bool down;
	bool recentred;
	uint32_t since;
};

enum class ViewEvent : uint8_t {
	None,
	Recentre,
	Tap,
};

constexpr uint32_t ViewHoldMs = 1000;

/** One frame of View: `pressed` now, at `now` milliseconds. A tap is reported when the button comes up. */
inline ViewEvent StepView(ViewButton &view, bool pressed, uint32_t now)
{
	ViewEvent event = ViewEvent::None;
	if (pressed && !view.down) {
		view.since = now;
		view.recentred = false;
	}
	if (pressed && !view.recentred && now - view.since >= ViewHoldMs) {
		view.recentred = true;
		event = ViewEvent::Recentre;
	}
	if (!pressed && view.down && !view.recentred)
		event = ViewEvent::Tap;
	view.down = pressed;
	return event;
}

struct Point3 {
	float x;
	float y;
	float z;
};

inline float Distance(const Point3 &a, const Point3 &b)
{
	const float dx = a.x - b.x;
	const float dy = a.y - b.y;
	const float dz = a.z - b.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

/** The belt as the renderer hangs it (actors.cpp AddBelt): eight slots round the front of the waist. */
constexpr int BeltSlots = 8;
constexpr float WaistBelowEyes = 0.68F;
constexpr float BeltRadius = 0.27F;
constexpr float BeltSlotAngle = 0.3F;
constexpr float BeltReach = 0.13F;

/**
 * Where a belt slot hangs, for eyes at `head` looking along the level direction (`forwardX`,
 * `forwardZ`, unit length). Slot 0 is on the left, as on the game's belt.
 */
inline Point3 BeltSlotPosition(int slot, const Point3 &head, float forwardX, float forwardZ)
{
	const float angle = (static_cast<float>(slot) - 3.5F) * BeltSlotAngle;
	// Right of forward, seen from above with y up: (-forwardZ, forwardX).
	const float outX = forwardX * std::cos(angle) - forwardZ * std::sin(angle);
	const float outZ = forwardZ * std::cos(angle) + forwardX * std::sin(angle);
	return { head.x + outX * BeltRadius, head.y - WaistBelowEyes, head.z + outZ * BeltRadius };
}

/**
 * The belt slot a hand at `hand` closes on: the nearest of the ones `usable` (bit n for slot n)
 * within reach, or -1.
 */
inline int NearestBeltSlot(const Point3 &hand, const Point3 &head, float forwardX, float forwardZ, uint32_t usable)
{
	int nearest = -1;
	float best = BeltReach;
	for (int slot = 0; slot < BeltSlots; slot++) {
		if ((usable & (1U << slot)) == 0)
			continue;
		const float away = Distance(hand, BeltSlotPosition(slot, head, forwardX, forwardZ));
		if (away < best) {
			best = away;
			nearest = slot;
		}
	}
	return nearest;
}

/** A potion is drunk when the hand holding it comes this close to the mouth. */
constexpr float MouthReach = 0.14F;

/**
 * Whether flipping a wall face makes its edge meet the previous face's edge clearly better: the
 * summed brightness difference down the shared edge drops by at least a fifth. `edge` is the
 * previous face's column at the seam, `straight` and `turned` this face's two candidates.
 */
inline bool FlipForSeam(const float *edge, const float *straight, const float *turned, int from, int to)
{
	float asIs = 0;
	float flipped = 0;
	for (int i = from; i < to; i++) {
		asIs += std::fabs(edge[i] - straight[i]);
		flipped += std::fabs(edge[i] - turned[i]);
	}
	return flipped < asIs * 0.8F;
}

/** How hard a blow buzzes: a light knock for a scratch, full strength for a quarter of the life. */
inline float BlowStrength(int lifeLost, int maxLife)
{
	const float share = static_cast<float>(lifeLost) / static_cast<float>(std::max(maxLife, 1));
	return std::clamp(0.35F + share * 4, 0.35F, 1.F);
}

} // namespace rules
} // namespace vr
} // namespace devilution

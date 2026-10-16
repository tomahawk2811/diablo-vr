#include <gtest/gtest.h>

#include <array>

#include "platform/vr/input_rules.hpp"

namespace devilution {
namespace vr {
namespace rules {
namespace {

TEST(VrInputRules, MenuButtonsOnlyOnFrameControllersInMenus)
{
	// Main menus (no world shown), and in a game the game menu, shops, dialogs and the quest log.
	EXPECT_TRUE(MenuButtonsWanted(true, false, false, false));
	EXPECT_TRUE(MenuButtonsWanted(true, true, true, false));
	EXPECT_TRUE(MenuButtonsWanted(true, true, false, true));
	// In play A and B stay the primary and secondary actions.
	EXPECT_FALSE(MenuButtonsWanted(true, true, false, false));
	// Touch keeps its own mapping everywhere.
	EXPECT_FALSE(MenuButtonsWanted(false, false, false, false));
	EXPECT_FALSE(MenuButtonsWanted(false, true, true, true));
}

TEST(VrInputRules, MenuButtonsSwitchOnlyWithBothButtonsUp)
{
	bool latched = false;
	// A pressed in play opens a shop: the shop wants menu buttons, but A is still held.
	EXPECT_FALSE(LatchMenuButtons(latched, true, false, true));
	EXPECT_FALSE(LatchMenuButtons(latched, true, false, true));
	// Released: now A confirms.
	EXPECT_TRUE(LatchMenuButtons(latched, false, false, true));
	// B closes the shop while held: still menu buttons until it comes up.
	EXPECT_TRUE(LatchMenuButtons(latched, false, true, false));
	EXPECT_FALSE(LatchMenuButtons(latched, false, false, false));
}

TEST(VrInputRules, ViewShortPressIsATapOnRelease)
{
	ViewButton view {};
	EXPECT_EQ(StepView(view, true, 100), ViewEvent::None);
	EXPECT_EQ(StepView(view, true, 400), ViewEvent::None);
	EXPECT_EQ(StepView(view, false, 450), ViewEvent::Tap);
	EXPECT_EQ(StepView(view, false, 500), ViewEvent::None);
}

TEST(VrInputRules, ViewHoldRecentresOnceAndIsNoTap)
{
	ViewButton view {};
	EXPECT_EQ(StepView(view, true, 0), ViewEvent::None);
	EXPECT_EQ(StepView(view, true, 999), ViewEvent::None);
	EXPECT_EQ(StepView(view, true, 1000), ViewEvent::Recentre);
	EXPECT_EQ(StepView(view, true, 2500), ViewEvent::None);
	// Letting go after a re-centre must not also go back or open the help.
	EXPECT_EQ(StepView(view, false, 2600), ViewEvent::None);
	// The next press starts afresh.
	EXPECT_EQ(StepView(view, true, 3000), ViewEvent::None);
	EXPECT_EQ(StepView(view, false, 3100), ViewEvent::Tap);
}

TEST(VrInputRules, BeltSlotsHangRoundTheFrontOfTheWaist)
{
	const Point3 head { 1, 1.7F, 2 };
	// Looking down -z: slot 0 is to the left (-x), slot 7 to the right, all below the eyes.
	const Point3 first = BeltSlotPosition(0, head, 0, -1);
	const Point3 last = BeltSlotPosition(7, head, 0, -1);
	EXPECT_LT(first.x, head.x);
	EXPECT_GT(last.x, head.x);
	EXPECT_LT(first.z, head.z);
	EXPECT_LT(last.z, head.z);
	EXPECT_FLOAT_EQ(first.y, head.y - WaistBelowEyes);
	for (int slot = 0; slot < BeltSlots; slot++) {
		const Point3 at = BeltSlotPosition(slot, head, 0, -1);
		const float flat = std::sqrt((at.x - head.x) * (at.x - head.x) + (at.z - head.z) * (at.z - head.z));
		EXPECT_NEAR(flat, BeltRadius, 1e-5F) << "slot " << slot;
	}
}

TEST(VrInputRules, BeltGrabTakesTheNearestUsablePotionWithinReach)
{
	const Point3 head { 0, 1.7F, 0 };
	const Point3 slot2 = BeltSlotPosition(2, head, 0, -1);
	const Point3 hand { slot2.x + 0.02F, slot2.y, slot2.z };
	EXPECT_EQ(NearestBeltSlot(hand, head, 0, -1, 0xFF), 2);
	// Slot 2 holds no potion: the slots hang 8 cm apart, so the hand takes the nearest one that does.
	EXPECT_EQ(NearestBeltSlot(hand, head, 0, -1, 0xFF & ~(1U << 2)), 3);
	EXPECT_EQ(NearestBeltSlot(hand, head, 0, -1, 1U << 6), -1);
	// Out of reach.
	const Point3 far { slot2.x, slot2.y + 0.3F, slot2.z };
	EXPECT_EQ(NearestBeltSlot(far, head, 0, -1, 0xFF), -1);
	// The belt turns with the head: where slot 2 hangs looking down +x, nothing hangs looking down +z.
	const Point3 turned = BeltSlotPosition(2, head, 1, 0);
	EXPECT_EQ(NearestBeltSlot(turned, head, 1, 0, 0xFF), 2);
	EXPECT_EQ(NearestBeltSlot(turned, head, 0, 1, 0xFF), -1);
}

TEST(VrInputRules, FlipOnlyWhenTheSeamGetsClearlyBetter)
{
	const std::array<float, 4> edge { 10, 20, 30, 40 };
	const std::array<float, 4> same { 10, 20, 30, 40 };
	const std::array<float, 4> other { 40, 30, 20, 10 };
	const std::array<float, 4> nearly { 11, 21, 31, 41 };
	const std::array<float, 4> slightlyBetter { 11, 21, 31, 40.5F };
	EXPECT_FALSE(FlipForSeam(edge.data(), same.data(), other.data(), 0, 4));
	EXPECT_TRUE(FlipForSeam(edge.data(), other.data(), same.data(), 0, 4));
	// Barely better flipped (3.5 against 4): leave it.
	EXPECT_FALSE(FlipForSeam(edge.data(), nearly.data(), slightlyBetter.data(), 0, 4));
	// Only the rows asked about count.
	EXPECT_FALSE(FlipForSeam(edge.data(), other.data(), same.data(), 2, 2));
}

TEST(VrInputRules, BlowStrengthGrowsWithTheLifeLost)
{
	EXPECT_FLOAT_EQ(BlowStrength(1, 1000), 0.354F);
	EXPECT_FLOAT_EQ(BlowStrength(0, 100), 0.35F);
	EXPECT_FLOAT_EQ(BlowStrength(25, 100), 1.F);
	EXPECT_FLOAT_EQ(BlowStrength(80, 100), 1.F);
	EXPECT_LT(BlowStrength(5, 100), BlowStrength(10, 100));
	// No division by zero for a character without life yet.
	EXPECT_FLOAT_EQ(BlowStrength(1, 0), 1.F);
}

} // namespace
} // namespace rules
} // namespace vr
} // namespace devilution

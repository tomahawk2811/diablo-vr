/**
 * @file xr_haptics.cpp
 *
 * What the hands feel, from what the game does. Nothing here changes the game: each frame it
 * compares the player, the items and the objects nearby with the frame before and buzzes the
 * hand that did it. The strength is the Haptics setting (Pulse).
 */
#include "platform/vr/vr_internal.h"

#include "platform/vr/input_rules.hpp"

#include <algorithm>
#include <unordered_map>

#include "control/control.hpp"
#include "cursor.h"
#include "diablo.h"
#include "levels/gendung.h"
#include "objects.h"
#include "player.h"
#include "qol/visual_store.h"
#include "stores.h"

namespace devilution {
namespace vr {
namespace {

struct Watched {
	bool known;
	int level;
	int levelType;
	int life;
	int mana;
	int characterLevel;
	int gold;
	int items;
	bool holding;
	std::string hover;
	int storeLine;
	int storeItem;
	bool clickDown;
};

Watched before;
/** The state of the objects within reach, and when each last buzzed. */
std::unordered_map<int, uint32_t> objectState;
std::unordered_map<int, Uint32> objectBuzzed;

int CountItems(const Player &player)
{
	int count = player._pNumInv;
	for (const Item &item : player.SpdList)
		count += item.isEmpty() ? 0 : 1;
	for (const Item &item : player.InvBody)
		count += item.isEmpty() ? 0 : 1;
	return count;
}

/** What changes when the player uses an object: opened, broken, pulled, prayed at. Not its animation. */
uint32_t ObjectState(const Object &object)
{
	uint32_t state = static_cast<uint32_t>(object.selectionRegion);
	state = state * 31 + static_cast<uint32_t>(static_cast<uint8_t>(object._oBreak));
	state = state * 31 + (object._oSolidFlag ? 1 : 0);
	if (object.isDoor())
		state = state * 31 + static_cast<uint32_t>(object._oVar4);
	if (object.IsChest())
		state = state * 31 + static_cast<uint32_t>(object._oAnimFrame);
	return state;
}

void WatchObjects(Hand hand, bool buzz)
{
	const Point at = MyPlayer->position.tile;
	const Uint32 now = SDL_GetTicks();
	for (int i = 0; i < ActiveObjectCount; i++) {
		const int index = ActiveObjects[i];
		const Object &object = Objects[index];
		if (object.position.ApproxDistance(at) > 2)
			continue;
		const uint32_t state = ObjectState(object);
		auto [entry, added] = objectState.try_emplace(index, state);
		if (added || entry->second == state)
			continue;
		entry->second = state;
		// One buzz per object a second: a sarcophagus's lid slides over several frames.
		Uint32 &last = objectBuzzed[index];
		if (!buzz || now - last < 1000)
			continue;
		last = now;
		if (object.isDoor())
			Pulse(hand, 0.35F, 50);
		else if (object.IsChest() || object._oBreak != 0 || object._otype == OBJ_SARC)
			Pulse(hand, 0.6F, 70);
		else
			Pulse(hand, 0.5F, 90); // Shrines, levers, books, fountains.
	}
}

} // namespace

void UpdateHaptics(Hand main, Hand off, bool onPanel, Hand pointingHand, bool mouseDown)
{
	if (!gbRunGame || MyPlayer == nullptr) {
		before.known = false;
		return;
	}
	const Player &me = *MyPlayer;
	Watched now;
	now.known = true;
	now.level = currlevel;
	now.levelType = leveltype;
	now.life = me._pHitPoints;
	now.mana = me._pMana;
	now.characterLevel = me.getCharacterLevel();
	now.gold = me._pGold;
	now.items = CountItems(me);
	now.holding = !me.HoldItem.isEmpty();
	now.hover = onPanel ? std::string(InfoString.str()) : std::string();
	now.storeLine = onPanel ? CurrentTextLine : -1;
	now.storeItem = onPanel ? pcursstoreitem : -1;
	now.clickDown = onPanel && mouseDown;

	// A new level or a loaded game changes everything at once: nothing to feel.
	const bool settled = before.known && now.level == before.level && now.levelType == before.levelType && !MyPlayerIsDead;
	if (!settled) {
		objectState.clear();
		objectBuzzed.clear();
	}
	if (settled) {
		const int maxLife = std::max(me._pMaxHP, 1);
		const int maxMana = std::max(me._pMaxMana, 1);
		if (now.life < before.life) {
			// A blow: both hands, harder the more it took.
			const float strength = rules::BlowStrength(before.life - now.life, maxLife);
			Pulse(Left, strength, 60);
			Pulse(Right, strength, 60);
		} else if (now.life - before.life > maxLife / 33) {
			Pulse(off, 0.3F, 90); // A gulp of a potion, or a shrine's healing.
		}
		if (now.mana - before.mana > maxMana / 20)
			Pulse(off, 0.2F, 60);
		if (now.characterLevel > before.characterLevel) {
			Pulse(Left, 0.7F, 200);
			Pulse(Right, 0.7F, 200);
		}
		if (now.items > before.items || (now.holding && !before.holding))
			Pulse(main, 0.45F, 40); // Picked up, into the pack or onto the hand.
		else if (!now.holding && before.holding)
			Pulse(main, 0.3F, 25); // Put down.
		if (now.gold > before.gold)
			Pulse(main, 0.3F, 30);
		// The interface: a light tick on moving onto something, a firmer one on a click.
		const bool hovered = (!now.hover.empty() && now.hover != before.hover)
		    || (now.storeLine != before.storeLine && now.storeLine >= 0) || (now.storeItem != before.storeItem && now.storeItem >= 0);
		if (now.clickDown && !before.clickDown)
			Pulse(pointingHand, 0.35F, 20);
		else if (hovered)
			Pulse(pointingHand, 0.15F, 10);
	}
	WatchObjects(main, settled);
	before = now;
}

} // namespace vr
} // namespace devilution

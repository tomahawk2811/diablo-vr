/**
 * @file vr_menu.cpp
 *
 * The VR settings in the game menu (Options, VR Settings): the settings page of options.h
 * (VrOptions) as three short pages of the game's own menu, so they can be changed in the headset
 * during a game. Choosing a line steps its value; every change is saved at once.
 */
#include "platform/vr/vr_internal.h"

#include <array>
#include <string>

#include "gmenu.h"
#include "options.h"
#include "utils/str_cat.hpp"

namespace devilution {
namespace vr {
namespace {

void (*backToOptions)(bool);

/** The lines' texts: the menu keeps pointers to them. */
std::array<std::string, 6> labels;

void ShowTop(bool);
void ShowComfort(bool);
void ShowInterface(bool);
void ShowHands(bool);

template <typename T>
void Step(OptionEntryEnum<T> &entry, int count)
{
	entry.SetValue(static_cast<T>((static_cast<int>(*entry) + 1) % count));
}

void StepList(OptionEntryListBase &entry)
{
	entry.SetActiveListIndex((entry.GetActiveListIndex() + 1) % entry.GetListSize());
}

std::string OnOff(bool on)
{
	return on ? "On" : "Off";
}

// ---- Comfort

TMenuItem comfortMenu[] = {
	// clang-format off
	{ GMENU_ENABLED, nullptr, nullptr },
	{ GMENU_ENABLED, nullptr, nullptr },
	{ GMENU_ENABLED, nullptr, nullptr },
	{ GMENU_ENABLED, nullptr, nullptr },
	{ GMENU_ENABLED, nullptr, nullptr },
	{ GMENU_ENABLED, "Previous Menu", &ShowTop },
	{ GMENU_ENABLED, nullptr, nullptr },
	// clang-format on
};

void LabelComfort()
{
	VrOptions &settings = GetOptions().Vr;
	const bool smooth = *settings.turnMode == VrTurnMode::Smooth;
	labels[0] = smooth ? "Turning: Smooth" : "Turning: Snap";
	labels[1] = StrCat("Snap Angle: ", *settings.snapAngle);
	labels[2] = StrCat("Turn Speed: ", *settings.smoothTurnSpeed);
	static constexpr const char *Vignettes[] = { "Off", "Low", "Medium", "High" };
	labels[3] = StrCat("Vignette: ", Vignettes[static_cast<int>(*settings.vignette)]);
	labels[4] = "Seated: " + OnOff(*settings.seated);
	for (int i = 0; i < 5; i++)
		comfortMenu[i].pszStr = labels[i].c_str();
	// What does not apply is shown, but greyed out.
	comfortMenu[1].setEnabled(!smooth);
	comfortMenu[2].setEnabled(smooth);
}

void ComfortChanged()
{
	SaveOptions();
	LabelComfort();
}

void ComfortTurning(bool)
{
	Step(GetOptions().Vr.turnMode, 2);
	ComfortChanged();
}
void ComfortSnapAngle(bool)
{
	StepList(GetOptions().Vr.snapAngle);
	ComfortChanged();
}
void ComfortTurnSpeed(bool)
{
	StepList(GetOptions().Vr.smoothTurnSpeed);
	ComfortChanged();
}
void ComfortVignette(bool)
{
	Step(GetOptions().Vr.vignette, 4);
	ComfortChanged();
}
void ComfortSeated(bool)
{
	GetOptions().Vr.seated.SetValue(!*GetOptions().Vr.seated);
	ComfortChanged();
}

void ShowComfort(bool)
{
	comfortMenu[0].fnMenu = &ComfortTurning;
	comfortMenu[1].fnMenu = &ComfortSnapAngle;
	comfortMenu[2].fnMenu = &ComfortTurnSpeed;
	comfortMenu[3].fnMenu = &ComfortVignette;
	comfortMenu[4].fnMenu = &ComfortSeated;
	LabelComfort();
	gmenu_set_items(comfortMenu, nullptr);
}

// ---- Interface

TMenuItem interfaceMenu[] = {
	// clang-format off
	{ GMENU_ENABLED, nullptr, nullptr },
	{ GMENU_ENABLED, nullptr, nullptr },
	{ GMENU_ENABLED, nullptr, nullptr },
	{ GMENU_ENABLED, nullptr, nullptr },
	{ GMENU_ENABLED, nullptr, nullptr },
	{ GMENU_ENABLED, "Previous Menu", &ShowTop },
	{ GMENU_ENABLED, nullptr, nullptr },
	// clang-format on
};

void LabelInterface()
{
	VrOptions &settings = GetOptions().Vr;
	labels[0] = *settings.dominantHand == VrHand::Left ? "Hand: Left" : "Hand: Right";
	labels[1] = StrCat("Haptics: ", *settings.haptics, "%");
	labels[2] = *settings.hud == VrHudPlace::Wrists ? "HUD: Wrists" : "HUD: Strip";
	labels[3] = "See-through: " + OnOff(*settings.seeThroughPanels);
	labels[4] = "Autosave: " + OnOff(*settings.autosave);
	for (int i = 0; i < 5; i++)
		interfaceMenu[i].pszStr = labels[i].c_str();
}

void InterfaceChanged()
{
	SaveOptions();
	LabelInterface();
}

void InterfaceHand(bool)
{
	Step(GetOptions().Vr.dominantHand, 2);
	InterfaceChanged();
}
void InterfaceHaptics(bool)
{
	StepList(GetOptions().Vr.haptics);
	InterfaceChanged();
}
void InterfaceHud(bool)
{
	Step(GetOptions().Vr.hud, 2);
	InterfaceChanged();
}
void InterfaceSeeThrough(bool)
{
	GetOptions().Vr.seeThroughPanels.SetValue(!*GetOptions().Vr.seeThroughPanels);
	InterfaceChanged();
}
void InterfaceAutosave(bool)
{
	GetOptions().Vr.autosave.SetValue(!*GetOptions().Vr.autosave);
	InterfaceChanged();
}

void ShowInterface(bool)
{
	interfaceMenu[0].fnMenu = &InterfaceHand;
	interfaceMenu[1].fnMenu = &InterfaceHaptics;
	interfaceMenu[2].fnMenu = &InterfaceHud;
	interfaceMenu[3].fnMenu = &InterfaceSeeThrough;
	interfaceMenu[4].fnMenu = &InterfaceAutosave;
	LabelInterface();
	gmenu_set_items(interfaceMenu, nullptr);
}

// ---- Hands

TMenuItem handsMenu[] = {
	// clang-format off
	{ GMENU_ENABLED, nullptr, nullptr },
	{ GMENU_ENABLED, nullptr, nullptr },
	{ GMENU_ENABLED, nullptr, nullptr },
	{ GMENU_ENABLED, "Previous Menu", &ShowTop },
	{ GMENU_ENABLED, nullptr, nullptr },
	// clang-format on
};

void LabelHands()
{
	VrOptions &settings = GetOptions().Vr;
	labels[0] = "Item Cards: " + OnOff(*settings.itemCards);
	labels[1] = "Belt Grab: " + OnOff(*settings.beltGrab);
	labels[2] = "Spell Gestures: " + OnOff(*settings.spellGestures);
	for (int i = 0; i < 3; i++)
		handsMenu[i].pszStr = labels[i].c_str();
}

void Toggle(OptionEntryBoolean &entry)
{
	entry.SetValue(!*entry);
	SaveOptions();
	LabelHands();
}

void HandsItemCards(bool)
{
	Toggle(GetOptions().Vr.itemCards);
}
void HandsBeltGrab(bool)
{
	Toggle(GetOptions().Vr.beltGrab);
}
void HandsSpellGestures(bool)
{
	Toggle(GetOptions().Vr.spellGestures);
}

void ShowHands(bool)
{
	handsMenu[0].fnMenu = &HandsItemCards;
	handsMenu[1].fnMenu = &HandsBeltGrab;
	handsMenu[2].fnMenu = &HandsSpellGestures;
	LabelHands();
	gmenu_set_items(handsMenu, nullptr);
}

// ---- The page the Options menu opens

void Back(bool activate)
{
	if (backToOptions != nullptr)
		backToOptions(activate);
}

TMenuItem topMenu[] = {
	// clang-format off
	{ GMENU_ENABLED, "Comfort",       &ShowComfort   },
	{ GMENU_ENABLED, "Interface",     &ShowInterface },
	{ GMENU_ENABLED, "Hands",         &ShowHands     },
	{ GMENU_ENABLED, "Previous Menu", &Back          },
	{ GMENU_ENABLED, nullptr,         nullptr        },
	// clang-format on
};

void ShowTop(bool)
{
	gmenu_set_items(topMenu, nullptr);
}

} // namespace
} // namespace vr

void VrSettingsMenu(void (*back)(bool))
{
	vr::backToOptions = back;
	vr::ShowTop(true);
}

} // namespace devilution

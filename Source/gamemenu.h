/**
 * @file gamemenu.h
 *
 * Interface of the in-game menu functions.
 */
#pragma once

#include "utils/attributes.h"

namespace devilution {

void gamemenu_on();
void gamemenu_off();
void gamemenu_handle_previous();
void gamemenu_exit_game(bool bActivate);
void gamemenu_quit_game(bool bActivate);
void gamemenu_load_game(bool bActivate);
void gamemenu_load_autosave(bool bActivate);
void gamemenu_save_game(bool bActivate);
/** @brief Whether the menu offers Load Autosave: the single player hero has an autosave (not in demos) */
bool gamemenu_can_load_autosave();

extern DVL_API_FOR_TEST bool isGameMenuOpen;

} // namespace devilution

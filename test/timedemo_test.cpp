#include <cstdio>
#include <expected>
#include <filesystem>
#include <gtest/gtest.h>
#include <iostream>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#endif

#include "engine/assets.hpp"
#include "engine/demomode.h"
#include "game_mode.hpp"
#include "headless_mode.hpp"
#include "init.hpp"
#include "levels/gendung.h"
#include "loadsave.h"
#include "lua/lua_global.hpp"
#include "mpq/mpq_reader.hpp"
#include "options.h"
#include "pfile.h"
#include "tables/monstdat.h"
#include "tables/playerdat.hpp"
#include "utils/display.h"
#include "utils/file_util.h"
#include "utils/paths.h"

using namespace devilution;

namespace {

bool Dummy_GetHeroInfo(_uiheroinfo *pInfo)
{
	return true;
}

/** @brief Raw (still encoded) content of a file in a save archive, empty if it is missing */
std::vector<std::byte> ReadSaveEntry(const std::string &archivePath, const char *name)
{
	std::expected<MpqArchive, std::string> archive = MpqArchive::Open(archivePath.c_str());
	if (!archive.has_value())
		return {};
	size_t size;
	int32_t error;
	const std::unique_ptr<std::byte[]> data = archive->ReadFile(name, size, error);
	if (error != 0 || data == nullptr)
		return {};
	return { data.get(), data.get() + size };
}

/** @brief What the hero selection lists for the demo's hero (save number 0) */
_uiheroinfo GetHeroInfo()
{
	static _uiheroinfo heroInfo;
	heroInfo = {};
	pfile_ui_set_hero_infos([](_uiheroinfo *info) {
		if (info->saveNumber == 0)
			heroInfo = *info;
		return true;
	});
	return heroInfo;
}

/** @brief Names of the level entries of a save archive with the given prefix ("perm" or "temp") */
std::vector<std::string> LevelEntryNames(const char *prefix)
{
	std::vector<std::string> names;
	for (const char suffix : { 'l', 's' }) {
		for (int i = 0; i < 17; i++) {
			char name[16];
			std::snprintf(name, sizeof(name), "%s%c%02d", prefix, suffix, i);
			names.emplace_back(name);
		}
	}
	return names;
}

/**
 * @brief Checks the autosave written during the demo
 *
 * The demo ends right after its last level change, so the autosave holds the end state. It must hold the
 * levels the regular save keeps (the in-play temp levels as perm levels), it must load, and neither loading
 * it nor the autosave itself may change what the regular save holds.
 */
void CheckAutosave(const std::string &savePath, const std::string &autosavePath, uint8_t endLevel)
{
	ASSERT_TRUE(FileExists(autosavePath)) << "no autosave after the level changes";
	EXPECT_FALSE(ReadSaveEntry(autosavePath, "game").empty());
	EXPECT_FALSE(ReadSaveEntry(autosavePath, "hero").empty());

	const std::vector<std::string> permNames = LevelEntryNames("perm");
	const std::vector<std::string> tempNames = LevelEntryNames("temp");
	std::vector<std::vector<std::byte>> autosaveLevels;
	std::vector<std::vector<std::byte>> permLevels;
	size_t inPlayLevels = 0;
	for (size_t i = 0; i < permNames.size(); i++) {
		autosaveLevels.push_back(ReadSaveEntry(autosavePath, permNames[i].c_str()));
		permLevels.push_back(ReadSaveEntry(savePath, permNames[i].c_str()));
		const std::vector<std::byte> tempLevel = ReadSaveEntry(savePath, tempNames[i].c_str());
		if (!tempLevel.empty())
			inPlayLevels++;
		EXPECT_EQ(autosaveLevels[i], tempLevel.empty() ? permLevels[i] : tempLevel) << permNames[i];
		EXPECT_TRUE(ReadSaveEntry(autosavePath, tempNames[i].c_str()).empty()) << tempNames[i];
	}
	EXPECT_GT(inPlayLevels, 0U) << "the demo should leave levels behind as temp levels";

	const std::vector<std::byte> game = ReadSaveEntry(savePath, "game");
	const std::vector<std::byte> hero = ReadSaveEntry(savePath, "hero");

	ASSERT_TRUE(LoadAutosave().has_value());
	EXPECT_EQ(currlevel, endLevel);
	EXPECT_EQ(ReadSaveEntry(savePath, "game"), game);
	EXPECT_EQ(ReadSaveEntry(savePath, "hero"), hero);
	for (size_t i = 0; i < permNames.size(); i++) {
		EXPECT_EQ(ReadSaveEntry(savePath, permNames[i].c_str()), permLevels[i]) << permNames[i];
		// The autosave's levels are now the in-play levels.
		EXPECT_EQ(ReadSaveEntry(savePath, tempNames[i].c_str()), autosaveLevels[i]) << tempNames[i];
	}

	// Load Game afterwards loads the regular save (on level 1) and drops the levels that came from the autosave.
	ASSERT_TRUE(LoadGame(false).has_value());
	EXPECT_EQ(currlevel, 1);
	EXPECT_EQ(ReadSaveEntry(savePath, "game"), game);
	for (const std::string &name : tempNames)
		EXPECT_TRUE(ReadSaveEntry(savePath, name.c_str()).empty()) << name;

	// The hero selection offers the autosave, and continuing from it there starts the game from the autosave.
	const _uiheroinfo heroInfo = GetHeroInfo();
	EXPECT_TRUE(heroInfo.hassaved);
	EXPECT_TRUE(heroInfo.hasautosave);
	ASSERT_TRUE(LoadAutosave(/*firstflag=*/true).has_value());
	EXPECT_EQ(currlevel, endLevel);
	for (size_t i = 0; i < permNames.size(); i++)
		EXPECT_EQ(ReadSaveEntry(savePath, tempNames[i].c_str()), autosaveLevels[i]) << tempNames[i];
}

void RunTimedemo(std::string timedemoFolderName, bool autosave = false)
{
	if (
#ifdef USE_SDL3
	    !SDL_Init(SDL_INIT_EVENTS)
#elif !defined(USE_SDL1)
	    SDL_Init(SDL_INIT_EVENTS) < 0
#else
	    SDL_Init(0) < 0
#endif
	) {
		ErrSdl();
	}

	LoadCoreArchives();
	LoadGameArchives();

	// The tests need spawn.mpq or diabdat.mpq
	if (!HaveMainData()) {
		GTEST_SKIP() << "MPQ assets (spawn.mpq or DIABDAT.MPQ) not found - skipping test";
	}

	const std::string unitTestFolderCompletePath = paths::BasePath() + "test/fixtures/timedemo/" + timedemoFolderName;
	paths::SetPrefPath(unitTestFolderCompletePath);
	paths::SetConfigPath(unitTestFolderCompletePath);

	InitKeymapActions();
	LoadOptions();
	demo::OverrideOptions();
	LuaInitialize();

	const int demoNumber = 0;

	Players.resize(1);
	MyPlayerId = demoNumber;
	MyPlayer = &Players[MyPlayerId];
	*MyPlayer = {};

	// Currently only spawn.mpq is present when building on github actions
	gbIsSpawn = true;
	gbIsHellfire = false;
	gbMusicOn = false;
	gbSoundOn = false;
	HeadlessMode = true;
	demo::InitPlayBack(demoNumber, true);

	LoadSpellData();
	LoadPlayerDataFiles();
	LoadMissileData();
	LoadMonsterData();
	LoadItemData();
	LoadObjectData();
	pfile_ui_set_hero_infos(Dummy_GetHeroInfo);
	gbLoadGame = true;

	demo::OverrideOptions();

	AdjustToScreenGeometry(forceResolution);

	const std::string savePath = unitTestFolderCompletePath + "/spawn_0.sv";
	const std::string autosavePath = unitTestFolderCompletePath + "/spawn_0_auto.sv";
	RemoveFile(autosavePath.c_str());
	AutosaveEnabled = autosave;

	StartGame(false, true);

	// With autosave on, the regular save must still come out the same as the reference.
	const HeroCompareResult result = pfile_compare_hero_demo(demoNumber, true);
	ASSERT_EQ(result.status, HeroCompareResult::Same) << result.message;
	ASSERT_FALSE(gbRunGame);
	if (autosave) {
		CheckAutosave(savePath, autosavePath, currlevel);
	} else {
		EXPECT_FALSE(FileExists(autosavePath));
		const _uiheroinfo heroInfo = GetHeroInfo();
		EXPECT_TRUE(heroInfo.hassaved);
		EXPECT_FALSE(heroInfo.hasautosave);
	}
	AutosaveEnabled = false;
	RemoveFile(autosavePath.c_str());
	gbRunGame = false;
	init_cleanup();
	LuaShutdown();
	SDL_Quit();
}

} // namespace

TEST(Timedemo, WarriorLevel1to2)
{
	RunTimedemo("WarriorLevel1to2");
}

TEST(Timedemo, WarriorLevel1to2Autosave)
{
	RunTimedemo("WarriorLevel1to2", /*autosave=*/true);
}

#pragma once

// RPCS3's per-game configuration database (bin/game_configs/config_database.json:
// RPCS3's own, as its Qt application downloads it, pinned in this fork and
// staged as the core's game_configs/config_database.json): the settings RPCS3
// recommends for each game, which it adds to the configuration at boot
// (Emulator::BootGame asks the frontend's get_database_config).

#include "util/types.hpp"

#include <string>

namespace libretro_game_configs
{
	// A game's entry, by its serial, as the configuration text RPCS3 adds at
	// boot; empty when the database has none. Settings the core fixes itself
	// (libretro_options::fixed_by_core) are left out. A game drawn at 1080p gets
	// the resolution scale that keeps its frame at `resolution_scale`'s 720p-based
	// size (the core's frame is 2160p at 300%). A frame limit of 30 is left out
	// when the game's frame-rate patch is on.
	std::string for_game(const std::string& serial, u32 resolution_scale, bool frame_rate_patch);

	// A configuration (YAML, possibly empty) with RPCS3's frame limit at 30.
	std::string with_frame_limit_30(const std::string& config);
}

#pragma once

#include "libretro_patches.h"

// RPCS3's settings as RetroArch core options (libretro v2, one category per
// section of RPCS3's configuration), built from the configuration tree itself
// (g_cfg): every setting with a set of values, its names, its default and
// whether it takes effect while a game runs come from there. What the core
// fixes itself (the renderer, the audio backend, the input devices, the Qt
// application's window behaviour) is left out.

#include "libretro.h"

struct cfg_root;

namespace libretro_options
{
	// Declares the options to RetroArch (retro_set_environment).
	void declare(retro_environment_t environ_cb);

	// RetroArch's values into a configuration (the global config.yml the boot
	// reads), with what the core fixes itself. True when anything changed.
	bool apply(retro_environment_t environ_cb, cfg_root& settings);

	// Options changed while a game runs: those RPCS3 takes at once go to the
	// running configuration; the rest wait for the next boot.
	void apply_live(retro_environment_t environ_cb);

	// Whether a setting (its path, section first: "Video/Renderer") is one the
	// core fixes itself.
	bool fixed_by_core(std::string_view path);

	// Whether games boot with RPCS3's per-game settings (libretro_game_configs.h).
	bool game_settings(retro_environment_t environ_cb);

	// Which frame-rate patches games boot with (libretro_patches.h).
	libretro_patches::frame_rate frame_rate_patches(retro_environment_t environ_cb);

	// The resolution scale RetroArch has set, in percent.
	u32 resolution_scale(retro_environment_t environ_cb);
}

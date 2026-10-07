#pragma once

// RPCS3's game patch database (bin/patches/patch.yml: RPCS3's own, pinned in
// this fork, staged as the core's patches/patch.yml), the fork's patches for
// games it lacks (bin/patches/<serial>_patch.yml), and which of their patches a
// boot applies. With frame-rate patches on, every game the database has one for
// gets it: "60 FPS" where there is one, else "Unlock FPS" (RPCS3's frame limit
// keeps an unlocked game at the display's rate), one per executable, serial and
// version. Other patches stay as RPCS3's patch_config.yml has them.

#include "util/types.hpp"

#include <string>

namespace libretro_patches
{
	// Which frame-rate patches games boot with: none; a game's patch made for
	// 60 fps ("60 FPS", "Full 60 FPS") or else its "Unlock FPS" patch with
	// RPCS3's frame limit at 30 (a 30 fps game paced by the emulator's timer
	// rather than by vertical blanks, so a frame a little over 33 ms does not
	// become a 50 ms one); or the same without the limit (as fast as the
	// emulation goes, up to the display's rate).
	enum class frame_rate { off, sixty, unlocked };

	// Writes RPCS3's patch_config.yml for the next boot, when it changes.
	// Returns how many executable, serial and version entries have their
	// frame-rate patch on.
	usz prepare(frame_rate choice);

	// Whether the last prepare() turned a frame-rate patch on for this serial.
	bool frame_rate_patch_on(const std::string& serial);

	// Whether the last prepare() gave this serial an Unlock FPS patch to run
	// at RPCS3's frame limit of 30 (frame_rate::sixty).
	bool frame_limit_30(const std::string& serial);
}

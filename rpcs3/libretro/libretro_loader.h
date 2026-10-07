#pragma once

// The loading screen the libretro core shows while RPCS3 installs the firmware
// or a package, and while a game starts, until RPCS3's first frame. It is
// drawn on the CPU and handed to RetroArch as a Vulkan image, one per
// RetroArch frame index.

#include "libretro.h"
#include "Emu/RSX/VK/VulkanAPI.h"
#include "libretro_vulkan.h"

#include <string>
#include <vector>

namespace libretro_loader
{
	struct screen
	{
		std::string heading;    // what happens, above the title: "Installing"
		std::string title;      // the game's name, or the firmware's
		std::string serial;     // the title ID, when there is one
		std::string detail;     // below the bar, on the left: sizes, counts
		std::string remaining;  // below the bar, on the right: percentage, time left
		std::string image_dir;  // the folder with the game's ICON0.PNG and PIC1.PNG
		double progress = -1;   // 0 to 1; negative while the amount is unknown
		bool failed = false;    // the heading says what went wrong
	};

	// The folders searched for the screen's fonts, in order: Inter (staged in
	// RPCS3's folder), then the firmware's own.
	void set_font_dirs(std::vector<std::string> dirs);

	// On RetroArch's thread, in retro_run: draws the screen for RetroArch's
	// current frame index and hands it over, at most `width` by `height`.
	void present(retro_hw_render_interface_vulkan& vulkan, retro_video_refresh_t video, const screen& shown, unsigned width, unsigned height);

	// Whether the screen holds Vulkan objects.
	bool active();

	// Frees them once the GPU is done with them: when the game's frames have
	// replaced the screen, or when RetroArch's context goes.
	void release(retro_hw_render_interface_vulkan& vulkan);
}

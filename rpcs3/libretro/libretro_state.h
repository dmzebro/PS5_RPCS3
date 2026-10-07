#pragma once

// Where the libretro core keeps an RPCS3 savestate: a file system in memory,
// mounted as a virtual device, which RPCS3 writes a savestate to
// (Emulator::savestate_path_override) and boots one from, the bytes RetroArch's
// save states carry. Nothing goes through the console's storage, which holds a
// title to a slow write rate after a burst (PS5_RetroArch docs/RPCS3_PORT.md).

#include "util/types.hpp"

#include <string>
#include <vector>

namespace libretro_state
{
	// The path of a file on the device; the device is mounted on first use.
	std::string path(std::string_view name);

	// A file's bytes, or none when there is no such file.
	std::vector<u8> read(const std::string& path);

	// Replaces a file with these bytes.
	void write(const std::string& path, std::vector<u8> bytes);

	void remove(const std::string& path);
}

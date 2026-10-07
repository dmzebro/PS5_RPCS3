#include "stdafx.h"
#include "libretro_patches.h"

#include "Utilities/File.h"
#include "Utilities/StrUtil.h"
#include "Utilities/bin_patch.h"

#include <map>
#include <set>
#include <tuple>

LOG_CHANNEL(libretro_patch_log, "libretro");

namespace libretro_patches
{
	namespace
	{
		// The serials whose frame-rate patch the last prepare() turned on, and
		// those of them whose patch unlocks the frame rate, held at 30.
		std::set<std::string> g_patched_serials;
		std::set<std::string> g_limited_serials;

		// How much a frame-rate patch is preferred; 0 for a patch that is not
		// one to turn on by itself: another rate (30, 50, 120), a variable or
		// fallback variant, fixes meant to go with another patch.
		int rank(std::string_view name)
		{
			const std::string lower = fmt::to_lower(name);
			const auto has = [&](std::string_view part) { return lower.find(part) != std::string::npos; };
			if (lower == "60 fps" || lower == "60fps")
				return 4;
			if (lower == "full 60fps" || lower == "full 60 fps" || (lower.starts_with("60 fps (") && !has("in case of issues")))
				return 3;
			if (lower == "unlock fps")
				return 2;
			if (lower.starts_with("unlock fps (") && !has("no user input"))
				return 1;
			return 0;
		}
	}

	bool frame_rate_patch_on(const std::string& serial)
	{
		return g_patched_serials.contains(serial);
	}

	bool frame_limit_30(const std::string& serial)
	{
		return g_limited_serials.contains(serial);
	}

	usz prepare(frame_rate choice)
	{

		patch_engine::patch_map map;
		const std::string database = patch_engine::get_patches_path() + "patch.yml";
		if (!fs::is_file(database) || !patch_engine::load(map, database))
		{
			libretro_patch_log.error("No patch database at %s", database);
			return 0;
		}
		// patch_config.yml is written from this map, so patches imported by hand
		// keep their state too; a game's own file (<serial>_patch.yml, which
		// RPCS3 reads for that game) holds patches the database lacks.
		if (const std::string imported = patch_engine::get_imported_patch_path(); fs::is_file(imported))
			patch_engine::load(map, imported);
		for (const fs::dir_entry& entry : fs::dir(patch_engine::get_patches_path()))
		{
			if (!entry.is_directory && entry.name.ends_with("_patch.yml") && entry.name != "imported_patch.yml")
				patch_engine::load(map, patch_engine::get_patches_path() + entry.name);
		}

		usz on = 0;
		bool changed = false;
		g_patched_serials.clear();
		g_limited_serials.clear();
		for (auto& [hash, container] : map)
		{
			// The best frame-rate patch of each serial and version; ties go to the
			// first name, so the choice does not depend on the map's order.
			std::map<std::pair<std::string, std::string>, std::tuple<int, std::string, patch_engine::patch_config_values*>> best;
			std::vector<patch_engine::patch_config_values*> candidates;
			for (auto& [description, info] : container.patch_info_map)
			{
				const int score = rank(description);
				if (!score)
					continue;
				const bool allowed = choice != frame_rate::off;
				for (auto& [title, serials] : info.titles)
					for (auto& [serial, versions] : serials)
						for (auto& [version, values] : versions)
						{
							candidates.push_back(&values);
							if (!allowed)
								continue;
							auto& chosen = best[{serial, version}];
							if (score > std::get<0>(chosen) || (score == std::get<0>(chosen) && description < std::get<1>(chosen)))
								chosen = {score, description, &values};
						}
			}
			for (patch_engine::patch_config_values* values : candidates)
			{
				bool wanted = false;
				if (choice != frame_rate::off)
					for (const auto& [key, chosen] : best)
						if (std::get<2>(chosen) == values)
						{
							wanted = true;
							g_patched_serials.insert(key.first);

							// An unlock patch (ranks 1 and 2) runs at 30 unless unlocked is asked for
							if (choice == frame_rate::sixty && std::get<0>(chosen) <= 2)
								g_limited_serials.insert(key.first);
						}
				changed |= values->enabled != wanted;
				values->enabled = wanted;
				on += wanted;
			}
		}

		if (changed)
			patch_engine::save_config(map);
		libretro_patch_log.notice("Frame-rate patches %s: %u game versions (patch_config.yml %s)",
			choice == frame_rate::off ? "off" : choice == frame_rate::sixty ? "60 fps, or unlocked held at 30" : "60 fps, or unlocked", on, changed ? "written" : "unchanged");
		return on;
	}
}

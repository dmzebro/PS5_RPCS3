#include "stdafx.h"
#include "libretro_options.h"

#include "Emu/system_config.h"
#include "Utilities/Config.h"

#include <cmath>
#include <cstdlib>
#include <deque>
#include <map>
#include <set>

namespace libretro_options
{
	namespace
	{
		struct option
		{
			std::vector<std::string> path; // RPCS3's names, section first
			std::string key;
			bool toggle = false;           // a cfg::_bool: "enabled"/"disabled" in RetroArch
			bool dynamic = false;          // RPCS3 takes it while a game runs
			std::vector<std::string> values; // as RPCS3 reads them
			std::string default_value;
		};

		struct section
		{
			const char* name;  // RetroArch's category
			const char* info;
		};

		// RPCS3's sections, by the names its configuration gives them.
		const std::map<std::string, section, std::less<>> sections =
		{
			{"Core", {"CPU", "The PPU and SPU recompilers, their threads and accuracy (RPCS3's Core section)."}},
			{"VFS", {"Virtual File System", "The PS3's drives and disk cache (RPCS3's VFS section)."}},
			{"Video", {"GPU", "The RSX renderer, resolution, shaders and the performance overlay (RPCS3's Video section)."}},
			{"Audio", {"Audio", "The PS3's audio output, its format, buffering and volume (RPCS3's Audio section)."}},
			{"Input/Output", {"Input", "How the RetroPads reach the game (RPCS3's Input/Output section)."}},
			{"System", {"System", "The console's region, language, button assignment and clock (RPCS3's System section)."}},
			{"Net", {"Network", "The PS3's network and PSN state (RPCS3's Net section)."}},
			{"Savestate", {"Save States", "How RPCS3 makes and resumes the savestates RetroArch's save states carry (RPCS3's Savestate section)."}},
			{"Miscellaneous", {"Miscellaneous", "RPCS3's hints and overlays (its Miscellaneous section)."}},
		};

		// What the core fixes itself, by path: its renderer, audio backend and
		// input devices, the Qt application's window behaviour, settings for
		// other hosts, RetroArch's own jobs (VSync), and RPCS3's savestate folder,
		// which the core does not use.
		const std::set<std::string, std::less<>> excluded =
		{
			"Video/Renderer", "Video/VSync Mode", "Video/Disable MSL Fast Math",
			"Video/Vulkan/Exclusive Fullscreen Mode",
			"Audio/Renderer", "Audio/Audio Provider", "Audio/Microphone Type", "Audio/Music Handler",
			"Input/Output/Keyboard", "Input/Output/Mouse", "Input/Output/Camera", "Input/Output/Camera type",
			"Input/Output/Camera flip", "Input/Output/Move", "Input/Output/Buzz emulated controller",
			"Input/Output/Turntable emulated controller", "Input/Output/GHLtar emulated controller",
			"Input/Output/Show move cursor", "Input/Output/Paint move spheres",
			"Input/Output/Allow move hue set by game", "Input/Output/Load SDL GameController Mappings",
			"Input/Output/Mouse-based gyro enabled", "Input/Output/Mouse Debug overlay",
			"Input/Output/Fake Move Rotation Cone", "Input/Output/Fake Move Rotation Cone (Vertical)",
			"Savestate/Maximum SaveState Files", "Savestate/Maximum SaveState Files Space (MiB)",
			"Miscellaneous/Automatically start games after boot", "Miscellaneous/Exit RPCS3 when process finishes",
			"Miscellaneous/Pause emulation on RPCS3 focus loss", "Miscellaneous/Start games in fullscreen mode",
			"Miscellaneous/Start Big Picture Mode on boot", "Miscellaneous/Prevent display sleep while running games",
			"Miscellaneous/Use native user interface", "Miscellaneous/Enable GameMode",
		};

		const std::string resolution_scale_path = "Video/Resolution Scale";

		// The core's own option beside RPCS3's settings: RPCS3's game patches
		// (libretro_patches.h), which its Qt application sets in a dialog.
		const std::string frame_rate_patches_key = "rpcs3_patches_frame_rate";
		const std::string game_settings_key = "rpcs3_patches_game_settings";

		std::vector<option> g_options;
		std::deque<std::string> g_text; // the definitions' strings, which RetroArch keeps pointers to
		std::vector<retro_core_option_v2_category> g_categories;
		std::vector<retro_core_option_v2_definition> g_definitions;
		retro_core_options_v2 g_set{};

		const char* keep(std::string text)
		{
			g_text.push_back(std::move(text));
			return g_text.back().c_str();
		}

		std::string slug(std::string_view text)
		{
			std::string out;
			for (const char c : text)
			{
				if (std::isalnum(static_cast<unsigned char>(c)))
					out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				else if (!out.empty() && out.back() != '_')
					out += '_';
			}
			while (!out.empty() && out.back() == '_')
				out.pop_back();
			return out;
		}

		std::string joined(const std::vector<std::string>& path)
		{
			std::string out;
			for (const std::string& part : path)
				out += (out.empty() ? "" : "/") + part;
			return out;
		}

		// Integers exactly, whatever their range (RPCS3 has settings over all of u64).
		using wide = __int128;

		std::string text(wide value)
		{
			if (value == 0)
				return "0";
			const bool negative = value < 0;
			unsigned __int128 magnitude = negative ? -static_cast<unsigned __int128>(value) : static_cast<unsigned __int128>(value);
			std::string out;
			for (; magnitude; magnitude /= 10)
				out.insert(out.begin(), static_cast<char>('0' + static_cast<int>(magnitude % 10)));
			return negative ? "-" + out : out;
		}

		wide parse(std::string_view value)
		{
			const bool negative = value.starts_with('-');
			wide out = 0;
			for (const char c : value.substr(negative))
			{
				if (c < '0' || c > '9')
					break;
				out = out * 10 + (c - '0');
			}
			return negative ? -out : out;
		}

		// The values an integer setting offers: all of a small range; otherwise
		// the 1-2-5 series, evenly spaced steps, the bounds and the default, at
		// most 100 of them.
		std::vector<std::string> integer_values(wide min, wide max, wide def)
		{
			std::set<wide> picked{min, max, def};
			// Half and two, three and four times the default: the rates a
			// setting like Vblank Rate (60) is set to, 120, 180 and 240.
			for (const wide v : {def * 2, def * 3, def * 4, def % 2 ? def : def / 2})
				if (v >= min && v <= max)
					picked.insert(v);
			if (max - min <= 100)
			{
				for (wide v = min; v <= max; v++)
					picked.insert(v);
			}
			else
			{
				if (min <= 0 && max >= 0)
					picked.insert(0);
				const wide reach = std::max(min < 0 ? -min : min, max < 0 ? -max : max);
				for (wide magnitude = 1; magnitude <= reach; magnitude *= 10)
				{
					for (const wide digit : {1, 2, 5})
					{
						for (const wide v : {digit * magnitude, -digit * magnitude})
							if (v >= min && v <= max)
								picked.insert(v);
					}
				}
				const wide raw_step = std::max<wide>((max - min) / 32, 1);
				wide magnitude = 1;
				while (magnitude * 10 <= raw_step)
					magnitude *= 10;
				const wide step = raw_step < 2 * magnitude ? 2 * magnitude : raw_step < 5 * magnitude ? 5 * magnitude : 10 * magnitude;
				wide first = min / step * step;
				if (first < min)
					first += step;
				for (wide v = first; v <= max; v += step)
					picked.insert(v);
			}
			std::vector<wide> ordered(picked.begin(), picked.end());
			while (ordered.size() > 100)
			{
				std::vector<wide> thinner;
				for (usz i = 0; i < ordered.size(); i++)
					if (i % 2 == 0 || i + 1 == ordered.size() || ordered[i] == def)
						thinner.push_back(ordered[i]);
				ordered = std::move(thinner);
			}
			std::vector<std::string> out;
			for (const wide v : ordered)
				out.push_back(text(v));
			return out;
		}

		// A float setting's number as RPCS3 reads it back: whole numbers without a fraction.
		std::string number(f64 value)
		{
			if (value == std::floor(value) && std::abs(value) < 1e15)
				return std::to_string(static_cast<s64>(value));
			return fmt::format("%g", value);
		}

		// The same for a float setting (its range is small).
		std::vector<std::string> float_values(f64 min, f64 max, f64 def)
		{
			std::set<f64> picked{min, max, def};
			if (max - min <= 100)
			{
				for (f64 v = std::ceil(min); v <= max; v += 1)
					picked.insert(v);
			}
			else
			{
				if (min <= 0 && max >= 0)
					picked.insert(0);
				for (f64 magnitude = 1; magnitude <= std::max(std::abs(min), std::abs(max)); magnitude *= 10)
				{
					for (const f64 digit : {1.0, 2.0, 5.0})
					{
						for (const f64 v : {digit * magnitude, -digit * magnitude})
							if (v >= min && v <= max)
								picked.insert(v);
					}
				}
				const f64 raw_step = (max - min) / 32;
				const f64 magnitude = std::pow(10.0, std::floor(std::log10(raw_step)));
				const f64 step = raw_step / magnitude < 2 ? 2 * magnitude : raw_step / magnitude < 5 ? 5 * magnitude : 10 * magnitude;
				for (f64 v = std::ceil(min / step) * step; v <= max; v += step)
					picked.insert(v);
			}
			std::vector<f64> ordered(picked.begin(), picked.end());
			while (ordered.size() > 100)
			{
				// Thin out every other value, keeping the bounds and the default.
				std::vector<f64> thinner;
				for (usz i = 0; i < ordered.size(); i++)
					if (i % 2 == 0 || i + 1 == ordered.size() || ordered[i] == def)
						thinner.push_back(ordered[i]);
				ordered = std::move(thinner);
			}
			std::vector<std::string> out;
			for (const f64 v : ordered)
				out.push_back(number(v));
			return out;
		}

		void collect(const cfg::node& node, std::vector<std::string> path)
		{
			for (const cfg::_base* entry : node.get_nodes())
			{
				std::vector<std::string> here = path;
				here.push_back(entry->get_name());
				const std::string full = joined(here);
				if (entry->get_type() == cfg::type::node)
				{
					collect(*static_cast<const cfg::node*>(entry), here);
					continue;
				}
				if (excluded.contains(full) || here.size() < 2)
					continue;

				option made;
				made.path = here;
				made.key = "rpcs3_" + slug(full);
				made.dynamic = entry->get_is_dynamic();
				switch (entry->get_type())
				{
				case cfg::type::_bool:
					made.toggle = true;
					made.values = {"true", "false"};
					made.default_value = entry->def_to_string();
					break;
				case cfg::type::_enum:
					made.values = entry->to_list();
					made.default_value = entry->def_to_string();
					break;
				case cfg::type::_int:
				case cfg::type::uint:
				{
					const std::vector<std::string> range = entry->to_list();
					if (range.size() != 2)
						continue;
					if (full == resolution_scale_path)
					{
						// The frame the core hands RetroArch is 720p at the scale, 2160p at most.
						for (u32 v = 100; v <= 300; v += 25)
							made.values.push_back(std::to_string(v));
						made.default_value = "300";
						break;
					}
					// cfg::_float reports its range with a fraction (std::to_string of a double)
					if (range[0].find('.') != umax || range[1].find('.') != umax)
					{
						const f64 def = std::strtod(entry->def_to_string().c_str(), nullptr);
						made.values = float_values(std::strtod(range[0].c_str(), nullptr), std::strtod(range[1].c_str(), nullptr), def);
						made.default_value = number(def);
					}
					else
					{
						const wide def = parse(entry->def_to_string());
						made.values = integer_values(parse(range[0]), parse(range[1]), def);
						made.default_value = text(def);
					}
					break;
				}
				default:
					// Paths, names, lists and logs have no set of values to offer.
					continue;
				}
				if (made.values.empty() || made.values.size() > RETRO_NUM_CORE_OPTION_VALUES_MAX - 1)
					continue;
				g_options.push_back(std::move(made));
			}
		}

		void build()
		{
			if (!g_options.empty())
				return;
			collect(g_cfg, {});

			std::set<std::string> used_sections;
			for (const option& made : g_options)
				used_sections.insert(made.path.front());
			for (const std::string& name : used_sections)
			{
				const auto known = sections.find(name);
				g_categories.push_back({keep(slug(name)), keep(known != sections.end() ? known->second.name : name),
					keep(known != sections.end() ? known->second.info : "RPCS3's " + name + " section.")});
			}
			g_categories.push_back({"patches", "Game Settings and Patches", "RPCS3's per-game recommended settings and its game patch database: what a game boots with."});
			g_categories.push_back({});

			for (const option& made : g_options)
			{
				retro_core_option_v2_definition definition{};
				definition.key = keep(made.key);
				const auto known = sections.find(made.path.front());
				const std::string category_name = known != sections.end() ? known->second.name : made.path.front();
				std::string name;
				for (usz i = 1; i < made.path.size(); i++)
					name += (i > 1 ? ": " : "") + made.path[i];
				definition.desc = keep(category_name + " > " + name);
				definition.desc_categorized = keep(name);
				const std::string when = made.dynamic ? "It takes effect at once." : "It takes effect when content loads.";
				definition.info = keep(fmt::format("RPCS3's \"%s\" setting (%s). %s", made.path.back(), joined(made.path), when));
				definition.category_key = keep(slug(made.path.front()));
				for (usz i = 0; i < made.values.size(); i++)
				{
					const std::string& value = made.values[i];
					if (made.toggle)
						definition.values[i] = {keep(value == "true" ? "enabled" : "disabled"), nullptr};
					else
						definition.values[i] = {keep(value), nullptr};
				}
				const std::string& def = made.default_value;
				definition.default_value = keep(made.toggle ? (def == "true" ? "enabled" : "disabled") : def);
				g_definitions.push_back(definition);
			}
			retro_core_option_v2_definition patches{};
			patches.key = frame_rate_patches_key.c_str();
			patches.desc = "Game Settings and Patches > Frame-rate patches";
			patches.desc_categorized = "Frame-rate patches";
			patches.info = "\"60 FPS or steady 30\": a game RPCS3's patch database has a 60 fps patch for boots with it; a "
				"game with only an \"Unlock FPS\" patch boots with that and RPCS3's frame limit at 30, paced by the emulator "
				"rather than by the game's wait for the display (where a frame a little late waits a whole refresh more). "
				"\"60 FPS or unlocked\": the same without the limit, as fast as the emulation goes up to the display's rate, "
				"which varies where a game is too heavy for 60. It takes effect when content loads.";
			patches.category_key = "patches";
			patches.values[0] = {"60 FPS or steady 30", nullptr};
			patches.values[1] = {"60 FPS or unlocked", nullptr};
			patches.values[2] = {"disabled", nullptr};
			patches.default_value = "60 FPS or steady 30";
			g_definitions.push_back(patches);
			retro_core_option_v2_definition settings{};
			settings.key = game_settings_key.c_str();
			settings.desc = "Game Settings and Patches > Per-game settings";
			settings.desc_categorized = "Per-game settings";
			settings.info = "Every game RPCS3's configuration database has an entry for boots with the settings RPCS3 recommends "
				"for it, over the core options. It takes effect when content loads.";
			settings.category_key = "patches";
			settings.values[0] = {"enabled", nullptr};
			settings.values[1] = {"disabled", nullptr};
			settings.default_value = "enabled";
			g_definitions.push_back(settings);
			g_definitions.push_back({});
			g_set = {g_categories.data(), g_definitions.data()};
		}

		// The value RetroArch holds, as RPCS3 reads it; empty when it has none.
		std::string current(retro_environment_t environ_cb, const option& made)
		{
			retro_variable variable{made.key.c_str(), nullptr};
			if (!environ_cb || !environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) || !variable.value)
				return {};
			const std::string value = variable.value;
			if (made.toggle)
				return value == "enabled" ? "true" : value == "disabled" ? "false" : std::string();
			return value;
		}

		cfg::_base* find(cfg::node& root, const std::vector<std::string>& path)
		{
			cfg::_base* at = &root;
			for (const std::string& name : path)
			{
				if (!at || at->get_type() != cfg::type::node)
					return nullptr;
				cfg::_base* next = nullptr;
				for (cfg::_base* child : static_cast<cfg::node*>(at)->get_nodes())
					if (child->get_name() == name)
						next = child;
				at = next;
			}
			return at;
		}
	}

	void declare(retro_environment_t environ_cb)
	{
		build();
		if (retro_log_callback logging{}; environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging) && logging.log)
		{
			std::map<std::string, u32> counts;
			for (const option& made : g_options)
				counts[made.path.front()]++;
			std::string summary;
			for (const auto& [name, count] : counts)
				summary += fmt::format(" %s=%u", name, count);
			logging.log(RETRO_LOG_INFO, "[rpcs3] %zu core options:%s\n", g_options.size(), summary.c_str());
		}
		unsigned version = 0;
		if (environ_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 2)
			environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &g_set);
	}

	bool apply(retro_environment_t environ_cb, cfg_root& settings)
	{
		build();
		bool changed = false;
		for (const option& made : g_options)
		{
			const std::string value = current(environ_cb, made);
			cfg::_base* const entry = find(settings, made.path);
			if (value.empty() || !entry || entry->to_string() == value)
				continue;
			changed |= entry->from_string(value);
		}
		// What the core fixes: RPCS3's own frontend's jobs, which a core cannot do.
		const auto fix = [&](cfg::_base& entry, std::string_view value)
		{
			if (entry.to_string() != value)
				changed |= entry.from_string(value);
		};
		// Testing: /app0/null-renderer.txt runs RPCS3's Null renderer, which
		// draws nothing, for the frame rate the game's own threads allow.
		fix(settings.video.renderer, fs::is_file("/app0/null-renderer.txt") ? "Null" : "Vulkan");
		fix(settings.misc.autostart, "true");
		fix(settings.misc.autoexit, "false");
		fix(settings.misc.autopause, "false");
		fix(settings.misc.use_native_interface, "true");
		// No debugger connects to a core: RPCS3's GDB server would only open a socket on the console.
		fix(settings.misc.gdb_server, "");
		return changed;
	}

	void apply_live(retro_environment_t environ_cb)
	{
		build();
		for (const option& made : g_options)
		{
			if (!made.dynamic)
				continue;
			const std::string value = current(environ_cb, made);
			cfg::_base* const entry = find(g_cfg, made.path);
			if (!value.empty() && entry && entry->to_string() != value)
				entry->from_string(value, true);
		}
	}

	bool fixed_by_core(std::string_view path)
	{
		return excluded.contains(path);
	}

	bool game_settings(retro_environment_t environ_cb)
	{
		retro_variable variable{game_settings_key.c_str(), nullptr};
		return !environ_cb || !environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) || !variable.value ||
			std::string_view(variable.value) != "disabled";
	}

	libretro_patches::frame_rate frame_rate_patches(retro_environment_t environ_cb)
	{
		retro_variable variable{frame_rate_patches_key.c_str(), nullptr};
		if (!environ_cb || !environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) || !variable.value)
			return libretro_patches::frame_rate::sixty;
		const std::string_view value = variable.value;
		if (value == "disabled")
			return libretro_patches::frame_rate::off;
		if (value == "60 FPS or unlocked")
			return libretro_patches::frame_rate::unlocked;
		return libretro_patches::frame_rate::sixty;
	}

	u32 resolution_scale(retro_environment_t environ_cb)
	{
		build();
		for (const option& made : g_options)
		{
			if (joined(made.path) != resolution_scale_path)
				continue;
			const u32 value = static_cast<u32>(std::strtoul(current(environ_cb, made).c_str(), nullptr, 10));
			return value >= 100 && value <= 300 ? value : 300;
		}
		return 300;
	}
}

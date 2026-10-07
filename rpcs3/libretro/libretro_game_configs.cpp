#include "stdafx.h"
#include "libretro_game_configs.h"
#include "libretro_options.h"

#include "Utilities/File.h"
#include "util/yaml.hpp"

#include <unordered_map>
#include <vector>

LOG_CHANNEL(libretro_game_config_log, "libretro");

namespace libretro_game_configs
{
	namespace
	{
		std::unordered_map<std::string, std::string> load()
		{
			std::unordered_map<std::string, std::string> games;
			const std::string path = fs::get_config_dir() + "game_configs/config_database.json";
			const fs::file file(path);
			if (!file)
			{
				libretro_game_config_log.error("No per-game configuration database at %s", path);
				return games;
			}

			// JSON is YAML too, and RPCS3 already carries a YAML reader.
			auto [root, error] = yaml_load(file.to_string());
			if (!error.empty() || !root["games"].IsMap())
			{
				libretro_game_config_log.error("The per-game configuration database %s is unreadable: %s", path, error);
				return games;
			}

			for (const auto& game : root["games"])
			{
				if (game.second.IsMap() && game.second["config"] && game.second["config"].IsScalar())
					games.emplace(game.first.Scalar(), game.second["config"].Scalar());
			}

			libretro_game_config_log.notice("Per-game configuration database: %u games", games.size());
			return games;
		}

		const std::unordered_map<std::string, std::string>& database()
		{
			static const auto games = load();
			return games;
		}

		// Leaves out what the core fixes itself, walking the configuration by path.
		void drop_fixed(YAML::Node node, const std::string& path)
		{
			if (!node.IsMap())
				return;
			std::vector<std::string> fixed;
			for (const auto& entry : node)
			{
				const std::string key = entry.first.Scalar();
				const std::string here = path.empty() ? key : path + "/" + key;
				if (libretro_options::fixed_by_core(here))
					fixed.push_back(key);
				else
					drop_fixed(entry.second, here);
			}
			for (const std::string& key : fixed)
				node.remove(key);
		}
	}

	std::string for_game(const std::string& serial, u32 resolution_scale, bool frame_rate_patch)
	{
		const auto found = database().find(serial);
		if (found == database().end())
			return {};

		auto [config, error] = yaml_load(found->second);
		if (!error.empty() || !config.IsMap())
		{
			libretro_game_config_log.error("The per-game configuration of %s is unreadable: %s", serial, error);
			return {};
		}

		drop_fixed(config, "");

		if (YAML::Node video = config["Video"]; video && video.IsMap())
		{
			// The scale is a multiple of the game's own frame: one drawn at 1080p
			// reaches the core's frame at two thirds of it.
			if (video["Resolution"] && video["Resolution"].Scalar() == "1920x1080")
				video["Resolution Scale"] = std::to_string(resolution_scale * 720 / 1080);

			if (frame_rate_patch && video["Frame limit"] && video["Frame limit"].Scalar() == "30")
				video.remove("Frame limit");
		}

		YAML::Emitter out;
		out << config;
		libretro_game_config_log.notice("Per-game configuration for %s:\n%s", serial, out.c_str());
		return out.c_str();
	}

	std::string with_frame_limit_30(const std::string& config)
	{
		YAML::Node node;

		if (!config.empty())
		{
			auto [parsed, error] = yaml_load(config);
			if (error.empty() && parsed.IsMap())
				node = parsed;
		}

		node["Video"]["Frame limit"] = "30";
		YAML::Emitter out;
		out << node;
		return out.c_str();
	}
}

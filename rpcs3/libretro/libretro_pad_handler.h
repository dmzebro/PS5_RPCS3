#pragma once

// The PS3's controllers in the libretro core: RetroArch's RetroPads, read on
// RetroArch's thread in retro_run (libretro.cpp) and handed to RPCS3's pad
// thread through its generic handler, with SDL's value conventions.

#include "Emu/Io/PadHandler.h"

#include <array>
#include <mutex>

class libretro_pad_handler final : public PadHandlerBase
{
public:
	static constexpr u32 max_pads = 4;

	// One RetroPad's state, as RetroArch reports it.
	struct state
	{
		bool connected = false;
		u16 buttons = 0;           // RETRO_DEVICE_ID_JOYPAD_* bits
		s16 axes[4]{};             // left X, left Y, right X, right Y
		u16 triggers[2]{};         // L2, R2, 0 to 0x7fff
	};

	// On RetroArch's thread, once per retro_run.
	static void set_state(u32 port, const state& input);

	static std::string device_name(u32 port);

	libretro_pad_handler();

	void init_config(cfg_pad* cfg) override;
	std::vector<pad_list_entry> list_devices() override;

private:
	struct device final : PadDevice
	{
		u32 port = 0;
	};

	std::array<std::shared_ptr<device>, max_pads> m_devices;

	std::shared_ptr<PadDevice> get_device(const std::string& name) override;
	connection update_connection(const std::shared_ptr<PadDevice>& device) override;
	std::unordered_map<u32, u16> get_button_values(const std::shared_ptr<PadDevice>& device) override;
	bool get_is_left_trigger(const std::shared_ptr<PadDevice>& device, u32 key_code) override;
	bool get_is_right_trigger(const std::shared_ptr<PadDevice>& device, u32 key_code) override;
	bool get_is_left_stick(const std::shared_ptr<PadDevice>& device, u32 key_code) override;
	bool get_is_right_stick(const std::shared_ptr<PadDevice>& device, u32 key_code) override;
};

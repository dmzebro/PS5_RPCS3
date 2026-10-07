#include "stdafx.h"
#include "libretro_pad_handler.h"
#include "libretro.h"

#include "Emu/Io/pad_config.h"

// The input configurations the Qt application keeps (rpcs3qt/pad_settings_dialog.cpp),
// and its command-line override (main.cpp), which the core never sets.
cfg_input_configurations g_cfg_input_configs;
std::string g_input_config_override;

namespace
{
	// RetroPad inputs as key codes: the digital buttons by their
	// RETRO_DEVICE_ID_JOYPAD_* numbers, then the stick directions and triggers.
	enum key : u32
	{
		b = RETRO_DEVICE_ID_JOYPAD_B,
		y = RETRO_DEVICE_ID_JOYPAD_Y,
		select = RETRO_DEVICE_ID_JOYPAD_SELECT,
		start = RETRO_DEVICE_ID_JOYPAD_START,
		up = RETRO_DEVICE_ID_JOYPAD_UP,
		down = RETRO_DEVICE_ID_JOYPAD_DOWN,
		left = RETRO_DEVICE_ID_JOYPAD_LEFT,
		right = RETRO_DEVICE_ID_JOYPAD_RIGHT,
		a = RETRO_DEVICE_ID_JOYPAD_A,
		x = RETRO_DEVICE_ID_JOYPAD_X,
		l = RETRO_DEVICE_ID_JOYPAD_L,
		r = RETRO_DEVICE_ID_JOYPAD_R,
		l2 = RETRO_DEVICE_ID_JOYPAD_L2,
		r2 = RETRO_DEVICE_ID_JOYPAD_R2,
		l3 = RETRO_DEVICE_ID_JOYPAD_L3,
		r3 = RETRO_DEVICE_ID_JOYPAD_R3,
		ls_x_neg = 16, ls_x_pos, ls_y_neg, ls_y_pos,
		rs_x_neg, rs_x_pos, rs_y_neg, rs_y_pos,
		none,
	};

	std::mutex g_state_lock;
	std::array<libretro_pad_handler::state, libretro_pad_handler::max_pads> g_state{};

	constexpr u16 axis_max = 0x7fff;
}

void libretro_pad_handler::set_state(u32 port, const state& input)
{
	if (port >= max_pads)
		return;
	std::lock_guard lock(g_state_lock);
	g_state[port] = input;
}

std::string libretro_pad_handler::device_name(u32 port)
{
	return fmt::format("RetroPad %u", port + 1);
}

libretro_pad_handler::libretro_pad_handler() : PadHandlerBase(pad_handler::libretro)
{
	button_list =
	{
		{ key::none, "" },
		{ key::b, "B" }, { key::y, "Y" }, { key::select, "Select" }, { key::start, "Start" },
		{ key::up, "Up" }, { key::down, "Down" }, { key::left, "Left" }, { key::right, "Right" },
		{ key::a, "A" }, { key::x, "X" }, { key::l, "L" }, { key::r, "R" },
		{ key::l2, "L2" }, { key::r2, "R2" }, { key::l3, "L3" }, { key::r3, "R3" },
		// Y as SDL names it: "Y-" is down on the stick, "Y+" is up.
		{ key::ls_x_neg, "LS X-" }, { key::ls_x_pos, "LS X+" }, { key::ls_y_neg, "LS Y-" }, { key::ls_y_pos, "LS Y+" },
		{ key::rs_x_neg, "RS X-" }, { key::rs_x_pos, "RS X+" }, { key::rs_y_neg, "RS Y-" }, { key::rs_y_pos, "RS Y+" },
	};

	init_configs();

	b_has_config = true;
	b_has_deadzones = true;
	b_has_pressure_intensity_button = false;
	b_has_analog_limiter_button = false;
	m_name_string = "RetroPad ";
	m_max_devices = max_pads;
	m_trigger_threshold = 0;
	m_thumb_threshold = 0;
	thumb_max = axis_max;
	trigger_max = axis_max;

	for (u32 port = 0; port < max_pads; port++)
	{
		m_devices[port] = std::make_shared<device>();
		m_devices[port]->port = port;
	}
}

void libretro_pad_handler::init_config(cfg_pad* cfg)
{
	if (!cfg) return;

	// A RetroPad is laid out as a PlayStation pad: B is the bottom face button.
	cfg->ls_left.def  = ::at32(button_list, key::ls_x_neg);
	cfg->ls_down.def  = ::at32(button_list, key::ls_y_neg);
	cfg->ls_right.def = ::at32(button_list, key::ls_x_pos);
	cfg->ls_up.def    = ::at32(button_list, key::ls_y_pos);
	cfg->rs_left.def  = ::at32(button_list, key::rs_x_neg);
	cfg->rs_down.def  = ::at32(button_list, key::rs_y_neg);
	cfg->rs_right.def = ::at32(button_list, key::rs_x_pos);
	cfg->rs_up.def    = ::at32(button_list, key::rs_y_pos);
	cfg->start.def    = ::at32(button_list, key::start);
	cfg->select.def   = ::at32(button_list, key::select);
	cfg->ps.def       = cfg_pad::make_button_string(button_list, {{key::start, key::select}});
	cfg->square.def   = ::at32(button_list, key::y);
	cfg->cross.def    = ::at32(button_list, key::b);
	cfg->circle.def   = ::at32(button_list, key::a);
	cfg->triangle.def = ::at32(button_list, key::x);
	cfg->left.def     = ::at32(button_list, key::left);
	cfg->down.def     = ::at32(button_list, key::down);
	cfg->right.def    = ::at32(button_list, key::right);
	cfg->up.def       = ::at32(button_list, key::up);
	cfg->r1.def       = ::at32(button_list, key::r);
	cfg->r2.def       = ::at32(button_list, key::r2);
	cfg->r3.def       = ::at32(button_list, key::r3);
	cfg->l1.def       = ::at32(button_list, key::l);
	cfg->l2.def       = ::at32(button_list, key::l2);
	cfg->l3.def       = ::at32(button_list, key::l3);

	cfg->pressure_intensity_button.def = ::at32(button_list, key::none);
	cfg->analog_limiter_button.def = ::at32(button_list, key::none);
	cfg->orientation_reset_button.def = ::at32(button_list, key::none);

	// RetroArch applies its own deadzone; RPCS3's are SDL's defaults.
	cfg->lstick_anti_deadzone.def = static_cast<u32>(0.13 * 255);
	cfg->rstick_anti_deadzone.def = static_cast<u32>(0.13 * 255);
	cfg->lstickdeadzone.def = 8000;
	cfg->rstickdeadzone.def = 8000;
	cfg->ltriggerthreshold.def = 0;
	cfg->rtriggerthreshold.def = 0;

	cfg->from_default();
}

std::vector<pad_list_entry> libretro_pad_handler::list_devices()
{
	std::vector<pad_list_entry> devices;
	for (u32 port = 0; port < max_pads; port++)
		devices.emplace_back(device_name(port), false);
	return devices;
}

std::shared_ptr<PadDevice> libretro_pad_handler::get_device(const std::string& name)
{
	for (u32 port = 0; port < max_pads; port++)
		if (name == device_name(port))
			return m_devices[port];
	return nullptr;
}

PadHandlerBase::connection libretro_pad_handler::update_connection(const std::shared_ptr<PadDevice>& pad_device)
{
	const auto* dev = static_cast<const device*>(pad_device.get());
	if (!dev)
		return connection::disconnected;
	std::lock_guard lock(g_state_lock);
	return g_state[dev->port].connected ? connection::connected : connection::disconnected;
}

std::unordered_map<u32, u16> libretro_pad_handler::get_button_values(const std::shared_ptr<PadDevice>& pad_device)
{
	std::unordered_map<u32, u16> values;
	const auto* dev = static_cast<const device*>(pad_device.get());
	if (!dev)
		return values;

	state input;
	{
		std::lock_guard lock(g_state_lock);
		input = g_state[dev->port];
	}

	for (u32 id = key::b; id <= key::r3; id++)
		values[id] = (input.buttons >> id) & 1 ? axis_max : 0;

	// Analog triggers where the RetroPad has them.
	values[key::l2] = std::max(values[key::l2], input.triggers[0]);
	values[key::r2] = std::max(values[key::r2], input.triggers[1]);

	const auto axis = [&](s16 value, key negative, key positive)
	{
		values[negative] = value < 0 ? static_cast<u16>(std::abs(value) - 1) : 0;
		values[positive] = value > 0 ? static_cast<u16>(value) : 0;
	};
	axis(input.axes[0], key::ls_x_neg, key::ls_x_pos);
	axis(input.axes[2], key::rs_x_neg, key::rs_x_pos);
	// Down is positive on a RetroPad: SDL's "Y-".
	axis(static_cast<s16>(-std::max<s32>(input.axes[1], -axis_max)), key::ls_y_neg, key::ls_y_pos);
	axis(static_cast<s16>(-std::max<s32>(input.axes[3], -axis_max)), key::rs_y_neg, key::rs_y_pos);
	return values;
}

bool libretro_pad_handler::get_is_left_trigger(const std::shared_ptr<PadDevice>&, u32 key_code)
{
	return key_code == key::l2;
}

bool libretro_pad_handler::get_is_right_trigger(const std::shared_ptr<PadDevice>&, u32 key_code)
{
	return key_code == key::r2;
}

bool libretro_pad_handler::get_is_left_stick(const std::shared_ptr<PadDevice>&, u32 key_code)
{
	return key_code >= key::ls_x_neg && key_code <= key::ls_y_pos;
}

bool libretro_pad_handler::get_is_right_stick(const std::shared_ptr<PadDevice>&, u32 key_code)
{
	return key_code >= key::rs_x_neg && key_code <= key::rs_y_pos;
}

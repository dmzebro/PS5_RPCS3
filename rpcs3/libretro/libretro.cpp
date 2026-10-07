// RPCS3 as a libretro core: the frontend that drives Emu in place of the Qt
// application (BUILD_LIBRETRO). The renderer is RPCS3's Vulkan backend on
// RetroArch's device (Emu/RSX/VK/vkutils/swapchain_libretro.h); audio and the
// RetroPads go through libretro_audio_backend and libretro_pad_handler
// (PS5_RetroArch docs/RPCS3_PORT.md).

#include "stdafx.h"
#include "libretro.h"
#include "Emu/RSX/VK/VulkanAPI.h"
#include "libretro_vulkan.h"

#include "Emu/System.h"
#include "Emu/system_progress.hpp"
#include "Emu/emu_callbacks.h"
#include "Emu/system_config.h"
#include "Emu/system_utils.hpp"
#include "Emu/IdManager.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/Cell/SPUThread.h"
#include "Emu/Audio/Null/NullAudioBackend.h"
#include "Emu/Audio/Null/null_enumerator.h"
#include "Emu/Io/Null/NullKeyboardHandler.h"
#include "Emu/Io/Null/NullMouseHandler.h"
#include "Emu/Io/Null/null_camera_handler.h"
#include "Emu/Io/Null/null_music_handler.h"
#include "Emu/Io/KeyboardHandler.h"
#include "Emu/Io/pad_config.h"
#include "Input/pad_thread.h"
#include "libretro_pad_handler.h"
#include "libretro_audio_backend.h"
#include "libretro_loader.h"
#include "libretro_state.h"
#include "libretro_options.h"
#include "libretro_patches.h"
#include "libretro_game_configs.h"

// The RSX thread's waits for occlusion query results (VKGSRender.cpp).
extern atomic_t<u64> g_occlusion_result_wait_us, g_occlusion_result_waits;
extern atomic_t<u64> g_rsx_fault_stats[4];
extern atomic_t<bool> g_keep_code_on_kill;
extern atomic_t<u64> g_ppu_syscall_tsc[16][1024];
extern atomic_t<u64> g_spu_rdch_tsc[32];
extern atomic_t<u64> g_spu_mfc_tsc[256];
extern atomic_t<u64> g_spu_xf_counts[24];
extern atomic_t<u64> g_spu_xf_replay_pcs[64];
extern atomic_t<u64> g_spu_xf_replay_lanes[64][4];
extern atomic_t<u64> g_spu_stop_tsc;
extern atomic_t<u64> g_usleep_samples[256];
extern atomic_t<u64> g_usleep_stats[3][3];
extern atomic_t<u32> g_usleep_sample_count;
std::string ppu_get_syscall_name(u64 code);

namespace vk
{
	// Vulkan submissions and their TSC ticks (VKCommandStream.cpp)
	extern atomic_t<u64> g_submit_stats[2];
}
#include "Emu/Io/MouseHandler.h"
#include "Emu/RSX/Null/NullGSRender.h"
#include "Emu/RSX/GSFrameBase.h"
#include "Emu/RSX/VK/VKGSRender.h"
#include "Emu/RSX/VK/VKPipelineCompiler.h"
#include "Emu/RSX/VK/vkutils/device.h"
#include "Emu/RSX/VK/vkutils/swapchain_libretro.h"
#include "Emu/Cell/Modules/cellMsgDialog.h"
#include "Emu/Cell/PPUAnalyser.h"
#include "Emu/Cell/PPUModule.h"
#include "Emu/Cell/lv2/sys_sync.h"
#include "Emu/timeline.h"
#include "Emu/Cell/Modules/cellOskDialog.h"
#include "Emu/Cell/Modules/cellSaveData.h"
#include "Emu/Cell/Modules/sceNpTrophy.h"
#include "util/video_source.h"
#include "util/sysinfo.hpp"
#include "util/tsc.hpp"
#include "Emu/Memory/vm_locking.h"
#include "Utilities/File.h"
#include "Utilities/JIT.h"
#include "Crypto/key_vault.h"
#include "Crypto/unpkg.h"
#include "Crypto/unself.h"
#include "Emu/VFS.h"
#include "Emu/vfs_config.h"
#include "Loader/PUP.h"
#include "Loader/TAR.h"

#include <stb_image.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#if defined(__x86_64__)
#include <cpuid.h>
#include <x86intrin.h>
#endif
#include <ctime>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

LOG_CHANNEL(libretro_log, "LIBRETRO");

// The frontend's directories (Utilities/File.cpp), shared with Android's.
extern std::string g_android_executable_dir;
extern std::string g_android_config_dir;
extern std::string g_android_cache_dir;

#if defined(__SCE__)
#include <sys/event.h>
#include <sys/select.h>
extern "C" int sceKernelUsleep(unsigned int microseconds);
extern "C" int sceKernelNanosleep(const struct timespec* request, struct timespec* remaining);
#endif

// Testing only (/app0/sleep-probe.txt at load): how long a short sleep takes
// through each way the console offers (RPCS3's usleep of 30 us took about
// 118 us, every host sleep about 80 us more than asked), a line each on stderr.
static void sleep_probe_body();

#if defined(ARCH_X64)
__attribute__((__target__("mwaitx"))) static void sleep_probe_mwaitx(u32 cycles)
{
	alignas(64) u64 monitor_var{};
	_mm_monitorx(&monitor_var, 0, 0);
	_mm_mwaitx(0x2, 0xf0, cycles);
}
#endif

static void sleep_probe()
{
	if (!fs::is_file("/app0/sleep-probe.txt"))
		return;

	// On an RPCS3 thread, which thread_ctrl::wait_for needs (it joins here)
	named_thread("Sleep probe", []() { sleep_probe_body(); });
}

static void sleep_probe_body()
{
	const auto measure = [](const char* name, u32 request_us, auto&& sleep_once)
	{
		constexpr u32 rounds = 300;
		std::vector<double> took(rounds);
		for (u32 i = 0; i < rounds; i++)
		{
			const auto start = std::chrono::steady_clock::now();
			sleep_once(request_us);
			took[i] = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
		}
		std::sort(took.begin(), took.end());
		double sum = 0;
		for (const double v : took)
			sum += v;
		std::fprintf(stderr, "rpcs3 sleep probe: %-22s asked=%3u us mean=%7.1f p10=%7.1f p50=%7.1f p90=%7.1f max=%8.1f\n",
			name, request_us, sum / rounds, took[rounds / 10], took[rounds / 2], took[rounds * 9 / 10], took[rounds - 1]);
	};

	for (const u32 us : {30u, 100u})
	{
		measure("nanosleep", us, [](u32 v) { const timespec ts{0, static_cast<long>(v) * 1000}; nanosleep(&ts, nullptr); });
		measure("usleep", us, [](u32 v) { usleep(v); });
		measure("clock_nanosleep abs", us, [](u32 v)
		{
			timespec ts{};
			clock_gettime(CLOCK_MONOTONIC, &ts);
			ts.tv_nsec += static_cast<long>(v) * 1000;
			if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
		});
		measure("thread_ctrl::wait_for", us, [](u32 v) { thread_ctrl::wait_for(v); });
		measure("condition_variable", us, [](u32 v)
		{
			static std::mutex m;
			static std::condition_variable cv;
			std::unique_lock lock(m);
			cv.wait_for(lock, std::chrono::microseconds(v));
		});
		measure("select", us, [](u32 v) { timeval tv{0, static_cast<suseconds_t>(v)}; select(0, nullptr, nullptr, nullptr, &tv); });
#if defined(__SCE__)
		measure("sceKernelUsleep", us, [](u32 v) { sceKernelUsleep(v); });
		measure("sceKernelNanosleep", us, [](u32 v) { const timespec ts{0, static_cast<long>(v) * 1000}; sceKernelNanosleep(&ts, nullptr); });
		measure("kevent timer", us, [](u32 v)
		{
			static const int kq = kqueue();
			struct kevent change{}, event{};
			EV_SET(&change, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_USECONDS, v, nullptr);
			kevent(kq, &change, 1, &event, 1, nullptr);
		});
#endif
		measure("yield until due", us, [](u32 v)
		{
			const auto due = std::chrono::steady_clock::now() + std::chrono::microseconds(v);
			while (std::chrono::steady_clock::now() < due)
				std::this_thread::yield();
		});
#if defined(ARCH_X64)
		// MONITORX and MWAITX (CPUID 0x80000001 ECX bit 29): the hardware thread
		// waits with its core's resources left to the other thread
		unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
		if (__get_cpuid(0x80000001, &eax, &ebx, &ecx, &edx) && (ecx & (1u << 29)) && utils::get_tsc_freq())
		{
			measure("mwaitx until due", us, [](u32 v)
			{
				const u64 due = utils::get_tsc() + v * (utils::get_tsc_freq() / 1000000);
				for (u64 now = utils::get_tsc(); now < due; now = utils::get_tsc())
					sleep_probe_mwaitx(static_cast<u32>(due - now));
			});
		}
		else
		{
			std::fprintf(stderr, "rpcs3 sleep probe: no MONITORX (cpuid ecx=0x%x)\n", ecx);
		}
#endif
	}
}

[[noreturn]] void report_fatal_error(std::string_view text, bool is_html, bool include_help_text)
{
	std::fprintf(stderr, "rpcs3 libretro: fatal: %.*s\n", static_cast<int>(text.size()), text.data());
	std::abort();
}

void run_main_calls();

// The Qt application's event pump while Emu waits for a state change (a
// shutdown, above all): here, the calls queued for the frontend's thread.
void qt_events_aware_op(int repeat_duration_ms, std::function<bool()> wrapped_op)
{
	while (!wrapped_op())
	{
		run_main_calls();
		std::this_thread::sleep_for(std::chrono::milliseconds(std::max(repeat_duration_ms, 1)));
	}
}

namespace
{
	retro_environment_t environ_cb;
	retro_video_refresh_t video_cb;
	retro_audio_sample_batch_t audio_batch_cb;
	retro_input_poll_t input_poll_cb;
	retro_input_state_t input_state_cb;
	retro_log_printf_t log_cb;

	void log(retro_log_level level, const char* format, ...)
	{
		char line[1024];
		va_list args;
		va_start(args, format);
		std::vsnprintf(line, sizeof(line), format, args);
		va_end(args);
		if (log_cb)
			log_cb(level, "[rpcs3] %s\n", line);
		else
			std::fprintf(stderr, "[rpcs3] %s\n", line);
	}

	// Calls RPCS3 makes "on the main thread" (call_from_main_thread): the
	// frontend's thread, which runs them in retro_run.
	std::mutex main_calls_lock;
	std::deque<std::pair<std::function<void()>, atomic_t<u32>*>> main_calls;

	// ---- Vulkan -----------------------------------------------------------------
	// RetroArch creates the instance and the device (context negotiation v2),
	// the device with RPCS3's own choices (vkutils/device.cpp); the renderer
	// starts once RetroArch's context is up, and each finished frame is handed
	// to RetroArch in retro_run.
	retro_hw_render_callback hw_render{};
	retro_hw_render_interface_vulkan* vulkan = nullptr;

	// The frame size: the PS3's 720p output at the resolution scale, as the
	// renderer's presentation images (RPCS3 scales its output to them).
	u32 output_width = 1280;
	u32 output_height = 720;
	u32 shown_width = 0;
	u32 shown_height = 0;

	void set_output_size(u32 scale)
	{
		output_width = std::min<u32>(1280 * scale / 100, 3840);
		output_height = std::min<u32>(720 * scale / 100, 2160);
	}

	// ---- Core options -----------------------------------------------------------
	// RPCS3's settings, built from its configuration (libretro_options.h), are
	// written to its global configuration (config.yml) when content loads, as
	// RPCS3's settings dialog writes them, so a game's custom configuration still
	// overrides them; those RPCS3 takes while a game runs apply at once.
	// The options into config.yml, which the boot reads (Emulator::Load). The
	// file is read into a configuration of its own: g_cfg may still hold an
	// earlier game's custom configuration.
	u32 apply_options()
	{
		const std::string path = fs::get_config_dir(true) + "config.yml";
		auto settings = std::make_unique<cfg_root>();
		settings->from_default();
		if (const fs::file file{path})
			settings->from_string(file.to_string());
		if (libretro_options::apply(environ_cb, *settings))
		{
			Emulator::SaveSettings(settings->to_string(), {});
			log(RETRO_LOG_INFO, "core options written to config.yml");
		}
		const usz patched = libretro_patches::prepare(libretro_options::frame_rate_patches(environ_cb));
		log(RETRO_LOG_INFO, "frame-rate patches on for %zu game versions", patched);
		return libretro_options::resolution_scale(environ_cb);
	}

	const VkApplicationInfo* vulkan_application_info()
	{
		// RPCS3's instance version (vkutils/instance.cpp).
		static const VkApplicationInfo info
		{
			.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
			.pApplicationName = "RPCS3",
			.pEngineName = "RPCS3",
			.apiVersion = VK_API_VERSION_1_2,
		};
		return &info;
	}

	VkInstance vulkan_create_instance(PFN_vkGetInstanceProcAddr get_instance_proc_addr, const VkApplicationInfo* app,
		retro_vulkan_create_instance_wrapper_t create_instance_wrapper, void* opaque)
	{
		volkInitializeCustom(get_instance_proc_addr);
		const VkInstanceCreateInfo info
		{
			.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
			.pApplicationInfo = app ? app : vulkan_application_info(),
		};
		const VkInstance instance = create_instance_wrapper(opaque, &info);
		if (instance)
			volkLoadInstanceOnly(instance);
		vk::libretro::frontend().instance = instance;
		return instance;
	}

	bool vulkan_create_device(retro_vulkan_context* context, VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR surface,
		PFN_vkGetInstanceProcAddr get_instance_proc_addr, retro_vulkan_create_device_wrapper_t create_device_wrapper, void* opaque)
	{
		volkInitializeCustom(get_instance_proc_addr);
		volkLoadInstanceOnly(instance);
		auto& frontend = vk::libretro::frontend();
		frontend.instance = instance;
		frontend.device = VK_NULL_HANDLE;

		if (!gpu)
		{
			u32 count = 1;
			if (vkEnumeratePhysicalDevices(instance, &count, &gpu) < 0 || !count)
				return false;
		}

		// A queue family with graphics and compute, which presents to RetroArch's surface.
		vk::physical_device physical;
		physical.create(instance, gpu, true);
		u32 family = umax;
		for (u32 i = 0; i < physical.get_queue_count() && family == umax; i++)
		{
			const VkQueueFlags flags = physical.get_queue_properties(i).queueFlags;
			VkBool32 presents = VK_TRUE;
			if (surface)
				vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface, &presents);
			if ((flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT) && presents)
				family = i;
		}
		if (family == umax)
		{
			log(RETRO_LOG_ERROR, "Vulkan: no queue family draws and presents");
			return false;
		}

		frontend.gpu = gpu;
		frontend.queue_family = family;
		frontend.create_device = [create_device_wrapper, opaque](VkPhysicalDevice device_gpu, const VkDeviceCreateInfo& info)
		{
			return create_device_wrapper(device_gpu, opaque, &info);
		};

		// RPCS3's device, through RetroArch; the renderer adopts it when it
		// starts. Only RPCS3's allocator goes with this first render_device.
		{
			vk::render_device device;
			device.create(physical, family, family, vk::libretro::transfer_queue_family(physical, family));
			device.destroy();
		}
		frontend.create_device = nullptr;
		if (!frontend.device)
			return false;

		context->gpu = gpu;
		context->device = frontend.device;
		vkGetDeviceQueue(frontend.device, family, 0, &context->queue);
		context->queue_family_index = family;
		context->presentation_queue = context->queue;
		context->presentation_queue_family_index = family;
		log(RETRO_LOG_INFO, "Vulkan: %s, queue family %u", physical.get_name().c_str(), family);
		return true;
	}

	// RetroArch negotiates a core's device only when the interface has this
	// first-version entry, even when it then calls create_device2; without it,
	// RetroArch made the device itself, with one graphics queue and its own
	// extensions and features, and RPCS3's transfer queue was null (asynchronous
	// texture streaming then submitted to nothing). A frontend that only knows
	// the first version gets RetroArch's own device.
	bool vulkan_create_device_v1(retro_vulkan_context*, VkInstance, VkPhysicalDevice, VkSurfaceKHR, PFN_vkGetInstanceProcAddr,
		const char**, unsigned, const char**, unsigned, const VkPhysicalDeviceFeatures*)
	{
		return false;
	}

	const retro_hw_render_context_negotiation_interface_vulkan vulkan_negotiation
	{
		.interface_type = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN,
		.interface_version = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION,
		.get_application_info = vulkan_application_info,
		.create_device = vulkan_create_device_v1,
		// None: RetroArch destroys the device itself, and calls this when its
		// video driver goes, which can be after the core is unloaded.
		.destroy_device = nullptr,
		.create_instance = vulkan_create_instance,
		.create_device2 = vulkan_create_device,
	};

	std::string pending_boot;
	void stop_emulation();

	// ---- Preparing content ------------------------------------------------------
	// The firmware and a package install on a worker thread while retro_run shows
	// the loading screen (libretro_loader.h); the game boots when they are done,
	// and the screen stays until RPCS3's first frame.
	enum class stage
	{
		idle,      // a game's frames, or nothing loaded
		firmware,  // the worker installs the firmware
		package,   // the worker installs a package
		starting,  // the game boots, until its first frame
		failed,    // the screen says what went wrong
	};

	struct preparation
	{
		stage now = stage::idle;
		std::string content;    // what retro_load_game was given
		std::string failure;    // the failed screen's heading
		std::string last_boot;  // what booted last: the content's executable, or a save state
		bool booted = false;
		bool resuming = false;  // the boot is a save state's

		std::unique_ptr<named_thread<std::function<void()>>> worker;
		atomic_t<bool> finished = false;
		atomic_t<bool> cancel = false;
		bool succeeded = false;  // the worker's result, read once it has finished
		std::string boot;        // what boots after a package installs

		// Written by the worker, read by the screen.
		atomic_t<u32> firmware_done = 0;
		atomic_t<u32> firmware_total = 0;
		std::mutex lock;
		std::string firmware_version;
		package_reader* reader = nullptr;
		std::string title;
		std::string serial;
		std::string image_dir;

		// The install's speed, measured each second and smoothed.
		std::chrono::steady_clock::time_point measured{};
		u64 measured_bytes = 0;
		double speed = 0;
		u32 samples = 0;
		double last_progress = 0;

		u64 frames = 0;      // retro_run calls
		u64 release_at = 0;  // when the screen's images may go
	};

	preparation prep;
	libretro_loader::screen loader_screen();

	// ---- Input ------------------------------------------------------------------
	// Which RetroPads RetroArch has plugged in (retro_set_controller_port_device);
	// the first is plugged in until it says otherwise.
	std::array<unsigned, libretro_pad_handler::max_pads> port_devices{ RETRO_DEVICE_JOYPAD };
	bool joypad_mask = false;

	// ---- Audio --------------------------------------------------------------------
	// cellAudio's output, taken at the rate real time passes (48 kHz).
	std::chrono::steady_clock::time_point last_audio{};
	u64 audio_carry = 0; // sample-rate ticks past the last whole frame

	void push_audio()
	{
		if (!audio_batch_cb)
			return;
		const auto now = std::chrono::steady_clock::now();
		u32 frames = 800;
		if (last_audio != std::chrono::steady_clock::time_point{})
		{
			const u64 ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_audio).count();
			const u64 ticks = std::min<u64>(ns, 100'000'000) * 48'000 + audio_carry;
			frames = static_cast<u32>(ticks / 1'000'000'000);
			audio_carry = ticks % 1'000'000'000;
		}
		last_audio = now;
		libretro_audio_backend::render(frames, audio_batch_cb);
	}

	void poll_pads()
	{
		if (!input_poll_cb || !input_state_cb)
			return;
		input_poll_cb();
		for (u32 port = 0; port < libretro_pad_handler::max_pads; port++)
		{
			libretro_pad_handler::state pad{};
			pad.connected = (port_devices[port] & RETRO_DEVICE_MASK) == RETRO_DEVICE_JOYPAD;
			if (pad.connected)
			{
				if (joypad_mask)
				{
					pad.buttons = static_cast<u16>(input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK));
				}
				else
				{
					for (unsigned id = RETRO_DEVICE_ID_JOYPAD_B; id <= RETRO_DEVICE_ID_JOYPAD_R3; id++)
						if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, id))
							pad.buttons |= static_cast<u16>(1u << id);
				}
				pad.axes[0] = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
				pad.axes[1] = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
				pad.axes[2] = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
				pad.axes[3] = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
				pad.triggers[0] = static_cast<u16>(std::max<s16>(0, input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_BUTTON, RETRO_DEVICE_ID_JOYPAD_L2)));
				pad.triggers[1] = static_cast<u16>(std::max<s16>(0, input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_BUTTON, RETRO_DEVICE_ID_JOYPAD_R2)));
			}
			// Testing: the first pad's input as the core receives it, on the trace
			// whenever a button or trigger changes or a stick moves by more than
			// its noise (a resting stick wanders by a few hundred every frame)
			if (port == 0)
			{
				static std::array<s32, 7> s_last{-1};
				const std::array<s32, 7> now{pad.buttons, pad.axes[0], pad.axes[1], pad.axes[2], pad.axes[3], pad.triggers[0], pad.triggers[1]};
				bool moved = now[0] != s_last[0] || now[5] != s_last[5] || now[6] != s_last[6];
				for (int i = 1; i <= 4; i++)
					moved = moved || std::abs(now[i] - s_last[i]) > 4096;
				if (moved)
				{
					s_last = now;
					std::fprintf(stderr, "rpcs3 pad0: buttons=0x%04x ls=%d,%d rs=%d,%d l2=%d r2=%d connected=%d\n", pad.buttons, pad.axes[0], pad.axes[1],
						pad.axes[2], pad.axes[3], pad.triggers[0], pad.triggers[1], pad.connected ? 1 : 0);
				}
			}
			libretro_pad_handler::set_state(port, pad);
		}
	}

	void context_reset()
	{
		retro_hw_render_interface* hw_interface = nullptr;
		if (!environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &hw_interface) || !hw_interface ||
			hw_interface->interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN)
		{
			log(RETRO_LOG_ERROR, "Vulkan: RetroArch gave no Vulkan interface");
			return;
		}
		vulkan = reinterpret_cast<retro_hw_render_interface_vulkan*>(hw_interface);
		if (vulkan->interface_version != RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION)
		{
			log(RETRO_LOG_ERROR, "Vulkan: interface version %u, expected %u", vulkan->interface_version, RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION);
			vulkan = nullptr;
			return;
		}

		auto& frontend = vk::libretro::frontend();
		frontend.device = vulkan->device;
		frontend.gpu = vulkan->gpu;
		frontend.instance = vulkan->instance;
		frontend.queue_family = vulkan->queue_index;
		frontend.frame_count = std::popcount(vulkan->get_sync_index_mask(vulkan->handle));
		frontend.lock_queue = [] { vulkan->lock_queue(vulkan->handle); };
		frontend.unlock_queue = [] { vulkan->unlock_queue(vulkan->handle); };
		log(RETRO_LOG_INFO, "Vulkan: context ready, %u frames in flight", frontend.frame_count);
	}

	void context_destroy()
	{
		// The renderer draws on RetroArch's device: it goes first, and then the
		// pipeline cache the boots on that device shared.
		stop_emulation();
		vk::drop_kept_pipeline_cache();
		vk::libretro::release_frames();
		if (vulkan)
			libretro_loader::release(*vulkan);
		vk::libretro::frontend() = {};
		vulkan = nullptr;
		shown_width = shown_height = 0;
	}

	// The game's frames as RetroArch shows them, in 10 s windows on the trace
	// (stderr): how many, the longest wait for a new one, and how many came
	// later than 60 fps allows (25 ms). A stutter shows in the last two.
	struct frame_window
	{
		std::chrono::steady_clock::time_point start{};
		std::chrono::steady_clock::time_point last{};
		u32 frames = 0;
		u32 late = 0;
		double longest_ms = 0;
	};

	frame_window frames_shown;

	// Testing only: /app0/ppu-dump.txt, when present while a game runs, has
	// the game's executable written, segment by segment, to
	// /app0/ppu-dump/<address>.bin, for studying a game (a patch for a version
	// RPCS3's patch database lacks). The file is never shipped, and the dump,
	// which is the game's code, stays on the console and on my machine.
	void dump_executable()
	{
		if (!fs::is_file("/app0/ppu-dump.txt"))
			return;
		fs::remove_file("/app0/ppu-dump.txt");
		const auto main = g_fxo->try_get<main_ppu_module<lv2_obj>>();
		if (!main || !fs::create_path("/app0/ppu-dump"))
			return;
		for (const ppu_segment& seg : main->segs)
		{
			if (!seg.size || !vm::check_addr(seg.addr, vm::page_readable, seg.size))
				continue;
			fs::write_file(fmt::format("/app0/ppu-dump/%08x.bin", seg.addr), fs::rewrite, vm::base(seg.addr), seg.size);
			std::fprintf(stderr, "rpcs3 dump: segment 0x%08x size 0x%x type %u flags 0x%x\n", seg.addr, seg.size, seg.type, seg.flags);
		}
	}

	void count_frame()
	{
		timeline::instant(timeline::frame, 0);
		timeline::poll();
		const auto now = std::chrono::steady_clock::now();
		frame_window& w = frames_shown;
		if (w.frames == 0 && w.start == std::chrono::steady_clock::time_point{})
			w.start = now;
		else if (w.last != std::chrono::steady_clock::time_point{})
		{
			const double gap = std::chrono::duration<double, std::milli>(now - w.last).count();
			w.longest_ms = std::max(w.longest_ms, gap);
			w.late += gap > 25.0;
		}
		w.last = now;
		w.frames++;
		if (const double seconds = std::chrono::duration<double>(now - w.start).count(); seconds >= 10.0)
		{
			const double tsc_ms = utils::get_tsc_freq() / 1000.0;
			std::fprintf(stderr, "rpcs3 frames: window fps=%.1f longest=%.1f ms late=%u of %u occlusion_waits=%llu occlusion_wait_ms=%.1f"
				" putllc=%llu heavy=%llu heavy_ms=%.1f ppu_wait_ms=%.1f guarded=%llu given_back=%llu unstopped=%llu"
				" rsx_faults=%llu/%llu outside_guest_page=%llu completed=%llu submits=%llu submit_ms=%.1f\n",
				w.frames / seconds, w.longest_ms, w.late, w.frames, static_cast<unsigned long long>(g_occlusion_result_waits.exchange(0)),
				g_occlusion_result_wait_us.exchange(0) / 1000.0,
				static_cast<unsigned long long>(vm::g_writer_lock_stats[3].exchange(0)),
				static_cast<unsigned long long>(vm::g_writer_lock_stats[0].exchange(0)),
				vm::g_writer_lock_stats[1].exchange(0) / tsc_ms, vm::g_writer_lock_stats[2].exchange(0) / tsc_ms,
				static_cast<unsigned long long>(vm::g_guard_stats[0].exchange(0)), static_cast<unsigned long long>(vm::g_guard_stats[1].exchange(0)),
				static_cast<unsigned long long>(vm::g_guard_stats[2].exchange(0)),
				static_cast<unsigned long long>(g_rsx_fault_stats[0].exchange(0)), static_cast<unsigned long long>(g_rsx_fault_stats[1].exchange(0)),
				static_cast<unsigned long long>(g_rsx_fault_stats[2].exchange(0)), static_cast<unsigned long long>(g_rsx_fault_stats[3].exchange(0)),
				static_cast<unsigned long long>(vk::g_submit_stats[0].exchange(0)), vk::g_submit_stats[1].exchange(0) / tsc_ms);
			// Where the emulated CPUs' time went: each busy PPU thread's syscalls
			// (the three longest), and the SPU threads' channel reads, MFC
			// commands and stops, all threads together.
			for (u32 slot = 0; slot < 16; slot++)
			{
				std::array<std::pair<u64, u32>, 1024> spent{};
				u64 total = 0;
				for (u32 code = 0; code < 1024; code++)
				{
					spent[code] = {g_ppu_syscall_tsc[slot][code].exchange(0), code};
					total += spent[code].first;
				}
				if (total < tsc_ms * 100)
					continue;
				std::partial_sort(spent.begin(), spent.begin() + 3, spent.end(), std::greater<>());
				const auto thread = idm::check<named_thread<ppu_thread>>(ppu_thread::id_base + slot, [](ppu_thread& ppu)
				{
					const auto tname = ppu.ppu_tname.load();
					return tname ? *tname : std::string();
				});
				const std::string name = thread ? thread.ret : std::string();
				std::string top;
				for (u32 i = 0; i < 3 && spent[i].first; i++)
					top += fmt::format(" %s=%.0f", ppu_get_syscall_name(spent[i].second), spent[i].first / tsc_ms);
				std::fprintf(stderr, "rpcs3 ppu syscalls: thread=%u name=%s total_ms=%.0f%s\n", slot, name.c_str(), total / tsc_ms, top.c_str());
			}
			// Where the PPU threads' sleeps were called from (one in 16 sampled):
			// the three most frequent return addresses of each thread
			{
				std::unordered_map<u64, u32> callers;
				for (auto& sample : g_usleep_samples)
					if (const u64 s = sample.exchange(0))
						callers[s]++;
				g_usleep_sample_count = 0;
				std::vector<std::pair<u32, u64>> order;
				for (const auto& [key, count] : callers)
					order.emplace_back(count, key);
				std::sort(order.begin(), order.end(), std::greater<>());
				std::string top;
				for (usz i = 0; i < order.size() && i < 8; i++)
					top += fmt::format(" t%u:0x%08x=%u", static_cast<u32>(order[i].second >> 32) - 1, static_cast<u32>(order[i].second), order[i].first);
				std::fprintf(stderr, "rpcs3 ppu sleeps:%s\n", top.c_str());
				std::string kinds;
				for (usz kind = 0; kind < 3; kind++)
				{
					const u64 calls = g_usleep_stats[kind][0].exchange(0), asked = g_usleep_stats[kind][1].exchange(0), slept = g_usleep_stats[kind][2].exchange(0);
					kinds += fmt::format(" %s: calls=%llu asked_ms=%.1f slept_ms=%.1f", kind == 0 ? "under_100us" : kind == 1 ? "under_1ms" : "longer", calls, asked / 1000.0, slept / 1000.0);
				}
				std::fprintf(stderr, "rpcs3 usleep:%s\n", kinds.c_str());
			}
			std::string spu;
			for (u32 ch = 0; ch < 32; ch++)
				if (const u64 t = g_spu_rdch_tsc[ch].exchange(0); t >= tsc_ms * 20)
					spu += fmt::format(" rdch%u=%.0f", ch, t / tsc_ms);
			for (u32 cmd = 0; cmd < 256; cmd++)
				if (const u64 t = g_spu_mfc_tsc[cmd].exchange(0); t >= tsc_ms * 20)
					spu += fmt::format(" mfc%02x=%.0f", cmd, t / tsc_ms);
			std::fprintf(stderr, "rpcs3 spu time:%s stop=%.0f\n", spu.c_str(), g_spu_stop_tsc.exchange(0) / tsc_ms);

			// Accurate xfloat's regions (the xf-count testing switch): how many
			// ended, how many were made again on the double path and why, and
			// where the most were
			if (const u64 ends = g_spu_xf_counts[0].exchange(0))
			{
				u64 c[8]{ends};
				for (u32 i = 1; i < 8; i++)
					c[i] = g_spu_xf_counts[i].exchange(0);
				// Where (with the count of each SPU slot that needed the double path)
				std::vector<std::tuple<u32, u32, u32>> pcs;
				for (u32 i = 0; i < 64; i++)
					if (const u64 v = g_spu_xf_replay_pcs[i].load())
						pcs.emplace_back(static_cast<u32>(v), static_cast<u32>(v >> 32), i);
				std::sort(pcs.rbegin(), pcs.rend());
				std::string top;
				for (usz i = 0; i < std::min<usz>(pcs.size(), 12); i++)
				{
					const u32 index = std::get<2>(pcs[i]);
					top += fmt::format(" 0x%05x=%u(%llu/%llu/%llu/%llu)", std::get<1>(pcs[i]), std::get<0>(pcs[i]),
						static_cast<unsigned long long>(g_spu_xf_replay_lanes[index][0].load()), static_cast<unsigned long long>(g_spu_xf_replay_lanes[index][1].load()),
						static_cast<unsigned long long>(g_spu_xf_replay_lanes[index][2].load()), static_cast<unsigned long long>(g_spu_xf_replay_lanes[index][3].load()));
				}
				std::fprintf(stderr, "rpcs3 xfloat regions: ends=%llu ops=%llu replays=%llu replayed_ops=%llu flt_max=%llu exp255=%llu neg0=%llu cmp255=%llu at:%s\n",
					static_cast<unsigned long long>(c[0]), static_cast<unsigned long long>(c[1]), static_cast<unsigned long long>(c[2]),
					static_cast<unsigned long long>(c[3]), static_cast<unsigned long long>(c[4]), static_cast<unsigned long long>(c[5]),
					static_cast<unsigned long long>(c[6]), static_cast<unsigned long long>(c[7]), top.c_str());
			}

			// The RSX thread: idle (its own count, microseconds; the semaphore
			// waits among it), and where its busy time went
			static u64 s_rsx_idle = 0;
			const u64 rsx_idle = rsx::get_current_renderer() ? rsx::get_current_renderer()->performance_counters.idle_time.load() : 0;
			std::fprintf(stderr, "rpcs3 rsx time: idle=%.0f offloader=%.0f writeback=%.0f/%llu flip=%.0f semaphore=%.0f draws=%llu draw_ms=%.0f submit=%.0f\n",
				(rsx_idle - std::exchange(s_rsx_idle, rsx_idle)) / 1000.0, rsx::g_rsx_time_stats[0].exchange(0) / tsc_ms,
				rsx::g_rsx_time_stats[1].exchange(0) / tsc_ms, static_cast<unsigned long long>(rsx::g_rsx_time_stats[7].exchange(0)),
				rsx::g_rsx_time_stats[2].exchange(0) / tsc_ms,
				rsx::g_rsx_time_stats[3].exchange(0) / tsc_ms, static_cast<unsigned long long>(rsx::g_rsx_time_stats[5].exchange(0)),
				rsx::g_rsx_time_stats[4].exchange(0) / tsc_ms, rsx::g_rsx_time_stats[6].exchange(0) / tsc_ms);
			std::fprintf(stderr, "rpcs3 rsx vertex cache: requests=%llu misses=%llu uncacheable=%llu persistent_mbytes=%.1f volatile_mbytes=%.1f local_mbytes=%.1f repeated_mbytes=%.1f\n",
				static_cast<unsigned long long>(rsx::g_rsx_vertex_cache_stats[0].exchange(0)), static_cast<unsigned long long>(rsx::g_rsx_vertex_cache_stats[1].exchange(0)),
				static_cast<unsigned long long>(rsx::g_rsx_vertex_cache_stats[2].exchange(0)), rsx::g_rsx_vertex_cache_stats[3].exchange(0) / 1048576.0,
				rsx::g_rsx_vertex_cache_stats[4].exchange(0) / 1048576.0, rsx::g_rsx_vertex_cache_stats[5].exchange(0) / 1048576.0,
				rsx::g_rsx_vertex_cache_stats[6].exchange(0) / 1048576.0);
			std::fprintf(stderr, "rpcs3 rsx offload: jobs=%llu mbytes=%.1f busy_ms=%.0f inline=%llu inline_mbytes=%.1f labels=%llu faults=%llu\n",
				static_cast<unsigned long long>(rsx::g_rsx_offload_stats[0].exchange(0)), rsx::g_rsx_offload_stats[1].exchange(0) / 1048576.0,
				rsx::g_rsx_offload_stats[2].exchange(0) / tsc_ms, static_cast<unsigned long long>(rsx::g_rsx_offload_stats[3].exchange(0)),
				rsx::g_rsx_offload_stats[4].exchange(0) / 1048576.0, static_cast<unsigned long long>(rsx::g_rsx_offload_stats[5].exchange(0)),
				static_cast<unsigned long long>(rsx::g_rsx_offload_stats[6].exchange(0)));
			{
				std::string by_method;
				for (u32 i = 0; i < 8; i++)
				{
					const u64 key = rsx::g_rsx_writeback_methods[2 * i].exchange(0);
					const u64 count = rsx::g_rsx_writeback_methods[2 * i + 1].exchange(0);
					if (key)
						by_method += fmt::format(" 0x%04x/%02x=%llu", (key - 1) >> 8, (key - 1) & 0xff, count);
				}
				std::fprintf(stderr, "rpcs3 rsx writebacks by method:%s\n", by_method.c_str());

				// The most written methods (read while the RSX thread writes: approximate)
				std::vector<std::pair<u32, u32>> counts;
				u64 total = 0;
				for (u32 reg = 0; reg < 0x4000; reg++)
				{
					if (const u32 n = std::exchange(rsx::g_rsx_method_counts[reg], 0))
					{
						counts.emplace_back(n, reg);
						total += n;
					}
				}
				std::sort(counts.rbegin(), counts.rend());
				std::string top;
				for (usz i = 0; i < std::min<usz>(counts.size(), 16); i++)
					top += fmt::format(" 0x%04x=%u", counts[i].second, counts[i].first);
				std::fprintf(stderr, "rpcs3 rsx methods: total=%llu%s\n", static_cast<unsigned long long>(total), top.c_str());
			}

			jit_map_flush();
			std::fprintf(stderr, "rpcs3 rsx offload waits: submit=%.0f/%llu release=%.0f/%llu pause=%.0f/%llu other=%.0f/%llu\n",
				rsx::g_rsx_offload_waits[0].exchange(0) / tsc_ms, static_cast<unsigned long long>(rsx::g_rsx_offload_waits[4].exchange(0)),
				rsx::g_rsx_offload_waits[1].exchange(0) / tsc_ms, static_cast<unsigned long long>(rsx::g_rsx_offload_waits[5].exchange(0)),
				rsx::g_rsx_offload_waits[2].exchange(0) / tsc_ms, static_cast<unsigned long long>(rsx::g_rsx_offload_waits[6].exchange(0)),
				rsx::g_rsx_offload_waits[3].exchange(0) / tsc_ms, static_cast<unsigned long long>(rsx::g_rsx_offload_waits[7].exchange(0)));
			std::fprintf(stderr, "rpcs3 rsx frames: frames=%llu draw_calls=%llu submits=%llu setup=%.0f vertex_upload=%.0f texture_upload=%.0f draw_exec=%.0f flip=%.0f\n",
				static_cast<unsigned long long>(rsx::g_rsx_frame_totals[7].exchange(0)), static_cast<unsigned long long>(rsx::g_rsx_frame_totals[5].exchange(0)),
				static_cast<unsigned long long>(rsx::g_rsx_frame_totals[6].exchange(0)), rsx::g_rsx_frame_totals[0].exchange(0) / 1000.0,
				rsx::g_rsx_frame_totals[1].exchange(0) / 1000.0, rsx::g_rsx_frame_totals[2].exchange(0) / 1000.0,
				rsx::g_rsx_frame_totals[3].exchange(0) / 1000.0, rsx::g_rsx_frame_totals[4].exchange(0) / 1000.0);

			// The lines the PPU-stopping reservation stores went to (one in 64 of
			// them sampled): the six most frequent, with the SPU program counter
			// seen most there and the guard state of the page.
			{
				std::unordered_map<u32, std::pair<u32, std::unordered_map<u32, u32>>> lines;
				for (auto& sample : vm::g_heavy_samples)
				{
					if (const u64 s = sample.exchange(0))
					{
						auto& line = lines[static_cast<u32>(s >> 32) & -128];
						line.first++;
						line.second[static_cast<u32>(s)]++;
					}
				}
				vm::g_heavy_sample_count = 0;
				std::vector<std::pair<u32, u32>> order;
				for (const auto& [addr, line] : lines)
					order.emplace_back(line.first, addr);
				std::sort(order.begin(), order.end(), std::greater<>());
				std::string top;
				for (usz i = 0; i < order.size() && i < 6; i++)
				{
					const auto& pcs = lines[order[i].second].second;
					const auto pc = std::max_element(pcs.begin(), pcs.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
					top += fmt::format(" 0x%08x=%u(pc 0x%05x, guard 0x%08x)", order[i].second, order[i].first, pc->first, vm::guard_state(order[i].second));
				}
				std::fprintf(stderr, "rpcs3 heavy lines: distinct=%u%s\n", static_cast<u32>(lines.size()), top.c_str());
			}

			// The threads those stores waited on to stop, and where the slow ones
			// (past 20 us) were
			{
				std::string by;
				for (u32 slot = 0; slot < 17; slot++)
					if (const u64 t = vm::g_writer_wait_by[slot].exchange(0); t >= tsc_ms * 10)
						by += slot < 16 ? fmt::format(" t%u=%.0f", slot, t / tsc_ms) : fmt::format(" other=%.0f", t / tsc_ms);
				std::unordered_map<u64, u32> slow;
				for (auto& sample : vm::g_writer_slow_samples)
					if (const u64 s = sample.exchange(0))
						slow[s]++;
				const u32 slow_count = vm::g_writer_slow_count.exchange(0);
				std::vector<std::pair<u32, u64>> order;
				for (const auto& [key, count] : slow)
					order.emplace_back(count, key);
				std::sort(order.begin(), order.end(), std::greater<>());
				std::string at;
				for (usz i = 0; i < order.size() && i < 6; i++)
					at += fmt::format(" t%u:0x%08x=%u", static_cast<u32>(order[i].second >> 32) - 1, static_cast<u32>(order[i].second), order[i].first);
				std::fprintf(stderr, "rpcs3 ppu stop waits:%s slow=%u at%s guarded_stores=%llu given_back_meanwhile=%llu\n", by.c_str(), slow_count, at.c_str(),
					static_cast<unsigned long long>(vm::g_guarded_store_stats[0].exchange(0)), static_cast<unsigned long long>(vm::g_guarded_store_stats[1].exchange(0)));
			}
			w = {};
			w.start = w.last = now;
			dump_executable();
		}
	}

	// Hands RetroArch the newest finished frame, or repeats the last.
	void present_frame()
	{
		if (!video_cb)
			return;
		// Testing (the Null renderer, libretro_options.cpp): nothing is drawn,
		// so the frame windows count the RSX's flips instead.
		if (g_cfg.video.renderer == video_renderer::null)
		{
			static u64 s_last_flip = 0;
			if (const auto rsxthr = rsx::get_current_renderer(); rsxthr && rsxthr->int_flip_index != s_last_flip)
			{
				s_last_flip = rsxthr->int_flip_index;
				// A flip stands for a frame shown: it ends a boot's or a load's start too
				if (prep.now == stage::idle)
					count_frame();
				else if (prep.now == stage::starting)
					prep.now = stage::idle;
			}
			return;
		}
		vk::libretro::frame frame;
		if (vulkan && vk::libretro::take_frame(vulkan->get_sync_index(vulkan->handle), frame))
		{
			// RetroArch keeps the pointer to repeat the frame (and to take a
			// screenshot of it): it outlives this call.
			static retro_vulkan_image image{};
			image = { frame.view, frame.layout, frame.view_info };
			vulkan->set_image(vulkan->handle, &image, 0, nullptr, VK_QUEUE_FAMILY_IGNORED);
			shown_width = frame.width;
			shown_height = frame.height;
			// RETRO_HW_FRAME_BUFFER_VALID, without the macro's C cast.
			video_cb(reinterpret_cast<const void*>(~uptr{0}), frame.width, frame.height, 0);
			if (prep.now == stage::idle)
				count_frame();
			if (prep.now == stage::starting)
			{
				// The loading screen's images go once RetroArch has shown this
				// frame at every frame index.
				prep.now = stage::idle;
				prep.release_at = prep.frames + vk::libretro::frontend().frame_count + 1;
			}
			return;
		}
		if (vulkan && prep.now != stage::idle)
		{
			libretro_loader::present(*vulkan, video_cb, loader_screen(), output_width, output_height);
			shown_width = shown_height = 0;
			return;
		}
		if (vulkan && libretro_loader::active() && prep.frames >= prep.release_at)
			libretro_loader::release(*vulkan);
		video_cb(nullptr, shown_width ? shown_width : output_width, shown_height ? shown_height : output_height, 0);
	}

	// What RPCS3's overlays play a game's boot video and music with (SND0.AT3,
	// ICON1.PAM), which the Qt application does with Qt Multimedia. Nothing
	// plays here yet: the overlays need an object, not a null one.
	class silent_video_source final : public video_source
	{
		bool m_active = false;

	public:
		void set_iso_path(const std::string&) override {}
		void set_video_path(const std::string&, bool) override {}
		void set_audio_path(const std::string&, bool) override {}
		void set_active(bool active) override { m_active = active; }
		bool get_active() const override { return m_active; }
		bool has_new() const override { return false; }
		void get_image(std::vector<u8>&, int& w, int& h, int& ch, int& bpp) override { w = h = ch = bpp = 0; }
	};

	// The window RPCS3's renderer draws for: RetroArch shows the frames.
	class libretro_frame final : public GSFrameBase
	{
	public:
		void close() override {}
		void reset() override {}
		bool shown() override { return true; }
		void hide() override {}
		void show() override {}
		void toggle_fullscreen() override {}
		void delete_context(draw_context_t) override {}
		// RSX makes its context as its thread starts, then starts the PPU if
		// the emulator is already starting (RSXThread.cpp); otherwise it waits
		// for a start nothing will make. In the Qt application the renderer's
		// setup outlasts the boot, which runs Emu.Run first; here it can be the
		// other way round, so the renderer waits for the boot to finish.
		draw_context_t make_context() override
		{
			for (;;)
			{
				const system_state state = Emu.GetStatus(false);
				if (state != system_state::loading && state != system_state::ready)
					return nullptr;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
		void set_current(draw_context_t) override {}
		void flip(draw_context_t, bool) override {}
		int client_width() override { return static_cast<int>(output_width); }
		int client_height() override { return static_cast<int>(output_height); }
		f64 client_display_rate() override { return 60.; }
		bool has_alpha() override { return false; }
		display_handle_t handle() const override { return nullptr; }
		bool can_consume_frame() const override { return false; }
		void present_frame(std::vector<u8>&&, u32, u32, u32, bool) const override {}
		void take_screenshot(std::vector<u8>&&, u32, u32, bool) override {}
		void update_title(double) override {}
	};

	// RPCS3's files: the firmware, dev_flash, dev_hdd0, its configuration and
	// caches, all in the frontend's system folder, where my firmware is.
	bool set_directories()
	{
		const char* system = nullptr;
		if (!environ_cb || !environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system) || !system)
			return false;
		std::string root = system;
		if (!root.empty() && root.back() != '/')
			root += '/';
		root += "RPCS3/";
		g_android_executable_dir = root;
		g_android_config_dir = root;
		g_android_cache_dir = root + "cache/";
		return fs::create_path(g_android_config_dir) && fs::create_path(g_android_cache_dir);
	}

	void create_callbacks()
	{
		g_emu_callbacks.call_from_main_thread = [](std::function<void()> func, atomic_t<u32>* wake_up)
		{
			std::lock_guard lock(main_calls_lock);
			main_calls.emplace_back(std::move(func), wake_up);
		};
		g_emu_callbacks.init_gs_render = [](utils::serial* ar)
		{
			if (vulkan)
				g_fxo->init<rsx::thread, named_thread<VKGSRender>>(ar);
			else
				g_fxo->init<rsx::thread, named_thread<NullGSRender>>(ar);
		};
		g_emu_callbacks.get_gs_frame = []() -> std::unique_ptr<GSFrameBase> { return std::make_unique<libretro_frame>(); };
		// Images for RPCS3's overlays (save data and trophy icons) and the photo
		// modules, as the Qt application reads them with QImageReader. Calling a
		// callback nobody set ends the process (std::bad_function_call).
		g_emu_callbacks.get_scaled_image = [](const std::string& path, s32 target_width, s32 target_height, s32& width, s32& height, u8* dst, bool force_fit) -> bool
		{
			width = 0;
			height = 0;
			if (target_width <= 0 || target_height <= 0 || !dst || !fs::is_file(path))
				return false;
			std::vector<u8> bytes;
			if (fs::file file{path}; !file || !file.read(bytes, file.size()))
				return false;
			int source_width = 0, source_height = 0, channels = 0;
			stbi_uc* const source = stbi_load_from_memory(bytes.data(), ::narrow<int>(bytes.size()), &source_width, &source_height, &channels, 4);
			if (!source)
				return false;
			width = source_width;
			height = source_height;
			// Scaled into the target keeping its aspect, as QImageReader::setScaledSize is given it.
			if (force_fit || width > target_width || height > target_height)
			{
				const f32 convert_ratio = (width / static_cast<f32>(height)) / (target_width / static_cast<f32>(target_height));
				width = convert_ratio < 1.0f ? static_cast<s32>(target_width * convert_ratio) : target_width;
				height = convert_ratio > 1.0f ? static_cast<s32>(target_height / convert_ratio) : target_height;
				width = std::max(width, 1);
				height = std::max(height, 1);
			}
			// Each pixel averages the source pixels it covers (or the nearest one when enlarging).
			const usz limit = static_cast<usz>(target_width) * target_height * 4;
			for (s32 y = 0; y < height; y++)
			{
				const s32 y0 = static_cast<s32>(static_cast<s64>(y) * source_height / height);
				const s32 y1 = std::max(y0 + 1, static_cast<s32>(static_cast<s64>(y + 1) * source_height / height));
				for (s32 x = 0; x < width; x++)
				{
					const s32 x0 = static_cast<s32>(static_cast<s64>(x) * source_width / width);
					const s32 x1 = std::max(x0 + 1, static_cast<s32>(static_cast<s64>(x + 1) * source_width / width));
					u32 sum[4]{};
					for (s32 sy = y0; sy < y1; sy++)
						for (s32 sx = x0; sx < x1; sx++)
							for (int c = 0; c < 4; c++)
								sum[c] += source[(static_cast<usz>(sy) * source_width + sx) * 4 + c];
					const u32 count = static_cast<u32>((y1 - y0) * (x1 - x0));
					const usz at = (static_cast<usz>(y) * width + x) * 4;
					if (at + 4 > limit)
						continue;
					for (int c = 0; c < 4; c++)
						dst[at + c] = static_cast<u8>(sum[c] / count);
				}
			}
			stbi_image_free(source);
			return true;
		};
		g_emu_callbacks.get_image_info = [](const std::string& path, std::string& sub_type, s32& width, s32& height, s32& orientation) -> bool
		{
			sub_type.clear();
			width = 0;
			height = 0;
			orientation = 0; // CELL_SEARCH_ORIENTATION_UNKNOWN
			std::vector<u8> bytes;
			if (fs::file file{path}; !file || !file.read(bytes, file.size()))
				return false;
			int w = 0, h = 0, channels = 0;
			if (!stbi_info_from_memory(bytes.data(), ::narrow<int>(bytes.size()), &w, &h, &channels))
				return false;
			width = w;
			height = h;
			orientation = 1; // CELL_SEARCH_ORIENTATION_TOP_LEFT: stb applies no transformation
			return true;
		};
		// A new photo's path in /dev_hdd0/photo, named by its title and the time.
		g_emu_callbacks.get_photo_path = [](std::string_view title) -> std::string
		{
			std::string_view extension = ".png";
			if (const usz start = title.find_last_of('.'); start != umax)
			{
				extension = title.substr(start);
				title = title.substr(0, start);
			}
			const std::time_t now = std::time(nullptr);
			std::tm t{};
			localtime_r(&now, &t);
			const std::string path = vfs::get(fmt::format("/dev_hdd0/photo/%04d/%02d/%02d/%s %02d-%02d-%04d %02d-%02d-%02d",
				t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, vfs::escape(title, true), t.tm_mday, t.tm_mon + 1, t.tm_year + 1900, t.tm_hour, t.tm_min, t.tm_sec));
			std::string suffix(extension);
			for (u32 counter = 0; !Emu.IsStopped() && fs::is_file(path + suffix);)
				suffix = fmt::format(" %d%s", ++counter, extension);
			return path + suffix;
		};
		// What the Qt application's QFileInfo::canonicalFilePath gives: absolute,
		// resolved, no trailing separator, empty when the path does not exist.
		// Emulator::Load cuts the game's folder name after "<hdd0>/game" plus a
		// separator of its own: the default, the path as given with its trailing
		// separator, cut the first letter off (NPUA80490 booted as PUA80490).
		g_emu_callbacks.resolve_path = [](std::string_view sv) -> std::string
		{
			// QFileInfo("") resolves to nothing, not to the working directory.
			if (sv.empty())
				return {};
			std::error_code error;
			const std::filesystem::path path = std::filesystem::canonical(std::filesystem::path(sv), error);
			return error ? std::string() : path.string();
		};
		// The same for the part that exists, and the rest cleaned as given.
		g_emu_callbacks.resolve_path_may_not_exist = [](std::string_view sv) -> std::string
		{
			if (sv.empty())
				return {};
			std::error_code error;
			std::string path = std::filesystem::weakly_canonical(std::filesystem::path(sv), error).lexically_normal().string();
			if (error)
				path = std::filesystem::path(sv).lexically_normal().string();
			while (path.size() > 1 && path.back() == '/')
				path.pop_back();
			return path;
		};
		g_emu_callbacks.close_gs_frame = []() {};
		g_emu_callbacks.get_audio = []() -> std::shared_ptr<AudioBackend> { return std::make_shared<libretro_audio_backend>(); };
		g_emu_callbacks.get_audio_enumerator = [](u64) -> std::shared_ptr<audio_device_enumerator> { return std::make_shared<null_enumerator>(); };
		g_emu_callbacks.init_kb_handler = []()
		{
			ensure(g_fxo->init<KeyboardHandlerBase, NullKeyboardHandler>(Emu.DeserialManager()));
		};
		g_emu_callbacks.init_mouse_handler = []()
		{
			ensure(g_fxo->init<MouseHandlerBase, NullMouseHandler>(Emu.DeserialManager()));
		};
		g_emu_callbacks.init_pad_handler = [](std::string_view title_id)
		{
			// The PS3's controllers are RetroArch's RetroPads: the input
			// configuration pad_thread loads says so, whatever was saved before.
			// Each with the RetroPad handler's own bindings, as the Qt
			// application's pad dialog applies a handler's defaults: RPCS3's
			// are empty, and a pad bound to nothing never pressed a button.
			g_cfg_input.from_default();
			std::shared_ptr<PadHandlerBase> handler;
			for (u32 port = 0; port < libretro_pad_handler::max_pads; port++)
			{
				g_cfg_input.player[port]->handler.set(pad_handler::libretro);
				g_cfg_input.player[port]->device.from_string(libretro_pad_handler::device_name(port));
				pad_thread::InitPadConfig(g_cfg_input.player[port]->config, pad_handler::libretro, handler);
			}
			g_cfg_input.save("", g_cfg_input_configs.default_config);
			ensure(g_fxo->init<named_thread<pad_thread>>(nullptr, nullptr, title_id));
			qt_events_aware_op(0, [] { return !!pad::g_started; });
		};
		g_emu_callbacks.get_camera_handler = []() -> std::shared_ptr<camera_handler_base> { return std::make_shared<null_camera_handler>(); };
		g_emu_callbacks.get_music_handler = []() -> std::shared_ptr<music_handler_base> { return std::make_shared<null_music_handler>(); };
		g_emu_callbacks.get_msg_dialog = []() -> std::shared_ptr<MsgDialogBase> { return {}; };
		g_emu_callbacks.get_osk_dialog = []() -> std::shared_ptr<OskDialogBase> { return {}; };
		g_emu_callbacks.get_save_dialog = []() -> std::unique_ptr<SaveDialogBase> { return {}; };
		g_emu_callbacks.get_trophy_notification_dialog = []() -> std::unique_ptr<TrophyNotificationBase> { return {}; };
		g_emu_callbacks.on_run = [](bool) {};
		g_emu_callbacks.on_pause = []() {};
		g_emu_callbacks.on_resume = []() {};
		g_emu_callbacks.on_stop = []() {};
		g_emu_callbacks.on_ready = []() {};
		g_emu_callbacks.on_emulation_stop_no_response = [](std::shared_ptr<atomic_t<bool>> closed, int)
		{
			if (!closed || !*closed)
				report_fatal_error("Stopping the emulator took too long.", false, false);
		};
		g_emu_callbacks.on_save_state_progress = [](std::shared_ptr<atomic_t<bool>>, stx::shared_ptr<utils::serial>, stx::atomic_ptr<std::string>*, std::shared_ptr<void>) {};
		g_emu_callbacks.enable_disc_eject = [](bool) {};
		g_emu_callbacks.enable_disc_insert = [](bool) {};
		g_emu_callbacks.on_missing_fw = []() { log(RETRO_LOG_ERROR, "the PS3 firmware is not installed"); };
		g_emu_callbacks.try_to_quit = [](bool force, std::function<void()> on_exit) -> bool
		{
			if (force && on_exit)
				on_exit();
			return force;
		};
		g_emu_callbacks.handle_taskbar_progress = [](s32, s32) {};
		g_emu_callbacks.get_localized_string = [](localized_string_id, const char*) -> std::string { return {}; };
		g_emu_callbacks.get_localized_u32string = [](localized_string_id, const char*) -> std::u32string { return {}; };
		g_emu_callbacks.get_localized_setting = [](const cfg::_base*, u32) -> std::string { return {}; };
		g_emu_callbacks.play_sound = [](const std::string&, std::optional<f32>) {};
		g_emu_callbacks.add_breakpoint = [](u32) {};
		g_emu_callbacks.display_sleep_control_supported = []() { return false; };
		g_emu_callbacks.enable_display_sleep = [](bool) {};
		g_emu_callbacks.check_microphone_permissions = []() {};
		g_emu_callbacks.make_video_source = []() -> std::unique_ptr<video_source> { return std::make_unique<silent_video_source>(); };
		g_emu_callbacks.enable_gamemode = [](bool) {};
		g_emu_callbacks.update_emu_settings = []() {};
		g_emu_callbacks.save_emu_settings = []()
		{
			Emulator::SaveSettings(g_cfg.to_string(), Emu.GetTitleID());
		};
		g_emu_callbacks.get_font_dirs = []() { return std::vector<std::string>{}; };
		g_emu_callbacks.on_install_pkgs = [](const std::vector<std::string>& pkgs, bool from_optical_drive)
		{
			for (const std::string& pkg : pkgs)
				if (!rpcs3::utils::install_pkg(pkg, from_optical_drive))
					return false;
			return true;
		};
		// RPCS3's per-game recommended settings (libretro_game_configs.h), as its
		// Qt application adds them from its configuration database.
		g_emu_callbacks.get_database_config = [](const std::string& title_id)
		{
			if (title_id.empty())
				return std::string();
			std::string config;
			if (libretro_options::game_settings(environ_cb))
				config = libretro_game_configs::for_game(title_id, libretro_options::resolution_scale(environ_cb),
					libretro_patches::frame_rate_patch_on(title_id));
			// A game running unlocked at a steady 30 (the frame-rate patches option)
			if (libretro_patches::frame_limit_30(title_id))
				config = libretro_game_configs::with_frame_limit_30(config);
			return config;
		};
	}

	bool emulator_ready = false;

	// RPCS3's own log, RPCS3.log in its folder, and its errors in RetroArch's.
	// Both exist from retro_init to retro_deinit: RetroArch loads and unloads
	// the core once just to ask for its information, and a listener destroyed
	// with the core's other static objects would outlive RPCS3's logger; the
	// file's writer thread must be joined before the core goes.
	std::unique_ptr<logs::listener> log_file;

	struct frontend_log_listener final : logs::listener
	{
		void log(u64, const logs::message& msg, std::string_view prefix, std::string_view text) override
		{
			const logs::level severity = msg;
			if (severity > logs::level::error || !log_cb)
				return;
			log_cb(severity == logs::level::always ? RETRO_LOG_INFO : RETRO_LOG_ERROR, "[rpcs3] %.*s%.*s\n",
				static_cast<int>(prefix.size()), prefix.data(), static_cast<int>(text.size()), text.data());
		}
	};
	std::unique_ptr<frontend_log_listener> frontend_log;

	void stop_logging()
	{
		logs::listener::sync_all();
		// Nothing broadcasts to the listeners any more, then they go.
		logs::listener::shutdown_all();
		log_file.reset();
		frontend_log.reset();
	}

	// Installs the firmware from RPCS3's folder: the work of the Qt
	// application's main_window::HandlePupInstallation, without its dialogs.
	bool install_firmware(const std::string& path)
	{
		fs::file pup_file(path);
		if (!pup_file)
		{
			log(RETRO_LOG_ERROR, "firmware: cannot open %s", path.c_str());
			return false;
		}
		pup_object pup(std::move(pup_file));
		if (pup.operator pup_error() != pup_error::ok)
		{
			log(RETRO_LOG_ERROR, "firmware: %s is not a valid PUP (%s)", path.c_str(), pup.get_formatted_error().c_str());
			return false;
		}
		fs::file update_files_file = pup.get_file(0x300);
		if (!update_files_file || !update_files_file.size())
		{
			log(RETRO_LOG_ERROR, "firmware: no update packages in %s", path.c_str());
			return false;
		}
		tar_object update_files(update_files_file);
		std::vector<std::string> packages = update_files.get_filenames();
		std::erase_if(packages, [](const std::string& name) { return name.find("dev_flash_") == umax; });
		if (packages.empty())
		{
			log(RETRO_LOG_ERROR, "firmware: no dev_flash packages in %s", path.c_str());
			return false;
		}
		std::string version;
		if (fs::file version_file = pup.get_file(0x100))
			version = version_file.to_string();
		if (const usz end = version.find('\n'); end != umax)
			version.erase(end);
		log(RETRO_LOG_INFO, "firmware: installing %s (%zu packages)", version.c_str(), packages.size());
		{
			std::lock_guard lock(prep.lock);
			prep.firmware_version = version;
		}
		prep.firmware_total = ::size32(packages);

		vfs::mount("/dev_flash", g_cfg_vfs.get_dev_flash());
		usz installed = 0;
		for (const std::string& name : packages)
		{
			if (prep.cancel)
			{
				// A partial firmware is not a firmware: the next start installs it again.
				fs::remove_file(g_cfg_vfs.get_dev_flash() + "vsh/etc/version.txt");
				log(RETRO_LOG_WARN, "firmware: installation stopped after %zu packages", installed);
				return false;
			}
			auto stream = update_files.get_file(name);
			if (stream->m_file_handler)
				stream->m_file_handler->handle_file_op(*stream, 0, stream->get_size(umax), nullptr);
			fs::file update_file = fs::make_stream(std::move(stream->data));
			SCEDecrypter decrypter(update_file);
			decrypter.LoadHeaders();
			decrypter.LoadMetadata(SCEPKG_ERK, SCEPKG_RIV);
			decrypter.DecryptData();
			auto files = decrypter.MakeFile();
			if (files.size() < 3)
			{
				log(RETRO_LOG_ERROR, "firmware: %s did not decrypt", name.c_str());
				return false;
			}
			tar_object dev_flash(files[2]);
			if (!dev_flash.extract())
			{
				log(RETRO_LOG_ERROR, "firmware: %s did not extract", name.c_str());
				return false;
			}
			installed++;
			prep.firmware_done = static_cast<u32>(installed);
		}
		update_files_file.close();
		const std::string now = utils::get_firmware_version();
		log(RETRO_LOG_INFO, "firmware: %zu packages installed, version %s", installed, now.empty() ? "unknown" : now.c_str());
		return !now.empty();
	}

	// A PSN package's licences (.rap) next to it go to the user's exdata, as
	// the Qt application's main_window::InstallFileInExData puts them.
	void install_licences(const std::string& folder)
	{
		const std::string exdata = rpcs3::utils::get_hdd0_dir() + "home/" + Emu.GetUsr() + "/exdata/";
		fs::create_path(exdata);
		for (const fs::dir_entry& entry : fs::dir(folder))
		{
			if (entry.is_directory || !entry.name.ends_with(".rap"))
				continue;
			const std::string target = exdata + entry.name;
			if (!fs::is_file(target) && fs::copy_file(folder + "/" + entry.name, target, false))
				log(RETRO_LOG_INFO, "licence installed: %s", entry.name.c_str());
		}
	}

	// Where a package's finished install is recorded: an EBOOT.BIN alone may be
	// what an interrupted install left.
	std::string file_name(std::string_view path)
	{
		return std::string(path.substr(path.find_last_of('/') + 1));
	}

	std::string install_record(const std::string& title_id)
	{
		return g_android_config_dir + "libretro/installed/" + title_id;
	}

	// The title ID (NPUA80490): the package's PARAM.SFO says it; the header's
	// field is the content ID (UP9000-NPUA80490_00-...), which holds it too.
	std::string package_title_id(const package_reader& reader)
	{
		if (const std::string id(psf::get_string(reader.get_psf(), "TITLE_ID")); !id.empty())
			return id;
		const auto& content = reader.get_header().title_id;
		const std::string id(content, strnlen(content, sizeof(content)));
		return id.size() >= 16 ? id.substr(7, 9) : std::string();
	}

	std::string installed_boot(const std::string& title_id)
	{
		const std::string eboot = rpcs3::utils::get_hdd0_dir() + "game/" + title_id + "/USRDIR/EBOOT.BIN";
		return !title_id.empty() && fs::is_file(eboot) && fs::is_file(install_record(title_id)) ? eboot : std::string();
	}

	// A package is installed once: its bootable EBOOT.BIN, from the package
	// or from the earlier install. Empty when there is none. On the worker.
	std::string install_package(const std::string& path)
	{
		std::deque<package_reader> readers;
		readers.emplace_back(path);
		package_reader& reader = readers.front();
		if (!reader.is_valid())
		{
			log(RETRO_LOG_ERROR, "package: %s is not a valid package", path.c_str());
			return {};
		}
		const std::string title_id = package_title_id(reader);
		if (const std::string installed = installed_boot(title_id); !installed.empty())
			return installed;
		{
			std::lock_guard lock(prep.lock);
			prep.title = std::string(psf::get_string(reader.get_psf(), "TITLE", title_id));
			prep.serial = title_id;
			prep.image_dir = rpcs3::utils::get_hdd0_dir() + "game/" + title_id;
			prep.reader = &reader;
		}
		// An install that stops part way through leaves no record.
		fs::remove_file(install_record(title_id));
		log(RETRO_LOG_INFO, "package: installing %s", title_id.c_str());
		std::deque<std::string> bootables;
		const package_install_result result = package_reader::extract_data(readers, bootables, false);
		{
			std::lock_guard lock(prep.lock);
			prep.reader = nullptr;
		}
		if (result.error != package_install_result::error_type::no_error)
		{
			log(RETRO_LOG_ERROR, "package: %s did not install", path.c_str());
			return {};
		}
		std::string boot;
		for (const std::string& bootable : bootables)
			if (boot.empty() && fs::is_file(bootable))
				boot = bootable;
		const std::string eboot = rpcs3::utils::get_hdd0_dir() + "game/" + title_id + "/USRDIR/EBOOT.BIN";
		if (boot.empty() && fs::is_file(eboot))
			boot = eboot;
		if (!boot.empty() && fs::create_path(fs::get_parent_dir(install_record(title_id))))
			fs::write_file(install_record(title_id), fs::rewrite, file_name(path) + "\n");
		return boot;
	}
}

void run_main_calls()
{
	for (;;)
	{
		std::pair<std::function<void()>, atomic_t<u32>*> call;
		{
			std::lock_guard lock(main_calls_lock);
			if (main_calls.empty())
				return;
			call = std::move(main_calls.front());
			main_calls.pop_front();
		}
		call.first();
		if (call.second)
		{
			*call.second = true;
			call.second->notify_one();
		}
	}
}

namespace
{
	// Stops the game and waits for its threads (the renderer's among them),
	// running the calls it makes on this thread meanwhile.
	void stop_emulation()
	{
		pending_boot.clear();
		if (!Emu.IsStopped(true))
			Emu.GracefulShutdown(false, false);
		run_main_calls();
	}

	// Stops the game at once, as loading a save state does: not the exit
	// request a closing game gets. For a save state of the same game, the
	// compiled PPU code stays for its boot (Emulator::Kill).
	void kill_emulation(bool keep_code = false)
	{
		pending_boot.clear();
		g_keep_code_on_kill = keep_code;
		if (!Emu.IsStopped(true))
			Emu.Kill(false);
		for (int waited = 0; !Emu.IsStopped(true) && waited < 30000; waited++)
		{
			run_main_calls();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		run_main_calls();
		g_keep_code_on_kill = false;
	}

	// ---- Save states ------------------------------------------------------------
	// RetroArch's save states are RPCS3 savestates. RPCS3 writes one to the core's
	// memory file system (libretro_state.h) and the game boots again from it, as
	// RPCS3's own "save state and continue" does; loading one boots it the same
	// way. RetroArch asks for a state's size right before it serializes one, so
	// the state is made then and kept until the next frame: one request costs
	// one savestate, made or refused. Upstream RetroArch also backs up what runs
	// before a load, which costs one more; the PS5 title's RetroArch skips that
	// for a core whose states are basic, as this one's core info says.
	constexpr u64 state_magic = 0x31524c3353435052; // "RPCS3LR1"

	struct state_header
	{
		u64 magic;
		u64 size;
	};

	struct made_state
	{
		std::vector<u8> bytes;
		u64 frame = 0;
		bool tried = false;  // at `frame`, made or refused
		bool made = false;
	};

	made_state made;

	std::string state_file()
	{
		return libretro_state::path("state.SAVESTAT.zst");
	}

	void make_state()
	{
		made = {};
		made.frame = prep.frames;
		made.tried = true;
		if (!prep.booted || pending_boot.size() || Emu.IsStopped(true) || Emu.IsStarting())
		{
			log(RETRO_LOG_WARN, "save state: no game is running");
			return;
		}
		const std::string file = state_file();
		libretro_state::remove(file);
		Emu.savestate_path_override = file;
		const auto started = std::chrono::steady_clock::now();
		Emu.GracefulShutdown(false, true, true);
		// Done when the emulator has stopped; refused when RPCS3 dropped the
		// request (an SPU it could not stop, a video decoder, a game saving) and
		// the game runs on.
		while (!Emu.IsStopped(true) && Emu.IsClosePending() && std::chrono::steady_clock::now() - started < std::chrono::minutes(2))
		{
			run_main_calls();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		run_main_calls();
		Emu.savestate_path_override.clear();
		if (!Emu.IsStopped(true))
		{
			log(RETRO_LOG_ERROR, "save state: RPCS3 could not make one now (RPCS3.log says why)");
			return;
		}
		made.bytes = libretro_state::read(file);
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
		if (made.bytes.empty())
		{
			// The game stopped and left no state: it starts again from what booted last.
			log(RETRO_LOG_ERROR, "save state: RPCS3 stopped without one; the game starts again");
			pending_boot = prep.last_boot;
			prep.now = stage::starting;
			prep.booted = false;
			return;
		}
		made.made = true;
		log(RETRO_LOG_INFO, "save state: %zu bytes in %.1f s; the game resumes from it", made.bytes.size(), seconds);
		// The game goes on from the state just made.
		pending_boot = file;
		prep.now = stage::starting;
		prep.booted = false;
		prep.resuming = true;
	}

	size_t state_size()
	{
		// Once a frame: RetroArch asks for the size more than once for one
		// state, and a state RPCS3 refused takes seconds to refuse again.
		if (!made.tried || made.frame != prep.frames)
			make_state();
		// Never 0: RetroArch loads no state into a core that answers 0.
		return sizeof(state_header) + (made.made ? made.bytes.size() : 0);
	}

	bool serialize_state(void* data, size_t size)
	{
		if (!made.made || made.frame != prep.frames || size < sizeof(state_header) + made.bytes.size())
			return false;
		const state_header header{state_magic, made.bytes.size()};
		std::memcpy(data, &header, sizeof(header));
		std::memcpy(static_cast<u8*>(data) + sizeof(header), made.bytes.data(), made.bytes.size());
		std::memset(static_cast<u8*>(data) + sizeof(header) + made.bytes.size(), 0, size - sizeof(header) - made.bytes.size());
		return true;
	}

	bool unserialize_state(const void* data, size_t size)
	{
		state_header header{};
		if (size < sizeof(header))
			return false;
		std::memcpy(&header, data, sizeof(header));
		if (header.magic != state_magic || header.size == 0 || header.size > size - sizeof(header))
		{
			log(RETRO_LOG_ERROR, "load state: not an RPCS3 state");
			return false;
		}
		if (prep.now == stage::firmware || prep.now == stage::package)
		{
			log(RETRO_LOG_ERROR, "load state: not while installing");
			return false;
		}
		// What runs goes; the state boots in its place (the same game's, whose
		// compiled code is kept).
		kill_emulation(true);
		const u8* const bytes = static_cast<const u8*>(data) + sizeof(header);
		libretro_state::write(state_file(), std::vector<u8>(bytes, bytes + header.size));
		made = {};
		pending_boot = state_file();
		prep.now = stage::starting;
		prep.booted = false;
		prep.resuming = true;
		log(RETRO_LOG_INFO, "load state: %llu bytes; the game resumes from it", static_cast<unsigned long long>(header.size));
		return true;
	}

	void fail(std::string what)
	{
		log(RETRO_LOG_ERROR, "%s", what.c_str());
		prep.failure = std::move(what);
		prep.now = stage::failed;
	}

	void boot_pending()
	{
		const std::string path = std::exchange(pending_boot, {});
		prep.last_boot = path;
		log(RETRO_LOG_INFO, "booting %s", path.c_str());
		const game_boot_result result = Emu.BootGame(path, "", true);
		if (is_error(result))
		{
			fail(fmt::format("The game did not start (%s)", result));
			return;
		}
		prep.booted = true;
		log(RETRO_LOG_WARN, "booted %s: state %u", path.c_str(), static_cast<u32>(Emu.GetStatus(false)));
	}

	void start_worker(stage next, std::function<void()> work)
	{
		prep.now = next;
		prep.finished = false;
		prep.succeeded = false;
		prep.worker = std::make_unique<named_thread<std::function<void()>>>("Content Installer", [work = std::move(work)]
		{
			work();
			prep.finished = true;
		});
	}

	// After the firmware: the package installs, or the content boots.
	void continue_to_content()
	{
		const std::string path = prep.content;
		if (path.ends_with(".pkg") || path.ends_with(".PKG"))
		{
			// An installed package boots at once.
			if (const std::string installed = installed_boot(package_title_id(package_reader(path))); !installed.empty())
			{
				install_licences(fs::get_parent_dir(path));
				pending_boot = installed;
				prep.now = stage::starting;
				return;
			}
			start_worker(stage::package, [path]
			{
				install_licences(fs::get_parent_dir(path));
				prep.boot = install_package(path);
				prep.succeeded = !prep.boot.empty();
			});
			return;
		}
		// A folder is the game in it: an installed game's (USRDIR/EBOOT.BIN) or
		// a disc game's (PS3_GAME/USRDIR/EBOOT.BIN). RPCS3 boots a bare folder as
		// "build the PPU cache of everything in it", which compiles and stops.
		if (fs::is_dir(path))
		{
			std::string folder = path;
			while (folder.size() > 1 && folder.back() == '/')
				folder.pop_back();
			for (const char* inside : {"/USRDIR/EBOOT.BIN", "/PS3_GAME/USRDIR/EBOOT.BIN"})
			{
				if (fs::is_file(folder + inside))
				{
					pending_boot = folder + inside;
					prep.now = stage::starting;
					return;
				}
			}
			fail("No PS3 game in this folder");
			return;
		}
		pending_boot = path;
		prep.now = stage::starting;
	}

	void begin_preparation(const std::string& path, const std::string& pup)
	{
		prep.content = path;
		prep.booted = false;
		prep.resuming = false;
		prep.failure.clear();
		{
			std::lock_guard lock(prep.lock);
			prep.firmware_version.clear();
			prep.title.clear();
			prep.serial.clear();
			prep.image_dir.clear();
		}
		prep.firmware_done = 0;
		prep.firmware_total = 0;
		prep.measured = {};
		prep.samples = 0;
		prep.speed = 0;
		prep.last_progress = 0;
		if (!pup.empty())
		{
			start_worker(stage::firmware, [pup] { prep.succeeded = install_firmware(pup); });
			return;
		}
		continue_to_content();
	}

	// In retro_run: a finished worker's next step.
	void advance_preparation()
	{
		prep.frames++;
		if (prep.now == stage::starting && prep.booted && pending_boot.empty() && Emu.IsStopped(true))
		{
			fail("The game stopped while starting");
			return;
		}
		// A game that stops by itself (it quit, or RPCS3 stopped it) says so,
		// rather than leave its last frame up.
		if (prep.now == stage::idle && prep.booted && pending_boot.empty() && Emu.IsStopped(true))
		{
			fail("The game has stopped");
			return;
		}
		if ((prep.now != stage::firmware && prep.now != stage::package) || !prep.finished)
			return;
		prep.worker.reset();
		if (prep.now == stage::firmware)
		{
			if (!prep.succeeded)
			{
				fail("The PS3 firmware did not install");
				return;
			}
			Emu.Init();
			continue_to_content();
			return;
		}
		if (!prep.succeeded)
		{
			fail("The game did not install");
			return;
		}
		pending_boot = prep.boot;
		prep.now = stage::starting;
	}

	// Stops an install (a package's is undone) and forgets the content.
	void stop_preparation()
	{
		prep.cancel = true;
		{
			std::lock_guard lock(prep.lock);
			if (prep.reader)
				prep.reader->abort_extract();
		}
		prep.worker.reset();
		prep.cancel = false;
		prep.now = stage::idle;
	}

	std::string format_bytes(double bytes)
	{
		char text[32];
		if (bytes >= 1e9)
			std::snprintf(text, sizeof(text), "%.2f GB", bytes / 1e9);
		else if (bytes >= 1e6)
			std::snprintf(text, sizeof(text), "%.0f MB", bytes / 1e6);
		else
			std::snprintf(text, sizeof(text), "%.0f KB", bytes / 1e3);
		return text;
	}

	std::string format_percent(double progress)
	{
		return fmt::format("%u%%", static_cast<u32>(std::clamp(progress, 0.0, 1.0) * 100));
	}

	std::string format_time_left(double seconds)
	{
		if (seconds < 60)
			return "Less than a minute left";
		const u64 minutes = static_cast<u64>(seconds / 60 + 0.5);
		if (minutes < 60)
			return fmt::format("About %u min left", minutes);
		return fmt::format("About %u h %u min left", minutes / 60, minutes % 60);
	}

	libretro_loader::screen loader_screen()
	{
		libretro_loader::screen shown;
		std::lock_guard lock(prep.lock);
		shown.title = prep.title;
		shown.serial = prep.serial;
		shown.image_dir = prep.image_dir;
		switch (prep.now)
		{
		case stage::firmware:
		{
			shown.heading = "Installing firmware";
			shown.title = prep.firmware_version.empty() ? "PS3 System Software" : "PS3 System Software " + prep.firmware_version;
			shown.serial = "RPCS3";
			if (const u32 total = prep.firmware_total)
			{
				const u32 done = std::min<u32>(prep.firmware_done, total);
				shown.progress = prep.last_progress = static_cast<double>(done) / total;
				shown.detail = fmt::format("Package %u of %u", std::min(done + 1, total), total);
				shown.remaining = format_percent(shown.progress);
			}
			break;
		}
		case stage::package:
		{
			shown.heading = "Installing";
			if (!prep.reader)
				break;
			const double total = static_cast<double>(prep.reader->get_header().data_size);
			const double progress = prep.reader->get_progress(1'000'000) / 1e6;
			const double done = total * progress;
			const auto now = std::chrono::steady_clock::now();
			if (prep.measured == std::chrono::steady_clock::time_point{})
			{
				prep.measured = now;
				prep.measured_bytes = static_cast<u64>(done);
			}
			else if (const double elapsed = std::chrono::duration<double>(now - prep.measured).count(); elapsed >= 1.0)
			{
				const double rate = (done - static_cast<double>(prep.measured_bytes)) / elapsed;
				prep.speed = prep.samples ? prep.speed * 0.8 + rate * 0.2 : rate;
				prep.samples++;
				prep.measured = now;
				prep.measured_bytes = static_cast<u64>(done);
			}
			shown.progress = prep.last_progress = progress;
			shown.detail = format_bytes(done) + " of " + format_bytes(total);
			shown.remaining = format_percent(progress);
			if (prep.samples >= 3 && prep.speed > 0)
			{
				shown.detail += "  \u00b7  " + format_bytes(prep.speed) + "/s";
				shown.remaining = format_time_left((total - done) / prep.speed) + "  \u00b7  " + shown.remaining;
			}
			break;
		}
		case stage::starting:
		{
			shown.heading = prep.resuming ? "Resuming" : "Starting";
			if (!Emu.GetTitle().empty())
				shown.title = Emu.GetTitle();
			if (!Emu.GetTitleID().empty())
				shown.serial = Emu.GetTitleID();
			if (const std::string dir = Emu.GetSfoDir(false); !dir.empty())
				shown.image_dir = dir;
			shown.detail = g_progr_text.operator std::string();
			if (const u32 total = g_progr_ptotal)
			{
				shown.progress = std::min<double>(g_progr_pdone, total) / total;
				shown.remaining = fmt::format("%u of %u", std::min<u32>(g_progr_pdone, total), total);
			}
			else if (const u32 files = g_progr_ftotal)
			{
				shown.progress = std::min<double>(g_progr_fdone, files) / files;
				shown.remaining = fmt::format("%u of %u", std::min<u32>(g_progr_fdone, files), files);
			}
			break;
		}
		case stage::failed:
			shown.heading = prep.failure;
			shown.failed = true;
			shown.progress = prep.last_progress;
			shown.detail = "RPCS3.log in RPCS3's folder says why";
			if (!Emu.GetTitle().empty())
				shown.title = Emu.GetTitle();
			break;
		case stage::idle:
			break;
		}
		if (shown.title.empty())
			shown.title = file_name(prep.content);
		return shown;
	}

	// The emulator's state every ten seconds while a game is loaded (stopped,
	// loading, stopping, running, paused, frozen, ready, starting).
	void report_state()
	{
		static std::chrono::steady_clock::time_point last{};
		const auto now = std::chrono::steady_clock::now();
		if (now - last < std::chrono::seconds(10))
			return;
		last = now;
		if (!Emu.IsStopped(true))
			log(RETRO_LOG_WARN, "state %u", static_cast<u32>(Emu.GetStatus(false)));
	}
}

extern "C"
{
	RETRO_API unsigned retro_api_version(void)
	{
		return RETRO_API_VERSION;
	}

	RETRO_API void retro_set_environment(retro_environment_t cb)
	{
		environ_cb = cb;
		retro_log_callback logging{};
		if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
			log_cb = logging.log;
		libretro_options::declare(cb);
	}

	RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
	RETRO_API void retro_set_audio_sample(retro_audio_sample_t) {}
	RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
	RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
	RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

	RETRO_API void retro_get_system_info(retro_system_info* info)
	{
		std::memset(info, 0, sizeof(*info));
		info->library_name = "RPCS3";
		info->library_version = "0.0.1";
		info->valid_extensions = "pkg|iso|bin|elf|self|sfo";
		info->need_fullpath = true;
		info->block_extract = true;
	}

	RETRO_API void retro_get_system_av_info(retro_system_av_info* info)
	{
		std::memset(info, 0, sizeof(*info));
		info->geometry.base_width = output_width;
		info->geometry.base_height = output_height;
		info->geometry.max_width = 3840;
		info->geometry.max_height = 2160;
		info->geometry.aspect_ratio = 16.0f / 9.0f;
		info->timing.fps = 60.0;
		info->timing.sample_rate = 48000.0;
	}

	RETRO_API void retro_init(void)
	{
		if (!set_directories())
		{
			log(RETRO_LOG_ERROR, "no system directory for RPCS3's files");
			return;
		}
		log_file = logs::make_file_listener(fs::get_log_dir() + "RPCS3.log", 256ull << 20);
		frontend_log = std::make_unique<frontend_log_listener>();
		logs::listener::add(frontend_log.get());
		// A listener's destruction silences every channel (an earlier
		// retro_deinit's): back to the default levels.
		logs::reset();
		// The thread pool's finalizer, as the Qt application makes it first.
		static_cast<void>(named_thread("", [](int) {}));
		sleep_probe();
		create_callbacks();
		joypad_mask = environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, nullptr);
		// RPCS3's Vulkan renderer, on RetroArch's device whatever its name
		// (Emu.Init sets the renderer to this default).
		Emu.SetSupportedRenderers({video_renderer::null, video_renderer::vulkan});
		Emu.SetDefaultRenderer(video_renderer::vulkan);
		Emu.SetDefaultGraphicsAdapter("RetroArch");
		Emu.SetHasGui(false);
		Emu.SetUsr("00000001");
		Emu.Init();
		emulator_ready = true;
		// The loading screen's fonts: Inter, staged with the core, then the
		// firmware's own.
		libretro_loader::set_font_dirs({g_android_config_dir + "fonts/", g_cfg_vfs.get_dev_flash() + "data/font/"});
		const std::string firmware = utils::get_firmware_version();
		log(RETRO_LOG_INFO, "RPCS3 %s, firmware %s", "libretro", firmware.empty() ? "not installed" : firmware.c_str());
	}

	RETRO_API void retro_deinit(void)
	{
		if (emulator_ready)
		{
			stop_preparation();
			stop_emulation();
			Emu.Kill();
			run_main_calls();
			// The Qt application's exit: the global object map is cleared
			// before the core's static objects go (Emulator::CleanUp).
			Emu.Quit(true);
		}
		emulator_ready = false;
		stop_logging();
	}

	RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
	{
		if (port < port_devices.size())
			port_devices[port] = device;
	}
	RETRO_API void retro_reset(void) {}

	RETRO_API void retro_run(void)
	{
		run_main_calls();
		if (bool updated = false; environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
			libretro_options::apply_live(environ_cb);
		advance_preparation();
		if (!pending_boot.empty() && vulkan)
			boot_pending();
		poll_pads();
		push_audio();
		present_frame();
		report_state();
	}

	RETRO_API size_t retro_serialize_size(void) { return emulator_ready ? state_size() : 0; }
	RETRO_API bool retro_serialize(void* data, size_t size) { return emulator_ready && serialize_state(data, size); }
	RETRO_API bool retro_unserialize(const void* data, size_t size) { return emulator_ready && unserialize_state(data, size); }
	RETRO_API void retro_cheat_reset(void) {}
	RETRO_API void retro_cheat_set(unsigned, bool, const char*) {}

	RETRO_API bool retro_load_game(const retro_game_info* game)
	{
		if (!emulator_ready || !game || !game->path)
			return false;
		const std::string path = game->path;

		// The firmware installs on the first start, from RPCS3's own folder.
		std::string pup;
		if (utils::get_firmware_version().empty())
		{
			pup = g_android_config_dir + "PS3UPDAT.PUP";
			if (!fs::is_file(pup))
			{
				log(RETRO_LOG_ERROR, "no PS3 firmware: put PS3UPDAT.PUP in %s", g_android_config_dir.c_str());
				return false;
			}
		}

		// RPCS3's renderer draws on RetroArch's Vulkan device: the game boots in
		// the first retro_run after RetroArch has created it, once the firmware
		// and a PSN package (with its licences) have installed.
		set_output_size(apply_options());
		hw_render = {};
		hw_render.context_type = RETRO_HW_CONTEXT_VULKAN;
		hw_render.version_major = VK_API_VERSION_1_2;
		hw_render.context_reset = context_reset;
		hw_render.context_destroy = context_destroy;
		if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
		{
			log(RETRO_LOG_ERROR, "RetroArch has no Vulkan context for RPCS3");
			return false;
		}
		environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE, const_cast<retro_hw_render_context_negotiation_interface_vulkan*>(&vulkan_negotiation));
		// Save states are RPCS3 savestates: their size is the game's, and they
		// hold the host's own data (JIT caches' keys, byte order).
		uint64_t quirks = RETRO_SERIALIZATION_QUIRK_CORE_VARIABLE_SIZE | RETRO_SERIALIZATION_QUIRK_PLATFORM_DEPENDENT;
		environ_cb(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS, &quirks);
		begin_preparation(path, pup);
		return true;
	}

	RETRO_API bool retro_load_game_special(unsigned, const retro_game_info*, size_t) { return false; }
	RETRO_API void retro_unload_game(void)
	{
		stop_preparation();
		stop_emulation();
		vk::drop_kept_pipeline_cache();
	}
	RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
	RETRO_API void* retro_get_memory_data(unsigned) { return nullptr; }
	RETRO_API size_t retro_get_memory_size(unsigned) { return 0; }
}

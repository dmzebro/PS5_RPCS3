#pragma once

// Vulkan in the libretro core (BUILD_LIBRETRO, rpcs3/libretro): RetroArch owns
// the instance and the device, which RPCS3 creates through RetroArch's context
// negotiation, shares the graphics queue with the renderer, and shows each
// finished frame itself. The renderer presents into images RetroArch samples.

#include "swapchain_core.h"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <vector>

namespace vk::libretro
{
	struct frontend_context
	{
		VkInstance instance = VK_NULL_HANDLE;
		VkPhysicalDevice gpu = VK_NULL_HANDLE;

		// The device RetroArch uses: render_device::create makes it once, through
		// create_device, and adopts it afterwards.
		VkDevice device = VK_NULL_HANDLE;

		// RetroArch's device creation, which adds what the frontend needs. Set
		// while RetroArch creates its context.
		std::function<VkDevice(VkPhysicalDevice, const VkDeviceCreateInfo&)> create_device;

		// The graphics queue family RetroArch uses too, and how many images
		// RetroArch may hold at once (its frame indices).
		u32 queue_family = 0;
		u32 frame_count = 0;

		// RetroArch submits to the shared queue from its own thread.
		std::function<void()> lock_queue;
		std::function<void()> unlock_queue;
	};

	frontend_context& frontend();

	// render_device::create's vkCreateDevice: RetroArch's device, created the
	// first time and adopted after that.
	VkResult create_device(VkPhysicalDevice gpu, const VkDeviceCreateInfo& info, VkDevice* device);

	// Whether the device is RetroArch's own, which RPCS3 never destroys.
	bool owns_device(VkDevice device);

	// The queue family render_device::create is given for transfers: RPCS3's
	// choice in instance::create_swapchain, made the same way.
	u32 transfer_queue_family(vk::physical_device& gpu, u32 graphics_family);

	// A frame for RetroArch (retro_vulkan_image, with its size).
	struct frame
	{
		VkImageView view = VK_NULL_HANDLE;
		VkImageViewCreateInfo view_info{};
		VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
		u32 width = 0;
		u32 height = 0;
	};

	// On RetroArch's thread, in retro_run: the newest finished frame, once, for
	// RetroArch's frame index `frame_index`. False when there is no new one
	// (RetroArch shows the last again).
	bool take_frame(u32 frame_index, frame& out);

	// On RetroArch's thread, when its context goes away: every presentation
	// image, the current renderer's and those it left behind.
	void release_frames();
}

namespace vk
{
	class swapchain_libretro final : public swapchain_base
	{
	public:
		struct presentable
		{
			enum class state
			{
				free,      // RPCS3 may acquire it
				acquired,  // RPCS3 renders into it
				ready,     // finished; RetroArch has not taken it
				shown,     // RetroArch shows it, and repeats it
				retiring,  // RetroArch may still read it, until frame index `release_at` comes around again
			};

			VkImage image = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			VkImageView view = VK_NULL_HANDLE;
			VkImageViewCreateInfo view_info{};
			VkFence fence = VK_NULL_HANDLE;  // signalled when the frame in it is finished
			bool fence_pending = false;
			state status = state::free;
			u32 release_at = 0;
		};

	private:
		std::vector<presentable> m_images;
		std::mutex m_lock;
		std::condition_variable m_freed;
		u32 m_ready = umax;
		u32 m_shown = umax;

		void signal_semaphore(VkSemaphore semaphore);
		void retire_images();

	protected:
		void init_swapchain_images(render_device& dev, u32 count) override;

	public:
		swapchain_libretro(physical_device& gpu, u32 graphics_queue, u32 transfer_queue);
		~swapchain_libretro() override;

		void create(display_handle_t&) override {}
		void destroy(bool full = true) override;

		using swapchain_base::init;
		bool init() override;

		u32 get_swap_image_count() const override { return ::size32(m_images); }
		VkImage get_image(u32 index) override { return m_images[index].image; }

		VkResult acquire_next_swapchain_image(VkSemaphore semaphore, u64 timeout, u32* result) override;
		void end_frame(command_buffer&, u32) override {}
		VkResult present(VkSemaphore semaphore, u32 index) override;

		// RetroArch samples the image.
		VkImageLayout get_optimal_present_layout() const override { return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }

		// The frame size is the core's, never a window's.
		bool supports_automatic_wm_reports() const override { return true; }

		bool take_frame(u32 frame_index, libretro::frame& out);
	};

	using swapchain_NATIVE = swapchain_libretro;
}

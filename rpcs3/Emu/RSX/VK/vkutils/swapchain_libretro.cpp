#include "stdafx.h"
#include "swapchain.h"
#include "device.h"
#include "../VKHelpers.h"

#include <chrono>

namespace vk::libretro
{
	namespace
	{
		frontend_context g_frontend;

		// The renderer's swapchain, which RetroArch's thread takes frames from,
		// and the images earlier renderers left behind, which RetroArch may still
		// show until a newer frame has replaced them.
		std::mutex g_live_lock;
		swapchain_libretro* g_live = nullptr;
		std::vector<swapchain_libretro::presentable> g_left_behind;
		bool g_left_behind_armed = false;
		u32 g_left_behind_release_at = 0;

		void free_presentable(swapchain_libretro::presentable& p)
		{
			const VkDevice device = g_frontend.device;
			if (p.view) vkDestroyImageView(device, p.view, nullptr);
			if (p.image) vkDestroyImage(device, p.image, nullptr);
			if (p.memory) vkFreeMemory(device, p.memory, nullptr);
			if (p.fence) vkDestroyFence(device, p.fence, nullptr);
			p = {};
		}

		// The frame RetroArch last used an image in, before frame `frame_index`,
		// is finished once that frame's index comes around again.
		u32 previous_frame_index(u32 frame_index)
		{
			const u32 count = std::max(g_frontend.frame_count, 1u);
			return (frame_index + count - 1) % count;
		}
	}

	frontend_context& frontend()
	{
		return g_frontend;
	}

	VkResult create_device(VkPhysicalDevice gpu, const VkDeviceCreateInfo& info, VkDevice* device)
	{
		if (!g_frontend.device)
		{
			if (!g_frontend.create_device || gpu != g_frontend.gpu)
			{
				rsx_log.error("libretro: RetroArch did not ask for this device");
				return VK_ERROR_INITIALIZATION_FAILED;
			}

			g_frontend.device = g_frontend.create_device(gpu, info);
			if (!g_frontend.device)
			{
				return VK_ERROR_INITIALIZATION_FAILED;
			}
		}

		*device = g_frontend.device;
#ifdef RPCS3_VULKAN_VOLK
		volkLoadDevice(g_frontend.device);
#endif
		return VK_SUCCESS;
	}

	bool owns_device(VkDevice device)
	{
		return device != VK_NULL_HANDLE && device == g_frontend.device;
	}

	u32 transfer_queue_family(vk::physical_device& gpu, u32 graphics_family)
	{
		for (u32 i = 0; i < gpu.get_queue_count(); ++i)
		{
			const auto flags = gpu.get_queue_properties(i).queueFlags;
			if (i != graphics_family && (flags & (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)) == (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT))
			{
				return i;
			}
		}

		return umax;
	}

	bool take_frame(u32 frame_index, frame& out)
	{
		std::lock_guard live(g_live_lock);

		if (g_left_behind_armed && frame_index == g_left_behind_release_at)
		{
			for (auto& p : g_left_behind)
			{
				free_presentable(p);
			}

			g_left_behind.clear();
			g_left_behind_armed = false;
		}

		if (!g_live || !g_live->take_frame(frame_index, out))
		{
			return false;
		}

		// A newer frame is shown: what earlier renderers left behind is last
		// read in the frame before this one.
		if (!g_left_behind.empty() && !g_left_behind_armed)
		{
			g_left_behind_armed = true;
			g_left_behind_release_at = previous_frame_index(frame_index);
		}

		return true;
	}

	void release_frames()
	{
		std::lock_guard live(g_live_lock);

		if (g_live)
		{
			rsx_log.error("libretro: RetroArch's Vulkan context went away under a running renderer");
		}

		if (g_left_behind.empty() || !g_frontend.device)
		{
			return;
		}

		if (g_frontend.lock_queue) g_frontend.lock_queue();
		vkDeviceWaitIdle(g_frontend.device);
		if (g_frontend.unlock_queue) g_frontend.unlock_queue();

		for (auto& p : g_left_behind)
		{
			free_presentable(p);
		}

		g_left_behind.clear();
		g_left_behind_armed = false;
	}
}

namespace vk
{
	using libretro::g_live;
	using libretro::g_live_lock;
	using libretro::g_left_behind;

	swapchain_libretro::swapchain_libretro(physical_device& gpu, u32 graphics_queue, u32 transfer_queue)
		: swapchain_base(gpu, graphics_queue, graphics_queue, transfer_queue, VK_FORMAT_B8G8R8A8_UNORM)
	{
	}

	swapchain_libretro::~swapchain_libretro()
	{
		std::lock_guard live(g_live_lock);

		if (g_live == this)
		{
			g_live = nullptr;
		}

		for (auto& p : m_images)
		{
			g_left_behind.push_back(p);
		}

		m_images.clear();
	}

	void swapchain_libretro::retire_images()
	{
		// RetroArch may be showing one of them: they stay alive until it
		// shows a newer frame, or its context goes away.
		for (auto& p : m_images)
		{
			g_left_behind.push_back(p);
		}

		m_images.clear();
		m_ready = umax;
		m_shown = umax;
	}

	void swapchain_libretro::destroy(bool full)
	{
		{
			std::lock_guard live(g_live_lock);

			if (g_live == this)
			{
				g_live = nullptr;
			}

			std::lock_guard lock(m_lock);
			retire_images();
		}

		if (full)
		{
			dev.destroy();
		}
	}

	void swapchain_libretro::init_swapchain_images(render_device& device, u32 count)
	{
		VkPhysicalDeviceMemoryProperties memory_properties;
		vkGetPhysicalDeviceMemoryProperties(device.gpu(), &memory_properties);

		m_images.resize(count);

		for (auto& p : m_images)
		{
			VkImageCreateInfo info
			{
				.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
				// RetroArch may view the image as sRGB.
				.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
				.imageType = VK_IMAGE_TYPE_2D,
				.format = m_surface_format,
				.extent = { m_width, m_height, 1 },
				.mipLevels = 1,
				.arrayLayers = 1,
				.samples = VK_SAMPLE_COUNT_1_BIT,
				.tiling = VK_IMAGE_TILING_OPTIMAL,
				.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
				.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
				.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			};
			CHECK_RESULT(vkCreateImage(device, &info, nullptr, &p.image));

			// Its own allocation, not the renderer's allocator: the image may
			// outlive the renderer while RetroArch still shows it.
			VkMemoryRequirements requirements;
			vkGetImageMemoryRequirements(device, p.image, &requirements);

			VkMemoryAllocateInfo allocation{ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size };
			allocation.memoryTypeIndex = umax;
			for (u32 type = 0; type < memory_properties.memoryTypeCount; ++type)
			{
				if ((requirements.memoryTypeBits & (1u << type)) && (memory_properties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
				{
					allocation.memoryTypeIndex = type;
					break;
				}
			}

			ensure(allocation.memoryTypeIndex != umax);
			CHECK_RESULT(vkAllocateMemory(device, &allocation, nullptr, &p.memory));
			CHECK_RESULT(vkBindImageMemory(device, p.image, p.memory, 0));

			p.view_info =
			{
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.image = p.image,
				.viewType = VK_IMAGE_VIEW_TYPE_2D,
				.format = m_surface_format,
				.components = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A },
				.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
			};
			CHECK_RESULT(vkCreateImageView(device, &p.view_info, nullptr, &p.view));

			VkFenceCreateInfo fence_info{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
			CHECK_RESULT(vkCreateFence(device, &fence_info, nullptr, &p.fence));
		}
	}

	bool swapchain_libretro::init()
	{
		std::lock_guard live(g_live_lock);
		std::lock_guard lock(m_lock);

		if (!m_images.empty())
		{
			retire_images();
		}

		// RetroArch shows one image and may still read others in the frames it
		// has in flight; one more is finished and waiting, and RPCS3 renders one.
		init_swapchain_images(dev, std::max(libretro::frontend().frame_count + 3, 4u));
		g_live = this;
		return true;
	}

	void swapchain_libretro::signal_semaphore(VkSemaphore semaphore)
	{
		// No window system signals the acquire semaphore: an empty submission does.
		const VkSubmitInfo info
		{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.signalSemaphoreCount = 1,
			.pSignalSemaphores = &semaphore,
		};

		acquire_global_submit_lock();
		const VkResult result = vkQueueSubmit(dev.get_graphics_queue(), 1, &info, VK_NULL_HANDLE);
		release_global_submit_lock();
		CHECK_RESULT(result);
	}

	VkResult swapchain_libretro::acquire_next_swapchain_image(VkSemaphore semaphore, u64 timeout, u32* result)
	{
		// RPCS3 polls with no timeout when it has few images; waiting a little
		// for RetroArch to give one back keeps that from spinning.
		const auto wait = std::chrono::nanoseconds(std::clamp<u64>(timeout, 2'000'000, 1'000'000'000));
		const auto deadline = std::chrono::steady_clock::now() + wait;

		std::unique_lock lock(m_lock);
		for (;;)
		{
			for (u32 index = 0; index < m_images.size(); ++index)
			{
				if (m_images[index].status == presentable::state::free)
				{
					m_images[index].status = presentable::state::acquired;
					lock.unlock();
					signal_semaphore(semaphore);
					*result = index;
					return VK_SUCCESS;
				}
			}

			if (m_freed.wait_until(lock, deadline) == std::cv_status::timeout)
			{
				return VK_TIMEOUT;
			}
		}
	}

	VkResult swapchain_libretro::present(VkSemaphore semaphore, u32 index)
	{
		auto& p = m_images[index];

		// Its last frame is finished: RetroArch waited for it, or RPCS3 rendered
		// into the image again after it on the same queue.
		CHECK_RESULT(vkWaitForFences(dev, 1, &p.fence, VK_TRUE, UINT64_MAX));
		CHECK_RESULT(vkResetFences(dev, 1, &p.fence));

		const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		const VkSubmitInfo info
		{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.waitSemaphoreCount = 1,
			.pWaitSemaphores = &semaphore,
			.pWaitDstStageMask = &stage,
		};

		acquire_global_submit_lock();
		const VkResult result = vkQueueSubmit(dev.get_graphics_queue(), 1, &info, p.fence);
		release_global_submit_lock();

		if (result != VK_SUCCESS)
		{
			return result;
		}

		std::lock_guard lock(m_lock);

		// RetroArch did not take the previous frame: it is dropped.
		if (m_ready != umax)
		{
			m_images[m_ready].status = presentable::state::free;
			m_freed.notify_all();
		}

		p.status = presentable::state::ready;
		m_ready = index;
		return VK_SUCCESS;
	}

	bool swapchain_libretro::take_frame(u32 frame_index, libretro::frame& out)
	{
		u32 index;
		{
			std::lock_guard lock(m_lock);

			for (auto& p : m_images)
			{
				if (p.status == presentable::state::retiring && p.release_at == frame_index)
				{
					p.status = presentable::state::free;
					m_freed.notify_all();
				}
			}

			if (m_ready == umax)
			{
				return false;
			}

			index = std::exchange(m_ready, umax);
			m_images[index].status = presentable::state::shown;
		}

		CHECK_RESULT(vkWaitForFences(dev, 1, &m_images[index].fence, VK_TRUE, UINT64_MAX));

		{
			std::lock_guard lock(m_lock);

			if (m_shown != umax)
			{
				m_images[m_shown].status = presentable::state::retiring;
				m_images[m_shown].release_at = libretro::previous_frame_index(frame_index);
			}

			m_shown = index;
		}

		const auto& p = m_images[index];
		out.view = p.view;
		out.view_info = p.view_info;
		out.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		out.width = m_width;
		out.height = m_height;
		return true;
	}
}

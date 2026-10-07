#pragma once

#include <util/types.hpp>
#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/Host/MM.h"

#include "context_accessors.define.h"

namespace rsx
{
	namespace util
	{
		template <bool FlushDMA, bool FlushPipe>
		static void write_gcm_label(context* ctx, u32 type, u32 address, u32 data)
		{
			const bool is_flip_sema = (address == (RSX(ctx)->label_addr + 0x10) || address == (RSX(ctx)->device_addr + 0x30));
			if (!is_flip_sema)
			{
				// First, queue the GPU work. If it flushes the queue for us, the following routines will be faster.
				const bool handled = RSX(ctx)->get_backend_config().supports_host_gpu_labels && RSX(ctx)->release_GCM_label(type, address, data);

				if constexpr (FlushDMA && !FlushPipe)
				{
					// With copies still queued on the offloader (the vertex data the
					// draws so far read), the label is written after them by the
					// offloader rather than waiting here for them: a texture read
					// release came about 350 times a frame in GTA IV, each waiting
					// for the queue to drain. It comes before the check below, as a
					// queued write may yet change the value it reads.
					if (!handled)
					{
						rsx::mm_flush();

						if (g_fxo->get<rsx::dma_manager>().defer_label_write(address, data))
						{
							return;
						}
					}
				}

				if (vm::_ref<RsxSemaphore>(address) == data && !g_fxo->get<rsx::dma_manager>().has_pending())
				{
					// It's a no-op to write the same value (although there is a delay in real-hw so it's more accurate to allow GPU label in this case)
					// There is no possible way for the guest to know that the label has been processed so we can skip MM sync here.
					// (Not with a release still queued on the offloader, which may change the value.)
					return;
				}

				if constexpr (FlushDMA || FlushPipe)
				{
					if constexpr (FlushDMA)
					{
						// Release op must be acoompanied by MM flush.
						// FlushPipe implicitly does a MM flush but FlushDMA does not. Trigger the flush here
						rsx::mm_flush();

						// If the backend handled the request, this call will basically be a NOP
						g_fxo->get<rsx::dma_manager>().sync(1);
					}

					if constexpr (FlushPipe)
					{
						// Syncronization point, may be associated with memory changes without actually changing addresses
						RSX(ctx)->m_graphics_state |= rsx::pipeline_state::fragment_program_needs_rehash;

						// Manually flush the pipeline.
						// It is possible to stream report writes using the host GPU, but that generates too much submit traffic.
						RSX(ctx)->sync();
					}
				}

				if (handled)
				{
					// Backend will handle it, nothing to write.
					return;
				}
			}

			// A release deferred above may still be queued: this one goes after it
			if (!g_fxo->get<rsx::dma_manager>().defer_label_write(address, data))
			{
				vm::write<atomic_t<RsxSemaphore>>(address, data);
			}
		}
	}
}

#include "context_accessors.undef.h"

#pragma once

#include "util/types.hpp"
#include "Utilities/address_range.h"
#include "gcm_enums.h"

#include <vector>

template <typename T>
class named_thread;

namespace rsx
{
	class dma_manager
	{
		enum op
		{
			raw_copy = 0,
			vector_copy = 1,
			index_emulate = 2,
			callback = 3,
			label_write = 4
		};

		struct transport_packet
		{
			op type{};
			std::vector<u8> opt_storage{};
			void* src{};
			void* dst{};
			u32 length{};
			u32 aux_param0{};
			u32 aux_param1{};

			transport_packet(void *_dst, void *_src, u32 len)
				: type(op::raw_copy), src(_src), dst(_dst), length(len)
			{}

			transport_packet(void *_dst, std::vector<u8>& _src, u32 len)
				: type(op::vector_copy), opt_storage(std::move(_src)), dst(_dst), length(len)
			{}

			transport_packet(void *_dst, rsx::primitive_type prim, u32 len)
				: type(op::index_emulate), dst(_dst), length(len), aux_param0(static_cast<u8>(prim))
			{}

			transport_packet(u32 command, void* args)
				: type(op::callback), src(args), aux_param0(command)
			{}

			transport_packet(op _type, u32 address, u32 value)
				: type(_type), aux_param0(address), aux_param1(value)
			{}

			transport_packet(const transport_packet&) = delete;
			transport_packet& operator=(const transport_packet&) = delete;
		};

		atomic_t<bool> m_mem_fault_flag = false;

		struct offload_thread;
		std::shared_ptr<named_thread<offload_thread>> m_thread;

		// TODO: Improved benchmarks here; value determined by profiling on a Ryzen CPU, rounded to the nearest 512 bytes
		const u32 max_immediate_transfer_size = 3584;

	public:
		dma_manager() = default;

		// initialization
		void init();

		// General tranport
		void copy(void *dst, std::vector<u8>& src, u32 length) const;
		void copy(void *dst, void *src, u32 length) const;

		// Vertex utilities
		void emulate_as_indexed(void *dst, rsx::primitive_type primitive, u32 count);

		// Renderer callback
		void backend_ctrl(u32 request_code, void* args);

		// A semaphore release behind the copies still queued: written by the
		// offloader once they are done, so the guest cannot see it before the
		// memory they read may change. False, and nothing queued, when no copy
		// is pending (the caller writes it).
		bool defer_label_write(u32 address, u32 value);

		// Work still queued (a deferred release among it, perhaps)
		bool has_pending() const;

		// A copy faulted and waits for the RSX thread to service it
		bool in_fault_recovery() const { return m_mem_fault_flag; }

		// Synchronization
		bool is_current_thread() const;
		// site (testing): where the wait comes from, for g_rsx_offload_waits:
		// 0 a submission, 1 a semaphore release, 2 a pause, 3 anything else
		bool sync(u32 site = 3) const;
		void join();
		void set_mem_fault_flag();
		void clear_mem_fault_flag();

		// Fault recovery
		utils::address_range32 get_fault_range(bool writing) const;
	};
}

#pragma once

#include "vm.h"
#include "Emu/RSX/Utils/rsx_utils.h"

class cpu_thread;
class shared_mutex;

namespace vm
{
	extern thread_local atomic_t<cpu_thread*>* g_tls_locked;

	// Reservation writes, for the libretro core's frame windows (read and
	// cleared there): [0] writer_locks taken with a range lock (the heavyweight
	// path of SPU and PPU reservation stores), [1] their TSC ticks in all, [2]
	// the ticks of those spent waiting for PPU threads to park, [3] SPU PUTLLCs.
	extern atomic_t<u64> g_writer_lock_stats[4];

	// Guarded pages: reservation stores without stopping the PPU threads, on a
	// host with no transactional memory (the PS5's Zen 2). A reservation store
	// writes a 128-byte line through the privileged mapping, and has to be
	// atomic with every plain store to that line; RPCS3 got that by stopping
	// every running PPU thread for each one (writer_lock). A host page that
	// takes many of them is made read-only in the guest mapping instead, so a
	// plain store to it faults rather than races, and the stores to it only
	// lock their line. The fault gives the page back (after the stores in
	// flight on it) and the store is made; a page that keeps faulting, or that
	// another protection covers (the RSX's), is left alone. vm.cpp.

	// A reservation store to addr's line may go without the PPU stop: true
	// (and the store in flight until guard_leave) when its page is guarded.
	bool guard_enter(u32 addr) noexcept;
	void guard_leave(u32 addr) noexcept;

	// Whether addr's page is guarded now (a hint: guard_enter decides)
	bool guard_peek(u32 addr) noexcept;

	// A reservation store on an unguarded page: counted, and the page guarded
	// once it has taken enough of them.
	void guard_note_heavy(u32 addr) noexcept;

	// A write fault in the guest mapping at addr: true when it was a guarded
	// page's, which is now given back (the store can be made again).
	bool guard_fault(u32 addr) noexcept;

	// The PPU recompiler's stores to a guarded page do not fault: its code
	// reads a byte for the host page (addr >> guard_check_shift()) from this
	// table, set while the page is guarded, and such a store is made by
	// guarded_store instead. So only other writers (code in the emulator, the
	// RSX, DMA) give a page back. Null when pages are never guarded.
	u8* guard_check_table() noexcept;
	u32 guard_check_shift() noexcept;

	// A store of 1 to 16 bytes (the guest's, in memory order) to a guarded
	// page by the PPU recompiler's code: through the privileged mapping, under
	// the reservation lock of each line a PPU-stopping reservation store has
	// been made to (they no longer stop the PPU threads there).
	void guarded_store(cpu_thread& cpu, u32 addr, u64 lo, u64 hi, u32 size) noexcept;

	// Testing: [0] stores made by guarded_store, [1] those whose page was given
	// back meanwhile
	extern atomic_t<u64> g_guarded_store_stats[2];

	// [0] pages guarded, [1] guards given back, [2] stores made without the PPU stop
	extern atomic_t<u64> g_guard_stats[3];

	// Testing: the guard word of addr's page (vm.cpp), bit 31 set when another
	// protection covers it
	u32 guard_state(u32 addr) noexcept;

	// Testing: one in 64 reservation stores that stopped the PPU threads, as
	// the line's address (high half) and the SPU's program counter
	extern atomic_t<u64> g_heavy_samples[256];
	extern atomic_t<u32> g_heavy_sample_count;

	// Testing: the PPU-stopping stores' waits by the thread waited on (PPU
	// threads by number modulo 16, [16] anything else), in TSC ticks; and
	// where the thread was (thread number high, guest pc low) when one wait
	// went past 20 us
	extern atomic_t<u64> g_writer_wait_by[17];
	extern atomic_t<u64> g_writer_slow_samples[64];
	extern atomic_t<u32> g_writer_slow_count;

	enum range_lock_flags : u64
	{
		/* flags (3 bits, W + R + Reserved) */

		range_writable = 4ull << 61,
		range_readable = 2ull << 61,
		range_reserved = 1ull << 61,
		range_full_mask = 7ull << 61,

		/* flag combinations with special meaning */

		range_locked = 4ull << 61, // R+W as well, but being exclusively accessed (size extends addr)
		range_allocation = 0, // Allocation, no safe access, g_shmem may change at ANY location

		range_pos = 61,
		range_bits = 3,
	};

	extern atomic_t<u64, 128> g_range_lock_bits[2];

	extern atomic_t<u64> g_shmem[];

	// Register reader
	void passive_lock(cpu_thread& cpu);

	// Register range lock for further use
	atomic_t<u64, 128>* alloc_range_lock();

	void range_lock_internal(atomic_t<u64, 128>* range_lock, u32 begin, u32 size);

	// Lock memory range ignoring memory protection (Size!=0 also implies aligned begin)
	template <uint Size = 0>
	FORCE_INLINE void range_lock(atomic_t<u64, 128>* range_lock, u32 begin, u32 _size)
	{
		if constexpr (Size == 0)
		{
			if (begin >> 28 == rsx::constants::local_mem_base >> 28)
			{
				return;
			}
		}

		// Optimistic locking.
		// Note that we store the range we will be accessing, without any clamping.
		range_lock->store(begin | (u64{_size} << 32));

		// Old-style conditional constexpr
		const u32 size = Size ? Size : _size;

		if (Size == 1 || (begin % 4096 + size % 4096) / 4096 == 0 ? !vm::check_addr(begin) : !vm::check_addr(begin, vm::page_readable, size))
		{
			range_lock->release(0);
			range_lock_internal(range_lock, begin, _size);
			return;
		}

		#ifndef _MSC_VER
		__asm__(""); // Tiny barrier
		#endif

		if (!g_range_lock_bits[1]) [[likely]]
		{
			return;
		}

		// Fallback to slow path
		range_lock_internal(range_lock, begin, size);
	}

	// Release it
	void free_range_lock(atomic_t<u64, 128>*) noexcept;

	// Optimization (set cpu_flag::memory)
	bool temporary_unlock(cpu_thread& cpu) noexcept;
	void temporary_unlock() noexcept;

	struct writer_lock final
	{
		atomic_t<u64, 128>* range_lock;

		// The store's page is guarded: the PPU threads were not stopped
		bool guarded = false;
		u32 guard_addr = 0;

		writer_lock(const writer_lock&) = delete;
		writer_lock& operator=(const writer_lock&) = delete;
		writer_lock() noexcept;
		writer_lock(u32 addr, atomic_t<u64, 128>* range_lock = nullptr, u32 size = 128, u64 flags = range_locked) noexcept;
		~writer_lock() noexcept;
	};
} // namespace vm

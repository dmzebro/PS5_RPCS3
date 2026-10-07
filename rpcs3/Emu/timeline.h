#pragma once

#include "util/types.hpp"
#include "util/atomic.hpp"
#include "util/tsc.hpp"

// Testing (/app0/timeline.txt, "START_S DURATION_MS"): for one window of a
// run, each thread's waits (PPU syscalls, SPU channel reads, MFC commands and
// stops) and the frames, stamped with the TSC, written to /app0/timeline.bin
// when the window ends. START_S counts from the first frame shown.
namespace timeline
{
	enum kind : u16
	{
		ppu_syscall = 1, // arg: syscall number, pc: the guest's return address
		spu_rdch = 2,    // arg: channel, pc: the SPU's
		spu_mfc = 3,     // arg: MFC command
		spu_stop = 4,    // arg: stop code
		frame = 5,       // a frame shown (RetroArch's)
		rsx_flip = 6,    // arg: buffer
	};

	extern atomic_t<u32> g_on;

	// Testing: /app0/counters.txt turns on the frame windows' counters and
	// timers in the emulator's hot paths. Always on, their TSC reads and
	// atomic adds on lines every SPU thread shares took about 3% of the SPU
	// threads' time in GTA IV.
	bool counters_file();

	inline bool counters()
	{
		static const bool s_on = counters_file();
		return s_on;
	}

	// tag: for SPU events, a fingerprint of the code around pc (spu_code_tag)
	void record(u16 kind, u32 arg, u32 pc, u64 start, u64 end, u32 tag = 0);

	// FNV-1a of the 32 bytes of local store around pc: which program the SPU
	// was in (the scratch tools match it against a dump of the programs)
	inline u32 spu_code_tag(const u8* ls, u32 pc)
	{
		const u8* p = ls + (pc & 0x3ffe0);
		u32 h = 0x811c9dc5u;

		for (u32 i = 0; i < 32; i++)
		{
			h = (h ^ p[i]) * 0x01000193u;
		}

		return h;
	}

	// An interval (waits shorter than a microsecond are left out)
	inline void interval(u16 kind, u32 arg, u32 pc, u64 start)
	{
		if (g_on) [[unlikely]]
		{
			record(kind, arg, pc, start, utils::get_tsc());
		}
	}

	inline void instant(u16 kind, u32 arg)
	{
		if (g_on) [[unlikely]]
		{
			const u64 now = utils::get_tsc();
			record(kind, arg, 0, now, now);
		}
	}

	// Called with each frame shown: starts and ends the window, and writes it
	void poll();
}

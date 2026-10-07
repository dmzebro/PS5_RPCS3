#pragma once

#include "util/types.hpp"

#include <string_view>

class spu_thread;

// Native SPU programs (SPUNativePrograms.h). /app0/spu-native.txt holding "run"
// makes them stand in for the recompiled programs they are versions of;
// "check" runs the recompiled programs and compares each native result with
// theirs at the thread's next chunk entry (a mismatch is logged); without the
// file they are not used.
//
// The testing capture: for the programs
// /app0/spu-capture.txt names (by their chunk names' hash, one a line, with an
// optional count of calls), each call's state at the program's entry and at
// the next chunk entry the thread makes (the state the program left) goes to
// /app0/spu-capture.bin, for the host harness that checks a native version of
// the program against what the recompiled program did.
namespace spu_native
{
	// Capture is on: the recompiler hooks the chunks
	bool capture_enabled();

	// The program is one to capture
	bool capture_wanted(std::string_view hash);

	// At a chunk entry: a wanted program's (program != 0; its entry point and
	// the local store range its code covers) or, while the thread is armed
	// (spu_thread::native_capture), any chunk's
	void capture_chunk(spu_thread* spu, u32 pc, u64 program, u32 entry, u32 lower, u32 end);

	enum class native_mode { off, check, run };

	native_mode mode();

	// The recompiler hooks every chunk (capture or checking)
	inline bool hooks_chunks()
	{
		return capture_enabled() || mode() == native_mode::check;
	}

	// The native version of the program being compiled (its entry and its code
	// from there): its index, or -1 (an index, not a pointer: compiled code is
	// kept between boots)
	s32 find(u32 entry, const u8* code, u32 bytes);

	// At the entry chunk of a program with a native version: the next pc when
	// the native version ran (run mode), 0 when the recompiled program is to
	// run (check mode arms the comparison)
	u32 run(spu_thread* spu, u32 pc, u32 index);
}

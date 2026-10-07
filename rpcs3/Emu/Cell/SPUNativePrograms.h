// Native versions of SPU programs (SPUNative.h). Each takes the thread's registers and local
// store at the program's entry and leaves them as the recompiled program would, returning the
// next pc (0: declined, the recompiled program runs instead). A program is found by its entry,
// its length and a hash of its words (FNV-1a over the words as the local store holds them), so
// no game code is kept here: only what the code does.
#pragma once

#include "SPUNativeOps.h"

namespace spu_native_programs
{
	using namespace spu_native_ops;

	// A sequential backward byte copy in the local store: the preferred slot of $13 bytes ending
	// at $6 to the bytes ending at $14 + $13, a quadword read-modify-write a byte. Leaves the loop
	// for 0x17938, its registers as the loop leaves them.
	inline u32 backward_byte_copy(reg* gpr, u8* ls)
	{
		const u32 exit_pc = 0x17938;
		gpr[12] = add_words(gpr[14], gpr[13]);
		const u32 n = word(gpr[13], 0);
		if (n == 0)
			return exit_pc;

		const u32 src = word(gpr[6], 0), dst = word(gpr[12], 0);
		const u32 last_src = src - n, last_dst = dst - n;

		// The byte the last store replaces, as it was: the last load of $8 comes before it
		const u8 before_last = ls[last_dst & ls_mask];

		// In bulk where that is the same as the byte loop: no wrap of the local store, and the
		// destination at or above the source or apart from it
		const u32 s0 = last_src & ls_mask, d0 = last_dst & ls_mask;
		const bool wraps = n > ls_mask || s0 + n > ls_mask + 1 || d0 + n > ls_mask + 1;
		if (!wraps && (d0 >= s0 || d0 + n <= s0))
		{
			std::memmove(ls + d0, ls + s0, n);
		}
		else
		{
			for (u32 i = 1; i <= n; i++)
				ls[(dst - i) & ls_mask] = ls[(src - i) & ls_mask];
		}

		gpr[6] = add_imm(gpr[6], 0u - n);
		gpr[12] = add_imm(gpr[12], 0u - n);
		for (int k = 0; k < 4; k++) set_word(gpr[7], k, n);
		for (int k = 0; k < 4; k++) set_word(gpr[2], k, word(gpr[13], k) == n ? 0xffffffffu : 0u);
		reg r8 = load(ls, last_src);
		if ((last_src & 0x3fff0) == (last_dst & 0x3fff0))
			set_byte(r8, last_dst & 15, before_last);
		gpr[8] = r8;
		gpr[5] = cbd(last_dst);
		gpr[3] = load(ls, last_dst);
		gpr[4] = rotqby(r8, word(gpr[6], 0) + 13);
		return exit_pc;
	}

	struct program
	{
		const char* name;
		u32 entry;     // local store address of the entry
		u32 words;     // the code's length from the entry, in words
		u64 hash;      // FNV-1a (64-bit) over those words' bytes as the local store holds them
		u32 (*run)(reg* gpr, u8* ls);
	};

	inline u64 code_hash(const u8* code, u32 bytes)
	{
		u64 h = 0xcbf29ce484222325ull;
		for (u32 i = 0; i < bytes; i++)
			h = (h ^ code[i]) * 0x100000001b3ull;
		return h;
	}

	inline const program table[] =
	{
		{"backward byte copy", 0x17e00, 20, 0x3bb1923a7620edaeull, backward_byte_copy},
	};
}

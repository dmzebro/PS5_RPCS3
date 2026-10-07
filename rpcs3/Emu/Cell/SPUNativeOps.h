// SPU register and local store helpers for native SPU programs (SPUNative.h), in RPCS3's layout:
// a register is 16 bytes, the SPU's byte b at host byte 15 - b (word slot k at host word 3 - k);
// the local store holds the SPU's bytes in order.
#pragma once
#include <cstdint>
#include <cstring>

namespace spu_native_ops
{
	using u8 = std::uint8_t; using u32 = std::uint32_t; using u64 = std::uint64_t;
	constexpr u32 ls_mask = 0x3ffff;

	struct reg { u8 b[16]; };

	inline u32 word(const reg& r, int slot) { u32 v; std::memcpy(&v, r.b + 4 * (3 - slot), 4); return v; }
	inline void set_word(reg& r, int slot, u32 v) { std::memcpy(r.b + 4 * (3 - slot), &v, 4); }
	inline u8 byte(const reg& r, int b) { return r.b[15 - b]; }
	inline void set_byte(reg& r, int b, u8 v) { r.b[15 - b] = v; }

	// lqd/lqx: the quadword at addr & 0x3fff0
	inline reg load(const u8* ls, u32 addr)
	{
		reg r; const u8* p = ls + (addr & 0x3fff0);
		for (int i = 0; i < 16; i++) r.b[15 - i] = p[i];
		return r;
	}

	inline void store(u8* ls, u32 addr, const reg& r)
	{
		u8* p = ls + (addr & 0x3fff0);
		for (int i = 0; i < 16; i++) p[i] = r.b[15 - i];
	}

	// rotqby: rotate left by (n & 15) bytes
	inline reg rotqby(const reg& a, u32 n)
	{
		reg r; n &= 15;
		for (int i = 0; i < 16; i++) set_byte(r, i, byte(a, (i + n) & 15));
		return r;
	}

	// cbd: the insertion control for a byte at (addr & 15)
	inline reg cbd(u32 addr)
	{
		reg r;
		for (int i = 0; i < 16; i++) set_byte(r, i, u8(0x10 + i));
		set_byte(r, addr & 15, 0x03);
		return r;
	}

	// shufb with the controls' special bytes
	inline reg shufb(const reg& a, const reg& b, const reg& c)
	{
		reg r;
		for (int i = 0; i < 16; i++)
		{
			const u8 x = byte(c, i);
			if (x & 0x80) set_byte(r, i, (x & 0x40) ? ((x & 0x20) ? 0x80 : 0xff) : 0x00);
			else set_byte(r, i, (x & 0x10) ? byte(b, x & 15) : byte(a, x & 15));
		}
		return r;
	}

	inline reg add_words(const reg& a, const reg& b)
	{
		reg r; for (int k = 0; k < 4; k++) set_word(r, k, word(a, k) + word(b, k)); return r;
	}

	inline reg add_imm(const reg& a, u32 imm)
	{
		reg r; for (int k = 0; k < 4; k++) set_word(r, k, word(a, k) + imm); return r;
	}
}

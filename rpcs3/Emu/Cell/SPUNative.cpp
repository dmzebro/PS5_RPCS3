#include "stdafx.h"
#include "SPUNative.h"

#include "SPUThread.h"
#include "SPUNativePrograms.h"
#include "Utilities/File.h"
#include "Emu/system_config.h"

#include <charconv>
#include <map>
#include <mutex>

#if defined(__SCE__)
#include <sys/stat.h>
#endif

namespace spu_native
{
	namespace
	{
		// The capture's record: this header, the 128 registers, the local store
		struct record_header
		{
			char magic[4];     // "SPCP"
			u32 kind;          // 0: the program's entry, 1: the state it left
			u32 pc;
			u32 thread;        // the SPU thread's lv2 id
			u64 program;       // the chunk names' hash (as a number)
			u64 call;          // the pair's number, over all programs
			u32 entry;         // the program's entry point
			u32 reserved[7];
		};

		static_assert(sizeof(record_header) == 64);

		struct capture_state
		{
			std::mutex mutex;
			std::map<std::string, u32, std::less<>> wanted; // hash -> calls left
			fs::file file;
			u64 calls = 0;
		};

		capture_state& state()
		{
			static capture_state s_state;
			[[maybe_unused]] static const bool s_loaded = []()
			{
#if defined(__SCE__)
				std::string text;
				if (fs::file list{"/app0/spu-capture.txt"})
					text = list.to_string();

				for (std::string_view rest = text; !rest.empty();)
				{
					const usz eol = rest.find('\n');
					std::string_view line = rest.substr(0, eol);
					rest = eol == umax ? std::string_view{} : rest.substr(eol + 1);

					while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
						line.remove_suffix(1);

					if (line.empty() || line[0] == '#')
						continue;

					const usz space = line.find(' ');
					u32 count = 8;

					if (space != umax)
					{
						const auto digits = line.substr(space + 1);
						std::from_chars(digits.data(), digits.data() + digits.size(), count);
						line = line.substr(0, space);
					}

					s_state.wanted.emplace(std::string(line), count);
				}
#endif
				return true;
			}();

			return s_state;
		}

		void write(capture_state& s, spu_thread* spu, u32 kind, u32 pc, u64 program, u64 call, u32 entry)
		{
			if (!s.file)
			{
				s.file.open("/app0/spu-capture.bin", fs::rewrite);
#if defined(__SCE__)
				::chmod("/app0/spu-capture.bin", 0666);
#endif
			}

			record_header head{};
			std::memcpy(head.magic, "SPCP", 4);
			head.kind = kind;
			head.pc = pc;
			head.thread = spu->lv2_id;
			head.program = program;
			head.call = call;
			head.entry = entry;
			s.file.write(&head, sizeof(head));
			s.file.write(spu->gpr.data(), sizeof(spu->gpr));
			s.file.write(spu->_ptr<u8>(0), SPU_LS_SIZE);
		}
	}

	bool capture_enabled()
	{
#if defined(__SCE__)
		static const bool s_on = fs::is_file("/app0/spu-capture.txt");
		return s_on;
#else
		return false;
#endif
	}

	bool capture_wanted(std::string_view hash)
	{
		if (!capture_enabled())
			return false;

		return state().wanted.contains(hash);
	}

	namespace
	{
		// What a native program left, to compare with what the recompiled one leaves
		struct expected_state
		{
			const spu_native_programs::program* program = nullptr;
			u32 pc = 0;
			u32 entry_pc = 0;
			std::array<v128, 128> gpr{};
			std::vector<u8> ls = std::vector<u8>(SPU_LS_SIZE);
		};

		struct check_stats
		{
			std::mutex mutex;
			std::map<u32, std::unique_ptr<expected_state>> expected; // by SPU thread id
			std::map<std::string, std::pair<u64, u64>, std::less<>> results; // same, different
			u64 logged = 0;
		};

		check_stats& checks()
		{
			static check_stats s_checks;
			return s_checks;
		}

		void compare(spu_thread* spu, u32 pc)
		{
			auto& c = checks();
			std::lock_guard lock(c.mutex);
			const auto found = c.expected.find(spu->lv2_id);

			if (found == c.expected.end() || !found->second->program)
				return;

			auto& e = *found->second;
			const bool same_pc = e.pc == pc;
			const bool same_gpr = std::memcmp(e.gpr.data(), spu->gpr.data(), sizeof(e.gpr)) == 0;
			const bool same_ls = std::memcmp(e.ls.data(), spu->_ptr<u8>(0), SPU_LS_SIZE) == 0;
			auto& result = c.results[e.program->name];
			(same_pc && same_gpr && same_ls ? result.first : result.second)++;

			if (!(same_pc && same_gpr && same_ls) && c.logged++ < 32)
			{
				std::string regs;

				for (u32 i = 0; i < 128; i++)
				{
					if (std::memcmp(&e.gpr[i], &spu->gpr[i], sizeof(v128)))
						fmt::append(regs, " $%u", i);
				}

				u32 first_ls = umax;

				for (u32 a = 0; a < SPU_LS_SIZE && !same_ls; a++)
				{
					if (e.ls[a] != spu->_ptr<u8>(0)[a])
					{
						first_ls = a;
						break;
					}
				}

				std::fprintf(stderr, "rpcs3 spu native: %s differs (entry 0x%05x): pc 0x%05x against 0x%05x, registers%s, local store from 0x%05x\n",
					e.program->name, e.entry_pc, e.pc, pc, regs.empty() ? " the same" : regs.c_str(), first_ls);
			}

			const u64 checked = result.first + result.second;

			if (checked == 1 || checked == 10 || checked == 100 || checked % 1000 == 0)
			{
				std::fprintf(stderr, "rpcs3 spu native: %s checked %llu times, %llu different\n", e.program->name,
					static_cast<unsigned long long>(result.first + result.second), static_cast<unsigned long long>(result.second));
			}

			e.program = nullptr;
		}
	}

	native_mode mode()
	{
#if defined(__SCE__)
		static const native_mode s_mode = []()
		{
			std::string text;
			if (fs::file f{"/app0/spu-native.txt"})
				text = f.to_string();
			if (text.starts_with("run"))
				return native_mode::run;
			if (text.starts_with("check"))
				return native_mode::check;
			return native_mode::off;
		}();
		return s_mode;
#else
		return native_mode::off;
#endif
	}

	s32 find(u32 entry, const u8* code, u32 bytes)
	{
		if (mode() == native_mode::off)
			return -1;

		for (u32 i = 0; i < std::size(spu_native_programs::table); i++)
		{
			const auto& p = spu_native_programs::table[i];

			if (p.entry == entry && p.words * 4 <= bytes && spu_native_programs::code_hash(code, p.words * 4) == p.hash)
			{
				std::fprintf(stderr, "rpcs3 spu native: %s found at 0x%05x (%s)\n", p.name, entry, mode() == native_mode::run ? "runs" : "checked");
				return static_cast<s32>(i);
			}
		}

		return -1;
	}

	u32 run(spu_thread* spu, u32 pc, u32 index)
	{
		const auto& p = spu_native_programs::table[index];
		u8* const ls = spu->_ptr<u8>(0);

		// The code as compiled: the program's entry checks it (SPU verification);
		// without that check, the native part is checked here
		if (pc != p.entry || (!g_cfg.core.spu_verification && spu_native_programs::code_hash(ls + p.entry, p.words * 4) != p.hash))
			return 0;

		if (mode() == native_mode::run)
		{
			return p.run(reinterpret_cast<spu_native_ops::reg*>(spu->gpr.data()), ls);
		}

		// Check: the native version on a copy, compared at the next chunk entry
		auto& c = checks();
		std::lock_guard lock(c.mutex);
		auto& e = c.expected[spu->lv2_id];

		if (!e)
			e = std::make_unique<expected_state>();

		e->gpr = spu->gpr;
		std::memcpy(e->ls.data(), ls, SPU_LS_SIZE);
		e->pc = p.run(reinterpret_cast<spu_native_ops::reg*>(e->gpr.data()), e->ls.data());

		if (!e->pc)
			return 0;

		e->program = &p;
		e->entry_pc = pc;
		spu->native_capture |= 2;
		return 0;
	}

	void capture_chunk(spu_thread* spu, u32 pc, u64 program, u32 entry, u32 lower, u32 end)
	{
		if (spu->native_capture & 2)
		{
			// A native program's result to compare
			spu->native_capture &= ~2;
			compare(spu, pc);
		}

		if (!capture_enabled())
			return;

		auto& s = state();
		std::lock_guard lock(s.mutex);

		if (spu->native_capture)
		{
			// The state the program left, unless the thread came back into it
			// (resumed after a stop, not a call it made)
			const bool inside = pc >= spu->native_capture_lower && pc < spu->native_capture_end && pc != spu->native_capture_entry;

			if (!inside)
			{
				write(s, spu, 1, pc, spu->native_capture_program, spu->native_capture_call, spu->native_capture_entry);
			}

			spu->native_capture = 0;
		}

		if (!program || pc != entry)
			return;

		const std::string name = fmt::format("%s", fmt::base57(be_t<u64>{program}));
		const auto found = s.wanted.find(name);

		if (found == s.wanted.end() || !found->second)
			return;

		found->second--;
		write(s, spu, 0, pc, program, s.calls, entry);
		spu->native_capture = 1;
		spu->native_capture_program = program;
		spu->native_capture_call = s.calls++;
		spu->native_capture_entry = entry;
		spu->native_capture_lower = lower;
		spu->native_capture_end = end;
	}
}

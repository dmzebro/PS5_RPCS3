#include "stdafx.h"
#include "timeline.h"

#include "Utilities/File.h"
#include "Utilities/Thread.h"
#include "util/sysinfo.hpp"

#include <chrono>
#include <mutex>
#include <vector>

#ifdef __SCE__
#include <sys/stat.h>
#endif

namespace timeline
{
	atomic_t<u32> g_on{0};

	bool counters_file()
	{
		return fs::is_file("/app0/counters.txt");
	}

	namespace
	{
		struct event
		{
			u64 start;
			u32 duration;
			u16 kind;
			u16 reserved;
			u32 arg;
			u32 pc;
			u32 tag;
			u32 reserved2;
		};

		struct buffer
		{
			std::string name;
			std::vector<event> events;
			usz count = 0;
		};

		constexpr usz s_capacity = 1 << 17;
		constexpr u64 s_min_ticks_div = 1'000'000; // one microsecond

		std::mutex s_mutex;
		std::vector<buffer*> s_buffers;
		thread_local buffer* t_buffer = nullptr;
		u64 s_min_ticks = 0;
	}

	void record(u16 kind, u32 arg, u32 pc, u64 start, u64 end, u32 tag)
	{
		if (end - start < s_min_ticks && kind != frame && kind != rsx_flip)
		{
			return;
		}

		buffer* buf = t_buffer;

		if (!buf) [[unlikely]]
		{
			buf = new buffer;
			buf->name = thread_ctrl::get_name();
			buf->events.resize(s_capacity);
			std::lock_guard lock(s_mutex);
			s_buffers.push_back(buf);
			t_buffer = buf;
		}

		if (buf->count < s_capacity)
		{
			buf->events[buf->count++] = event{start, static_cast<u32>(std::min<u64>(end - start, u32{umax})), kind, 0, arg, pc, tag, 0};
		}
	}

	void poll()
	{
		static int s_state = -1; // -1 unread, 0 off, 1 waiting, 2 recording, 3 stopping, 4 done
		static double s_start_s = 0, s_duration_ms = 0;
		static std::chrono::steady_clock::time_point s_t0;
		static u64 s_tsc_start = 0, s_tsc_end = 0;

		if (s_state == -1)
		{
			s_state = 0;
			fs::file f("/app0/timeline.txt");
			if (f)
			{
				const std::string text = f.to_string();
				if (std::sscanf(text.c_str(), "%lf %lf", &s_start_s, &s_duration_ms) == 2)
				{
					s_state = 1;
					s_t0 = std::chrono::steady_clock::now();
					s_min_ticks = utils::get_tsc_freq() / s_min_ticks_div;
					std::fprintf(stderr, "rpcs3 timeline: from %.1f s for %.0f ms\n", s_start_s, s_duration_ms);
				}
			}
		}

		if (s_state <= 0 || s_state == 4)
		{
			return;
		}

		const double now_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_t0).count();

		if (s_state == 1 && now_s >= s_start_s)
		{
			s_tsc_start = utils::get_tsc();
			g_on = 1;
			s_state = 2;
			std::fprintf(stderr, "rpcs3 timeline: recording\n");
		}
		else if (s_state == 2 && now_s >= s_start_s + s_duration_ms / 1000.0)
		{
			g_on = 0;
			s_tsc_end = utils::get_tsc();
			s_state = 3;
		}
		else if (s_state == 3)
		{
			// A frame later: the records in flight are done
			s_state = 4;
			std::lock_guard lock(s_mutex);
			fs::file out("/app0/timeline.bin", fs::rewrite);
			if (!out)
			{
				std::fprintf(stderr, "rpcs3 timeline: cannot write\n");
				return;
			}
			// "RPCS3TL2": 32-byte events with a tag
			const u64 header[4]{0x324c545233435052ull, utils::get_tsc_freq(), s_tsc_start, s_tsc_end};
			out.write(header, sizeof(header));
			const u32 n = static_cast<u32>(s_buffers.size());
			out.write(&n, sizeof(n));
			usz total = 0;
			for (const buffer* buf : s_buffers)
			{
				char name[64]{};
				std::memcpy(name, buf->name.data(), std::min<usz>(buf->name.size(), 63));
				out.write(name, sizeof(name));
				const u32 count = static_cast<u32>(buf->count);
				out.write(&count, sizeof(count));
				out.write(buf->events.data(), buf->count * sizeof(event));
				total += buf->count;
			}
			out.close();
#ifdef __SCE__
			::chmod("/app0/timeline.bin", 0666);
#endif
			std::fprintf(stderr, "rpcs3 timeline: written, %u threads, %zu events\n", n, total);
		}
	}
}

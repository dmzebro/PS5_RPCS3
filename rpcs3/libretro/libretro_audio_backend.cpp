#include "stdafx.h"
#include "libretro_audio_backend.h"

#include <mutex>
#include <vector>

LOG_CHANNEL(libretro_audio, "LibretroAudio");

namespace
{
	// The backend cellAudio has open; RetroArch's thread takes from it.
	std::mutex g_live_lock;
	libretro_audio_backend* g_live = nullptr;
}

libretro_audio_backend::libretro_audio_backend()
{
}

libretro_audio_backend::~libretro_audio_backend()
{
	Close();
}

bool libretro_audio_backend::Open(std::string_view /*dev_id*/, AudioFreq freq, AudioSampleSize sample_size, AudioChannelCnt ch_cnt, audio_channel_layout layout)
{
	Close();
	std::lock_guard lock(g_live_lock);

	if (freq != AudioFreq::FREQ_48K)
	{
		// RetroArch was told 48 kHz (retro_get_system_av_info).
		libretro_audio.error("cellAudio opened at %u Hz; RetroArch plays 48000", static_cast<u32>(freq));
	}

	m_sampling_rate = freq;
	m_sample_size = sample_size;
	// RetroArch plays stereo: RPCS3 downmixes to it.
	setup_channel_layout(static_cast<u32>(ch_cnt), 2, layout, libretro_audio);
	g_live = this;
	return true;
}

void libretro_audio_backend::Close()
{
	std::lock_guard lock(g_live_lock);
	m_playing = false;
	if (g_live == this)
	{
		g_live = nullptr;
	}
}

void libretro_audio_backend::Play()
{
	std::lock_guard lock(g_live_lock);
	m_playing = true;
}

void libretro_audio_backend::Pause()
{
	std::lock_guard lock(g_live_lock);
	m_playing = false;
}

void libretro_audio_backend::render(u32 frames, retro_audio_sample_batch_t batch)
{
	thread_local std::vector<u8> raw;
	thread_local std::vector<s16> out;

	u32 produced = 0;
	{
		std::lock_guard lock(g_live_lock);
		libretro_audio_backend* const backend = g_live;
		if (!backend || !backend->m_playing || !frames)
		{
			return;
		}

		std::lock_guard cb_lock(backend->m_cb_mutex);
		if (!backend->m_write_callback)
		{
			return;
		}

		const u32 frame_bytes = backend->get_channels() * backend->get_sample_size();
		raw.resize(usz{frames} * frame_bytes);
		const u32 written = std::min(backend->m_write_callback(::size32(raw), raw.data()), ::size32(raw));
		produced = written / frame_bytes;
		out.resize(usz{produced} * 2);

		// Stereo 16-bit: RetroArch's format.
		const u32 channels = backend->get_channels();
		for (u32 frame = 0; frame < produced; frame++)
		{
			for (u32 side = 0; side < 2; side++)
			{
				const u32 index = frame * channels + std::min(side, channels - 1);
				if (backend->get_convert_to_s16())
				{
					out[frame * 2 + side] = reinterpret_cast<const s16*>(raw.data())[index];
				}
				else
				{
					const f32 sample = reinterpret_cast<const f32*>(raw.data())[index];
					out[frame * 2 + side] = static_cast<s16>(std::clamp(sample * 32768.5f, -32768.0f, 32767.0f));
				}
			}
		}
	}

	for (u32 done = 0; done < produced;)
	{
		const usz taken = batch(out.data() + usz{done} * 2, produced - done);
		if (!taken)
		{
			break;
		}
		done += static_cast<u32>(taken);
	}
}

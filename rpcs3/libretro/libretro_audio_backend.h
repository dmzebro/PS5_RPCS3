#pragma once

// cellAudio's output in the libretro core: RetroArch's thread takes it, in
// retro_run, at the rate real time passes, and hands RetroArch what RPCS3 has
// produced, no more. What the game did not produce in time is not padded, so
// RetroArch's own silence counts it.

#include "Emu/Audio/AudioBackend.h"

#include "libretro.h"

class libretro_audio_backend final : public AudioBackend
{
public:
	libretro_audio_backend();
	~libretro_audio_backend() override;

	std::string_view GetName() const override { return "libretro"sv; }

	bool Open(std::string_view dev_id, AudioFreq freq, AudioSampleSize sample_size, AudioChannelCnt ch_cnt, audio_channel_layout layout) override;
	void Close() override;

	// The span one retro_run takes, at 60 frames a second.
	f64 GetCallbackFrameLen() override { return 1.0 / 60.0; }

	void Play() override;
	void Pause() override;

	// On RetroArch's thread: up to `frames` stereo frames from the live backend.
	static void render(u32 frames, retro_audio_sample_batch_t batch);
};

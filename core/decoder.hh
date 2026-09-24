#pragma once

// Decodes a music file to 16-bit stereo PCM at its own sample rate (Wwise resamples to 48 kHz itself).
//
// MP3, FLAC, WAV and Ogg Vorbis go through decoders compiled into the .asi (dr_mp3, dr_flac, dr_wav,
// stb_vorbis), so they behave identically on Windows and under Wine/Proton/CrossOver, and give exact
// lengths. Anything else (M4A/AAC, WMA, ALAC, Opus...) falls back to Media Foundation (decoder_mf.cc):
// fine on Windows, dependent on the Wine build elsewhere. The log names the backend of every track.

#include <cstdint>
#include <functional>
#include <string>

#include "tags.hh"

namespace decoder
{
	constexpr uint32_t kChannels = 2;
	constexpr uint32_t kBlockAlign = kChannels * sizeof(int16_t);

	struct Info
	{
		uint32_t mSampleRate = 0;
		uint64_t mFrames = 0;     // exact for the bundled decoders, from the container's duration for MF
		tags::Tags mTags;         // whatever the file carries; empty fields if nothing
		const char* mBackend = ""; // "dr_mp3", "dr_flac", "dr_wav", "stb_vorbis", "Media Foundation"
	};

	// Reads format, length and tags without decoding the audio. False if no backend can open the file.
	bool Probe(const std::wstring& path, Info& out);

	// Decodes from the start. `sink` gets interleaved stereo frames and returns false to stop early. `rate`
	// receives the sample rate before the first frame. False on errors (what was delivered stays valid).
	// Media Foundation fallback needs COM (MTA) on the calling thread.
	bool Decode(const std::wstring& path, uint32_t& rate, const std::function<bool(const int16_t* frames, size_t count)>& sink);

	// Folds `channels`-channel interleaved frames (WAVEFORMATEXTENSIBLE order) into stereo.
	void ToStereo(const int16_t* in, uint32_t channels, size_t frames, int16_t* out);

	namespace mf
	{
		bool Startup(); // MFStartup once; false where Media Foundation is missing
		bool Probe(const std::wstring& path, Info& out);
		bool Decode(const std::wstring& path, uint32_t& rate, const std::function<bool(const int16_t*, size_t)>& sink);
	}
}

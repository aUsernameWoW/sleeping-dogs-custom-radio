#pragma once

// Decodes a music file to 16-bit PCM at its own sample rate (Wwise resamples to 48 kHz itself) and in its own
// channel layout where Wwise maps it (layout.hh), else folded to stereo.
//
// MP3, FLAC, WAV and Ogg Vorbis go through decoders compiled into the .asi (dr_mp3, dr_flac, dr_wav,
// stb_vorbis), so they behave identically on Windows and under Wine/Proton/CrossOver, and give exact
// lengths. Anything else (M4A/AAC, WMA, ALAC, Opus...) falls back to Media Foundation (decoder_mf.cc):
// fine on Windows, dependent on the Wine build elsewhere. The log names the backend of every track.

#include <cstdint>
#include <functional>
#include <string>

#include "layout.hh"
#include "tags.hh"

namespace decoder
{
	struct Info
	{
		uint32_t mSampleRate = 0;
		uint64_t mFrames = 0;     // exact for the bundled decoders, from the container's duration for MF
		tags::Tags mTags;         // whatever the file carries; empty fields if nothing
		const char* mBackend = ""; // "dr_mp3", "dr_flac", "dr_wav", "stb_vorbis", "Media Foundation"
		layout::Layout mSource;   // as the decoder delivers it
		layout::Layout mLayout;   // what the stream carries: layout::ForWwise(mSource)
	};

	// Reads format, length and tags without decoding the audio. False if no backend can open the file.
	bool Probe(const std::wstring& path, Info& out);

	// Decodes from the start. `sink` gets interleaved frames in layout `want` (the scan's mLayout: whatever the
	// decoder delivers now is converted to it, since the stream's header is already out) and returns false to
	// stop early. `rate` receives the sample rate before the first frame. False on errors (what was delivered
	// stays valid). Media Foundation fallback needs COM (MTA) on the calling thread.
	using Sink = std::function<bool(const int16_t* frames, size_t count)>;
	bool Decode(const std::wstring& path, const layout::Layout& want, uint32_t& rate, const Sink& sink);

	namespace mf
	{
		bool Startup(); // MFStartup once; false where Media Foundation is missing
		bool Probe(const std::wstring& path, Info& out);
		// Frames in the layout the reader delivers (`got`, known before the first frame).
		bool Decode(const std::wstring& path, uint32_t& rate, layout::Layout& got, const Sink& sink);
	}
}

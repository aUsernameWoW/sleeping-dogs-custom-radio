#pragma once

// Decoding a never-ending stream pushed in pieces as it arrives from the network (decoder.hh decodes whole
// files): MP3 through dr_mp3's low-level frame decoder, Ogg Vorbis through stb_vorbis's pushdata API, both
// bundled, so it works the same under Wine. Output is 16-bit stereo at the stream's rate; Resampler brings
// it to the live station's 48 kHz.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "icy.hh"

namespace streamdec
{
	// Interleaved stereo frames at `rate`.
	using Sink = std::function<void(const int16_t* frames, size_t count, uint32_t rate)>;

	class Decoder
	{
	public:
		virtual ~Decoder() = default;
		// Decodes whatever complete frames `data` (appended to what came before) makes available. False if the
		// stream can't be decoded at all (it isn't this format).
		virtual bool Push(const uint8_t* data, size_t size, const Sink& sink) = 0;
		// "Artist - Title" from the stream itself (Vorbis comments), when it changed since the last call.
		virtual bool TakeTitle(std::string& title) { (void)title; return false; }
	};

	// MP3 or Ogg Vorbis; nullptr for formats not decoded here.
	std::unique_ptr<Decoder> Create(icy::Format format);

	// Linear-interpolating resampler to 48 kHz, stereo in and out.
	class Resampler
	{
	public:
		void Process(const int16_t* frames, size_t count, uint32_t rate, std::vector<int16_t>& out);

	private:
		uint32_t mRate = 0;
		bool mHavePrevious = false;
		int16_t mPrevious[2] = {};
		double mPosition = 0.0; // of the next output frame, in input frames after mPrevious
	};
}

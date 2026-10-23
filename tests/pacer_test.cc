// Internet radio pacing (core/pacer.cc) and resampling (core/stream_decoder.cc): the FIFO waits for kStartMs,
// then releases audio by the clock, rebuffers after an underrun, and releases titles only once their audio is
// out; the resampler keeps the rate and continuity across chunks; the decoders refuse junk. Compiles them with
// dr_mp3 and stb_vorbis by relative path; argv[1] is unused.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
// The libraries first: stream_decoder.cc names them as <dr_mp3.h> / <stb_vorbis.c>, which (as in logo_test)
// then resolve to the same, already included files although tests get no include paths. stb_vorbis's
// implementation goes at the very end: it defines one-letter macros that break anything after it.
#define DR_MP3_IMPLEMENTATION
#include "../../../reference/dr_libs/dr_mp3.h"
#define STB_VORBIS_HEADER_ONLY
#include "../../../reference/stb/stb_vorbis.c"
#include "../core/pacer.cc"
#include "../core/stream_decoder.cc"
#include "../core/tags.cc"

#include <cmath>
#include <cstdio>

namespace
{
	int gFailures = 0;

	void Check(bool ok, const char* what)
	{
		if (!ok) {
			std::printf("FAIL: %s\n", what);
			++gFailures;
		}
	}

	void AddMs(netradio::Pacer& pacer, uint32_t ms)
	{
		std::vector<int16_t> samples(static_cast<size_t>(ms) * live::kRate / 1000 * 2, 1);
		pacer.Add(samples.data(), samples.size());
	}

	void TestPacer()
	{
		netradio::Pacer pacer;
		std::vector<int16_t> out;
		const size_t perMs = live::kRate / 1000 * 2;
		AddMs(pacer, netradio::kStartMs - 100);
		Check(pacer.Take(1000, out) == 0 && !pacer.Playing(), "waits until kStartMs are buffered");
		std::string title;
		Check(!pacer.TakeTitle(title), "no title yet");
		pacer.Mark("Second");       // from 2900 ms of audio on
		AddMs(pacer, 3000);         // 5900 ms buffered

		Check(pacer.Take(2000, out) == (5900 - netradio::kRebufferMs) * perMs && pacer.Playing(), "hands over all but kRebufferMs at once");
		static_assert(5900 - netradio::kRebufferMs <= live::kPrerollMs, "up to the live preroll");
		Check(pacer.TakeTitle(title) && title == "Second", "the title comes with its audio (it was in the head start)");
		Check(!pacer.TakeTitle(title), "a title is reported once");
		out.clear();
		Check(pacer.Take(2500, out) == 500 * perMs, "then releases by the clock: 500 ms after 500 ms");
		out.clear();
		Check(pacer.Take(2500, out) == 0, "nothing more until time passes");

		pacer.Take(10'000, out);    // more due than there is
		Check(!pacer.Playing() && pacer.Underruns() == 1, "an underrun stops it");
		pacer.Mark("Third");        // from the audio after the underrun on
		Check(!pacer.TakeTitle(title), "a title waits for its audio");
		AddMs(pacer, netradio::kRebufferMs - 10);
		out.clear();
		Check(pacer.Take(11'000, out) == 0, "rebuffering waits for kRebufferMs");
		AddMs(pacer, 20);
		Check(pacer.Take(11'100, out) == 10 * perMs && pacer.Playing(), "then hands over what it has beyond kRebufferMs");
		Check(pacer.TakeTitle(title) && title == "Third", "and the title with it");
		AddMs(pacer, 500);
		out.clear();
		Check(pacer.Take(11'200, out) == 100 * perMs, "with a fresh clock");
	}

	void TestResampler()
	{
		// 1 s of 44.1 kHz in chunks of 441 frames: ~48000 frames out, a continuous sine.
		streamdec::Resampler resampler;
		std::vector<int16_t> out;
		std::vector<int16_t> chunk(441 * 2);
		for (int c = 0; c < 100; ++c) {
			for (int i = 0; i < 441; ++i) {
				const double t = (c * 441 + i) / 44100.0;
				chunk[i * 2] = chunk[i * 2 + 1] = static_cast<int16_t>(10000 * std::sin(2 * 3.14159265358979 * 440 * t));
			}
			resampler.Process(chunk.data(), 441, 44100, out);
		}
		const size_t frames = out.size() / 2;
		Check(frames >= 47990 && frames <= 48000, "44.1 kHz -> 48 kHz frame count");
		double worst = 0;
		for (size_t i = 0; i < frames; ++i) {
			const double expected = 10000 * std::sin(2 * 3.14159265358979 * 440 * i / 48000.0);
			worst = (std::max)(worst, std::abs(out[i * 2] - expected));
		}
		Check(worst < 200, "the resampled sine stays on the curve (no jumps at chunk joins)");

		out.clear();
		resampler.Process(chunk.data(), 441, 48000, out);
		Check(out.size() == 441 * 2 && out[2] == chunk[2], "48 kHz passes through");
	}

	void TestJunk()
	{
		std::vector<uint8_t> junk(300 * 1024);
		for (size_t i = 0; i < junk.size(); ++i) junk[i] = static_cast<uint8_t>(i * 7 + 3);
		size_t frames = 0;
		auto sink = [&](const int16_t*, size_t count, uint32_t) { frames += count; };
		auto mp3 = streamdec::Create(icy::Format::Mp3);
		bool ok = true;
		for (size_t at = 0; at < junk.size(); at += 4096) ok = mp3->Push(junk.data() + at, 4096, sink);
		Check(!ok && frames == 0, "MP3: junk is refused");
		auto ogg = streamdec::Create(icy::Format::Ogg);
		for (size_t at = 0; at < junk.size(); at += 4096) ok = ogg->Push(junk.data() + at, 4096, sink);
		Check(!ok && frames == 0, "Ogg: junk is refused");
		Check(!streamdec::Create(icy::Format::Aac), "no AAC decoder");
	}
}

int main()
{
	TestPacer();
	TestResampler();
	TestJunk();
	if (gFailures) {
		return 1;
	}
	std::printf("PASS\n");
	return 0;
}

#undef STB_VORBIS_HEADER_ONLY
#include "../../../reference/stb/stb_vorbis.c"

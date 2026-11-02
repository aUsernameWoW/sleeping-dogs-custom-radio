// The live station's ring and reader (core/live.cc): wrapping, preroll, reads in order, the header, a late source
// padded with silence at once and rebuffered without losing audio, jumping ahead after a stall, seeks keeping the
// frame phase, starting with a fresh or a running source. Compiles live.cc directly; argv[1] is unused.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "../core/live.cc"
#include "../core/log.cc"

#include <cstdio>
#include <vector>

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

	// Pushes `ms` of audio whose every sample is its own index (mod 2^15), so any byte tells where it came from.
	void PushMs(live::Ring& ring, uint32_t ms, uint32_t& counter)
	{
		std::vector<int16_t> samples(static_cast<size_t>(live::MsToBytes(ms) / 2));
		for (int16_t& s : samples) {
			s = static_cast<int16_t>(counter++ & 0x7FFF);
		}
		ring.Push(samples.data(), samples.size());
	}

	// The ring byte offset a sample value came from (value = (offset / 2) mod 2^15).
	bool FromOffset(const uint8_t* bytes, uint64_t offset)
	{
		int16_t v;
		std::memcpy(&v, bytes, 2);
		return static_cast<uint64_t>(v) == ((offset / 2) & 0x7FFF);
	}

	bool AllZero(const uint8_t* bytes, size_t size)
	{
		for (size_t i = 0; i < size; ++i) {
			if (bytes[i]) return false;
		}
		return true;
	}

	void TestHudText()
	{
		live::SetNowPlaying("Artist - Title");
		const uint32_t before = live::NowPlayingGeneration();
		live::SetWarning("cloudmusic.exe is muted in the volume mixer");
		Check(live::HudText() == "cloudmusic.exe is muted in the volume mixer", "a warning takes the title's place");
		Check(live::NowPlayingGeneration() != before, "and has the HUD refresh");
		Check(live::NowPlaying() == "Artist - Title", "the title stays as it was");
		const uint32_t warned = live::NowPlayingGeneration();
		live::SetWarning("cloudmusic.exe is muted in the volume mixer");
		Check(live::NowPlayingGeneration() == warned, "the same warning again changes nothing");
		live::SetWarning({});
		Check(live::HudText() == "Artist - Title" && live::NowPlayingGeneration() != warned, "cleared, the title is back");
		live::SetNowPlaying({});
	}

	void TestRing()
	{
		live::Ring ring(1000);
		std::vector<int16_t> samples(300);
		for (size_t i = 0; i < samples.size(); ++i) samples[i] = static_cast<int16_t>(i);
		ring.Push(samples.data(), 300); // 600 bytes
		ring.Push(samples.data(), 300); // 1200: wraps, the first 200 bytes are gone
		Check(ring.Written() == 1200, "ring counts written bytes");
		uint8_t out[400];
		Check(!ring.Copy(100, out, 100), "overwritten bytes can't be copied");
		Check(!ring.Copy(1100, out, 200), "unwritten bytes can't be copied");
		Check(ring.Copy(500, out, 400), "a range across the wrap copies");
		int16_t first;
		std::memcpy(&first, out, 2);
		Check(first == 250, "copy across the wrap has the right data");

		std::vector<int16_t> big(1000, 7);
		ring.Push(big.data(), big.size()); // more than the capacity at once
		Check(ring.Written() == 3200, "an oversized push still counts every byte");
		Check(ring.Copy(2200, out, 400) && out[0] == 7, "an oversized push keeps the newest bytes");
	}

	void TestReader()
	{
		live::Ring ring;
		uint32_t counter = 0;
		uint8_t header[live::kHeaderSize];
		for (uint32_t i = 0; i < live::kHeaderSize; ++i) header[i] = static_cast<uint8_t>(0xA0 + i);
		live::Reader reader(ring, header);

		std::vector<uint8_t> out(8192);
		reader.Fill(0, 68, out.data());
		Check(out[0] == 0xA0 && out[67] == 0xA0 + 67, "header bytes");

		PushMs(ring, live::kPrerollMs + 1000, counter);
		const uint64_t start = ring.Written() - live::MsToBytes(live::kPrerollMs);
		reader.Fill(68, 4096, out.data());
		Check(FromOffset(out.data(), start), "the stream starts one preroll behind the newest audio");
		Check(FromOffset(out.data() + 4094, start + 4094), "a read is contiguous");
		reader.Fill(68 + 4096, 4096, out.data());
		Check(FromOffset(out.data(), start + 4096), "the next read continues where the last ended");

		// Reading faster than the source: drain everything, then ask for more than there is.
		uint64_t position = 68 + 8192;
		uint64_t cursor = start + 8192;
		while (ring.Written() - cursor >= 4096) {
			reader.Fill(position, 4096, out.data());
			position += 4096;
			cursor += 4096;
		}
		const uint64_t short_by = 4096 - (ring.Written() - cursor);
		reader.Fill(position, 4096, out.data());
		Check(FromOffset(out.data(), cursor), "a read past the newest audio gets what there is at once");
		Check(out[4095] == 0 && out[4094] == 0, "and silence for the rest");
		Check(reader.GetStats().mSilenceBytes == short_by && reader.GetStats().mUnderruns == 1, "padded silence and the underrun are counted");
		position += 4096;
		cursor = ring.Written(); // the cursor only moved over real audio

		// Rebuffering: silence until kResumeMs are there, then on from where the audio stopped.
		PushMs(ring, live::kResumeMs - 100, counter);
		reader.Fill(position, 4096, out.data());
		Check(AllZero(out.data(), 4096), "a rebuffering reader reads silence");
		Check(reader.GetStats().mUnderruns == 1, "one underrun, however long it rebuffers");
		position += 4096;
		PushMs(ring, 100, counter);
		reader.Fill(position, 4096, out.data());
		Check(FromOffset(out.data(), cursor), "after an underrun the stream resumes without a gap in the audio");
		position += 4096;

		// A stall: the reader falls more than kMaxLagMs behind and jumps to the preroll distance.
		PushMs(ring, live::kMaxLagMs + 500, counter);
		reader.Fill(position, 4096, out.data());
		Check(reader.GetStats().mJumps == 1, "a reader too far behind jumps ahead");
		Check(FromOffset(out.data(), ring.Written() - live::MsToBytes(live::kPrerollMs)), "and lands one preroll behind");
		position += 4096;

		// A seek (the game's resume) to a position whose frame phase is 2: the stream follows the phase.
		const uint32_t seeksBefore = reader.GetStats().mSeeks;
		reader.Fill(68 + 1'000'002, 4096, out.data());
		Check(reader.GetStats().mSeeks == seeksBefore + 1, "a read elsewhere counts as a seek");

		// A read straddling the header.
		live::Reader fresh(ring, header);
		fresh.Fill(0, 100, out.data());
		Check(out[0] == 0xA0 && out[67] == 0xA0 + 67, "straddling read starts with the header");
		Check(FromOffset(out.data() + 68, ring.Written() - live::MsToBytes(live::kPrerollMs)), "and continues with audio");
	}

	void TestPhase()
	{
		live::Ring ring;
		uint32_t counter = 0;
		uint8_t header[live::kHeaderSize] = {};
		live::Reader reader(ring, header);
		PushMs(ring, live::kPrerollMs + 1000, counter);
		std::vector<uint8_t> out(64);
		reader.Fill(68 + 6, 16, out.data()); // frame phase 2
		const uint64_t start = ring.Written() - live::MsToBytes(live::kPrerollMs);
		Check(FromOffset(out.data(), start + 2), "the cursor takes the file position's frame phase");
	}

	void TestStart()
	{
		std::vector<uint8_t> out(131072);
		uint8_t header[live::kHeaderSize] = {};

		// A source just started (internet radio: connecting): Wwise's opening burst reads silence at once, and the
		// stream starts with the source's oldest audio once kResumeMs are there.
		{
			live::Ring ring;
			uint32_t counter = 0;
			live::Reader reader(ring, header);
			PushMs(ring, live::kResumeMs - 500, counter);
			reader.Fill(0, 131072, out.data());
			Check(AllZero(out.data() + 68, 131072 - 68), "a source still starting reads as silence");
			Check(reader.GetStats().mUnderruns == 0, "which isn't an underrun");
			PushMs(ring, 500, counter);
			reader.Fill(131072, 131072, out.data());
			Check(FromOffset(out.data(), 0), "then the stream starts at the source's oldest audio");
		}

		// A source already running (switching back to the station): the header read and Wwise's burst of 4 reads
		// (~3.4 s together) come out of the preroll, and enough is left for the next steady read.
		{
			live::Ring ring;
			uint32_t counter = 0;
			live::Reader reader(ring, header);
			PushMs(ring, live::kRingMs, counter);
			for (uint32_t i = 0; i < 5; ++i) {
				reader.Fill(131072u * i, 131072, out.data());
			}
			Check(reader.GetStats().mSilenceBytes == 0, "the opening burst is all audio");
			Check(reader.Lead() >= 131072 + live::MsToBytes(500), "and leaves more than a read plus 0.5 s behind live");
		}
	}
}

namespace
{
	void TestNowPlaying()
	{
		const uint32_t before = live::NowPlayingGeneration();
		live::SetNowPlaying("Artist - Title");
		Check(live::NowPlaying() == "Artist - Title" && live::NowPlayingGeneration() == before + 1, "a new title counts");
		live::SetNowPlaying("Artist - Title");
		Check(live::NowPlayingGeneration() == before + 1, "the same title again doesn't");
		live::SetNowPlaying({});
		Check(live::NowPlaying().empty() && live::NowPlayingGeneration() == before + 2, "clearing it does");
	}

	void TestListening()
	{
		Check(!live::Listening(3000), "nothing open: not listening");
		live::Acquire(); // no source set: nothing starts
		Check(live::Listening(3000), "just opened: listening");
		Sleep(200); // GetTickCount moves in ~16 ms steps
		Check(!live::Listening(50), "open but not read for a while (the game's pause menu): not listening");
		live::NoteRead();
		Check(live::Listening(50), "read again: listening");
		live::Release();
		Check(!live::Listening(3000), "closed: not listening");
	}
}

int main()
{
	TestNowPlaying();
	TestListening();
	static_assert(live::kHeaderSize - 8 + live::kFrames * live::kBlockAlign <= 0xFFFFFFFFull, "RIFF size fits");
	static_assert(live::kMaxLagMs > live::kPrerollMs && live::kMaxLagMs < live::kRingMs, "a jump lands inside the ring and below the lag limit");
	TestStart();
	TestHudText();
	TestRing();
	TestReader();
	TestPhase();

	// The radio's gain in the game (Init.bnk / station bank curves), which a directly heard app follows.
	const auto close = [](float a, float b) { return std::fabs(a - b) < 0.001f; };
	Check(close(live::GameRadioGain(0.0f, 10.0f), 1.0f), "in the car at full radio volume: unity");
	Check(close(live::GameRadioGain(85.0f, 10.0f), 1.0f), "unity up to 85 m");
	Check(close(live::GameRadioGain(92.5f, 10.0f), 1.0f - 0.996f * 0.5f), "half way down the S-curve at 92.5 m");
	Check(live::GameRadioGain(88.0f, 10.0f) > live::GameRadioGain(97.0f, 10.0f), "fading as the player walks away");
	Check(close(live::GameRadioGain(100.0f, 10.0f), 0.004f) && close(live::GameRadioGain(500.0f, 10.0f), 0.004f), "-48 dB from 100 m on");
	Check(close(live::GameRadioGain(0.0f, 5.0f), 0.5f) && close(live::GameRadioGain(0.0f, 0.0f), 0.0f), "the radio volume option, linear");
	Check(close(live::GameRadioGain(0.0f, 12.0f), 1.0f), "the option clamped at 10");
	Check(close(live::RadioGain(), 1.0f), "unity until the hooks say otherwise");
	live::SetRadioDistance(100.0f);
	live::SetRadioVolume(5.0f);
	Check(close(live::RadioGain(), 0.002f), "the hooks' values combine");
	live::SetRadioDistance(0.0f);
	live::SetRadioVolume(10.0f);

	// Getting out: the track's 0.5 s fade × 1.5 s to -96 dB; getting in: 2.5 s up from -96 dB, in dB.
	Check(close(live::RadioStateGain(false, 1.0f, 0), 1.0f), "off: starts where the radio was");
	Check(close(live::RadioStateGain(false, 1.0f, 250), 0.5f * std::pow(10.0f, -96.0f / 6.0f / 20.0f)), "off: a quarter second in");
	Check(live::RadioStateGain(false, 1.0f, 500) == 0.0f && live::RadioStateGain(false, 1.0f, 5'000) == 0.0f, "off: silent after the track's fade");
	Check(live::RadioStateGain(true, 0.0f, 0) == 0.0f, "on: starts silent");
	Check(close(live::RadioStateGain(true, 0.0f, 1'250), std::pow(10.0f, -48.0f / 20.0f)), "on: -48 dB half way");
	Check(close(live::RadioStateGain(true, 0.0f, 2'500), 1.0f) && close(live::RadioStateGain(true, 0.0f, 9'000), 1.0f), "on: full after 2.5 s");
	Check(close(live::RadioStateGain(true, 0.5f, 0), 0.5f) && live::RadioStateGain(true, 0.5f, 1'000) > 0.5f,
		"on: back in during a fade out, from where it had got to");
	Check(close(live::RadioGain(), 1.0f), "on until the game says otherwise");
	live::SetRadioOn(false);
	Sleep(600);
	Check(live::RadioGain() == 0.0f, "turned off: silent after 0.5 s");
	live::SetRadioOn(true);
	Check(live::RadioGain() < 0.01f, "turned on: starts silent");
	Sleep(2'600);
	Check(close(live::RadioGain(), 1.0f), "turned on: full after 2.5 s");

	// Leaving the live station: the track's 0.5 s stop fade, and LeftFor for pausing at once.
	Check(!live::LeftFor(0), "on the station: not left");
	live::SetLiveTuned(false);
	Check(live::LeftFor(0) && !live::LeftFor(400), "left just now");
	Sleep(250);
	const float halfway = live::RadioGain();
	Check(halfway > 0.3f && halfway < 0.7f, "half way through the track's fade");
	Sleep(300);
	Check(live::RadioGain() == 0.0f && live::LeftFor(500), "silent after 0.5 s, time to pause");
	live::SetLiveTuned(true);
	Check(close(live::RadioGain(), 1.0f) && !live::LeftFor(0), "back on the station: full at once");
	live::SetRadioOn(false);
	Sleep(550);
	Check(live::LeftFor(500), "the radio turned off counts as leaving too");
	live::SetRadioOn(true);
	Check(!live::LeftFor(0), "turned on again: not left");
	Sleep(2'600); // the radio's fade in

	// The pause menu: 1 s linear out, pause; 1 s back in.
	Check(close(live::PauseGain(true, 1.0f, 0), 1.0f) && close(live::PauseGain(true, 1.0f, 500), 0.5f) &&
		live::PauseGain(true, 1.0f, 1'000) == 0.0f, "pausing: 1 s linear fade");
	Check(live::PauseGain(false, 0.0f, 0) == 0.0f && close(live::PauseGain(false, 0.0f, 250), 0.25f) &&
		close(live::PauseGain(false, 0.0f, 1'000), 1.0f), "resuming: 1 s back in");
	Check(close(live::PauseGain(false, 0.6f, 500), 0.8f), "resumed during the fade: from where it had got to");
	Check(!live::FadedOut(), "playing: not faded out");
	live::SetGamePaused(true);
	Sleep(600);
	Check(live::LeftFor(0) && !live::FadedOut() && live::RadioGain() > 0.2f && live::RadioGain() < 0.6f,
		"0.6 s into the pause menu: still fading, not time to pause");
	Sleep(500);
	Check(live::FadedOut() && live::RadioGain() == 0.0f, "1 s in: silent, time to pause");
	live::SetGamePaused(false);
	Check(!live::LeftFor(0) && live::RadioGain() < 0.1f, "resumed: back on, from silence");
	Sleep(1'100);
	Check(close(live::RadioGain(), 1.0f), "resumed: full after 1 s");
	live::SetLiveTuned(false);
	Sleep(550);
	Check(live::FadedOut(), "another station: faded out after 0.5 s");
	live::SetLiveTuned(true);
	if (gFailures) {
		return 1;
	}
	std::printf("PASS\n");
	return 0;
}

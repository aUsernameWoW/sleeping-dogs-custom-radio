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

int main()
{
	static_assert(live::kHeaderSize - 8 + live::kFrames * live::kBlockAlign <= 0xFFFFFFFFull, "RIFF size fits");
	static_assert(live::kMaxLagMs > live::kPrerollMs && live::kMaxLagMs < live::kRingMs, "a jump lands inside the ring and below the lag limit");
	TestStart();
	TestRing();
	TestReader();
	TestPhase();
	if (gFailures) {
		return 1;
	}
	std::printf("PASS\n");
	return 0;
}

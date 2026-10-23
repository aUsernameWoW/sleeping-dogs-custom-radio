// Plays an internet radio station into the live ring for a while, outside the game, and reports what arrived:
// connection, format, titles, and whether the ring filled at real-time pace. Needs the network, so it is
// compiled but not run by build.ps1 / CI. Usage: netradio_manual <url> [seconds]; prints the log at the end.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define DR_MP3_IMPLEMENTATION
#include "../../../reference/dr_libs/dr_mp3.h"
#define STB_VORBIS_HEADER_ONLY
#include "../../../reference/stb/stb_vorbis.c"
#include "../core/icy.cc"
#include "../core/live.cc"
#include "../core/log.cc"
#include "../core/netradio.cc"
#include "../core/pacer.cc"
#include "../core/stream_decoder.cc"
#include "../core/tags.cc"

#pragma comment(lib, "winhttp.lib")

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv)
{
	if (argc < 2) {
		std::printf("usage: netradio_manual <url> [seconds]\n");
		return 2;
	}
	const int seconds = argc > 2 ? std::atoi(argv[2]) : 20;
	DeleteFileW(L"netradio_manual.log");
	logger::Open(L"netradio_manual.log");

	std::atomic<bool> stop{ false };
	const std::string url = argv[1];
	std::thread radio([&] { netradio::Run(url, stop); });

	// Wwise as the game's log shows it reading a live track: 128 KB (682 ms) per read, one at a time, keeping
	// ~4 reads (2.7 s) buffered; playback starts with the first completed read and drains in real time. Counts
	// how long reads wait, the silence the reader pads, and how long the voice would starve.
	struct Emulation
	{
		uint32_t mReads = 0;
		DWORD mLongestWait = 0;
		double mStarvedMs = 0;
		double mLowestBufferMs = 1e9; // after the first 5 s
		DWORD mFirstAudioAt = 0;
	} emulation;
	std::thread wwise([&] {
		uint8_t header[live::kHeaderSize] = {};
		live::Reader reader(live::Stream(), header);
		std::vector<uint8_t> chunk(131072);
		const double chunkMs = (131072.0 / live::kBlockAlign) * 1000 / live::kRate;
		uint64_t position = 0;
		double buffer = 0;
		bool playing = false, inflight = false;
		DWORD issued = 0, last = GetTickCount();
		const DWORD begin = last;
		while (!stop.load()) {
			Sleep(5);
			const DWORD now = GetTickCount();
			if (playing) {
				buffer -= now - last;
				if (buffer < 0) {
					emulation.mStarvedMs -= buffer;
					buffer = 0;
				}
				if (now - begin > 5000 + emulation.mFirstAudioAt) emulation.mLowestBufferMs = (std::min)(emulation.mLowestBufferMs, buffer);
			}
			last = now;
			if (!inflight && buffer < 4 * chunkMs) {
				inflight = true;
				issued = now;
			}
			if (inflight) {
				reader.Fill(position, 131072, chunk.data());
				position += 131072;
				emulation.mLongestWait = (std::max)(emulation.mLongestWait, now - issued);
				++emulation.mReads;
				buffer += chunkMs;
				inflight = false;
				if (!playing) {
					playing = true;
					emulation.mFirstAudioAt = now - begin;
				}
			}
		}
		const live::Reader::Stats& s = reader.GetStats();
		std::printf("Wwise emulation: %u reads, first read after %lu ms, longest wait %lu ms, %llu ms silence padded, %u underruns, %u jumps, %.0f ms starved, lowest buffer %.0f ms\n",
			emulation.mReads, emulation.mFirstAudioAt, emulation.mLongestWait, s.mSilenceBytes * 1000 / (live::kRate * live::kBlockAlign),
			s.mUnderruns, s.mJumps, emulation.mStarvedMs, emulation.mLowestBufferMs);
	});

	const DWORD start = GetTickCount();
	uint64_t firstAt = 0, firstBytes = 0;
	for (int i = 0; i < seconds; ++i) {
		Sleep(1000);
		const uint64_t written = live::Stream().Written();
		if (written && !firstBytes) {
			firstAt = GetTickCount() - start;
			firstBytes = written;
		}
		std::printf("%2d s: %.2f s of audio in the ring\n", i + 1, written / double(live::kRate * live::kBlockAlign));
	}
	stop = true;
	wwise.join();
	radio.join();

	const uint64_t written = live::Stream().Written();
	const double audio = (written - firstBytes) / double(live::kRate * live::kBlockAlign);
	const double wall = (GetTickCount() - start - firstAt) / 1000.0;
	std::printf("audio since the first push: %.2f s in %.2f s (rate %.3f)\n", audio, wall, wall > 0 ? audio / wall : 0.0);
	// Shared: the logger still has it open for writing.
	if (FILE* log = _wfsopen(L"netradio_manual.log", L"rb", _SH_DENYNO)) {
		char buffer[4096];
		size_t n;
		while ((n = fread(buffer, 1, sizeof(buffer), log)) > 0) fwrite(buffer, 1, n, stdout);
		fclose(log);
	}
	return written ? 0 : 1;
}

#undef STB_VORBIS_HEADER_ONLY
#include "../../../reference/stb/stb_vorbis.c"

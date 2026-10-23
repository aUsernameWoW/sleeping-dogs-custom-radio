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

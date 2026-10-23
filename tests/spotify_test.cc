// The Spotify source (core/spotify.cc): librespot's event log parsed into a title (Rust escapes, several
// artists, UTF-8), and, with librespot_fake_manual.exe standing in for librespot (built next to this test), the
// whole source: arguments and environment, the title reaching the HUD, audio entering the live ring at real-time
// pace once the game plays the station, and librespot held (nothing more taken) while it doesn't. argv[1] is
// unused.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
// The decoders first and stb_vorbis's implementation last, as in pacer_test (stream_decoder.cc, for the
// resampler, needs them).
#define DR_MP3_IMPLEMENTATION
#include "../../../reference/dr_libs/dr_mp3.h"
#define STB_VORBIS_HEADER_ONLY
#include "../../../reference/stb/stb_vorbis.c"
#include "../core/live.cc"
#include "../core/log.cc"
#include "../core/spotify.cc"
#include "../core/stream_decoder.cc"
#include "../core/tags.cc"

#include <cstdio>
#include <thread>

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

	void TestParsing()
	{
		Check(spotify::Unescape(R"(a\"b\\c\nd\u{e9}\u{1F600})") == "a\"b\\c\nd\xC3\xA9\xF0\x9F\x98\x80", "Rust escapes");
		Check(spotify::Unescape("\xE4\xBD\xA0\xE5\xA5\xBD") == "\xE4\xBD\xA0\xE5\xA5\xBD", "UTF-8 passes through");

		spotify::EventParser parser;
		spotify::Event event;
		const char* lines[] = {
			"[2025-11-10T12:00:00Z INFO  librespot_playback::player] <Song> (200000 ms) loaded",
			"[2025-11-10T12:00:02Z DEBUG librespot::player_event_handler] Running cmd /c rem with environment variables:",
			"{",
			"    \"PLAYER_EVENT\": \"track_changed\",",
			"    \"NAME\": \"Song \\\"One\\\"\",",
			"    \"ARTISTS\": \"A\\nB\",",
			"    \"COVERS\": \"\",",
		};
		bool ended = false;
		for (const char* line : lines) ended |= parser.Feed(line, event);
		Check(!ended, "no event before the block closes");
		Check(parser.Feed("}", event), "the closing brace ends the event");
		Check(event.mType == "track_changed" && event.mName == "Song \"One\"" && event.mArtists == "A\nB", "event fields");
		Check(spotify::Title(event) == "A, B - Song \"One\"", "title with two artists");
		Check(spotify::Title({ "track_changed", "Solo", "" }) == "Solo", "title without artists");
		Check(spotify::Title({ "track_changed", "", "A" }).empty(), "no name, no title");

		Check(!parser.Feed("{", event) && !parser.Feed("    \"PLAYER_EVENT\": \"x\",", event), "lines outside a block are ignored");
	}

	void TestWithFake()
	{
		wchar_t path[MAX_PATH];
		GetModuleFileNameW(nullptr, path, MAX_PATH);
		std::wstring dir = path;
		dir = dir.substr(0, dir.find_last_of(L"\\/"));
		spotify::Options options{ dir + L"\\librespot_fake_manual.exe", "Test Device", dir + L"\\spotify_test_cache" };
		if (GetFileAttributesW(options.mLibrespot.c_str()) == INVALID_FILE_ATTRIBUTES) {
			std::printf("no librespot_fake_manual.exe next to the test: skipping the source test\n");
			return;
		}

		std::atomic<bool> listening{ true }, stop{ false };
		live::Acquire();
		std::thread game([&] {
			while (!stop.load()) {
				if (listening.load()) live::NoteRead();
				Sleep(100);
			}
		});
		std::thread source([&] { spotify::Run(options, stop); });

		const DWORD begin = GetTickCount();
		Sleep(3'000);
		const double playedS = live::Stream().Written() / double(live::kRate * live::kBlockAlign);
		const double elapsedS = (GetTickCount() - begin) / 1000.0;
		std::printf("after %.2f s: %.2f s of audio in the ring, title \"%s\"\n", elapsedS, playedS, live::NowPlaying().c_str());
		Check(live::NowPlaying() == "G.E.M., Guest - \xE8\x87\xAA\xE7\x94\xB1\xE7\x9A\x84\xE4\xBD\xA0 \"Live\"", "the event's title reaches the HUD");
		// The head (2 s at once) plus real time, less librespot's start-up: never far beyond it.
		Check(playedS > 2.5 && playedS < elapsedS + 2.2, "audio enters the ring at real-time pace after the head");

		// The game stops playing the station: nothing more enters the ring (librespot is held).
		listening = false;
		live::Release();
		Sleep(500);
		const uint64_t held = live::Stream().Written();
		Sleep(1'500);
		Check(live::Stream().Written() == held, "nothing is fed while the game doesn't play the station");

		stop = true;
		source.join();
		game.join();
	}
}

int main()
{
	TestParsing();
	TestWithFake();
	if (gFailures) {
		return 1;
	}
	std::printf("PASS\n");
	return 0;
}

#undef STB_VORBIS_HEADER_ONLY
#include "../../../reference/stb/stb_vorbis.c"

// App capture (core/app_audio.cc) without playing anything: finding a process tree's root by name, and opening a
// process loopback capture either working (Windows 11, Windows Server 2025) or failing with a reason, never
// crashing (Wine has no process loopback). The capture itself is tested by app_audio_manual. Also the media
// session matching and title format (core/now_playing.cc). argv[1] is unused.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "../core/app_audio.cc"
#include "../core/live.cc"
#include "../core/now_playing.cc"
#include "../core/log.cc"

#include <cstdio>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "oleaut32.lib")
// As in the .asi: the test must start where WinRT is missing.
#pragma comment(lib, "delayimp.lib")
#pragma comment(linker, "/DELAYLOAD:api-ms-win-core-winrt-l1-1-0.dll")
#pragma comment(linker, "/DELAYLOAD:api-ms-win-core-winrt-error-l1-1-1.dll")

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
}

int main()
{
	wchar_t path[MAX_PATH];
	GetModuleFileNameW(nullptr, path, MAX_PATH);
	std::wstring name = path;
	name = name.substr(name.find_last_of(L"\\/") + 1);
	for (wchar_t& c : name) c = static_cast<wchar_t>(towupper(c));

	Check(appaudio::FindProcess(name) == GetCurrentProcessId(), "finds this process by its name, case-insensitively");
	Check(appaudio::FindProcess(L"no-such-program-sdradio.exe") == 0, "a program that isn't running isn't found");

	Check(nowplaying::MatchKey(L"CloudMusic.EXE") == "cloudmusic", "the match key is the lowercased name without .exe");
	Check(nowplaying::MatchKey(L"chrome") == "chrome", "a name without .exe stays");
	Check(nowplaying::Matches("cloudmusic.exe", "cloudmusic"), "a desktop app's session ID is its exe name");
	Check(nowplaying::Matches("SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify", "spotify"), "a packaged app's ID contains its name");
	Check(nowplaying::Matches("Chrome", "chrome"), "case doesn't matter");
	Check(!nowplaying::Matches("Spotify.exe", "cloudmusic"), "another app's session doesn't match");
	Check(!nowplaying::Matches("anything", ""), "an empty key matches nothing");
	Check(nowplaying::Format("Artist", "Title") == "Artist - Title", "artist and title");
	Check(nowplaying::Format("", "Title") == "Title", "title alone");
	Check(nowplaying::Format("Artist", "").empty(), "no title, no text");

	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	appaudio::Capture capture;
	std::string error;
	if (capture.Open(GetCurrentProcessId(), error)) {
		std::vector<int16_t> samples;
		Check(capture.Read(samples, 200, error), "an open capture reads");
		std::printf("process loopback works here (%zu samples of this silent process)\n", samples.size());
	}
	else {
		Check(!error.empty(), "a capture that can't open says why");
		std::printf("process loopback isn't available here: %s\n", error.c_str());
	}

	if (gFailures) {
		return 1;
	}
	std::printf("PASS\n");
	return 0;
}

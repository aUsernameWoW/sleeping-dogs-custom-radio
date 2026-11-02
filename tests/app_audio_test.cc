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
	Check(nowplaying::Format("Olivia Rodrigo \xE2\x80\x94 GUTS", "vampire", "GUTS") == "Olivia Rodrigo - vampire",
		"an \"Artist \xE2\x80\x94 Album\" artist loses the album");
	Check(nowplaying::AppleMusicArtist("Olivia Rodrigo \xE2\x80\x94 you seem pretty sad for a girl so in love") == "Olivia Rodrigo",
		"Apple Music's artist without its album (seen 2026-10-27)");
	Check(nowplaying::AppleMusicArtist("Olivia Rodrigo") == "Olivia Rodrigo", "an Apple Music artist without an album");
	Check(nowplaying::Matches("AppleInc.AppleMusicWin_nzyj5cx40ttqa!App", nowplaying::kAppleMusic), "Apple Music's session");
	Check(nowplaying::Matches("AppleInc.AppleMusicWin_nzyj5cx40ttqa!App", nowplaying::MatchKey(L"AppleMusic.exe")),
		"App = AppleMusic.exe finds its session");
	Check(nowplaying::Format("Olivia Rodrigo", "vampire", "GUTS") == "Olivia Rodrigo - vampire", "a plain artist stays");
	Check(nowplaying::Format("GUTS", "vampire", "GUTS") == "GUTS - vampire", "an artist that is only the album name stays");
	Check(nowplaying::Format("Band \xE2\x80\x94 Live", "Song", "Other") == "Band \xE2\x80\x94 Live - Song", "a dash in the artist itself stays");

	// App = auto: which media session it follows.
	using nowplaying::AutoChoose;
	const std::string apple = "AppleInc.AppleMusicWin_nzyj5cx40ttqa!App";
	Check(AutoChoose({}, {}, "", false) == -1, "no sessions, nothing to follow");
	Check(AutoChoose({ apple, "Chrome" }, { false, false }, "", false) == -1, "nothing plays: wait for one that does");
	Check(AutoChoose({ apple, "Chrome" }, { false, true }, "", false) == 1, "the one that plays");
	Check(AutoChoose({ apple, "Chrome" }, { true, true }, "Chrome", false) == 1, "the followed one while it plays, though another does too");
	Check(AutoChoose({ apple, "Chrome" }, { false, true }, apple, true) == 0, "paused by us, it stays the music while the browser plays");
	Check(AutoChoose({ apple, "Chrome" }, { false, true }, apple, false) == 1, "paused by the player while another plays: follow that");
	Check(AutoChoose({ apple, "Chrome" }, { false, false }, apple, false) == 0, "paused by the player and nothing else plays: stay");
	Check(AutoChoose({ "Chrome" }, { false }, apple, true) == -1, "the followed app closed and nothing plays");
	Check(AutoChoose({ "Chrome", "Chrome" }, { false, true }, "Chrome", false) == 1, "of two sessions of the followed app, the one that plays");

	// App = auto: which process the followed session's sound comes from.
	using appaudio::Renderer;
	const std::vector<Renderer> renderers = {
		{ 100, L"Discord.exe", L"" },
		{ 200, L"AMPLibraryAgent.exe", L"AppleInc.AppleMusicWin_nzyj5cx40ttqa" },
		{ 300, L"chrome.exe", L"" },
	};
	Check(appaudio::MatchRenderer(renderers, apple, false) == 1, "a Store app's sound by its package (Apple Music's agent)");
	Check(appaudio::MatchRenderer(renderers, "Chrome", false) == 2, "a desktop app by its name");
	Check(appaudio::MatchRenderer(renderers, "308046B0AF4A39CB", true) == -1, "an ID that names no process, several renderers: none");
	Check(appaudio::MatchRenderer({ renderers[2] }, "308046B0AF4A39CB", true) == 0, "an ID that names no process, one renderer: that one");
	Check(appaudio::MatchRenderer({ renderers[2] }, "", false) == -1, "no session and no fallback: none");
	Check(appaudio::MatchRenderer({ renderers[0] }, "", true) == 0, "no session: the only program making sound");
	Check(appaudio::MatchRenderer({ { 1, L"y.exe", L"AppleInc.AppleMusicWin_nzyj5cx40ttqaX" } }, apple, false) == -1,
		"a package family must end where the ID's does");
	Check(appaudio::MatchRenderer({ { 1, L"x.exe", L"" } }, apple, false) == -1, "a short name inside a long ID isn't a match");
	Check(appaudio::MatchRenderer({ { 1, L"Spotify.exe", L"" } }, "SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify", false) == 0,
		"Spotify from the Store, by its app name after the '!'");
	Check(appaudio::MatchRenderer({ { 1, L"vlc.exe", L"" } }, "VideoLAN.VLC", false) == 0, "a dotted ID's word");
	Check(appaudio::MatchRenderer({ { 1, L"msedge.exe", L"" } }, "MSEdge", false) == 0, "Edge");
	Check(appaudio::IsAuto(L"") && appaudio::IsAuto(L"AUTO") && !appaudio::IsAuto(L"AppleMusic.exe"), "App = auto or empty");

	using appaudio::Session;
	const auto session = [](bool defaultDevice, bool active, bool muted, float volume) {
		Session s;
		s.mDevice = defaultDevice ? "Speakers" : "Steam Streaming Speakers";
		s.mDefaultDevice = defaultDevice;
		s.mActive = active;
		s.mMuted = muted;
		s.mVolume = volume;
		return s;
	};
	Check(appaudio::Warnings({}) == 0, "no sessions, no warnings");
	Check(appaudio::Warnings({ session(false, true, false, 1.0f) }) == 0, "playing unmuted on another device is right");
	Check(appaudio::Warnings({ session(false, true, true, 1.0f) }) == appaudio::kWarnMuted, "a muted session");
	Check(appaudio::Warnings({ session(false, true, false, 0.0f) }) == appaudio::kWarnMuted, "a session at 0 is muted too");
	Check(appaudio::Warnings({ session(true, true, false, 0.5f) }) == appaudio::kWarnDefaultDevice, "playing on the default device");
	Check(appaudio::Warnings({ session(true, false, true, 0.0f), session(false, true, false, 1.0f) }) == 0,
		"an idle session (left over on the default device) says nothing");
	Check(appaudio::Warnings({ session(true, true, true, 1.0f) }) == (appaudio::kWarnMuted | appaudio::kWarnDefaultDevice), "both");
	Check(appaudio::Describe({}) == "none", "no sessions described");
	Check(appaudio::Describe({ session(true, true, false, 1.0f), session(false, false, true, 0.25f) }) ==
		"Speakers (default): active, 100%; Steam Streaming Speakers: idle, muted, 25%", "sessions described");

	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const std::vector<Session> own = appaudio::Sessions(GetCurrentProcessId());
	std::printf("this process's audio sessions: %s\n", appaudio::Describe(own).c_str());
	Check(appaudio::Sessions(0).empty(), "no process, no sessions");
	Check(appaudio::PackageFamily(GetCurrentProcessId()).empty(), "an ordinary program has no package");
	Check(appaudio::AudioProcess(GetCurrentProcessId()) == GetCurrentProcessId(), "an ordinary program is captured itself");
	Check(appaudio::AudioProcess(0) == 0, "no process, nothing to capture");
	const std::vector<Renderer> live = appaudio::Renderers();
	Check(std::none_of(live.begin(), live.end(), [](const Renderer& r) { return r.mProcess == GetCurrentProcessId(); }),
		"the game (here: the test) is never a renderer");
	std::printf("programs making sound now: %zu\n", live.size());
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

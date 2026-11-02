#pragma once

// The title another app is playing, from the system media sessions (SMTC, Windows.Media.Control through
// C++/WinRT), for the HUD while the live station plays that app (app_audio.hh); and pausing that app while the
// game doesn't play the station (an idea from FH6 Universal Radio, which does the same). An app shows up there only if
// it publishes a session: Spotify and browsers do; NetEase Cloud Music only with its "SMTC" setting ticked.
// Windows 10 1809+; Wine has no Windows.Media.Control, which is logged once. The WinRT API sets are
// delay-loaded, so the .asi loads where they're missing.

#include <atomic>
#include <string>
#include <vector>

namespace nowplaying
{
	// What a session's app ID is matched against: the process name, lowercased, without ".exe".
	std::string MatchKey(const std::wstring& exeName);
	// Whether a session's SourceAppUserModelId (e.g. "cloudmusic.exe", "Spotify.exe", "Chrome", a packaged
	// app's "SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify") belongs to the app with that key.
	bool Matches(const std::string& appId, const std::string& key);
	// "Artist - Title", or the title alone; empty without a title. An artist ending in " — <album>" loses that part.
	std::string Format(const std::string& artist, const std::string& title, const std::string& album = {});
	// Apple Music's session (its package name, as Matches takes it) and its artist without the " — Album" it adds.
	constexpr const char* kAppleMusic = "appleinc.applemusicwin";
	std::string AppleMusicArtist(const std::string& artist);

	// The session `App = auto` follows, from the sessions' app IDs and whether each plays: the current one while it
	// exists, unless it stopped playing (and not because we paused it) and another plays; else the first that plays.
	// -1 for none.
	int AutoChoose(const std::vector<std::string>& appIds, const std::vector<bool>& playing, const std::string& current, bool pausedByUs);
	// The app ID of the session `App = auto` follows now (empty for none), for app_audio.cc's choice of process.
	std::string ChosenApp();

	// Polls every 2 s and sets live::SetNowPlaying to the matching session's title; an empty `exeName` follows
	// AutoChoose's session instead. With `control`, also pauses the app while the game doesn't play the live
	// station (live::Listening) and resumes it when it does again. Returns once `stop` is set.
	void Run(const std::wstring& exeName, bool control, const std::atomic<bool>& stop);
}

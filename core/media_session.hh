#pragma once

// The system's media sessions (Windows SMTC, GlobalSystemMediaTransportControlsSessionManager): which app is
// playing what, and play/pause/skip. Adapted from FH6 Universal Radio
// (https://github.com/g0ldyy/fh6-universal-radio, src/sources/external_media_session.cpp, GPL-3.0); under the
// GPL-3.0 (LICENSE).
//
// Every function must run on a thread in the multithreaded apartment (they block on WinRT async calls,
// which an STA thread must not do). Windows 10 1809+; Wine has no Windows.Media.Control, so Open fails there.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace media
{
	struct Session
	{
		std::string mId; // the app's AppUserModelID, e.g. "Spotify.exe" or "Chrome"
		bool mCurrent = false; // the one the system media overlay shows
	};

	struct Track
	{
		std::string mApp;
		std::string mTitle;
		std::string mArtist;
		std::string mAlbum;
		std::string mStatus; // "playing", "paused", "stopped", ...
		uint64_t mPositionMs = 0;
		uint64_t mDurationMs = 0;
	};

	// Gets the session manager; false (with the HRESULT and message in `error`) where there is none.
	bool Open(std::string& error);

	std::vector<Session> List();

	// `select`: a case-insensitive part of the app ID ("spotify"); empty = the current session. A selection
	// never falls back to another app, which would then be read or paused instead.
	std::optional<Track> Read(const std::string& select);

	bool Play(const std::string& select);
	bool Pause(const std::string& select);
	bool Next(const std::string& select);
	bool Previous(const std::string& select);
}

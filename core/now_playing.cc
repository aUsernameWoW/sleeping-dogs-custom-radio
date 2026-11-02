#include "now_playing.hh"

#include <Windows.h>
#include <objbase.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>

#include <chrono>
#include <mutex>

#include "live.hh"
#include "log.hh"

namespace nowplaying
{
	namespace
	{
		using winrt::Windows::Foundation::AsyncStatus;
		using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession;
		using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
		using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;

		std::string Lower(std::string s)
		{
			for (char& c : s) {
				if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
			}
			return s;
		}

		// The delay-loaded API sets C++/WinRT calls into; missing under Wine and before Windows 8.
		bool WinRtPresent()
		{
			return LoadLibraryW(L"api-ms-win-core-winrt-l1-1-0.dll") && LoadLibraryW(L"api-ms-win-core-winrt-error-l1-1-1.dll");
		}

		// Waits for a WinRT async operation without blocking forever: a broken media service has been seen not
		// to answer RequestAsync for over 10 s.
		template <typename Operation>
		bool Finish(const Operation& operation)
		{
			return operation.wait_for(std::chrono::seconds(5)) == AsyncStatus::Completed;
		}

		// Whether the app is paused because the game stopped playing the live station. Outlives a source run (it
		// stops 10 s after the station is left), so the next run knows to resume it.
		std::atomic<bool> gPausedByUs{ false };

		std::mutex gChosenLock;
		std::string gChosen; // AutoChoose's session's app ID

		// Pauses the app once the game hasn't played the live station for a moment, and resumes it when the game
		// plays it again, but only a pause of ours: a pause the player made stays, and a program the player
		// restarted meanwhile isn't paused again until the next time the station is left.
		void Control(const GlobalSystemMediaTransportControlsSession& session, DWORD now, DWORD& quietSince, const std::string& name)
		{
			constexpr DWORD kQuietMs = 3'000; // Wwise reads every ~0.7 s while it plays
			constexpr DWORD kGraceMs = 1'500; // a track change closes and reopens the file at once
			const bool listening = live::Listening(kQuietMs);
			if (listening) {
				quietSince = 0;
			}
			else if (!quietSince) {
				quietSince = now ? now : 1;
			}
			const auto status = session.GetPlaybackInfo().PlaybackStatus();
			if (!listening && !gPausedByUs.load() && now - quietSince >= kGraceMs && status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) {
				auto pause = session.TryPauseAsync();
				if (Finish(pause) && pause.GetResults()) {
					gPausedByUs = true;
					LOG("media: paused %s (the game isn't playing the live station)", name.c_str());
				}
			}
			else if (listening && gPausedByUs.exchange(false) && status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused) {
				auto play = session.TryPlayAsync();
				if (Finish(play) && play.GetResults()) {
					LOG("media: resumed %s", name.c_str());
				}
			}
		}
	}

	std::string MatchKey(const std::wstring& exeName)
	{
		std::string key = Lower(logger::ToUtf8(exeName.c_str()));
		if (key.size() > 4 && key.compare(key.size() - 4, 4, ".exe") == 0) {
			key.resize(key.size() - 4);
		}
		return key;
	}

	bool Matches(const std::string& appId, const std::string& key)
	{
		return !key.empty() && Lower(appId).find(key) != std::string::npos;
	}

	std::string AppleMusicArtist(const std::string& artist)
	{
		// Apple Music for Windows (1.1540, 2026-10-27) reports "Artist — Album" as the artist and no AlbumTitle.
		const size_t dash = artist.find(" \xE2\x80\x94 ");
		return dash == std::string::npos || dash == 0 ? artist : artist.substr(0, dash);
	}

	std::string Format(const std::string& artist, const std::string& title, const std::string& album)
	{
		if (title.empty()) {
			return {};
		}
		// An app that appends the album to the artist would crowd the HUD's one line.
		std::string who = artist;
		for (const char* dash : { " \xE2\x80\x94 ", " \xE2\x80\x93 ", " - " }) {
			const std::string suffix = dash + album;
			if (!album.empty() && who.size() > suffix.size() && who.compare(who.size() - suffix.size(), suffix.size(), suffix) == 0) {
				who.resize(who.size() - suffix.size());
				break;
			}
		}
		return who.empty() ? title : who + " - " + title;
	}

	int AutoChoose(const std::vector<std::string>& appIds, const std::vector<bool>& playing, const std::string& current, bool pausedByUs)
	{
		int kept = -1, firstPlaying = -1;
		for (size_t i = 0; i < appIds.size() && i < playing.size(); ++i) {
			if (!current.empty() && appIds[i] == current && (kept < 0 || playing[i])) {
				kept = static_cast<int>(i);
			}
			if (playing[i] && firstPlaying < 0) {
				firstPlaying = static_cast<int>(i);
			}
		}
		// Paused by us, it's still the player's music: the game only left the station. Paused by the player while
		// another app plays, the player has moved on.
		if (kept >= 0 && (playing[kept] || pausedByUs || firstPlaying < 0)) {
			return kept;
		}
		return firstPlaying;
	}

	std::string ChosenApp()
	{
		std::lock_guard<std::mutex> lock(gChosenLock);
		return gChosen;
	}

	void Run(const std::wstring& exeName, bool control, const std::atomic<bool>& stop)
	{
		const std::string key = MatchKey(exeName);
		const bool autoPick = key.empty();
		// Not cleared here: like gPausedByUs, the choice outlives a source run (it stops 10 s after the station is
		// left). Cleared, the next run found nothing playing (the app paused by us) and never resumed it (2026-11-02,
		// in game).
		const auto choose = [](const std::string& id) {
			std::lock_guard<std::mutex> lock(gChosenLock);
			gChosen = id;
		};
		if (!WinRtPresent()) {
			LOG("media: no WinRT here; the HUD shows the program's name instead of its song");
			return;
		}
		const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		std::string name = autoPick ? (ChosenApp().empty() ? std::string("(no app yet)") : ChosenApp()) : logger::ToUtf8(exeName.c_str());

		GlobalSystemMediaTransportControlsSessionManager manager{ nullptr };
		GlobalSystemMediaTransportControlsSession session{ nullptr };
		std::string loggedIds = "-";
		std::string shown;
		bool missingLogged = false;
		int loggedStatus = -1;
		bool unavailable = false;
		DWORD refreshAt = GetTickCount();
		DWORD quietSince = 0; // when the game stopped playing the live station, 0 while it plays
		// Tuning in starts the app if it's paused, as tuning a radio to a station plays it: no switching out of the
		// game to press play (2026-11-02, the user's request). Only at the tune-in: a pause made while on the
		// station stays. Handled at the next refresh, which has the sessions.
		bool wasListening = false;
		bool tunedIn = false;
		while (!stop.load() && !unavailable) {
			try {
				const DWORD now = GetTickCount();
				const bool listening = live::Listening(3'000);
				if (listening && !wasListening) {
					tunedIn = control;
					refreshAt = now; // now rather than in up to 2 s
				}
				else if (!listening) {
					tunedIn = false;
				}
				wasListening = listening;
				if (control && session) {
					Control(session, now, quietSince, name);
				}
				if (static_cast<int32_t>(now - refreshAt) < 0) {
					Sleep(250);
					continue;
				}
				refreshAt = now + 2'000;
				if (!manager) {
					auto request = GlobalSystemMediaTransportControlsSessionManager::RequestAsync();
					if (Finish(request)) {
						manager = request.GetResults();
					}
					else {
						request.Cancel();
						LOG("media: the system's media sessions don't answer; trying again");
					}
				}
				if (manager) {
					// The app's session, preferring one that plays (a browser can have several).
					GlobalSystemMediaTransportControlsSession found{ nullptr };
					bool foundPlaying = false;
					std::string ids;
					std::vector<GlobalSystemMediaTransportControlsSession> all;
					std::vector<std::string> allIds;
					std::vector<bool> allPlaying;
					for (const auto& candidate : manager.GetSessions()) {
						const std::string id = winrt::to_string(candidate.SourceAppUserModelId());
						ids += (ids.empty() ? "" : ", ") + id;
						const bool playing = candidate.GetPlaybackInfo().PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
						if (autoPick) {
							all.push_back(candidate);
							allIds.push_back(id);
							allPlaying.push_back(playing);
							continue;
						}
						if (!Matches(id, key) || foundPlaying) {
							continue;
						}
						if (!found || playing) {
							found = candidate;
							foundPlaying = playing;
						}
					}
					if (ids != loggedIds) {
						LOG("media: sessions: %s", ids.empty() ? "(none)" : ids.c_str());
						loggedIds = ids;
					}
					if (autoPick) {
						const std::string current = ChosenApp();
						int chosen = AutoChoose(allIds, allPlaying, current, gPausedByUs.load());
						// Tuned in with nothing playing: the only media session there is is the player's music.
						if (chosen < 0 && tunedIn && allIds.size() == 1) {
							chosen = 0;
						}
						else if (chosen < 0 && tunedIn && allIds.size() > 1) {
							LOG("media: App = auto: %zu media sessions and none plays; press play in the one to hear", allIds.size());
							tunedIn = false;
						}
						const std::string id = chosen >= 0 ? allIds[chosen] : std::string();
						if (chosen >= 0) {
							found = all[chosen];
						}
						if (id != current && !id.empty()) {
							LOG("media: App = auto: following %s (%s)", id.c_str(), allPlaying[chosen] ? "it plays" : "the only media session");
							name = id;
							choose(id);
							loggedStatus = -1;
							// A pause of ours belonged to the app we followed before.
							gPausedByUs = false;
						}
					}

					session = found;
					// A pause of ours is Control's to undo.
					if (tunedIn && found && !gPausedByUs.load() &&
						found.GetPlaybackInfo().PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused) {
						auto play = found.TryPlayAsync();
						if (Finish(play) && play.GetResults()) {
							LOG("media: started %s (tuned in to the station)", name.c_str());
						}
					}
					tunedIn = false;
					std::string title;
					if (found) {
						missingLogged = false;
						const int status = static_cast<int>(found.GetPlaybackInfo().PlaybackStatus());
						if (status != loggedStatus) {
							// 4 = playing, 5 = paused (GlobalSystemMediaTransportControlsSessionPlaybackStatus)
							LOG("media: %s's playback status %d", name.c_str(), status);
							loggedStatus = status;
						}
						auto properties = found.TryGetMediaPropertiesAsync();
						if (Finish(properties)) {
							const auto media = properties.GetResults();
							const std::string artist = winrt::to_string(media.Artist()), album = winrt::to_string(media.AlbumTitle());
							const std::string app = winrt::to_string(found.SourceAppUserModelId());
							title = Format(Matches(app, kAppleMusic) ? AppleMusicArtist(artist) : artist, winrt::to_string(media.Title()), album);
							if (title != shown) {
								// What the app put where: Apple Music sends "Artist — Album" as the artist.
								LOG("media: artist \"%s\", album \"%s\"", artist.c_str(), album.c_str());
							}
						}
					}
					else if (!missingLogged && autoPick) {
						LOG("media: App = auto: no media session plays yet; waiting for one (NetEase Cloud Music: tick SMTC in its settings)");
						missingLogged = true;
					}
					else if (!missingLogged) {
						LOG("media: no media session of %s, so no song title (NetEase Cloud Music: tick SMTC in its settings)", name.c_str());
						missingLogged = true;
					}
					if (title != shown) {
						shown = title;
						live::SetNowPlaying(title);
						if (!title.empty()) {
							LOG("media: now playing \"%s\"", title.c_str());
						}
					}
				}
			}
			catch (const winrt::hresult_error& e) {
				LOG("media: %s (0x%08lX)", winrt::to_string(e.message()).c_str(), static_cast<unsigned long>(e.code().value));
				manager = nullptr;
				session = nullptr;
				// No Windows.Media.Control (Wine, Windows before 1809): nothing will change.
				unavailable = e.code() == REGDB_E_CLASSNOTREG;
			}
			Sleep(250);
		}
		session = nullptr; // released before COM is
		manager = nullptr;
		if (SUCCEEDED(com)) {
			CoUninitialize();
		}
	}
}

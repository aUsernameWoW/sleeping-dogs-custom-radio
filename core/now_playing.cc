#include "now_playing.hh"

#include <Windows.h>
#include <objbase.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>

#include <chrono>

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

	std::string Format(const std::string& artist, const std::string& title)
	{
		if (title.empty()) {
			return {};
		}
		return artist.empty() ? title : artist + " - " + title;
	}

	void Run(const std::wstring& exeName, bool control, const std::atomic<bool>& stop)
	{
		const std::string key = MatchKey(exeName);
		if (key.empty()) {
			return;
		}
		if (!WinRtPresent()) {
			LOG("media: no WinRT here; the HUD shows the program's name instead of its song");
			return;
		}
		const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		const std::string name = logger::ToUtf8(exeName.c_str());

		GlobalSystemMediaTransportControlsSessionManager manager{ nullptr };
		GlobalSystemMediaTransportControlsSession session{ nullptr };
		std::string loggedIds = "-";
		std::string shown;
		bool missingLogged = false;
		int loggedStatus = -1;
		bool unavailable = false;
		DWORD refreshAt = GetTickCount();
		DWORD quietSince = 0; // when the game stopped playing the live station, 0 while it plays
		while (!stop.load() && !unavailable) {
			try {
				const DWORD now = GetTickCount();
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
					for (const auto& candidate : manager.GetSessions()) {
						const std::string id = winrt::to_string(candidate.SourceAppUserModelId());
						ids += (ids.empty() ? "" : ", ") + id;
						if (!Matches(id, key) || foundPlaying) {
							continue;
						}
						const bool playing = candidate.GetPlaybackInfo().PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
						if (!found || playing) {
							found = candidate;
							foundPlaying = playing;
						}
					}
					if (ids != loggedIds) {
						LOG("media: sessions: %s", ids.empty() ? "(none)" : ids.c_str());
						loggedIds = ids;
					}

					session = found;
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
							title = Format(winrt::to_string(media.Artist()), winrt::to_string(media.Title()));
						}
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

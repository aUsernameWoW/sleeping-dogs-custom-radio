// Adapted from FH6 Universal Radio (https://github.com/g0ldyy/fh6-universal-radio,
// src/sources/external_media_session.cpp, GPL-3.0); under the GPL-3.0 (LICENSE).

#include "media_session.hh"

#include <Windows.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>

namespace media
{
	namespace
	{
		namespace control = winrt::Windows::Media::Control;

		// RequestAsync is a cross-process call; one manager serves all later calls. Leaked on purpose: as a
		// static it would be released in DLL_PROCESS_DETACH, a COM call to the session broker after the other
		// threads are gone, and that left a test process unable to exit for minutes (its last thread waiting
		// for an LPC reply) and the system's media sessions unresponsive meanwhile.
		std::mutex gMutex;
		// (WinRT types delete their own operator new, hence the holder.)
		struct ManagerHolder
		{
			control::GlobalSystemMediaTransportControlsSessionManager mManager{ nullptr };
		};
		ManagerHolder* gHolder = new ManagerHolder();
		control::GlobalSystemMediaTransportControlsSessionManager* gManager = &gHolder->mManager;

		std::string Utf8(const winrt::hstring& value)
		{
			return winrt::to_string(value);
		}

		std::string Lower(std::string text)
		{
			std::transform(text.begin(), text.end(), text.begin(), [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
			return text;
		}

		uint64_t Milliseconds(const winrt::Windows::Foundation::TimeSpan& span)
		{
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(span).count();
			return ms > 0 ? static_cast<uint64_t>(ms) : 0;
		}

		const char* StatusName(control::GlobalSystemMediaTransportControlsSessionPlaybackStatus status)
		{
			using S = control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;
			switch (status) {
			case S::Closed: return "closed";
			case S::Opened: return "opened";
			case S::Changing: return "changing";
			case S::Stopped: return "stopped";
			case S::Playing: return "playing";
			case S::Paused: return "paused";
			}
			return "unknown";
		}

		// Waits for a WinRT async operation at most `ms` instead of get()'s forever: the session broker can stop
		// answering (see gManager), which must not hang the calling thread for good.
		template <class Operation> auto Await(const Operation& operation, DWORD ms = 5000)
		{
			const DWORD start = GetTickCount();
			while (operation.Status() == winrt::Windows::Foundation::AsyncStatus::Started) {
				if (GetTickCount() - start >= ms) {
					operation.Cancel();
					throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
				}
				Sleep(10);
			}
			return operation.GetResults();
		}

		control::GlobalSystemMediaTransportControlsSessionManager Manager()
		{
			std::lock_guard lock(gMutex);
			return *gManager;
		}

		control::GlobalSystemMediaTransportControlsSession Pick(const std::string& select)
		{
			const auto manager = Manager();
			if (!manager) {
				return nullptr;
			}
			if (select.empty()) {
				return manager.GetCurrentSession();
			}
			const std::string wanted = Lower(select);
			for (const auto& session : manager.GetSessions()) {
				if (Lower(Utf8(session.SourceAppUserModelId())).find(wanted) != std::string::npos) {
					return session;
				}
			}
			return nullptr;
		}

		template <class Fn> bool WithSession(const std::string& select, Fn&& fn)
		{
			try {
				const auto session = Pick(select);
				return session ? fn(session) : false;
			}
			catch (...) {
				return false;
			}
		}
	}

	bool Open(std::string& error)
	{
		// The WinRT API sets are delay-loaded: a missing one would be an SEH exception at the first call,
		// which the catch below can't stop.
		const struct
		{
			const wchar_t* mDll;
			const char* mFunction;
		} required[] = { { L"api-ms-win-core-winrt-l1-1-0.dll", "RoGetActivationFactory" },
			{ L"api-ms-win-core-winrt-error-l1-1-1.dll", "RoOriginateLanguageException" } };
		for (const auto& [dll, function] : required) {
			const HMODULE module = LoadLibraryW(dll);
			if (!module || !GetProcAddress(module, function)) {
				error = std::string("no ") + function + " (no WinRT)";
				return false;
			}
		}

		try {
			auto manager = Await(control::GlobalSystemMediaTransportControlsSessionManager::RequestAsync());
			std::lock_guard lock(gMutex);
			*gManager = manager;
			return true;
		}
		catch (const winrt::hresult_error& e) {
			char buffer[64];
			std::snprintf(buffer, sizeof(buffer), "0x%08lX ", static_cast<unsigned long>(e.code().value));
			error = buffer + Utf8(e.message());
		}
		catch (...) {
			error = "unknown exception";
		}
		return false;
	}

	std::vector<Session> List()
	{
		std::vector<Session> sessions;
		try {
			const auto manager = Manager();
			if (!manager) {
				return sessions;
			}
			const auto current = manager.GetCurrentSession();
			const std::string currentId = current ? Utf8(current.SourceAppUserModelId()) : std::string();
			for (const auto& session : manager.GetSessions()) {
				Session item;
				item.mId = Utf8(session.SourceAppUserModelId());
				item.mCurrent = !currentId.empty() && item.mId == currentId;
				if (!item.mId.empty()) {
					sessions.push_back(std::move(item));
				}
			}
		}
		catch (...) {
		}
		return sessions;
	}

	std::optional<Track> Read(const std::string& select)
	{
		try {
			const auto session = Pick(select);
			if (!session) {
				return std::nullopt;
			}
			Track track;
			track.mApp = Utf8(session.SourceAppUserModelId());
			const auto props = Await(session.TryGetMediaPropertiesAsync());
			track.mTitle = Utf8(props.Title());
			track.mArtist = Utf8(props.Artist());
			if (track.mArtist.empty()) {
				track.mArtist = Utf8(props.AlbumArtist());
			}
			track.mAlbum = Utf8(props.AlbumTitle());
			track.mStatus = StatusName(session.GetPlaybackInfo().PlaybackStatus());
			// Not every app publishes a timeline.
			try {
				const auto timeline = session.GetTimelineProperties();
				track.mPositionMs = Milliseconds(timeline.Position());
				const uint64_t start = Milliseconds(timeline.StartTime());
				const uint64_t end = Milliseconds(timeline.EndTime());
				if (end > start) {
					track.mDurationMs = end - start;
				}
			}
			catch (...) {
			}
			return track;
		}
		catch (...) {
			return std::nullopt;
		}
	}

	bool Play(const std::string& select)
	{
		return WithSession(select, [](const auto& s) { return Await(s.TryPlayAsync()); });
	}

	bool Pause(const std::string& select)
	{
		return WithSession(select, [](const auto& s) { return Await(s.TryPauseAsync()); });
	}

	bool Next(const std::string& select)
	{
		return WithSession(select, [](const auto& s) { return Await(s.TrySkipNextAsync()); });
	}

	bool Previous(const std::string& select)
	{
		return WithSession(select, [](const auto& s) { return Await(s.TrySkipPreviousAsync()); });
	}
}

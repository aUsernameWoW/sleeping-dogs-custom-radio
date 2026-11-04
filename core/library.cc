#include "library.hh"

#include <objbase.h>

#include <algorithm>
#include <thread>

#include "bank.hh"
#include "decoder.hh"
#include "log.hh"

namespace library
{
	namespace
	{
		std::vector<Station> gStations;
		HANDLE gDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);

		// The bundled decoders' formats, then what Media Foundation may handle (Windows; Wine depends on its
		// GStreamer support). A file no backend can open is skipped with a log line.
		constexpr const wchar_t* kExtensions[] = { L".mp3", L".flac", L".wav", L".ogg", L".m4a", L".aac", L".mp4", L".wma", L".alac", L".opus" };

		bool IsMusic(const std::wstring& name)
		{
			const size_t dot = name.find_last_of(L'.');
			if (dot == std::wstring::npos) {
				return false;
			}
			for (const wchar_t* e : kExtensions) {
				if (_wcsicmp(name.c_str() + dot, e) == 0) {
					return true;
				}
			}
			return false;
		}

		void Collect(const std::wstring& dir, std::vector<std::wstring>& out)
		{
			WIN32_FIND_DATAW data;
			HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &data);
			if (find == INVALID_HANDLE_VALUE) {
				return;
			}
			do {
				const std::wstring name = data.cFileName;
				if (name == L"." || name == L"..") {
					continue;
				}
				const std::wstring path = dir + L"\\" + name;
				if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
					Collect(path, out);
				}
				else if (IsMusic(name)) {
					out.push_back(path);
				}
			} while (FindNextFileW(find, &data));
			FindClose(find);
		}

		bool Before(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; }

		// The folder's own files, and each subfolder with everything below it, as separate groups.
		std::vector<std::pair<std::wstring, std::vector<std::wstring>>> Groups(const std::wstring& folder)
		{
			std::vector<std::pair<std::wstring, std::vector<std::wstring>>> groups(1);
			groups[0].first = folder;
			WIN32_FIND_DATAW data;
			HANDLE find = FindFirstFileW((folder + L"\\*").c_str(), &data);
			if (find == INVALID_HANDLE_VALUE) {
				return groups;
			}
			std::vector<std::wstring> subfolders;
			do {
				const std::wstring name = data.cFileName;
				if (name == L"." || name == L"..") {
					continue;
				}
				if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
					subfolders.push_back(name);
				}
				else if (IsMusic(name)) {
					groups[0].second.push_back(folder + L"\\" + name);
				}
			} while (FindNextFileW(find, &data));
			FindClose(find);

			std::sort(subfolders.begin(), subfolders.end(), Before);
			for (const std::wstring& name : subfolders) {
				std::vector<std::wstring> paths;
				Collect(folder + L"\\" + name, paths);
				groups.emplace_back(folder + L"\\" + name, std::move(paths));
			}
			for (auto& group : groups) {
				std::sort(group.second.begin(), group.second.end(), Before);
			}
			return groups;
		}

		void ScanTracks(const std::vector<std::wstring>& paths, std::vector<Track>& tracks)
		{
			for (const std::wstring& path : paths) {
				if (tracks.size() == bank::kMaxTracks) {
					LOG("library: stopping at %u tracks (the game stores the track index in a byte)", bank::kMaxTracks);
					break;
				}

				decoder::Info info;
				if (!decoder::Probe(path, info) || info.mFrames == 0) {
					LOG("library: skipping %s (no decoder for it, or no length)", logger::ToUtf8(path.c_str()).c_str());
					continue;
				}

				Track t;
				t.mPath = path;
				t.mArtist = info.mTags.mArtist;
				t.mTitle = info.mTags.mTitle;
				t.mSampleRate = info.mSampleRate;
				t.mFrames = info.mFrames;
				t.mLayout = info.mLayout;
				if (t.mTitle.empty()) {
					const size_t slash = path.find_last_of(L"\\/");
					std::wstring name = path.substr(slash + 1);
					name = name.substr(0, name.find_last_of(L'.'));
					t.mTitle = logger::ToUtf8(name.c_str());
				}

				// Stereo (and mono, played as stereo) as before; any other layout named, and how it's passed on.
				std::string channels;
				if (info.mSource.mChannels > 2) {
					char a[16], b[16];
					channels = std::string(", ") + layout::Name(info.mSource, a);
					channels += info.mLayout == info.mSource ? "" : std::string(" as ") + layout::Name(info.mLayout, b);
				}
				LOG("library: %3zu  %s - %s  (%s, %u Hz, %.1f s%s)", tracks.size() + 1, t.mArtist.c_str(), t.mTitle.c_str(), info.mBackend,
					t.mSampleRate, static_cast<double>(t.mFrames) / t.mSampleRate, channels.c_str());
				tracks.push_back(std::move(t));
			}
		}

		void Scan(std::wstring folder)
		{
			// Only the Media Foundation fallback needs COM.
			CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			const DWORD start = GetTickCount();

			auto groups = Groups(folder);
			size_t files = 0, tracks = 0;
			for (const auto& group : groups) {
				files += group.second.size();
			}
			LOG("library: %zu music files in %s, %zu subfolders", files, logger::ToUtf8(folder.c_str()).c_str(), groups.size() - 1);

			for (auto& [dir, paths] : groups) {
				Station station;
				station.mFolder = dir;
				if (dir.size() > folder.size()) {
					station.mName = logger::ToUtf8(dir.c_str() + folder.size() + 1);
				}
				if (!paths.empty()) {
					LOG("library: station \"%s\": %zu music files", station.mName.empty() ? "(music folder)" : station.mName.c_str(), paths.size());
				}
				ScanTracks(paths, station.mTracks);
				if (!station.mTracks.empty()) {
					tracks += station.mTracks.size();
					gStations.push_back(std::move(station));
				}
			}

			LOG("library: %zu tracks ready (%zu stations) in %lu ms", tracks, gStations.size(), GetTickCount() - start);
			CoUninitialize();
			SetEvent(gDone);
		}
	}

	void StartScan(const std::wstring& folder)
	{
		std::thread(Scan, folder).detach();
	}

	const std::vector<Station>* Wait(DWORD timeoutMs)
	{
		return WaitForSingleObject(gDone, timeoutMs) == WAIT_OBJECT_0 ? &gStations : nullptr;
	}
}

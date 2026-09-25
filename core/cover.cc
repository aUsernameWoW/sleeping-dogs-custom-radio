#include "cover.hh"

#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

#include "decoder.hh"
#include "log.hh"

namespace cover
{
	namespace
	{
		constexpr const wchar_t* kNames[] = { L"cover", L"folder", L"front", L"logo" };
		constexpr const wchar_t* kExtensions[] = { L".jpg", L".jpeg", L".png", L".bmp", L".gif" };
		constexpr size_t kMaxFileSize = 64u << 20;

		std::vector<std::wstring> gTracks;
		std::wstring gRoot;

		std::mutex gLock;
		std::condition_variable gWake;
		uint32_t gRequested = 0; // track waiting to be prepared, 0 = none
		std::shared_ptr<const coverimage::Image> gCurrent;
		std::atomic<uint32_t> gGeneration{ 0 };

		// Folder pictures by path: an album's tracks share one.
		std::map<std::wstring, std::shared_ptr<const coverimage::Image>> gFolderCache;
		// Results by track: the game re-posts the playing track on every switch back to the station.
		std::map<uint32_t, std::pair<std::shared_ptr<const coverimage::Image>, std::string>> gTrackCache;
		constexpr size_t kTrackCacheSize = 16; // ~180 KB each

		std::vector<uint8_t> ReadWholeFile(const std::wstring& path)
		{
			std::vector<uint8_t> data;
			HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				return data;
			}
			LARGE_INTEGER size{};
			if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && static_cast<uint64_t>(size.QuadPart) <= kMaxFileSize) {
				data.resize(static_cast<size_t>(size.QuadPart));
				DWORD read = 0;
				if (!ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) || read != data.size()) {
					data.clear();
				}
			}
			CloseHandle(file);
			return data;
		}

		std::wstring Parent(const std::wstring& path)
		{
			const size_t slash = path.find_last_of(L"\\/");
			return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
		}

		// The first picture file in `dir` or its parents, stopping at the music folder. `what` names the source.
		std::shared_ptr<const coverimage::Image> FolderPicture(std::wstring dir, std::string& what)
		{
			while (!dir.empty()) {
				for (const wchar_t* name : kNames) {
					for (const wchar_t* ext : kExtensions) {
						const std::wstring path = dir + L"\\" + name + ext;
						if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
							continue;
						}
						what = logger::ToUtf8(path.c_str());
						if (auto it = gFolderCache.find(path); it != gFolderCache.end()) {
							return it->second;
						}
						const std::vector<uint8_t> data = ReadWholeFile(path);
						auto image = coverimage::FromEncoded(data.data(), data.size());
						if (!image) {
							LOG("cover: %s (%s) doesn't decode, skipped", what.c_str(), coverimage::FormatName(data.data(), data.size()));
							continue;
						}
						gFolderCache[path] = image;
						return image;
					}
				}
				if (dir.size() <= gRoot.size()) {
					break;
				}
				dir = Parent(dir);
			}
			return nullptr;
		}

		void Publish(std::shared_ptr<const coverimage::Image> image)
		{
			std::lock_guard lock(gLock);
			if (image == gCurrent) {
				return; // same album art: nothing to upload
			}
			gCurrent = std::move(image);
			gGeneration.fetch_add(1);
		}

		void Prepare(uint32_t track)
		{
			if (auto it = gTrackCache.find(track); it != gTrackCache.end()) {
				LOG("cover: track %u: %s (cached)", track, it->second.second.c_str());
				Publish(it->second.first);
				return;
			}
			const DWORD start = GetTickCount();
			const std::wstring& path = gTracks[track - 1];
			std::string what;
			std::shared_ptr<const coverimage::Image> image;

			std::vector<uint8_t> embedded;
			if (decoder::ReadPicture(path, embedded)) {
				image = coverimage::FromEncoded(embedded.data(), embedded.size());
				if (image) {
					what = std::string("embedded ") + coverimage::FormatName(embedded.data(), embedded.size());
				}
				else {
					LOG("cover: track %u: embedded picture (%s, %zu bytes) doesn't decode", track,
						coverimage::FormatName(embedded.data(), embedded.size()), embedded.size());
				}
			}
			if (!image) {
				image = FolderPicture(Parent(path), what);
			}
			if (!image) {
				image = coverimage::Blank();
				what = "none (blank logo)";
			}

			if (image == coverimage::Blank()) {
				LOG("cover: track %u: %s", track, what.c_str());
			}
			else {
				LOG("cover: track %u: %s, %ux%u, ready in %lu ms", track, what.c_str(), image->mSourceWidth, image->mSourceHeight, GetTickCount() - start);
			}
			if (gTrackCache.size() >= kTrackCacheSize) {
				gTrackCache.clear();
			}
			gTrackCache[track] = { image, what };
			Publish(std::move(image));
		}

		void Worker()
		{
			std::string what;
			auto logo = FolderPicture(gRoot, what);
			if (logo) {
				LOG("cover: station logo %s, %ux%u", what.c_str(), logo->mSourceWidth, logo->mSourceHeight);
			}
			Publish(logo ? logo : coverimage::Blank());

			for (;;) {
				uint32_t track;
				{
					std::unique_lock lock(gLock);
					gWake.wait(lock, [] { return gRequested != 0; });
					track = gRequested;
					gRequested = 0;
				}
				Prepare(track);
			}
		}
	}

	void Start(std::vector<std::wstring> tracks, std::wstring root)
	{
		gTracks = std::move(tracks);
		gRoot = std::move(root);
		std::thread(Worker).detach();
	}

	void Request(uint32_t track)
	{
		if (track == 0 || track > gTracks.size()) {
			return;
		}
		{
			std::lock_guard lock(gLock);
			gRequested = track; // a newer request replaces one not started yet
		}
		gWake.notify_one();
	}

	Current Get()
	{
		std::lock_guard lock(gLock);
		return { gCurrent, gGeneration.load() };
	}

	uint32_t Generation()
	{
		return gGeneration.load(std::memory_order_relaxed);
	}
}

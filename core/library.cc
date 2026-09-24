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
		std::vector<Track> gTracks;
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

		void Scan(std::wstring folder)
		{
			// Only the Media Foundation fallback needs COM.
			CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			const DWORD start = GetTickCount();

			std::vector<std::wstring> paths;
			Collect(folder, paths);
			std::sort(paths.begin(), paths.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
			LOG("library: %zu music files in %s", paths.size(), logger::ToUtf8(folder.c_str()).c_str());

			for (const std::wstring& path : paths) {
				if (gTracks.size() == bank::kMaxTracks) {
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
				if (t.mTitle.empty()) {
					const size_t slash = path.find_last_of(L"\\/");
					std::wstring name = path.substr(slash + 1);
					name = name.substr(0, name.find_last_of(L'.'));
					t.mTitle = logger::ToUtf8(name.c_str());
				}

				LOG("library: %3zu  %s - %s  (%s, %u Hz, %.1f s)", gTracks.size() + 1, t.mArtist.c_str(), t.mTitle.c_str(), info.mBackend,
					t.mSampleRate, static_cast<double>(t.mFrames) / t.mSampleRate);
				gTracks.push_back(std::move(t));
			}

			LOG("library: %zu tracks ready in %lu ms", gTracks.size(), GetTickCount() - start);
			CoUninitialize();
			SetEvent(gDone);
		}
	}

	void StartScan(const std::wstring& folder)
	{
		std::thread(Scan, folder).detach();
	}

	const std::vector<Track>* Wait(DWORD timeoutMs)
	{
		return WaitForSingleObject(gDone, timeoutMs) == WAIT_OBJECT_0 ? &gTracks : nullptr;
	}
}

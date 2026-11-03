#pragma once

// The music folder: every playable file in it, sorted by path, with the tags and stream format a station
// needs before anything is decoded, grouped into stations: the files directly in the folder are one, each
// subfolder (with everything below it) another, named after the subfolder. Scanned once on a background
// thread started from DllMain; the Radios.xml hook waits for it.

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace library
{
	struct Track
	{
		std::wstring mPath;
		std::string mArtist; // UTF-8; may be empty
		std::string mTitle;  // UTF-8; the file name if the file has no title tag
		uint32_t mSampleRate = 0;
		uint64_t mFrames = 0;
	};

	struct Station
	{
		std::wstring mFolder;
		std::string mName; // UTF-8 subfolder name; empty for the music folder's own files
		std::vector<Track> mTracks; // at most bank::kMaxTracks, never empty
	};

	void StartScan(const std::wstring& folder);

	// The scan result: the music folder's own station first (if it has music), then the subfolders' in name
	// order; nullptr if it isn't finished within `timeoutMs`.
	const std::vector<Station>* Wait(DWORD timeoutMs);
}

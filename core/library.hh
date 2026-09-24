#pragma once

// The music folder: every playable file in it (subfolders included), sorted by path, with the tags and
// stream format the station needs before anything is decoded. Scanned once on a background thread started
// from DllMain; the Radios.xml hook waits for it.

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

	void StartScan(const std::wstring& folder);

	// The scan result, or nullptr if it isn't finished within `timeoutMs`.
	const std::vector<Track>* Wait(DWORD timeoutMs);
}

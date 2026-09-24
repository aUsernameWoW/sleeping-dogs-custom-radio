#pragma once

// Files that exist only for Wwise: the generated station bank and one WAV stream per track, served through
// the game's low-level I/O hooks (see hooks.cc). Wwise opens them by ID like any file in SFX.pck and reads
// them asynchronously; a track is decoded on its own thread when it is opened, and reads complete (on the
// completion thread) as soon as the decoder has produced the bytes they ask for.
//
// A track file is a RIFF/WAVE image: WAVE_FORMAT_EXTENSIBLE (CAkSrcFilePCM::ParseHeader rejects anything
// else), 16-bit stereo at the file's own rate, a data chunk sized from the scanned duration. If the decoder
// yields less, the rest is silence; if more, the end is cut.

#include <cstdint>
#include <string>
#include <vector>

#include "ak.hh"

namespace streamio
{
	constexpr uint32_t kWavHeaderSize = 68; // RIFF(12) + fmt (8 + 40) + data header (8)

	struct TrackSource
	{
		uint32_t mFileId;
		std::wstring mPath;
		uint32_t mSampleRate;
		uint64_t mFrames;
	};

	// Registers what can be opened. Called once, before Wwise can ask for any of it.
	void Register(uint32_t bankId, std::vector<uint8_t> bank, std::vector<TrackSource> tracks);

	bool IsOurs(uint32_t fileId);

	// Fills `desc` for one of our files. `deviceId` is the game's streaming device.
	bool Open(uint32_t fileId, uint32_t deviceId, ak::FileDesc& desc);

	// Both return false if `file` isn't one of ours (the hooks then call the game's function).
	bool Read(void* file, ak::AsyncIOTransferInfo& info);
	bool Close(void* file);

	// The WAV header for `frames` stereo 16-bit frames at `rate` (exposed for the tests).
	void WriteWavHeader(uint8_t* out, uint32_t rate, uint64_t frames);
}

#pragma once

// Capturing what another app plays (WASAPI). Two ways:
// - process loopback (Windows 10 build 20348+): only one process tree's audio, no virtual cable needed;
// - endpoint loopback: everything a playback device plays (the way FH6 Universal Radio does it, from which the
//   endpoint code and the format conversion in pcm.cc are adapted: https://github.com/g0ldyy/fh6-universal-radio,
//   src/sources/external_audio_source.cpp, GPL-3.0). Under the GPL-3.0 (LICENSE).

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace capture
{
	// Receives 48 kHz 16-bit stereo samples (interleaved) on the capture thread.
	using Sink = std::function<void(const int16_t* samples, size_t count)>;

	struct Options
	{
		DWORD mProcessId = 0; // process loopback of this process tree; 0 = endpoint loopback
		std::string mEndpoint; // endpoint loopback: part of a playback device's name or ID; empty = default device
	};

	// Captures on the calling thread until `stop` is set or an error occurs (returned in `error`). COM must be
	// initialized on the thread (multithreaded apartment).
	bool Run(const Options& options, const std::atomic<bool>& stop, const Sink& sink, std::string& error);

	// The process whose executable is `exeName` (case-insensitive) and whose parent isn't: the root of the
	// app's process tree (browsers and Spotify play from a child process). 0 if none.
	DWORD FindProcess(const std::wstring& exeName);

	// `root` and all its descendants.
	std::vector<DWORD> ProcessTree(DWORD root);

	// The app's audio session(s) on the default playback device as the volume mixer shows them.
	struct SessionState
	{
		int mCount = 0; // sessions found for the process tree
		float mVolume = 0.0f; // of the first one
		bool mMuted = false;
		bool mActive = false; // any of them currently playing
	};
	SessionState AppSessions(const std::vector<DWORD>& processes);
}

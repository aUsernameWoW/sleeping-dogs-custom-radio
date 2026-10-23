#pragma once

// The FIFO between internet radio decoding and the live ring (netradio.hh): once kStartMs have collected
// (kRebufferMs after an underrun) it hands over what it has, up to the live reader's preroll, at once, and the
// rest by the clock; titles wait for their audio. Pure, and not thread-safe: netradio guards it with a lock.
// See pacer_test.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace netradio
{
	// As much as the live reader's preroll (live::kPrerollMs), all of which then goes out at once.
	constexpr uint32_t kStartMs = 3'000;
	constexpr uint32_t kRebufferMs = 1'000;
	constexpr uint32_t kMaxBufferedMs = 15'000;

	class Pacer
	{
	public:
		// 48 kHz stereo samples (interleaved).
		void Add(const int16_t* samples, size_t count);
		// A title that applies from the audio added next on. With up to kMaxBufferedMs queued, the ICY
		// metadata is that far ahead of what is heard, so titles wait for their audio (TakeTitle).
		void Mark(std::string title);
		// Moves the samples due by `nowMs` into `out` (appending); 0 while buffering.
		size_t Take(uint64_t nowMs, std::vector<int16_t>& out);
		// The latest title whose audio has been taken, if it changed since the last call.
		bool TakeTitle(std::string& title);
		uint64_t BufferedMs() const;
		bool Playing() const { return mPlaying; }
		uint32_t Underruns() const { return mUnderruns; }

	private:
		std::deque<int16_t> mFifo;
		bool mPlaying = false;
		bool mStarted = false; // played before: rebuffering waits for kRebufferMs, not kStartMs
		uint64_t mStartMs = 0;
		uint64_t mSentFrames = 0; // since playing (re)started
		uint64_t mHeadFrames = 0; // released at once when playing (re)started
		uint64_t mAddedTotal = 0; // frames, ever
		uint64_t mTakenTotal = 0;
		std::deque<std::pair<uint64_t, std::string>> mTitles; // (from frame, title)
		std::string mCurrent;
		bool mTitleChanged = false;
		uint32_t mUnderruns = 0;
	};
}

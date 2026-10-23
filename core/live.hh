#pragma once

// The live station: audio that arrives in real time (a test signal now; internet radio, another app's audio
// later) played as an endless track. A source pushes 48 kHz 16-bit stereo into a ring; the station's track
// files are WAV images whose data chunk is as long as a WAV can say (~6.2 h), and Wwise's reads of them are
// served from the ring instead of the file position (Reader).
//
// Latency: a stream starts kPrerollMs behind the newest audio. Wwise keeps ~0.4 s read ahead of playback
// (AkDeviceSettings' default target buffer), so the rest of the preroll absorbs the source's jitter. A read
// the source can't satisfy within kMaxWaitMs completes with what there is plus silence, rather than starving
// the voice; a reader more than kMaxLagMs behind (after a stall) jumps back to the preroll distance.

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace live
{
	constexpr uint32_t kRate = 48000;
	constexpr uint32_t kBlockAlign = 4;
	constexpr uint32_t kHeaderSize = 68; // as streamio::kWavHeaderSize
	// The longest data chunk a RIFF size field allows, in whole frames: ~6.2 h. The game then starts the
	// station's other track, which plays the same stream.
	constexpr uint64_t kFrames = (0xFFFFFFFFull - (kHeaderSize - 8)) / kBlockAlign;

	constexpr uint32_t kRingMs = 10'000;
	constexpr uint32_t kPrerollMs = 1'000;
	constexpr uint32_t kMaxWaitMs = 250;
	constexpr uint32_t kMaxLagMs = 3'000;

	constexpr uint64_t MsToBytes(uint64_t ms) { return ms * kRate / 1000 * kBlockAlign; }

	// The ring every live track reads from. Positions are absolute byte counts since the source started.
	class Ring
	{
	public:
		explicit Ring(uint64_t capacity = MsToBytes(kRingMs));

		void Push(const int16_t* samples, size_t count);
		uint64_t Written() const;
		// Copies [from, from + size); false (nothing copied) if part of it is no longer or not yet in the ring.
		bool Copy(uint64_t from, uint8_t* out, size_t size) const;
		// Back to empty, position 0 (a new source).
		void Reset();
		// Called after every push (the stream completion thread's wake-up).
		void SetListener(std::function<void()> listener);

	private:
		mutable std::mutex mLock;
		std::vector<uint8_t> mData;
		uint64_t mWritten = 0;
		std::function<void()> mListener;
	};

	// One open live track file: maps Wwise's reads to the ring. Used from one thread (stream completion).
	class Reader
	{
	public:
		Reader(const Ring& ring, const uint8_t* header);

		// Whether a read of `size` bytes at file `position` can complete now: its audio is in the ring, or it
		// waited `waitedMs` >= kMaxWaitMs. Reads must be asked and filled in the order Wwise issued them.
		bool Ready(uint64_t position, uint32_t size, DWORD waitedMs);
		// Fills `out` for a read that is Ready (header bytes, ring audio, silence for what isn't there).
		void Fill(uint64_t position, uint32_t size, uint8_t* out);

		struct Stats
		{
			uint32_t mReads = 0;
			uint64_t mSilenceBytes = 0; // padded because the source was late
			uint32_t mJumps = 0;        // fell behind (or out of the ring) and skipped ahead
			uint32_t mSeeks = 0;        // reads not following the previous one (the game's resume seek)
		};
		const Stats& GetStats() const { return mStats; }

	private:
		// Starts or realigns the cursor so its byte phase matches the file position's frame phase.
		void Prepare(uint64_t position);

		const Ring& mRing;
		uint8_t mHeader[kHeaderSize];
		bool mStarted = false;
		uint64_t mCursor = 0;
		uint64_t mExpected = 0; // file position the next read would have if Wwise reads straight on
		Stats mStats;
	};

	// The ring the station plays.
	Ring& Stream();

	// Picks the source by its config name ("test"); false if there's no such source.
	bool Configure(const std::string& source);
	std::string SourceName();

	// A live file was opened / closed. The source runs while any is open, and stops 10 s after the last
	// closes (the game reopens the stream on every track start and station switch).
	void Acquire();
	void Release();
}

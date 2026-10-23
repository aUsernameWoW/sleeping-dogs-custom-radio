#pragma once

// The live station: audio that arrives in real time (a test signal now; internet radio, another app's audio
// later) played as an endless track. A source pushes 48 kHz 16-bit stereo into a ring; the station's track
// files are WAV images whose data chunk lasts an hour (see kFrames), and Wwise's reads of them are
// served from the ring instead of the file position (Reader).
//
// How the game reads it (in game, 2026-10-23): 128 KB (682 ms) per read; at open the header, then a seek to the
// game's track timer, then a burst of 4 reads (~2.7 s), then one read every ~0.69 s. Wwise's buffer is the
// cushion against a late source, and it only fills if that first burst finds audio: so a stream starts
// kPrerollMs (more than the burst) behind the newest audio, and its reads wait up to kStartWaitMs for that
// much to be there. Without it (internet radio, 2026-10-23) the burst found an empty ring, the stream ran at
// the live edge with every read waiting a whole chunk's 0.68 s, and playback stuttered. After the start, a read
// the source can't satisfy within kMaxWaitMs completes with what there is plus silence rather than starving
// the voice; a reader more than kMaxLagMs behind (after a stall) jumps back to the preroll distance.

#include <Windows.h>

#include <atomic>
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
	// One hour (~690 MB of data); the game then starts the station's other track, which plays the same stream.
	// The first try declared the longest data chunk a RIFF allows (~4 GB, ~6.2 h): in game Wwise read it only
	// 9 times in 87 s and played a looping, dial-up-like noise, presumably a size or position overflowing a
	// signed 32-bit value, so this stays far below 2 GB.
	constexpr uint64_t kFrames = uint64_t{ kRate } * 3600;

	constexpr uint32_t kRingMs = 10'000;
	constexpr uint32_t kPrerollMs = 3'000;
	// A source just started (internet radio: connecting, then its own buffer) gets this long to deliver the
	// preroll before the stream starts with silence instead.
	constexpr uint32_t kStartWaitMs = 10'000;
	constexpr uint32_t kMaxWaitMs = 1'500;
	constexpr uint32_t kMaxLagMs = 6'000;

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
		// How far the reader is behind the newest audio, in bytes.
		uint64_t Lead() const;

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

	// A source runs on its own thread, pushes into Stream() at real-time pace, and returns once `stop` is set.
	using Source = std::function<void(const std::atomic<bool>& stop)>;

	// The station's source (set before the station can play) and its name for the log.
	void SetSource(std::string name, Source source);
	std::string SourceName();

	// The "test" source: every second a 0.15 s beep, 880 Hz on the first beat of four and 440 Hz on the
	// others, at -12 dBFS.
	void TestSignal(const std::atomic<bool>& stop);

	// A live file was opened / closed. The source runs while any is open, and stops 10 s after the last
	// closes (the game reopens the stream on every track start and station switch).
	void Acquire();
	void Release();
}

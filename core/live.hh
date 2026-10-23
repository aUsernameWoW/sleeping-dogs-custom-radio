#pragma once

// The live station: audio that arrives in real time (a test signal now; internet radio, another app's audio
// later) played as an endless track. A source pushes 48 kHz 16-bit stereo into a ring; the station's track
// files are WAV images whose data chunk lasts an hour (see kFrames), and Wwise's reads of them are
// served from the ring instead of the file position (Reader).
//
// How the game reads it (in game, 2026-10-23): 128 KB (682 ms) per read; at open the header, then a seek to the
// game's track timer, then a burst of 4 reads (~2.7 s), then one read every ~0.69 s. Reads complete at once,
// never waiting for the source: a pending transfer holds one of the Wwise device's concurrent I/O slots, which
// every game stream and bank load on that device shares (CAkIOThread::IncrementIOCount closes the I/O gate at
// uMaxConcurrentIO). Reads that waited for audio (up to 10 s at the start, ~0.4 s each in steady state at the
// live edge, 1.5 s each while the network was down) made the game hitch and, once they piled up, kept the next
// station's bank from loading, so the station couldn't be switched (2026-10-23). So the cushion is in the
// ring instead: a stream starts kPrerollMs behind the newest audio (more than Wwise's burst plus a read), a
// read the ring can't fill gets what there is plus silence and the reader rebuffers until kResumeMs are
// there, then goes on where it stopped; a reader more than kMaxLagMs behind jumps back to the preroll distance.
// A source still starting reads as silence until kResumeMs have arrived.

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
	// Wwise's opening burst takes ~3.4 s of it at once, which leaves ~1.6 s for the steady reads (one needs 0.68).
	constexpr uint32_t kPrerollMs = 5'000;
	constexpr uint32_t kResumeMs = 2'000;
	constexpr uint32_t kMaxLagMs = 8'000;

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

	private:
		mutable std::mutex mLock;
		std::vector<uint8_t> mData;
		uint64_t mWritten = 0;
	};

	// One open live track file: maps Wwise's reads to the ring. Used from one thread (stream completion).
	class Reader
	{
	public:
		Reader(const Ring& ring, const uint8_t* header);

		// Fills `out` for a read of `size` bytes at file `position` (header bytes, ring audio, silence for what
		// isn't there), never waiting. Reads must be filled in the order Wwise issued them.
		void Fill(uint64_t position, uint32_t size, uint8_t* out);

		struct Stats
		{
			uint32_t mReads = 0;
			uint64_t mSilenceBytes = 0; // padded because the source was late
			uint32_t mJumps = 0;        // fell behind (or out of the ring) and skipped ahead
			uint32_t mSeeks = 0;        // reads not following the previous one (the game's resume seek)
			uint32_t mUnderruns = 0;    // reads the ring couldn't fill: rebuffered
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
		bool mPlaying = false; // false while rebuffering
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

	// What the source plays now, for the HUD ("Artist - Title" in UTF-8; empty if it hasn't said), and a counter
	// that changes with every new title. Cleared when a source starts.
	void SetNowPlaying(std::string title);
	std::string NowPlaying();
	uint32_t NowPlayingGeneration();

	// A live file was opened / closed. The source runs while any is open, and stops 10 s after the last
	// closes (the game reopens the stream on every track start and station switch).
	void Acquire();
	void Release();
}

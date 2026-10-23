#include "live.hh"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>

#include "log.hh"

namespace live
{
	Ring::Ring(uint64_t capacity) : mData(static_cast<size_t>(capacity)) {}

	void Ring::Push(const int16_t* samples, size_t count)
	{
		std::function<void()> listener;
		{
			std::lock_guard lock(mLock);
			const auto* bytes = reinterpret_cast<const uint8_t*>(samples);
			size_t size = count * sizeof(int16_t);
			const size_t capacity = mData.size();
			if (size > capacity) {
				// Only the newest `capacity` bytes can stay.
				bytes += size - capacity;
				mWritten += size - capacity;
				size = capacity;
			}
			const size_t at = static_cast<size_t>(mWritten % capacity);
			const size_t first = (std::min)(size, capacity - at);
			std::memcpy(mData.data() + at, bytes, first);
			std::memcpy(mData.data(), bytes + first, size - first);
			mWritten += size;
			listener = mListener;
		}
		if (listener) {
			listener();
		}
	}

	uint64_t Ring::Written() const
	{
		std::lock_guard lock(mLock);
		return mWritten;
	}

	bool Ring::Copy(uint64_t from, uint8_t* out, size_t size) const
	{
		std::lock_guard lock(mLock);
		const size_t capacity = mData.size();
		if (from + size > mWritten || mWritten - from > capacity) {
			return false;
		}
		const size_t at = static_cast<size_t>(from % capacity);
		const size_t first = (std::min)(size, capacity - at);
		std::memcpy(out, mData.data() + at, first);
		std::memcpy(out + first, mData.data(), size - first);
		return true;
	}

	void Ring::Reset()
	{
		std::lock_guard lock(mLock);
		mWritten = 0;
	}

	void Ring::SetListener(std::function<void()> listener)
	{
		std::lock_guard lock(mLock);
		mListener = std::move(listener);
	}

	Reader::Reader(const Ring& ring, const uint8_t* header) : mRing(ring)
	{
		std::memcpy(mHeader, header, kHeaderSize);
	}

	void Reader::Prepare(uint64_t position)
	{
		const uint64_t written = mRing.Written();
		const uint64_t preroll = MsToBytes(kPrerollMs);
		if (!mStarted || written - (std::min)(written, mCursor) > MsToBytes(kMaxLagMs)) {
			if (mStarted) {
				++mStats.mJumps;
			}
			mCursor = written > preroll ? (written - preroll) & ~uint64_t{ kBlockAlign - 1 } : 0;
			mStarted = true;
		}
		// Wwise counts frames from the data chunk; after a seek to a frame boundary the ring must be at one too.
		if (position >= kHeaderSize) {
			const uint64_t phase = (position - kHeaderSize) % kBlockAlign;
			while (mCursor % kBlockAlign != phase) {
				++mCursor;
			}
		}
	}

	uint64_t Reader::Lead() const
	{
		const uint64_t written = mRing.Written();
		return written > mCursor ? written - mCursor : 0;
	}

	bool Reader::Ready(uint64_t position, uint32_t size, DWORD waitedMs)
	{
		const uint64_t end = position + size;
		if (end <= kHeaderSize) {
			return true;
		}
		Prepare((std::max)(position, uint64_t{ kHeaderSize }));
		const uint64_t need = end - (std::max)(position, uint64_t{ kHeaderSize });
		return mRing.Written() >= mCursor + need || waitedMs >= kMaxWaitMs;
	}

	void Reader::Fill(uint64_t position, uint32_t size, uint8_t* out)
	{
		++mStats.mReads;
		if (position != mExpected) {
			++mStats.mSeeks;
		}
		mExpected = position + size;

		uint64_t at = position;
		const uint64_t end = position + size;
		if (at < kHeaderSize) {
			const uint64_t n = (std::min)(end, uint64_t{ kHeaderSize }) - at;
			std::memcpy(out, mHeader + at, static_cast<size_t>(n));
			out += n;
			at += n;
		}
		if (at >= end) {
			return;
		}

		Prepare(at);
		const uint64_t need = end - at;
		const uint64_t written = mRing.Written();
		uint64_t have = written > mCursor ? (std::min)(need, written - mCursor) : 0;
		if (have && !mRing.Copy(mCursor, out, static_cast<size_t>(have))) {
			// Overwritten meanwhile (can't happen with a 10 s ring unless the reader stalled): start over.
			++mStats.mJumps;
			mStarted = false;
			have = 0;
		}
		mCursor += have;
		if (have < need) {
			std::memset(out + have, 0, static_cast<size_t>(need - have));
			mStats.mSilenceBytes += need - have;
		}
	}

	namespace
	{
		Ring gStream;

		std::mutex gLock;
		std::string gSourceName;
		Source gSource;
		int gOpen = 0;
		DWORD gLastClose = 0;
		bool gManagerRunning = false;
		// Each run gets its own stop flag: a source still winding down (a network read can take a while) must
		// not be revived by the next start.
		std::shared_ptr<std::atomic<bool>> gStop;
		std::thread gThread;

		// Under gLock.
		void StartSource()
		{
			if (gThread.joinable() || !gSource) {
				return;
			}
			gStream.Reset();
			gStop = std::make_shared<std::atomic<bool>>(false);
			gThread = std::thread([source = gSource, stop = gStop] { source(*stop); });
			LOG("live: source \"%s\" started", gSourceName.c_str());
		}

		// Stops the source once nothing has listened for a while.
		void Manager()
		{
			for (;;) {
				Sleep(1000);
				std::thread finished;
				{
					std::lock_guard lock(gLock);
					if (gOpen == 0 && gThread.joinable() && GetTickCount() - gLastClose >= 10'000) {
						*gStop = true;
						finished = std::move(gThread);
					}
				}
				if (finished.joinable()) {
					finished.join();
					LOG("live: source \"%s\" stopped (nothing listening)", gSourceName.c_str());
				}
			}
		}
	}

	void TestSignal(const std::atomic<bool>& stop)
	{
		LARGE_INTEGER frequency, start, now;
		QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&start);
		uint64_t produced = 0;
		double phase = 0.0;
		std::vector<int16_t> chunk;
		while (!stop.load()) {
			Sleep(10);
			QueryPerformanceCounter(&now);
			const uint64_t due = static_cast<uint64_t>((now.QuadPart - start.QuadPart) * kRate / frequency.QuadPart);
			chunk.clear();
			for (; produced < due; ++produced) {
				const uint64_t inSecond = produced % kRate;
				const bool accent = (produced / kRate) % 4 == 0;
				float v = 0.0f;
				if (inSecond < kRate * 15 / 100) {
					// 5 ms fades so the beeps don't click.
					const float fade = (std::min)({ 1.0f, inSecond / 240.0f, (kRate * 15 / 100 - inSecond) / 240.0f });
					v = 0.25f * fade * static_cast<float>(std::sin(phase));
					phase += 2.0 * 3.14159265358979 * (accent ? 880.0 : 440.0) / kRate;
				}
				else {
					phase = 0.0;
				}
				const auto s = static_cast<int16_t>(v * 32767.0f);
				chunk.push_back(s);
				chunk.push_back(s);
			}
			if (!chunk.empty()) {
				gStream.Push(chunk.data(), chunk.size());
			}
		}
	}

	Ring& Stream()
	{
		return gStream;
	}

	void SetSource(std::string name, Source source)
	{
		std::lock_guard lock(gLock);
		gSourceName = std::move(name);
		gSource = std::move(source);
	}

	std::string SourceName()
	{
		std::lock_guard lock(gLock);
		return gSourceName;
	}

	void Acquire()
	{
		std::lock_guard lock(gLock);
		++gOpen;
		StartSource();
		if (!gManagerRunning) {
			gManagerRunning = true;
			std::thread(Manager).detach();
		}
	}

	void Release()
	{
		std::lock_guard lock(gLock);
		if (gOpen > 0) {
			--gOpen;
		}
		gLastClose = GetTickCount();
	}
}

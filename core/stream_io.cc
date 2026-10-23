#include "stream_io.hh"

#include <Windows.h>
#include <objbase.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "decoder.hh"
#include "live.hh"
#include "log.hh"

namespace streamio
{
	namespace
	{
		struct File
		{
			uint32_t mId = 0;
			const TrackSource* mTrack = nullptr; // null for the bank
			std::unique_ptr<live::Reader> mLive; // a live track: reads come from the ring, not mData
			uint64_t mSize = 0;
			std::unique_ptr<uint8_t[]> mData;
			std::atomic<uint64_t> mReady{ 0 }; // bytes [0, mReady) are valid
			std::atomic<bool> mDone{ false };  // nothing more will come; the rest reads as silence
			std::atomic<bool> mClosed{ false };
			uint32_t mReads = 0;
			DWORD mOpenedAt = 0;
			DWORD mLongestWait = 0;
		};

		struct Request
		{
			std::shared_ptr<File> mFile;
			ak::AsyncIOTransferInfo* mInfo;
			DWORD mQueuedAt;
			bool mFilled = false; // live reads are filled while gLock is held, in order
		};

		std::mutex gLock;
		std::condition_variable gWake;
		std::map<uint32_t, std::vector<uint8_t>> gBanks;
		std::map<uint32_t, TrackSource> gTracks;
		std::map<void*, std::shared_ptr<File>> gOpen;
		std::vector<Request> gPending;
		bool gCompleterRunning = false;

		void Complete(const Request& r, ak::AKRESULT result)
		{
			ak::AsyncIOTransferInfo& info = *r.mInfo;
			if (result == ak::kSuccess && !r.mFilled) {
				const File& f = *r.mFile;
				const uint64_t begin = info.mFilePosition;
				const uint64_t end = begin + info.mRequestedSize;
				const uint64_t ready = f.mReady.load(std::memory_order_acquire);
				auto* out = static_cast<uint8_t*>(info.mBuffer);
				const uint64_t valid = ready > begin ? (std::min)(end, ready) - begin : 0;
				if (valid) {
					std::memcpy(out, f.mData.get() + begin, static_cast<size_t>(valid));
				}
				if (valid < info.mRequestedSize) {
					std::memset(out + valid, 0, static_cast<size_t>(info.mRequestedSize - valid));
				}
			}
			info.mCallback(&info, result);
		}

		// Completes every pending read whose bytes are there. Wwise reads a stream ahead of playback in
		// chunks, so after the first few hundred milliseconds of decoding nothing ever waits here.
		void Completer()
		{
			std::vector<Request> ready;
			std::vector<std::string> lines;
			for (;;) {
				{
					std::unique_lock lock(gLock);
					gWake.wait(lock, [] { return !gPending.empty(); });
					for (auto it = gPending.begin(); it != gPending.end();) {
						File& f = *it->mFile;
						if (f.mLive) {
							// Never waits for the source (see live.hh: a pending read holds one of the device's I/O
							// slots). Consecutive audio from the ring, so filled in the order Wwise issued them.
							const ak::AsyncIOTransferInfo& info = *it->mInfo;
							const DWORD waited = GetTickCount() - it->mQueuedAt;
							const live::Reader::Stats before = f.mLive->GetStats();
							f.mLive->Fill(info.mFilePosition, info.mRequestedSize, static_cast<uint8_t*>(info.mBuffer));
							it->mFilled = true;
							live::NoteRead();
							// The first reads of each open, every 100th, underruns and jumps; logged after the lock
							// is released, since Wwise's I/O thread takes it in Read.
							const live::Reader::Stats& after = f.mLive->GetStats();
							const uint32_t n = after.mReads;
							const bool underrun = after.mUnderruns != before.mUnderruns;
							const bool jump = after.mJumps != before.mJumps;
							if (n <= 20 || n % 100 == 0 || underrun || jump) {
								char line[320];
								std::snprintf(line, sizeof(line),
									"live: read %u at %lu ms: position %llu, %u bytes (%llu ms), waited %lu ms, %llu ms silence, now %llu ms behind live%s%s",
									n, GetTickCount() - f.mOpenedAt, info.mFilePosition, info.mRequestedSize,
									static_cast<uint64_t>(info.mRequestedSize) * 1000 / (live::kRate * live::kBlockAlign), waited,
									(after.mSilenceBytes - before.mSilenceBytes) * 1000 / (live::kRate * live::kBlockAlign),
									f.mLive->Lead() * 1000 / (live::kRate * live::kBlockAlign),
									underrun ? " (source late: rebuffering)" : "", jump ? " (jumped ahead)" : "");
								lines.emplace_back(line);
							}
							if (waited > f.mLongestWait) f.mLongestWait = waited;
							ready.push_back(std::move(*it));
							it = gPending.erase(it);
							continue;
						}
						const uint64_t end = it->mInfo->mFilePosition + it->mInfo->mRequestedSize;
						if (f.mDone.load(std::memory_order_acquire) || f.mReady.load(std::memory_order_acquire) >= end) {
							const DWORD waited = GetTickCount() - it->mQueuedAt;
							if (waited > it->mFile->mLongestWait) it->mFile->mLongestWait = waited;
							ready.push_back(std::move(*it));
							it = gPending.erase(it);
						}
						else {
							++it;
						}
					}
					if (ready.empty()) {
						// Something is pending but not decoded yet: sleep until the decoder reports progress.
						gWake.wait_for(lock, std::chrono::milliseconds(50));
						continue;
					}
				}
				for (const Request& r : ready) {
					Complete(r, ak::kSuccess);
				}
				ready.clear();
				for (const std::string& line : lines) {
					LOG("%s", line.c_str());
				}
				lines.clear();
			}
		}

		void Decode(std::shared_ptr<File> file)
		{
			CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			const TrackSource& track = *file->mTrack;
			const DWORD start = GetTickCount();
			uint64_t notifiedAt = file->mReady.load();
			uint32_t rate = 0;

			const bool ok = decoder::Decode(track.mPath, rate, [&](const int16_t* frames, size_t count) {
				if (file->mClosed.load(std::memory_order_relaxed)) {
					return false;
				}
				const uint64_t at = file->mReady.load(std::memory_order_relaxed);
				const uint64_t bytes = (std::min)(static_cast<uint64_t>(count) * decoder::kBlockAlign, file->mSize - at);
				std::memcpy(file->mData.get() + at, frames, static_cast<size_t>(bytes));
				file->mReady.store(at + bytes, std::memory_order_release);
				if (at + bytes - notifiedAt >= 64 * 1024 || at + bytes == file->mSize) {
					notifiedAt = at + bytes;
					gWake.notify_one();
				}
				return at + bytes < file->mSize;
			});

			if (rate && rate != track.mSampleRate) {
				// The header already went out with the scanned rate: this track plays at the wrong speed.
				LOG("stream: %08X decodes at %u Hz but the scan said %u Hz; it will play off-speed", track.mFileId, rate, track.mSampleRate);
			}
			const uint64_t decoded = (file->mReady.load() - kWavHeaderSize) / decoder::kBlockAlign;
			LOG("stream: %08X decoded %llu of %llu frames in %lu ms%s", track.mFileId, decoded, track.mFrames, GetTickCount() - start,
				ok ? "" : " (decoder error, rest is silence)");

			{
				std::lock_guard lock(gLock);
				file->mDone.store(true, std::memory_order_release);
			}
			gWake.notify_one();
			CoUninitialize();
		}
	}

	void WriteWavHeader(uint8_t* out, uint32_t rate, uint64_t frames)
	{
		const uint32_t dataSize = static_cast<uint32_t>(frames * decoder::kBlockAlign);
		auto put32 = [&](size_t at, uint32_t v) { std::memcpy(out + at, &v, 4); };
		auto put16 = [&](size_t at, uint16_t v) { std::memcpy(out + at, &v, 2); };
		std::memcpy(out, "RIFF", 4);
		put32(4, kWavHeaderSize - 8 + dataSize);
		std::memcpy(out + 8, "WAVE", 4);
		std::memcpy(out + 12, "fmt ", 4);
		put32(16, 40);
		put16(20, 0xFFFE);                                // WAVE_FORMAT_EXTENSIBLE
		put16(22, static_cast<uint16_t>(decoder::kChannels));
		put32(24, rate);
		put32(28, rate * decoder::kBlockAlign);
		put16(32, static_cast<uint16_t>(decoder::kBlockAlign));
		put16(34, 16);
		put16(36, 22);                                    // cbSize
		put16(38, 16);                                    // valid bits
		put32(40, 0x3);                                   // SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT
		static constexpr uint8_t kSubtypePcm[16] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
		std::memcpy(out + 44, kSubtypePcm, 16);
		std::memcpy(out + 60, "data", 4);
		put32(64, dataSize);
	}

	void Register(uint32_t bankId, std::vector<uint8_t> bank, std::vector<TrackSource> tracks)
	{
		std::lock_guard lock(gLock);
		gBanks[bankId] = std::move(bank);
		for (TrackSource& t : tracks) {
			gTracks[t.mFileId] = std::move(t);
		}
	}

	bool IsOurs(uint32_t fileId)
	{
		std::lock_guard lock(gLock);
		return gBanks.count(fileId) || gTracks.count(fileId);
	}

	bool Open(uint32_t fileId, uint32_t deviceId, ak::FileDesc& desc)
	{
		auto file = std::make_shared<File>();
		file->mId = fileId;
		file->mOpenedAt = GetTickCount();
		{
			std::lock_guard lock(gLock);
			if (auto bank = gBanks.find(fileId); bank != gBanks.end()) {
				file->mSize = bank->second.size();
				file->mData.reset(new uint8_t[file->mSize]);
				std::memcpy(file->mData.get(), bank->second.data(), bank->second.size());
				file->mReady = file->mSize;
				file->mDone = true;
			}
			else {
				auto it = gTracks.find(fileId);
				if (it == gTracks.end()) {
					return false;
				}
				if (it->second.mLive) {
					file->mTrack = &it->second;
					file->mSize = kWavHeaderSize + live::kFrames * decoder::kBlockAlign;
					uint8_t header[kWavHeaderSize];
					WriteWavHeader(header, live::kRate, live::kFrames);
					file->mLive = std::make_unique<live::Reader>(live::Stream(), header);
					live::Acquire();
				}
				else {
					// A 4-minute track is ~42 MB; new[] of that size only reserves pages, so this is cheap on
					// whichever thread Wwise opens from. The decoder commits them as it goes.
					file->mTrack = &it->second;
					file->mSize = kWavHeaderSize + it->second.mFrames * decoder::kBlockAlign;
					file->mData.reset(new (std::nothrow) uint8_t[file->mSize]);
					if (!file->mData) {
						LOG("stream: out of memory for %08X (%llu bytes)", fileId, file->mSize);
						return false;
					}
					WriteWavHeader(file->mData.get(), it->second.mSampleRate, it->second.mFrames);
					file->mReady = kWavHeaderSize;
				}
			}
			gOpen[file.get()] = file;
			if (!gCompleterRunning) {
				gCompleterRunning = true;
				std::thread(Completer).detach();
			}
		}

		desc = {};
		desc.mFileSize = static_cast<int64_t>(file->mSize);
		desc.mCustomParamSize = 1; // block size 1, as for the files in SFX.pck
		desc.mFile = file.get();
		desc.mDeviceID = deviceId;

		if (file->mLive) {
			LOG("stream: open live track %08X (source \"%s\")", fileId, live::SourceName().c_str());
		}
		else if (file->mTrack) {
			LOG("stream: open track %08X %s (%llu bytes)", fileId, logger::ToUtf8(file->mTrack->mPath.c_str()).c_str(), file->mSize);
			std::thread(Decode, file).detach();
		}
		else {
			LOG("stream: open bank %08X (%llu bytes)", fileId, file->mSize);
		}
		return true;
	}

	bool Read(void* handle, ak::AsyncIOTransferInfo& info)
	{
		std::lock_guard lock(gLock);
		auto it = gOpen.find(handle);
		if (it == gOpen.end()) {
			return false;
		}
		File& f = *it->second;
		if (info.mFilePosition + info.mRequestedSize > f.mSize) {
			// Never seen from Wwise (it knows the size), but don't read past the buffer if it happens.
			info.mRequestedSize = info.mFilePosition < f.mSize ? static_cast<uint32_t>(f.mSize - info.mFilePosition) : 0;
		}
		++f.mReads;
		if (f.mLive && (f.mReads <= 20 || f.mReads % 100 == 0)) {
			LOG("live: request %u at %lu ms: position %llu, %u bytes", f.mReads, GetTickCount() - f.mOpenedAt, info.mFilePosition, info.mRequestedSize);
		}
		gPending.push_back({ it->second, &info, GetTickCount() });
		gWake.notify_one();
		return true;
	}

	bool Close(void* handle)
	{
		std::shared_ptr<File> file;
		std::vector<Request> orphans;
		{
			std::lock_guard lock(gLock);
			auto it = gOpen.find(handle);
			if (it == gOpen.end()) {
				return false;
			}
			file = std::move(it->second);
			gOpen.erase(it);
			file->mClosed = true;
			for (auto p = gPending.begin(); p != gPending.end();) {
				if (p->mFile == file) {
					orphans.push_back(std::move(*p));
					p = gPending.erase(p);
				}
				else {
					++p;
				}
			}
		}
		// Wwise closes only after its transfers completed; fail anything left rather than leave a caller hanging.
		for (const Request& r : orphans) {
			Complete(r, ak::kFail);
		}
		if (file->mLive) {
			live::Release();
			const live::Reader::Stats& s = file->mLive->GetStats();
			LOG("stream: live track %08X: %u reads, %llu ms of silence padded, %u underruns, %u jumps ahead, %u seeks", file->mId,
				s.mReads, s.mSilenceBytes * 1000 / (live::kRate * live::kBlockAlign), s.mUnderruns, s.mJumps, s.mSeeks);
		}
		LOG("stream: close %08X after %lu ms, %u reads, longest wait %lu ms%s", file->mId, GetTickCount() - file->mOpenedAt, file->mReads,
			file->mLongestWait, orphans.empty() ? "" : " (had pending reads)");
		// The decoder thread holds its own reference and stops at its next chunk.
		return true;
	}
}

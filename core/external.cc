#include "external.hh"

#include "capture.hh"
#include "config.hh"
#include "log.hh"
#include "media_session.hh"
#include "pcm.hh"

#include <Windows.h>
#include <objbase.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace external
{
	namespace
	{
		std::mutex gMeterMutex;
		pcm::Meter gMeter;
		std::atomic<bool> gStop{ false };

		void LogSessions()
		{
			const auto sessions = media::List();
			LOG("media: %zu session(s)", sessions.size());
			for (const auto& session : sessions) {
				LOG("media:   %s%s", session.mId.c_str(), session.mCurrent ? " (current)" : "");
			}
		}

		std::string Describe(const media::Track& track)
		{
			std::string text = track.mApp + " " + track.mStatus + ": " + track.mArtist + " - " + track.mTitle;
			if (track.mDurationMs) {
				char buffer[48];
				std::snprintf(buffer, sizeof(buffer), " (%llu/%llu s)", track.mPositionMs / 1000, track.mDurationMs / 1000);
				text += buffer;
			}
			return text;
		}

		// Captures until an error, retrying (the app may start, stop or come back later).
		void CaptureLoop()
		{
			CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			std::string lastError;
			while (!gStop.load()) {
				capture::Options options;
				options.mEndpoint = gConfig.mCaptureDevice;
				if (!gConfig.mCaptureProcess.empty()) {
					options.mProcessId = capture::FindProcess(gConfig.mCaptureProcess);
					if (!options.mProcessId) {
						const std::string error = "process " + logger::ToUtf8(gConfig.mCaptureProcess.c_str()) + " is not running";
						if (error != lastError) {
							LOG("capture: %s, waiting", error.c_str());
							lastError = error;
						}
						Sleep(5000);
						continue;
					}
				}
				std::string error;
				const bool ok = capture::Run(options, gStop, [](const int16_t* samples, size_t count) {
					std::lock_guard lock(gMeterMutex);
					gMeter.Add(samples, count);
				}, error);
				if (!ok && error != lastError) {
					LOG("capture: %s, retrying every 5 s", error.c_str());
					lastError = error;
				}
				Sleep(5000);
			}
		}

		void Diagnostics()
		{
			CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			LOG("external: diagnostics on (media session \"%s\", capture %s%s)", gConfig.mMediaSession.c_str(),
				gConfig.mCaptureProcess.empty() ? "playback device " : "process ",
				gConfig.mCaptureProcess.empty() ? (gConfig.mCaptureDevice.empty() ? "(default)" : gConfig.mCaptureDevice.c_str())
												 : logger::ToUtf8(gConfig.mCaptureProcess.c_str()).c_str());

			std::thread(CaptureLoop).detach();

			std::string error;
			const bool haveMedia = media::Open(error);
			if (haveMedia) {
				LogSessions();
			}
			else {
				LOG("media: no session manager (%s)", error.c_str());
			}

			// Every 5 s for the first 10 minutes, then every minute.
			std::string lastTrack;
			for (int tick = 0; !gStop.load(); ++tick) {
				Sleep(5000);
				if (tick >= 120 && tick % 12 != 0) {
					continue;
				}

				pcm::Meter meter;
				{
					std::lock_guard lock(gMeterMutex);
					meter = gMeter;
					gMeter.Reset();
				}
				std::string line;
				char buffer[160];
				std::snprintf(buffer, sizeof(buffer), "level: %.1f dBFS rms, %.1f peak over %llu samples", meter.RmsDb(), meter.PeakDb(),
					static_cast<unsigned long long>(meter.mSamples));
				line = buffer;

				if (!gConfig.mCaptureProcess.empty()) {
					const DWORD pid = capture::FindProcess(gConfig.mCaptureProcess);
					const capture::SessionState state = capture::AppSessions(capture::ProcessTree(pid));
					if (state.mCount) {
						std::snprintf(buffer, sizeof(buffer), "; mixer: %d session(s), volume %.2f, %s, %s", state.mCount, state.mVolume,
							state.mMuted ? "MUTED" : "not muted", state.mActive ? "active" : "inactive");
					}
					else {
						std::snprintf(buffer, sizeof(buffer), "; mixer: no session for PID %lu", pid);
					}
					line += buffer;
				}
				LOG("%s", line.c_str());

				if (haveMedia) {
					const auto track = media::Read(gConfig.mMediaSession);
					const std::string text = track ? Describe(*track) : std::string("no matching session");
					// Position aside, log only changes.
					const std::string key = track ? track->mApp + track->mStatus + track->mArtist + track->mTitle : text;
					if (key != lastTrack) {
						LOG("media: %s", text.c_str());
						lastTrack = key;
					}
				}
			}
		}
	}

	void Start()
	{
		if (gConfig.mExternal) {
			std::thread(Diagnostics).detach();
		}
	}
}

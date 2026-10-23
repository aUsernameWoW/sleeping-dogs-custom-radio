#pragma once

// Another app's audio as a live source (`[Live] Source = app`, `App = <exe name>`): WASAPI process loopback
// captures what that process (and its children) renders, as 48 kHz 16-bit stereo, into the live ring.
// Windows 10 build 20348+ only (in practice Windows 11): ActivateAudioInterfaceAsync with
// AUDIOCLIENT_ACTIVATION_PARAMS, looked up at run time so the .asi still loads where it's missing (Wine).
// Process loopback only delivers packets while the app renders, so silence is padded by the clock to keep
// the ring moving. Own code, from Microsoft's documentation of the API.

#include <Windows.h>
#include <audioclient.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace appaudio
{
	constexpr uint32_t kRate = 48000;
	constexpr uint32_t kChannels = 2;

	// The root of the process tree running `exeName` (case-insensitive, e.g. "cloudmusic.exe"): a process with
	// that name whose parent isn't one too. 0 if none runs.
	DWORD FindProcess(const std::wstring& exeName);

	// One process loopback capture. Use from one thread with COM initialized (any apartment).
	class Capture
	{
	public:
		Capture() = default;
		Capture(const Capture&) = delete;
		Capture& operator=(const Capture&) = delete;
		~Capture();

		// Starts capturing `pid` and its children. False with `error` if the system can't.
		bool Open(DWORD pid, std::string& error);
		// Waits up to `timeoutMs` for audio and appends what has arrived (interleaved stereo samples). False once
		// the capture failed (the audio service restarted...); `error` says why.
		bool Read(std::vector<int16_t>& out, DWORD timeoutMs, std::string& error);

	private:
		void Close();

		IAudioClient* mClient = nullptr;
		IAudioCaptureClient* mCapture = nullptr;
		HANDLE mEvent = nullptr;
	};

	// The live source: waits for the app, captures it, starts over when it exits; meanwhile now_playing.hh puts
	// the app's song on the HUD and, with `controlApp`, pauses the app while the station isn't played. Returns
	// once `stop` is set.
	void Run(const std::wstring& exeName, bool controlApp, const std::atomic<bool>& stop);
}

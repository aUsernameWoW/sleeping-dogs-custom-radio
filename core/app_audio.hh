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

	// One of the app's audio sessions, as the volume mixer shows it.
	struct Session
	{
		std::string mDevice;          // the output device's name (UTF-8)
		bool mDefaultDevice = false;  // the default output device, where the game plays too
		bool mActive = false;         // rendering now
		bool mMuted = false;
		float mVolume = 1.0f;         // the mixer's slider, 0..1
		DWORD mProcess = 0;
	};

	// The sessions of `pid` and its children on every active output device. Use with COM initialized.
	std::vector<Session> Sessions(DWORD pid);

	// The package family of a packaged (Store) app's process ("AppleInc.AppleMusicWin_nzyj5cx40ttqa"); empty
	// for an ordinary program.
	std::wstring PackageFamily(DWORD pid);
	// The process to capture for the app whose tree starts at `root`: `root`, unless no session of its tree renders
	// and another process of the same package has one that does (or its tree has none and that one is idle). Apple Music for Windows (1.1540) plays from
	// AMPLibraryAgent.exe, which COM starts outside AppleMusic.exe's tree. Use with COM initialized.
	DWORD AudioProcess(DWORD root);

	// What the player probably didn't mean, from the app's sessions that render now (an idle session says nothing
	// about what the player hears).
	enum Warning : unsigned
	{
		// Muted or at 0 in the mixer: process loopback taps after the session volume, so the station gets silence
		// (the first in-game try, 2026-10-23).
		kWarnMuted = 1,
		// On the default output device: the player hears it twice, and also while not on the station.
		kWarnDefaultDevice = 2,
	};
	unsigned Warnings(const std::vector<Session>& sessions);

	// The sessions for the log, e.g. "Speakers (default): active, 100%; Steam Streaming Speakers: active, muted".
	std::string Describe(const std::vector<Session>& sessions);

	// The live source: waits for the app, captures it, starts over when it exits; meanwhile now_playing.hh puts
	// the app's song on the HUD and, with `controlApp`, pauses the app while the station isn't played. Returns
	// once `stop` is set.
	void Run(const std::wstring& exeName, bool controlApp, const std::atomic<bool>& stop);
}

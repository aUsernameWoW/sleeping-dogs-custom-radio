// The mute question for app capture (core/app_audio.cc), outside the game: can a program's sound be captured by
// process loopback while the player doesn't hear it? A child of this test plays a -20 dBFS sine on a chosen
// output device; the test captures the child for a few seconds as it is, then with that device muted (restored
// afterwards). Compiled, not run by build.ps1 / CI (it plays sound and mutes a device).
//   app_audio_manual list                 output devices
//   app_audio_manual <index> [mute]       the experiment on that device (mute: also with the device muted)
//   app_audio_manual <index> sessions     the volume mixer warnings: the child's session as it is, then muted
//   app_audio_manual titles <exe>         the HUD title for that program, from its media session
//   app_audio_manual control <exe>        pausing and resuming that program as the station is left and played
//   app_audio_manual play <index> <s>     (the child) play the sine

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "../core/app_audio.cc"
#include "../core/live.cc"
#include "../core/now_playing.cc"
#include "../core/log.cc"

#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cstdio>
#include <cstdlib>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "oleaut32.lib")

namespace
{
	struct Device
	{
		std::wstring mId, mName;
		bool mDefault = false;
	};

	std::vector<Device> Devices()
	{
		std::vector<Device> devices;
		IMMDeviceEnumerator* enumerator = nullptr;
		if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator)))) {
			return devices;
		}
		std::wstring defaultId;
		IMMDevice* def = nullptr;
		if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &def))) {
			LPWSTR id = nullptr;
			def->GetId(&id);
			defaultId = id ? id : L"";
			CoTaskMemFree(id);
			def->Release();
		}
		IMMDeviceCollection* collection = nullptr;
		if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) {
			UINT count = 0;
			collection->GetCount(&count);
			for (UINT i = 0; i < count; ++i) {
				IMMDevice* device = nullptr;
				if (FAILED(collection->Item(i, &device))) continue;
				Device d;
				LPWSTR id = nullptr;
				device->GetId(&id);
				d.mId = id ? id : L"";
				CoTaskMemFree(id);
				IPropertyStore* props = nullptr;
				if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
					PROPVARIANT name;
					PropVariantInit(&name);
					if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR) d.mName = name.pwszVal;
					PropVariantClear(&name);
					props->Release();
				}
				d.mDefault = d.mId == defaultId;
				devices.push_back(d);
				device->Release();
			}
			collection->Release();
		}
		enumerator->Release();
		return devices;
	}

	IMMDevice* OpenDevice(const std::wstring& id)
	{
		IMMDeviceEnumerator* enumerator = nullptr;
		IMMDevice* device = nullptr;
		if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator)))) {
			enumerator->GetDevice(id.c_str(), &device);
			enumerator->Release();
		}
		return device;
	}

	// Plays a -20 dBFS 440 Hz sine on the device for `seconds` (shared mode, the mix format: float or 16-bit).
	int Play(const std::wstring& id, int seconds)
	{
		IMMDevice* device = OpenDevice(id);
		IAudioClient* client = nullptr;
		WAVEFORMATEX* format = nullptr;
		if (!device || FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) ||
			FAILED(client->GetMixFormat(&format)) || FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2'000'000, 0, format, nullptr))) {
			std::printf("play: can't open the device\n");
			return 1;
		}
		bool isFloat = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
		if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
			isFloat = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
		}
		IAudioRenderClient* render = nullptr;
		client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render));
		UINT32 bufferFrames = 0;
		client->GetBufferSize(&bufferFrames);
		client->Start();
		double phase = 0.0;
		const double step = 2.0 * 3.14159265358979 * 440.0 / format->nSamplesPerSec;
		const DWORD end = GetTickCount() + static_cast<DWORD>(seconds) * 1000;
		while (static_cast<int32_t>(GetTickCount() - end) < 0) {
			Sleep(10);
			UINT32 padding = 0;
			client->GetCurrentPadding(&padding);
			const UINT32 frames = bufferFrames - padding;
			BYTE* data = nullptr;
			if (!frames || FAILED(render->GetBuffer(frames, &data))) continue;
			for (UINT32 f = 0; f < frames; ++f) {
				const float v = 0.1f * static_cast<float>(std::sin(phase)); // -20 dBFS
				phase += step;
				for (WORD c = 0; c < format->nChannels; ++c) {
					if (isFloat) reinterpret_cast<float*>(data)[f * format->nChannels + c] = v;
					else reinterpret_cast<int16_t*>(data)[f * format->nChannels + c] = static_cast<int16_t>(v * 32767);
				}
			}
			render->ReleaseBuffer(frames, 0);
		}
		client->Stop();
		return 0;
	}

	// Mutes (or unmutes) `pid`'s sessions on the device, as the volume mixer would. Returns how many it set.
	int MuteSessions(const std::wstring& id, DWORD pid, BOOL mute)
	{
		IMMDevice* device = OpenDevice(id);
		IAudioSessionManager2* manager = nullptr;
		IAudioSessionEnumerator* sessions = nullptr;
		int count = 0, set = 0;
		if (device && SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&manager))) &&
			SUCCEEDED(manager->GetSessionEnumerator(&sessions)) && SUCCEEDED(sessions->GetCount(&count))) {
			for (int i = 0; i < count; ++i) {
				IAudioSessionControl* control = nullptr;
				IAudioSessionControl2* control2 = nullptr;
				ISimpleAudioVolume* volume = nullptr;
				DWORD owner = 0;
				if (SUCCEEDED(sessions->GetSession(i, &control)) &&
					SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&control2))) &&
					SUCCEEDED(control2->GetProcessId(&owner)) && owner == pid &&
					SUCCEEDED(control->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&volume))) &&
					SUCCEEDED(volume->SetMute(mute, nullptr))) {
					++set;
				}
				if (volume) volume->Release();
				if (control2) control2->Release();
				if (control) control->Release();
			}
		}
		if (sessions) sessions->Release();
		if (manager) manager->Release();
		if (device) device->Release();
		return set;
	}

	// Captures `pid` for `ms` and returns the peak in dBFS (-96 if nothing).
	double CapturePeak(DWORD pid, DWORD ms)
	{
		appaudio::Capture capture;
		std::string error;
		if (!capture.Open(pid, error)) {
			std::printf("capture: %s\n", error.c_str());
			return -999.0;
		}
		std::vector<int16_t> samples;
		const DWORD end = GetTickCount() + ms;
		int peak = 0;
		size_t frames = 0;
		while (static_cast<int32_t>(GetTickCount() - end) < 0) {
			samples.clear();
			if (!capture.Read(samples, 100, error)) {
				std::printf("capture: %s\n", error.c_str());
				break;
			}
			frames += samples.size() / 2;
			for (const int16_t s : samples) peak = (std::max)(peak, std::abs(static_cast<int>(s)));
		}
		std::printf("  captured %zu ms of audio\n", frames * 1000 / appaudio::kRate);
		return peak ? 20.0 * std::log10(peak / 32768.0) : -96.0;
	}
}

int wmain(int argc, wchar_t** argv)
{
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const std::vector<Device> devices = Devices();
	if (argc >= 2 && std::wstring(argv[1]) == L"list") {
		for (size_t i = 0; i < devices.size(); ++i) {
			std::printf("%zu%s %s\n", i, devices[i].mDefault ? "*" : " ", logger::ToUtf8(devices[i].mName.c_str()).c_str());
		}
		return 0;
	}
	if (argc >= 3 && std::wstring(argv[1]) == L"titles") {
		// What the HUD would show for that program: its media session's title, for 5 s (log in app_audio_manual.log).
		DeleteFileW(L"app_audio_manual.log");
		logger::Open(L"app_audio_manual.log");
		std::atomic<bool> stop{ false };
		std::thread titles([&] { nowplaying::Run(argv[2], false, stop); });
		Sleep(5000);
		stop = true;
		titles.join();
		std::printf("now playing: \"%s\"\n", live::NowPlaying().c_str());
		return 0;
	}
	if (argc >= 3 && std::wstring(argv[1]) == L"control") {
		// Pausing and resuming that program (it should be playing): 5 s with no live file open (paused after
		// ~1.5 s), then 5 s as if the game read the station (resumed). Log in app_audio_manual.log.
		DeleteFileW(L"app_audio_manual.log");
		logger::Open(L"app_audio_manual.log");
		std::atomic<bool> stop{ false };
		std::thread control([&] { nowplaying::Run(argv[2], true, stop); });
		Sleep(5000);
		live::Acquire();
		for (int i = 0; i < 10; ++i) {
			live::NoteRead();
			Sleep(500);
		}
		live::Release();
		stop = true;
		control.join();
		return 0;
	}
	if (argc >= 4 && std::wstring(argv[1]) == L"play") {
		const size_t index = static_cast<size_t>(_wtoi(argv[2]));
		return index < devices.size() ? Play(devices[index].mId, _wtoi(argv[3])) : 2;
	}
	if (argc < 2) {
		std::printf("usage: app_audio_manual list | <index> [mute | sessions]\n");
		return 2;
	}
	const size_t index = static_cast<size_t>(_wtoi(argv[1]));
	const bool mute = argc >= 3 && std::wstring(argv[2]) == L"mute";
	const bool sessions = argc >= 3 && std::wstring(argv[2]) == L"sessions";
	if (index >= devices.size()) {
		std::printf("no device %zu\n", index);
		return 2;
	}
	std::printf("device %zu: %s%s\n", index, logger::ToUtf8(devices[index].mName.c_str()).c_str(), devices[index].mDefault ? " (default)" : "");

	wchar_t self[MAX_PATH];
	GetModuleFileNameW(nullptr, self, MAX_PATH);
	std::wstring command = L"\"" + std::wstring(self) + L"\" play " + std::to_wstring(index) + L" 12";
	STARTUPINFOW si = { sizeof(si) };
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
		std::printf("can't start the player\n");
		return 1;
	}
	Sleep(1000);
	std::printf("as it is:\n");
	const double plain = CapturePeak(pi.dwProcessId, 4000);
	std::printf("  peak %.1f dBFS (played at -20)\n", plain);

	bool sessionsOk = true;
	if (sessions) {
		// The child plays on that device, so its session is there and active; muted (the child's own session,
		// nothing of the player's) the capture is silent and the warning comes. A non-default device warns of
		// nothing else; the default one warns that it doubles the sound.
		const auto report = [&](const char* when) {
			const std::vector<appaudio::Session> found = appaudio::Sessions(pi.dwProcessId);
			const unsigned warnings = appaudio::Warnings(found);
			std::printf("%s: sessions %s; warnings%s%s%s\n", when, appaudio::Describe(found).c_str(), warnings ? "" : " none",
				warnings & appaudio::kWarnMuted ? " muted" : "", warnings & appaudio::kWarnDefaultDevice ? " default-device" : "");
			return warnings;
		};
		const unsigned onDevice = devices[index].mDefault ? unsigned{ appaudio::kWarnDefaultDevice } : 0u;
		sessionsOk = report("as it is") == onDevice;
		const int set = MuteSessions(devices[index].mId, pi.dwProcessId, TRUE);
		std::printf("muted %d session(s) of the child\n", set);
		Sleep(300);
		sessionsOk = report("session muted") == (onDevice | appaudio::kWarnMuted) && set > 0 && sessionsOk;
		const double silent = CapturePeak(pi.dwProcessId, 2000);
		std::printf("  peak %.1f dBFS while its session is muted\n", silent);
		sessionsOk = silent < -60.0 && sessionsOk;
		MuteSessions(devices[index].mId, pi.dwProcessId, FALSE);
	}

	double muted = 0.0;
	if (mute) {
		IMMDevice* device = OpenDevice(devices[index].mId);
		IAudioEndpointVolume* volume = nullptr;
		BOOL wasMuted = FALSE;
		if (device && SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&volume)))) {
			volume->GetMute(&wasMuted);
			volume->SetMute(TRUE, nullptr);
			std::printf("device muted:\n");
			muted = CapturePeak(pi.dwProcessId, 4000);
			std::printf("  peak %.1f dBFS\n", muted);
			volume->SetMute(wasMuted, nullptr);
			std::printf("device mute restored (%s)\n", wasMuted ? "muted" : "unmuted");
			volume->Release();
		}
		if (device) device->Release();
	}
	TerminateProcess(pi.hProcess, 0);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return plain > -30.0 && (!mute || muted > -30.0) && sessionsOk ? 0 : 1;
}

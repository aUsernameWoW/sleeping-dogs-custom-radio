#include "app_audio.hh"

#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "live.hh"
#include "log.hh"
#include "now_playing.hh"

namespace appaudio
{
	namespace
	{
		static_assert(kRate == live::kRate && kChannels * sizeof(int16_t) == live::kBlockAlign, "the ring's format");

		std::string Hex(HRESULT hr)
		{
			char text[16];
			std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(hr));
			return text;
		}

		using ActivateFn = HRESULT(WINAPI*)(LPCWSTR, REFIID, PROPVARIANT*, IActivateAudioInterfaceCompletionHandler*, IActivateAudioInterfaceAsyncOperation**);

		// Exported by mmdevapi.dll since Windows 8; process loopback activation needs build 20348.
		ActivateFn ActivateFunction()
		{
			static const ActivateFn function = [] {
				HMODULE module = LoadLibraryW(L"mmdevapi.dll");
				return module ? reinterpret_cast<ActivateFn>(GetProcAddress(module, "ActivateAudioInterfaceAsync")) : nullptr;
			}();
			return function;
		}

		// ActivateAudioInterfaceAsync calls back on a system thread; it requires the handler to be agile.
		class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject
		{
		public:
			ActivationHandler() : mDone(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
			{
				if (!out) return E_POINTER;
				if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
					*out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
				}
				else if (riid == __uuidof(IAgileObject)) {
					*out = static_cast<IAgileObject*>(this);
				}
				else {
					*out = nullptr;
					return E_NOINTERFACE;
				}
				AddRef();
				return S_OK;
			}
			ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&mRefs)); }
			ULONG STDMETHODCALLTYPE Release() override
			{
				const LONG refs = InterlockedDecrement(&mRefs);
				if (refs == 0) {
					delete this;
				}
				return static_cast<ULONG>(refs);
			}

			HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override
			{
				HRESULT result = E_FAIL;
				IUnknown* activated = nullptr;
				const HRESULT hr = operation->GetActivateResult(&result, &activated);
				mResult = FAILED(hr) ? hr : result;
				if (SUCCEEDED(mResult) && activated) {
					mResult = activated->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(&mClient));
				}
				if (activated) {
					activated->Release();
				}
				SetEvent(mDone);
				return S_OK;
			}

			// The activated client (ownership passes to the caller), or null with the reason in `result`.
			IAudioClient* Wait(DWORD timeoutMs, HRESULT& result)
			{
				if (WaitForSingleObject(mDone, timeoutMs) != WAIT_OBJECT_0) {
					result = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
					return nullptr;
				}
				result = mResult;
				IAudioClient* client = mClient;
				mClient = nullptr;
				return client;
			}

		private:
			~ActivationHandler()
			{
				if (mClient) mClient->Release();
				if (mDone) CloseHandle(mDone);
			}

			LONG mRefs = 1;
			HANDLE mDone;
			HRESULT mResult = E_FAIL;
			IAudioClient* mClient = nullptr;
		};

		// Keeps the ring at real-time pace: captured audio goes in as it comes, and while none comes (the app
		// renders nothing, or isn't running) silence fills in by the clock.
		class Feeder
		{
		public:
			Feeder()
			{
				QueryPerformanceFrequency(&mFrequency);
				QueryPerformanceCounter(&mStart);
			}

			void Push(const std::vector<int16_t>& samples)
			{
				live::Stream().Push(samples.data(), samples.size());
				mProduced += samples.size() / kChannels;
			}

			// Pads silence up to now once the source is more than kGapMs behind the clock (packets come every
			// ~10 ms while the app plays, so a gap that long means it doesn't). Returns the frames padded.
			uint64_t PadSilence()
			{
				constexpr uint64_t kGapFrames = uint64_t{ kRate } * 200 / 1000;
				LARGE_INTEGER now;
				QueryPerformanceCounter(&now);
				const uint64_t due = static_cast<uint64_t>((now.QuadPart - mStart.QuadPart) * kRate / mFrequency.QuadPart);
				if (due < mProduced + kGapFrames) {
					return 0;
				}
				const uint64_t frames = due - mProduced;
				mSilence.assign(static_cast<size_t>(frames * kChannels), 0);
				live::Stream().Push(mSilence.data(), mSilence.size());
				mProduced = due;
				return frames;
			}

		private:
			LARGE_INTEGER mFrequency{}, mStart{};
			uint64_t mProduced = 0;
			std::vector<int16_t> mSilence;
		};

		std::wstring Lower(std::wstring s)
		{
			for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
			return s;
		}

		double PeakDb(int peak)
		{
			return peak > 0 ? 20.0 * std::log10(peak / 32768.0) : -96.0;
		}

		// `root` and every process below it: the sessions belong to whichever child renders.
		std::unordered_set<DWORD> ProcessTree(DWORD root)
		{
			std::unordered_set<DWORD> tree;
			HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
			if (snapshot == INVALID_HANDLE_VALUE) {
				return tree;
			}
			std::unordered_multimap<DWORD, DWORD> children; // parent pid -> pid
			PROCESSENTRY32W entry = { sizeof(entry) };
			for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
				children.emplace(entry.th32ParentProcessID, entry.th32ProcessID);
			}
			CloseHandle(snapshot);
			std::vector<DWORD> pending{ root };
			while (!pending.empty()) {
				const DWORD pid = pending.back();
				pending.pop_back();
				// The System Idle Process (0) is its own parent; a recycled pid could make a loop too.
				if (!tree.insert(pid).second) {
					continue;
				}
				const auto [first, last] = children.equal_range(pid);
				for (auto it = first; it != last; ++it) {
					pending.push_back(it->second);
				}
			}
			return tree;
		}

		std::wstring DeviceId(IMMDevice* device)
		{
			LPWSTR id = nullptr;
			std::wstring result;
			if (SUCCEEDED(device->GetId(&id)) && id) {
				result = id;
			}
			CoTaskMemFree(id);
			return result;
		}

		std::string DeviceName(IMMDevice* device)
		{
			std::string name;
			IPropertyStore* props = nullptr;
			if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
				PROPVARIANT value;
				PropVariantInit(&value);
				if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR) {
					name = logger::ToUtf8(value.pwszVal);
				}
				PropVariantClear(&value);
				props->Release();
			}
			return name.empty() ? "(unnamed device)" : name;
		}

		// The sessions on one device that belong to `tree`.
		void DeviceSessions(IMMDevice* device, const std::unordered_set<DWORD>& tree, Session base, std::vector<Session>& out)
		{
			IAudioSessionManager2* manager = nullptr;
			if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&manager)))) {
				return;
			}
			IAudioSessionEnumerator* sessions = nullptr;
			int count = 0;
			if (SUCCEEDED(manager->GetSessionEnumerator(&sessions)) && SUCCEEDED(sessions->GetCount(&count))) {
				for (int i = 0; i < count; ++i) {
					IAudioSessionControl* control = nullptr;
					IAudioSessionControl2* control2 = nullptr;
					ISimpleAudioVolume* volume = nullptr;
					DWORD pid = 0;
					AudioSessionState state = AudioSessionStateInactive;
					// GetProcessId fails (AUDCLNT_S_NO_SINGLE_PROCESS is a success code, with pid 0) for
					// cross-process sessions; those aren't the app's.
					if (SUCCEEDED(sessions->GetSession(i, &control)) &&
						SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&control2))) &&
						SUCCEEDED(control2->GetProcessId(&pid)) && pid && tree.count(pid) &&
						SUCCEEDED(control->GetState(&state)) &&
						SUCCEEDED(control->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&volume)))) {
						Session session = base;
						BOOL muted = FALSE;
						volume->GetMute(&muted);
						volume->GetMasterVolume(&session.mVolume);
						session.mMuted = muted != FALSE;
						session.mActive = state == AudioSessionStateActive;
						out.push_back(std::move(session));
					}
					if (volume) volume->Release();
					if (control2) control2->Release();
					if (control) control->Release();
				}
			}
			if (sessions) sessions->Release();
			manager->Release();
		}
	}

	std::vector<Session> Sessions(DWORD pid)
	{
		std::vector<Session> result;
		const std::unordered_set<DWORD> tree = ProcessTree(pid);
		IMMDeviceEnumerator* enumerator = nullptr;
		if (!pid || FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
			reinterpret_cast<void**>(&enumerator)))) {
			return result;
		}
		// eConsole: the "Default Device" in Sound settings, which the game (and a session left at "Default") plays on.
		std::wstring defaultId;
		IMMDevice* defaultDevice = nullptr;
		if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice))) {
			defaultId = DeviceId(defaultDevice);
			defaultDevice->Release();
		}
		IMMDeviceCollection* devices = nullptr;
		UINT count = 0;
		if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices)) && SUCCEEDED(devices->GetCount(&count))) {
			for (UINT i = 0; i < count; ++i) {
				IMMDevice* device = nullptr;
				if (FAILED(devices->Item(i, &device))) {
					continue;
				}
				Session base;
				base.mDevice = DeviceName(device);
				base.mDefaultDevice = !defaultId.empty() && DeviceId(device) == defaultId;
				DeviceSessions(device, tree, base, result);
				device->Release();
			}
		}
		if (devices) devices->Release();
		enumerator->Release();
		return result;
	}

	unsigned Warnings(const std::vector<Session>& sessions)
	{
		unsigned warnings = 0;
		for (const Session& s : sessions) {
			if (!s.mActive) {
				continue;
			}
			if (s.mMuted || s.mVolume <= 0.0f) {
				warnings |= kWarnMuted;
			}
			if (s.mDefaultDevice) {
				warnings |= kWarnDefaultDevice;
			}
		}
		return warnings;
	}

	std::string Describe(const std::vector<Session>& sessions)
	{
		if (sessions.empty()) {
			return "none";
		}
		std::string text;
		for (const Session& s : sessions) {
			char line[64];
			std::snprintf(line, sizeof(line), "%s, %s%d%%", s.mActive ? "active" : "idle", s.mMuted ? "muted, " : "",
				static_cast<int>(std::lround(s.mVolume * 100.0f)));
			if (!text.empty()) text += "; ";
			text += s.mDevice + (s.mDefaultDevice ? " (default): " : ": ") + line;
		}
		return text;
	}

	DWORD FindProcess(const std::wstring& exeName)
	{
		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE) {
			return 0;
		}
		const std::wstring wanted = Lower(exeName);
		std::unordered_map<DWORD, DWORD> parents; // pid -> parent pid, for processes of that name
		PROCESSENTRY32W entry = { sizeof(entry) };
		for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
			if (Lower(entry.szExeFile) == wanted) {
				parents[entry.th32ProcessID] = entry.th32ParentProcessID;
			}
		}
		CloseHandle(snapshot);
		// Browsers and Electron apps (NetEase Cloud Music, Spotify) run a tree of same-named processes and play
		// from a child: capture the root's whole tree. Lowest pid first among several roots, for a stable choice.
		DWORD root = 0;
		for (const auto& [pid, parent] : parents) {
			if (!parents.count(parent) && (!root || pid < root)) {
				root = pid;
			}
		}
		return root;
	}

	Capture::~Capture()
	{
		Close();
	}

	void Capture::Close()
	{
		if (mClient) mClient->Stop();
		if (mCapture) mCapture->Release();
		if (mClient) mClient->Release();
		if (mEvent) CloseHandle(mEvent);
		mCapture = nullptr;
		mClient = nullptr;
		mEvent = nullptr;
	}

	bool Capture::Open(DWORD pid, std::string& error)
	{
		Close();
		const ActivateFn activate = ActivateFunction();
		if (!activate) {
			error = "ActivateAudioInterfaceAsync isn't available";
			return false;
		}

		AUDIOCLIENT_ACTIVATION_PARAMS params = {};
		params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
		params.ProcessLoopbackParams.TargetProcessId = pid;
		params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
		PROPVARIANT variant = {};
		variant.vt = VT_BLOB;
		variant.blob.cbSize = sizeof(params);
		variant.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

		auto* handler = new ActivationHandler();
		IActivateAudioInterfaceAsyncOperation* operation = nullptr;
		HRESULT hr = activate(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &variant, handler, &operation);
		if (SUCCEEDED(hr)) {
			mClient = handler->Wait(5'000, hr);
		}
		if (operation) operation->Release();
		handler->Release();
		if (!mClient) {
			// Before build 20348 the activation itself fails (E_INVALIDARG or class/file not found under Wine).
			error = "process loopback activation failed (" + Hex(hr) + "; needs Windows 10 build 20348+, i.e. Windows 11)";
			return false;
		}

		// Process loopback takes the format it's asked for (it has no mix format of its own to report).
		WAVEFORMATEX format = {};
		format.wFormatTag = WAVE_FORMAT_PCM;
		format.nChannels = static_cast<WORD>(kChannels);
		format.nSamplesPerSec = kRate;
		format.wBitsPerSample = 16;
		format.nBlockAlign = static_cast<WORD>(kChannels * 2);
		format.nAvgBytesPerSec = kRate * format.nBlockAlign;
		const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
			AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
		constexpr REFERENCE_TIME kBuffer = 2'000'000; // 200 ms
		hr = mClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBuffer, 0, &format, nullptr);
		if (FAILED(hr)) {
			error = "IAudioClient::Initialize failed (" + Hex(hr) + ")";
			Close();
			return false;
		}
		mEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (FAILED(hr = mClient->SetEventHandle(mEvent)) ||
			FAILED(hr = mClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&mCapture))) ||
			FAILED(hr = mClient->Start())) {
			error = "starting the capture failed (" + Hex(hr) + ")";
			Close();
			return false;
		}
		return true;
	}

	bool Capture::Read(std::vector<int16_t>& out, DWORD timeoutMs, std::string& error)
	{
		if (!mCapture) {
			error = "not open";
			return false;
		}
		WaitForSingleObject(mEvent, timeoutMs);
		for (;;) {
			UINT32 packet = 0;
			HRESULT hr = mCapture->GetNextPacketSize(&packet);
			if (FAILED(hr)) {
				error = "GetNextPacketSize failed (" + Hex(hr) + ")";
				return false;
			}
			if (packet == 0) {
				return true;
			}
			BYTE* data = nullptr;
			UINT32 frames = 0;
			DWORD flags = 0;
			hr = mCapture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
			if (FAILED(hr)) {
				error = "GetBuffer failed (" + Hex(hr) + ")";
				return false;
			}
			const size_t count = static_cast<size_t>(frames) * kChannels;
			if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
				out.insert(out.end(), count, 0);
			}
			else {
				const auto* samples = reinterpret_cast<const int16_t*>(data);
				out.insert(out.end(), samples, samples + count);
			}
			mCapture->ReleaseBuffer(frames);
		}
	}

	void Run(const std::wstring& exeName, bool controlApp, const std::atomic<bool>& stop)
	{
		const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		const std::string name = logger::ToUtf8(exeName.c_str());
		// The song title for the HUD, on its own thread: its WinRT calls may block for seconds, the capture
		// must not.
		std::thread titles([&] { nowplaying::Run(exeName, controlApp, stop); });
		Feeder feeder;
		bool waitingLogged = false;
		std::vector<int16_t> samples;
		while (!stop.load()) {
			const DWORD pid = exeName.empty() ? 0 : FindProcess(exeName);
			HANDLE process = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
			Capture capture;
			std::string error;
			if (!process || !capture.Open(pid, error)) {
				if (pid && process) {
					LOG("app: can't capture %s (process %lu): %s", name.c_str(), pid, error.c_str());
				}
				else if (!waitingLogged) {
					LOG("app: %s isn't running; waiting for it", exeName.empty() ? "(no [Live] App set)" : name.c_str());
				}
				waitingLogged = true;
				if (process) CloseHandle(process);
				// Retry every 2 s (5 s after a failed capture), silent meanwhile.
				for (int i = 0; i < (pid ? 50 : 20) && !stop.load(); ++i) {
					Sleep(100);
					feeder.PadSilence();
				}
				continue;
			}
			waitingLogged = false;
			LOG("app: capturing %s (process %lu and its children)", name.c_str(), pid);

			// Every 10 s: how loud the capture was and how much of it was silence, so a log tells whether the
			// app was heard (the mute question: see CLAUDE.md).
			DWORD reportAt = GetTickCount() + 10'000;
			uint64_t captured = 0, padded = 0;
			int peak = 0;
			// Every 5 s: the app's sessions in the volume mixer, logged when they change, with a warning for a
			// setting that silences the station or doubles the sound.
			DWORD sessionsAt = GetTickCount();
			std::string sessionsLogged;
			unsigned warned = 0;
			while (!stop.load()) {
				if (static_cast<int32_t>(GetTickCount() - sessionsAt) >= 0) {
					sessionsAt = GetTickCount() + 5'000;
					const std::vector<Session> sessions = Sessions(pid);
					const std::string described = Describe(sessions);
					if (described != sessionsLogged) {
						sessionsLogged = described;
						LOG("app: %s's audio sessions: %s", name.c_str(), described.c_str());
					}
					const unsigned warnings = Warnings(sessions);
					if ((warnings & kWarnMuted) && !(warned & kWarnMuted)) {
						LOG("app: WARNING: %s is muted (or at 0) in the volume mixer, so the station hears silence. Unmute it "
							"and mute or turn down its output device instead", name.c_str());
					}
					if ((warnings & kWarnDefaultDevice) && !(warned & kWarnDefaultDevice)) {
						LOG("app: WARNING: %s plays on the default output device, where the game plays too: you hear it twice, "
							"also off the radio. Send it to an unused output device (Settings > System > Sound > Volume mixer; "
							"NetEase Cloud Music: in its own settings)", name.c_str());
					}
					if (warned && !warnings) {
						LOG("app: %s's volume mixer settings look right now", name.c_str());
					}
					// Only the mute on the HUD: the player hears that one as silence and may never open the log; a
					// doubled sound they notice themselves.
					if ((warnings ^ warned) & kWarnMuted) {
						live::SetWarning(warnings & kWarnMuted ? name + " is muted in the volume mixer" : std::string());
					}
					warned = warnings;
				}
				if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
					LOG("app: %s exited", name.c_str());
					break;
				}
				samples.clear();
				if (!capture.Read(samples, 100, error)) {
					LOG("app: capture of %s stopped: %s", name.c_str(), error.c_str());
					break;
				}
				if (!samples.empty()) {
					for (const int16_t s : samples) {
						peak = (std::max)(peak, std::abs(static_cast<int>(s)));
					}
					captured += samples.size() / kChannels;
					feeder.Push(samples);
				}
				padded += feeder.PadSilence();
				if (static_cast<int32_t>(GetTickCount() - reportAt) >= 0) {
					LOG("app: last 10 s of %s: %llu ms captured (peak %.1f dBFS), %llu ms of silence padded", name.c_str(),
						captured * 1000 / kRate, PeakDb(peak), padded * 1000 / kRate);
					reportAt = GetTickCount() + 10'000;
					captured = padded = 0;
					peak = 0;
				}
			}
			CloseHandle(process);
			live::SetWarning({});
		}
		titles.join();
		if (SUCCEEDED(com)) {
			CoUninitialize();
		}
	}
}

// The endpoint loopback in this file is adapted from FH6 Universal Radio (https://github.com/g0ldyy/fh6-universal-radio,
// src/sources/external_audio_source.cpp, GPL-3.0); under the GPL-3.0 (LICENSE).

#include "capture.hh"

#include "log.hh"
#include "pcm.hh"

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <TlHelp32.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>

namespace capture
{
	namespace
	{
		// PKEY_Device_FriendlyName, defined here: the header's declaration needs INITGUID or a library.
		constexpr PROPERTYKEY kFriendlyName = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

		template <class T> class Com
		{
		public:
			Com() = default;
			Com(const Com&) = delete;
			Com& operator=(const Com&) = delete;
			~Com() { Reset(); }

			T* Get() const { return mPtr; }
			T** Put()
			{
				Reset();
				return &mPtr;
			}
			T* operator->() const { return mPtr; }
			explicit operator bool() const { return mPtr != nullptr; }
			void Reset()
			{
				if (mPtr) {
					mPtr->Release();
					mPtr = nullptr;
				}
			}

		private:
			T* mPtr = nullptr;
		};

		std::string Failed(const char* what, HRESULT hr)
		{
			char buffer[160];
			std::snprintf(buffer, sizeof(buffer), "%s failed: 0x%08lX", what, static_cast<unsigned long>(hr));
			return buffer;
		}

		std::wstring Lower(std::wstring text)
		{
			std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
			return text;
		}

		std::wstring Widen(const std::string& utf8)
		{
			if (utf8.empty()) {
				return {};
			}
			const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
			std::wstring result(static_cast<size_t>(size), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), result.data(), size);
			return result;
		}

		std::wstring FriendlyName(IMMDevice* device)
		{
			Com<IPropertyStore> props;
			if (FAILED(device->OpenPropertyStore(STGM_READ, props.Put()))) {
				return {};
			}
			PROPVARIANT value;
			PropVariantInit(&value);
			std::wstring name;
			if (SUCCEEDED(props->GetValue(kFriendlyName, &value)) && value.vt == VT_LPWSTR) {
				name = value.pwszVal;
			}
			PropVariantClear(&value);
			return name;
		}

		std::wstring DeviceId(IMMDevice* device)
		{
			wchar_t* id = nullptr;
			std::wstring result;
			if (SUCCEEDED(device->GetId(&id)) && id) {
				result = id;
				CoTaskMemFree(id);
			}
			return result;
		}

		HRESULT FindEndpoint(IMMDeviceEnumerator* enumerator, const std::string& endpoint, IMMDevice** device)
		{
			if (endpoint.empty()) {
				return enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device);
			}
			Com<IMMDeviceCollection> devices;
			HRESULT hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.Put());
			if (FAILED(hr)) {
				return hr;
			}
			UINT count = 0;
			devices->GetCount(&count);
			const std::wstring wanted = Lower(Widen(endpoint));
			for (UINT i = 0; i < count; ++i) {
				Com<IMMDevice> candidate;
				if (FAILED(devices->Item(i, candidate.Put()))) {
					continue;
				}
				if (Lower(FriendlyName(candidate.Get())).find(wanted) != std::wstring::npos
					|| Lower(DeviceId(candidate.Get())).find(wanted) != std::wstring::npos) {
					*device = candidate.Get();
					(*device)->AddRef();
					return S_OK;
				}
			}
			return E_NOTFOUND;
		}

		// ActivateAudioInterfaceAsync calls back on a worker thread; the handler must be agile.
		class ActivateHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject
		{
		public:
			ActivateHandler() : mDone(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
			~ActivateHandler()
			{
				if (mDone) {
					CloseHandle(mDone);
				}
			}

			HANDLE Done() const { return mDone; }

			STDMETHODIMP QueryInterface(REFIID iid, void** out) override
			{
				if (iid == __uuidof(IUnknown) || iid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
					*out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
				}
				else if (iid == __uuidof(IAgileObject)) {
					*out = static_cast<IAgileObject*>(this);
				}
				else {
					*out = nullptr;
					return E_NOINTERFACE;
				}
				AddRef();
				return S_OK;
			}
			STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&mRefs); }
			STDMETHODIMP_(ULONG) Release() override
			{
				const ULONG refs = InterlockedDecrement(&mRefs);
				if (refs == 0) {
					delete this;
				}
				return refs;
			}
			STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override
			{
				SetEvent(mDone);
				return S_OK;
			}

		private:
			LONG mRefs = 1;
			HANDLE mDone;
		};

		using ActivateAudioInterfaceAsyncFn = HRESULT(WINAPI*)(LPCWSTR, REFIID, PROPVARIANT*, IActivateAudioInterfaceCompletionHandler*,
			IActivateAudioInterfaceAsyncOperation**);

		// Looked up at run time: an import the system lacks (older Windows, Wine) would keep the whole .asi
		// from loading.
		HRESULT ActivateProcessLoopback(DWORD pid, IAudioClient** client)
		{
			const HMODULE mmdevapi = LoadLibraryW(L"mmdevapi.dll");
			const auto activate = mmdevapi ? reinterpret_cast<ActivateAudioInterfaceAsyncFn>(GetProcAddress(mmdevapi, "ActivateAudioInterfaceAsync")) : nullptr;
			if (!activate) {
				return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
			}

			AUDIOCLIENT_ACTIVATION_PARAMS params = {};
			params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
			params.ProcessLoopbackParams.TargetProcessId = pid;
			params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
			PROPVARIANT activation = {};
			activation.vt = VT_BLOB;
			activation.blob.cbSize = sizeof(params);
			activation.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

			ActivateHandler* handler = new ActivateHandler();
			Com<IActivateAudioInterfaceAsyncOperation> operation;
			HRESULT hr = activate(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &activation, handler, operation.Put());
			if (SUCCEEDED(hr)) {
				hr = WaitForSingleObject(handler->Done(), 5000) == WAIT_OBJECT_0 ? S_OK : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
			}
			if (SUCCEEDED(hr)) {
				HRESULT activateResult = E_FAIL;
				IUnknown* unknown = nullptr;
				hr = operation->GetActivateResult(&activateResult, &unknown);
				if (SUCCEEDED(hr)) {
					hr = activateResult;
				}
				if (SUCCEEDED(hr) && unknown) {
					hr = unknown->QueryInterface(IID_PPV_ARGS(client));
				}
				if (unknown) {
					unknown->Release();
				}
			}
			handler->Release();
			return hr;
		}

		// Drains the packets that are ready. False on error.
		bool Drain(IAudioCaptureClient* capture, pcm::Resampler& resampler, const Sink& sink, std::string& error)
		{
			UINT32 frames = 0;
			HRESULT hr = capture->GetNextPacketSize(&frames);
			while (SUCCEEDED(hr) && frames > 0) {
				BYTE* data = nullptr;
				DWORD flags = 0;
				hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
				if (FAILED(hr)) {
					break;
				}
				resampler.Push(data, frames, (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
				capture->ReleaseBuffer(frames);
				if (!resampler.mOut.empty()) {
					sink(resampler.mOut.data(), resampler.mOut.size());
					resampler.mOut.clear();
				}
				hr = capture->GetNextPacketSize(&frames);
			}
			if (FAILED(hr)) {
				error = Failed("capture read", hr);
				return false;
			}
			return true;
		}
	}

	bool Run(const Options& options, const std::atomic<bool>& stop, const Sink& sink, std::string& error)
	{
		Com<IAudioClient> client;
		pcm::Resampler resampler;
		HANDLE ready = nullptr;
		std::string source;

		if (options.mProcessId) {
			HRESULT hr = ActivateProcessLoopback(options.mProcessId, client.Put());
			if (FAILED(hr)) {
				error = Failed("process loopback activation", hr);
				return false;
			}
			// A process loopback client has no mix format of its own (GetMixFormat: E_NOTIMPL); it converts to
			// whatever we ask for.
			WAVEFORMATEX format = {};
			format.wFormatTag = WAVE_FORMAT_PCM;
			format.nChannels = 2;
			format.nSamplesPerSec = pcm::kRate;
			format.wBitsPerSample = 16;
			format.nBlockAlign = 4;
			format.nAvgBytesPerSec = pcm::kRate * 4;
			hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
				200000, 0, &format, nullptr);
			if (FAILED(hr)) {
				error = Failed("IAudioClient::Initialize (process loopback)", hr);
				return false;
			}
			std::string parseError;
			pcm::ParseFormat(format, resampler.mFormat, parseError);
			ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			client->SetEventHandle(ready);
			char buffer[64];
			std::snprintf(buffer, sizeof(buffer), "process loopback of PID %lu", options.mProcessId);
			source = buffer;
		}
		else {
			Com<IMMDeviceEnumerator> enumerator;
			HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.Put()));
			if (FAILED(hr)) {
				error = Failed("CoCreateInstance(MMDeviceEnumerator)", hr);
				return false;
			}
			Com<IMMDevice> device;
			hr = FindEndpoint(enumerator.Get(), options.mEndpoint, device.Put());
			if (FAILED(hr)) {
				error = Failed("finding the playback device", hr);
				return false;
			}
			hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.Put()));
			if (FAILED(hr)) {
				error = Failed("IMMDevice::Activate", hr);
				return false;
			}
			WAVEFORMATEX* mix = nullptr;
			hr = client->GetMixFormat(&mix);
			if (FAILED(hr)) {
				error = Failed("IAudioClient::GetMixFormat", hr);
				return false;
			}
			const bool parsed = pcm::ParseFormat(*mix, resampler.mFormat, error);
			if (parsed) {
				hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000, 0, mix, nullptr);
			}
			CoTaskMemFree(mix);
			if (!parsed) {
				return false;
			}
			if (FAILED(hr)) {
				error = Failed("IAudioClient::Initialize (endpoint loopback)", hr);
				return false;
			}
			source = "endpoint loopback of \"" + logger::ToUtf8(FriendlyName(device.Get()).c_str()) + "\"";
		}

		Com<IAudioCaptureClient> capture;
		HRESULT hr = client->GetService(IID_PPV_ARGS(capture.Put()));
		if (SUCCEEDED(hr)) {
			hr = client->Start();
		}
		if (FAILED(hr)) {
			if (ready) {
				CloseHandle(ready);
			}
			error = Failed("starting the capture", hr);
			return false;
		}
		LOG("capture: %s, %s", source.c_str(), pcm::Describe(resampler.mFormat).c_str());

		bool ok = true;
		while (ok && !stop.load()) {
			// Endpoint loopback has no event in older Windows versions (and Wine), so it is polled. A device
			// loopback delivers nothing at all while nothing plays.
			if (ready) {
				WaitForSingleObject(ready, 100);
			}
			else {
				Sleep(5);
			}
			ok = Drain(capture.Get(), resampler, sink, error);
		}

		client->Stop();
		if (ready) {
			CloseHandle(ready);
		}
		return ok;
	}

	DWORD FindProcess(const std::wstring& exeName)
	{
		const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE) {
			return 0;
		}
		struct Entry
		{
			DWORD mPid;
			DWORD mParent;
		};
		std::vector<Entry> matches;
		const std::wstring wanted = Lower(exeName);
		PROCESSENTRY32W entry = { sizeof(entry) };
		for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
			if (Lower(entry.szExeFile) == wanted) {
				matches.push_back({ entry.th32ProcessID, entry.th32ParentProcessID });
			}
		}
		CloseHandle(snapshot);
		for (const Entry& match : matches) {
			if (std::none_of(matches.begin(), matches.end(), [&](const Entry& other) { return other.mPid == match.mParent; })) {
				return match.mPid;
			}
		}
		return matches.empty() ? 0 : matches.front().mPid;
	}

	std::vector<DWORD> ProcessTree(DWORD root)
	{
		std::vector<DWORD> tree;
		if (!root) {
			return tree;
		}
		tree.push_back(root);
		const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE) {
			return tree;
		}
		std::vector<std::pair<DWORD, DWORD>> all;
		PROCESSENTRY32W entry = { sizeof(entry) };
		for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
			all.emplace_back(entry.th32ProcessID, entry.th32ParentProcessID);
		}
		CloseHandle(snapshot);
		for (size_t i = 0; i < tree.size(); ++i) {
			for (const auto& [pid, parent] : all) {
				if (parent == tree[i] && pid != tree[i] && std::find(tree.begin(), tree.end(), pid) == tree.end()) {
					tree.push_back(pid);
				}
			}
		}
		return tree;
	}

	SessionState AppSessions(const std::vector<DWORD>& processes)
	{
		SessionState state;
		Com<IMMDeviceEnumerator> enumerator;
		Com<IMMDevice> device;
		Com<IAudioSessionManager2> manager;
		Com<IAudioSessionEnumerator> sessions;
		if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.Put())))
			|| FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.Put()))
			|| FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(manager.Put())))
			|| FAILED(manager->GetSessionEnumerator(sessions.Put()))) {
			return state;
		}
		int count = 0;
		sessions->GetCount(&count);
		for (int i = 0; i < count; ++i) {
			Com<IAudioSessionControl> control;
			Com<IAudioSessionControl2> control2;
			if (FAILED(sessions->GetSession(i, control.Put())) || FAILED(control->QueryInterface(IID_PPV_ARGS(control2.Put())))) {
				continue;
			}
			DWORD pid = 0;
			control2->GetProcessId(&pid);
			if (std::find(processes.begin(), processes.end(), pid) == processes.end()) {
				continue;
			}
			AudioSessionState sessionState = AudioSessionStateInactive;
			control->GetState(&sessionState);
			state.mActive = state.mActive || sessionState == AudioSessionStateActive;
			if (state.mCount++ == 0) {
				Com<ISimpleAudioVolume> volume;
				if (SUCCEEDED(control->QueryInterface(IID_PPV_ARGS(volume.Put())))) {
					BOOL muted = FALSE;
					volume->GetMasterVolume(&state.mVolume);
					volume->GetMute(&muted);
					state.mMuted = muted != FALSE;
				}
			}
		}
		return state;
	}
}

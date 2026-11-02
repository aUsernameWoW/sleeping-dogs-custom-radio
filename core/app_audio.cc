#include "app_audio.hh"

#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <tlhelp32.h>

#include <appmodel.h>

#include <algorithm>
#include <cmath>
#include <functional>
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

		std::wstring Widen(const std::string& utf8)
		{
			std::wstring wide(utf8.size(), L'\0');
			wide.resize(static_cast<size_t>((std::max)(0, MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
				wide.data(), static_cast<int>(wide.size())))));
			return wide;
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

		// The sessions on one device whose process `wanted` accepts.
		void DeviceSessions(IMMDevice* device, const std::function<bool(DWORD)>& wanted, Session base, std::vector<Session>& out)
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
						SUCCEEDED(control2->GetProcessId(&pid)) && pid && wanted(pid) &&
						SUCCEEDED(control->GetState(&state)) &&
						SUCCEEDED(control->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&volume)))) {
						Session session = base;
						session.mProcess = pid;
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

	namespace
	{
		std::vector<Session> AllSessions(const std::function<bool(DWORD)>& wanted)
		{
			std::vector<Session> result;
			IMMDeviceEnumerator* enumerator = nullptr;
			if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
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
					DeviceSessions(device, wanted, base, result);
					device->Release();
				}
			}
			if (devices) devices->Release();
			enumerator->Release();
			return result;
		}

		// Every session on every active output device, with what it takes to change its volume.
		void ForEachSession(const std::function<void(DWORD pid, const std::wstring& session, ISimpleAudioVolume* volume)>& visit)
		{
			IMMDeviceEnumerator* enumerator = nullptr;
			if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
				reinterpret_cast<void**>(&enumerator)))) {
				return;
			}
			IMMDeviceCollection* devices = nullptr;
			UINT count = 0;
			if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices)) && SUCCEEDED(devices->GetCount(&count))) {
				for (UINT d = 0; d < count; ++d) {
					IMMDevice* device = nullptr;
					IAudioSessionManager2* manager = nullptr;
					IAudioSessionEnumerator* sessions = nullptr;
					int n = 0;
					if (SUCCEEDED(devices->Item(d, &device)) &&
						SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&manager))) &&
						SUCCEEDED(manager->GetSessionEnumerator(&sessions)) && SUCCEEDED(sessions->GetCount(&n))) {
						for (int i = 0; i < n; ++i) {
							IAudioSessionControl* control = nullptr;
							IAudioSessionControl2* control2 = nullptr;
							ISimpleAudioVolume* volume = nullptr;
							LPWSTR id = nullptr;
							DWORD pid = 0;
							if (SUCCEEDED(sessions->GetSession(i, &control)) &&
								SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&control2))) &&
								SUCCEEDED(control2->GetProcessId(&pid)) && SUCCEEDED(control2->GetSessionIdentifier(&id)) && id &&
								SUCCEEDED(control->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&volume)))) {
								visit(pid, id, volume);
							}
							CoTaskMemFree(id);
							if (volume) volume->Release();
							if (control2) control2->Release();
							if (control) control->Release();
						}
					}
					if (sessions) sessions->Release();
					if (manager) manager->Release();
					if (device) device->Release();
				}
			}
			if (devices) devices->Release();
			enumerator->Release();
		}

		std::string ProcessName(DWORD pid)
		{
			HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
			wchar_t path[MAX_PATH];
			DWORD size = MAX_PATH;
			const bool ok = process && QueryFullProcessImageNameW(process, 0, path, &size);
			if (process) CloseHandle(process);
			if (!ok) return "process " + std::to_string(pid);
			const std::wstring full(path, size);
			return logger::ToUtf8(full.substr(full.find_last_of(L'\\') + 1).c_str());
		}
	}

	std::vector<Session> Sessions(DWORD pid)
	{
		if (!pid) {
			return {};
		}
		const std::unordered_set<DWORD> tree = ProcessTree(pid);
		return AllSessions([&](DWORD session) { return tree.count(session) != 0; });
	}

	std::wstring PackageFamily(DWORD pid)
	{
		HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (!process) {
			return {};
		}
		wchar_t name[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1];
		UINT32 length = ARRAYSIZE(name);
		const LONG result = GetPackageFamilyName(process, &length, name);
		CloseHandle(process);
		return result == ERROR_SUCCESS ? std::wstring(name) : std::wstring();
	}

	DWORD AudioProcess(DWORD root)
	{
		if (!root) {
			return root;
		}
		// Restarted, Apple Music's own process has an idle session too (2026-10-28) while its agent plays: only one
		// that renders keeps the choice in the named tree.
		const std::vector<Session> own = Sessions(root);
		if (std::any_of(own.begin(), own.end(), [](const Session& s) { return s.mActive; })) {
			return root;
		}
		const std::wstring family = PackageFamily(root);
		if (family.empty()) {
			return root;
		}
		const std::unordered_set<DWORD> tree = ProcessTree(root);
		std::unordered_map<DWORD, bool> sameFamily;
		const std::vector<Session> found = AllSessions([&](DWORD pid) {
			if (tree.count(pid)) return false;
			const auto [it, added] = sameFamily.try_emplace(pid, false);
			if (added) it->second = PackageFamily(pid) == family;
			return it->second;
		});
		// One that renders now; else the named tree's idle session, else any (an idle session is where it will
		// play next).
		DWORD best = root;
		bool bestActive = false;
		for (const Session& s : found) {
			if ((best == root && own.empty()) || (s.mActive && !bestActive)) {
				best = s.mProcess;
				bestActive = s.mActive;
			}
		}
		return best;
	}

	float FollowVolume(float& player, float set, float current, float gain)
	{
		// Someone else's value since our last set: the player moved the slider. Near silence the division would
		// blow a rounding step up, so it waits until the gain is back.
		if (set >= 0.0f && std::fabs(current - set) > 0.02f && gain > 0.05f) {
			player = (std::min)(current / gain, 1.0f);
		}
		return (std::clamp)(player * gain, 0.0f, 1.0f);
	}

	void VolumeFollower::Apply(DWORD pid, float gain)
	{
		const std::unordered_set<DWORD> tree = ProcessTree(pid);
		bool save = false;
		ForEachSession([&](DWORD process, const std::wstring& session, ISimpleAudioVolume* volume) {
			float current = 1.0f;
			if (!tree.count(process) || FAILED(volume->GetMasterVolume(&current))) {
				return;
			}
			auto held = std::find_if(mHeld.begin(), mHeld.end(), [&](const Held& h) { return h.mSession == session; });
			if (held == mHeld.end()) {
				if (std::fabs(gain - 1.0f) < 0.005f) {
					return;
				}
				// Saved before the first change, so a crash right after it still leaves the way back.
				mHeld.push_back({ session, current, -1.0f });
				held = mHeld.end() - 1;
				save = true;
				Save();
			}
			const float before = held->mPlayer;
			const float target = FollowVolume(held->mPlayer, held->mSet, current, gain);
			if (held->mPlayer != before) {
				LOG("app: the player set the volume to %d%%; following the game from there", static_cast<int>(std::lround(held->mPlayer * 100.0f)));
				save = true;
			}
			if (std::fabs(target - current) > 0.002f && SUCCEEDED(volume->SetMasterVolume(target, nullptr))) {
				held->mSet = target;
			}
		});
		if (save) {
			Save();
		}
	}

	void VolumeFollower::Restore()
	{
		if (mHeld.empty()) {
			return;
		}
		ForEachSession([&](DWORD, const std::wstring& session, ISimpleAudioVolume* volume) {
			const auto held = std::find_if(mHeld.begin(), mHeld.end(), [&](const Held& h) { return h.mSession == session; });
			float current = 0.0f;
			// A value the player chose after our last change stays.
			if (held != mHeld.end() && SUCCEEDED(volume->GetMasterVolume(&current)) && (held->mSet < 0.0f || std::fabs(current - held->mSet) <= 0.02f)) {
				volume->SetMasterVolume(held->mPlayer, nullptr);
			}
		});
		mHeld.clear();
		DeleteFileW(mStateFile.c_str());
	}

	void VolumeFollower::Save() const
	{
		FILE* file = nullptr;
		if (_wfopen_s(&file, mStateFile.c_str(), L"wb") != 0 || !file) {
			return;
		}
		for (const Held& h : mHeld) {
			std::fprintf(file, "%.4f\t%s\n", h.mPlayer, logger::ToUtf8(h.mSession.c_str()).c_str());
		}
		std::fclose(file);
	}

	void VolumeFollower::RestoreLeftover(const std::wstring& stateFile)
	{
		FILE* file = nullptr;
		if (_wfopen_s(&file, stateFile.c_str(), L"rb") != 0 || !file) {
			return;
		}
		std::vector<std::pair<float, std::wstring>> left;
		char line[2048];
		while (std::fgets(line, sizeof(line), file)) {
			char* tab = std::strchr(line, '\t');
			if (!tab) continue;
			std::string session = tab + 1;
			while (!session.empty() && (session.back() == '\n' || session.back() == '\r')) session.pop_back();
			left.emplace_back(static_cast<float>(std::atof(line)), Widen(session));
		}
		std::fclose(file);
		ForEachSession([&](DWORD, const std::wstring& session, ISimpleAudioVolume* volume) {
			for (auto it = left.begin(); it != left.end(); ++it) {
				if (it->second == session && SUCCEEDED(volume->SetMasterVolume(it->first, nullptr))) {
					LOG("app: put a program's volume back to %d%% (the game ended before SDRadio could)", static_cast<int>(std::lround(it->first * 100.0f)));
					left.erase(it);
					break;
				}
			}
		});
		// One that isn't running now keeps its line for the next start.
		if (left.empty()) {
			DeleteFileW(stateFile.c_str());
		}
		else if (_wfopen_s(&file, stateFile.c_str(), L"wb") == 0 && file) {
			for (const auto& [player, session] : left) {
				std::fprintf(file, "%.4f\t%s\n", player, logger::ToUtf8(session.c_str()).c_str());
			}
			std::fclose(file);
		}
	}

	Sound ParseSound(const std::string& text)
	{
		std::string lower = text;
		for (char& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
		return lower == "direct" ? Sound::kDirect : lower == "radio" ? Sound::kRadio : Sound::kAuto;
	}

	bool Direct(Sound mode, const std::vector<Session>& sessions, bool current)
	{
		if (mode != Sound::kAuto) {
			return mode == Sound::kDirect;
		}
		bool renders = false;
		for (const Session& s : sessions) {
			if (s.mActive) {
				renders = true;
				if (!s.mDefaultDevice) {
					return false;
				}
			}
		}
		return renders ? true : current;
	}

	bool IsAuto(const std::wstring& app)
	{
		return app.empty() || Lower(app) == L"auto";
	}

	std::vector<Renderer> Renderers(bool idle)
	{
		const DWORD self = GetCurrentProcessId();
		std::vector<Renderer> result;
		for (const Session& s : AllSessions([self](DWORD pid) { return pid != self; })) {
			if ((!s.mActive && !idle) || std::any_of(result.begin(), result.end(), [&](const Renderer& r) { return r.mProcess == s.mProcess; })) {
				continue;
			}
			Renderer renderer;
			renderer.mProcess = s.mProcess;
			renderer.mExe = Widen(ProcessName(s.mProcess));
			renderer.mFamily = PackageFamily(s.mProcess);
			result.push_back(std::move(renderer));
		}
		return result;
	}

	int MatchRenderer(const std::vector<Renderer>& renderers, const std::string& appId, bool allowSole)
	{
		if (!appId.empty()) {
			const std::wstring id = Lower(Widen(appId));
			// A Store app's ID is "<family>!<app>"; its sound may come from another process of the package
			// (Apple Music's AMPLibraryAgent.exe), which has the family but not the name.
			for (size_t i = 0; i < renderers.size(); ++i) {
				const std::wstring family = Lower(renderers[i].mFamily);
				if (!family.empty() && id.compare(0, family.size(), family) == 0 && (id.size() == family.size() || id[family.size()] == L'!')) {
					return static_cast<int>(i);
				}
			}
			// The exe's name as a whole word of the ID ("Spotify.exe", "Chrome", "VideoLAN.VLC", a path), not a
			// substring as with a named App: these names are whatever runs, and a short one ("x.exe") would match
			// inside any long ID.
			const auto isWordChar = [](wchar_t c) { return iswalnum(c) != 0; };
			for (size_t i = 0; i < renderers.size(); ++i) {
				std::wstring key = Lower(renderers[i].mExe);
				if (key.size() > 4 && key.compare(key.size() - 4, 4, L".exe") == 0) {
					key.resize(key.size() - 4);
				}
				for (size_t at = key.empty() ? std::wstring::npos : id.find(key); at != std::wstring::npos; at = id.find(key, at + 1)) {
					const size_t end = at + key.size();
					if ((at == 0 || !isWordChar(id[at - 1])) && (end == id.size() || !isWordChar(id[end]))) {
						return static_cast<int>(i);
					}
				}
			}
		}
		return allowSole && renderers.size() == 1 ? 0 : -1;
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

	void Run(const std::wstring& app, bool controlApp, Sound sound, const std::atomic<bool>& stop)
	{
		const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		const bool autoPick = IsAuto(app);
		if (sound != Sound::kRadio && !controlApp) {
			LOG("app: PauseApp = 0 with AppSound = %s: heard directly, the app keeps playing off the station too",
				sound == Sound::kDirect ? "direct" : "auto");
		}
		// Heard directly (the station silent) or through the station; kept across captures, so a paused app that
		// says nothing keeps the last choice. Auto starts direct: most apps play on the default device.
		bool direct = sound != Sound::kRadio;
		int loggedDirect = -1;
		// Heard directly, the app follows the radio's gain in the game through its mixer volume.
		const std::wstring stateFile = [] {
			HMODULE module = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&IsAuto), &module);
			wchar_t path[MAX_PATH];
			const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
			std::wstring dir(path, length);
			return dir.substr(0, dir.find_last_of(L"\\/") + 1) + L"SDRadio-app-volume.txt";
		}();
		VolumeFollower::RestoreLeftover(stateFile);
		VolumeFollower follower(stateFile);
		// The song title for the HUD, on its own thread: its WinRT calls may block for seconds, the capture
		// must not. With App = auto it also decides which app is the player's music.
		std::thread titles([&] { nowplaying::Run(autoPick ? std::wstring() : app, controlApp, stop); });
		Feeder feeder;
		bool waitingLogged = false;
		std::vector<int16_t> samples;
		while (!stop.load()) {
			std::wstring exeName = app;
			DWORD renderer = 0;
			if (autoPick) {
				std::vector<Renderer> renderers = Renderers();
				const std::string chosen = nowplaying::ChosenApp();
				int match = MatchRenderer(renderers, chosen, true);
				// The followed app paused (by us, as the player left the car): its session is idle. Captured all the
				// same, so the volume follower holds it down before it plays again: getting in, the radio fades in
				// over 2.5 s, and an app resumed before the capture attached would start at full volume.
				if (match < 0 && !chosen.empty()) {
					renderers = Renderers(true);
					match = MatchRenderer(renderers, chosen, false);
				}
				exeName = match >= 0 ? renderers[match].mExe : std::wstring();
				renderer = match >= 0 ? renderers[match].mProcess : 0;
			}
			const std::string name = logger::ToUtf8(exeName.c_str());
			DWORD root = exeName.empty() ? 0 : FindProcess(exeName);
			// Another tree of the same name (a second instance) mustn't take the place of the one that renders.
			if (renderer && !ProcessTree(root).count(renderer)) {
				root = renderer;
			}
			// Apple Music plays from AMPLibraryAgent.exe, a process of its package outside its own tree.
			const DWORD pid = AudioProcess(root);
			HANDLE process = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
			Capture capture;
			std::string error;
			if (!process || !capture.Open(pid, error)) {
				if (pid && process) {
					LOG("app: can't capture %s (process %lu): %s", name.c_str(), pid, error.c_str());
				}
				else if (!waitingLogged && autoPick) {
					LOG("app: App = auto: no program plays music yet; waiting for one (a media session that plays, or the only "
						"program making sound)");
				}
				else if (!waitingLogged) {
					LOG("app: %s isn't running; waiting for it", name.c_str());
				}
				waitingLogged = true;
				if (process) CloseHandle(process);
				// Retry every 2 s (5 s after a failed capture), silent meanwhile. App = auto every 0.5 s: its app is
				// typically there but paused (by us, until the station plays again), and the resume shouldn't wait.
				for (int i = 0; i < (pid ? 50 : autoPick ? 5 : 20) && !stop.load(); ++i) {
					Sleep(100);
					feeder.PadSilence();
				}
				continue;
			}
			waitingLogged = false;
			if (pid == root) {
				LOG("app: capturing %s (process %lu and its children)", name.c_str(), pid);
			}
			else {
				LOG("app: capturing %s through %s (process %lu, same app package, where its sound plays)", name.c_str(),
					ProcessName(pid).c_str(), pid);
			}

			// Every 10 s: how loud the capture was and how much of it was silence, so a log tells whether the
			// app was heard (the mute question: see CLAUDE.md).
			DWORD reportAt = GetTickCount() + 10'000;
			uint64_t captured = 0, padded = 0;
			int peak = 0;
			// Every 5 s: the app's sessions in the volume mixer, logged when they change, with a warning for a
			// setting that silences the station or doubles the sound.
			DWORD sessionsAt = GetTickCount();
			float appliedGain = -1.0f;
			DWORD appliedAt = GetTickCount();
			std::string sessionsLogged;
			unsigned warned = 0;
			while (!stop.load()) {
				if (static_cast<int32_t>(GetTickCount() - sessionsAt) >= 0) {
					sessionsAt = GetTickCount() + 5'000;
					const std::vector<Session> sessions = Sessions(pid);
					// Started before it played anything, or the app restarted: another process of its package may be
					// the one that renders now. Only one that does: paused for ~8 s, Apple Music's agent goes idle
					// too, and the capture would fall back to the silent UI process just before the player resumes
					// (2026-10-28, in game).
					const auto renders = [](const std::vector<Session>& list) {
						return std::any_of(list.begin(), list.end(), [](const Session& s) { return s.mActive; });
					};
					const DWORD other = renders(sessions) ? pid : AudioProcess(root);
					if (other != pid && (sessions.empty() || renders(Sessions(other)))) {
						LOG("app: %s's sound plays from another of its processes now; capturing that instead", name.c_str());
						break;
					}
					// App = auto: the player started another app (its media session now plays, see AutoChoose). Only a
					// media session moves the capture: the only process making sound picks the first app, but a
					// notification sound later mustn't take the station over.
					if (autoPick) {
						const std::vector<Renderer> renderers = Renderers();
						const int match = MatchRenderer(renderers, nowplaying::ChosenApp(), false);
						if (match >= 0 && !ProcessTree(root).count(renderers[match].mProcess) && renderers[match].mProcess != pid) {
							LOG("app: App = auto: the music plays from %s now; capturing that instead",
								logger::ToUtf8(renderers[match].mExe.c_str()).c_str());
							break;
						}
					}
					direct = Direct(sound, sessions, direct);
					if (static_cast<int>(direct) != loggedDirect) {
						loggedDirect = direct;
						const char* why = sound == Sound::kDirect ? "direct"
							: sound == Sound::kRadio                ? "radio"
							: !renders(sessions)                    ? "auto, until it plays somewhere"
							: direct                                ? "auto, it plays on the default output device"
																	: "auto, it plays on another output device";
						if (direct) {
							LOG("app: %s heard directly (AppSound = %s): the station stays silent and only pauses/resumes it", name.c_str(), why);
						}
						else {
							LOG("app: %s heard through the station (AppSound = %s)", name.c_str(), why);
						}
					}
					const std::string described = Describe(sessions);
					if (described != sessionsLogged) {
						sessionsLogged = described;
						LOG("app: %s's audio sessions: %s", name.c_str(), described.c_str());
					}
					// Heard directly, the default device is where it belongs.
					const unsigned warnings = Warnings(sessions) & (direct ? ~unsigned{ kWarnDefaultDevice } : ~0u);
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
				if (direct) {
					// While it changes (the radio's 0.5-2.5 s fades), every 30 ms at most: the loop runs per capture
					// packet (~10 ms) and enumerating every device's sessions isn't free. Unchanged, every 2 s (a
					// session that appeared, a slider the player moved).
					const float gain = live::RadioGain();
					const int32_t since = static_cast<int32_t>(GetTickCount() - appliedAt);
					if ((std::fabs(gain - appliedGain) > 0.002f && since >= 30) || since >= 2'000) {
						follower.Apply(pid, gain);
						appliedGain = gain;
						appliedAt = GetTickCount();
					}
				}
				else {
					follower.Restore();
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
					// Heard directly: the station plays silence, or the player would hear the app twice.
					if (direct) {
						std::fill(samples.begin(), samples.end(), int16_t{ 0 });
					}
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
			follower.Restore();
			CloseHandle(process);
			live::SetWarning({});
		}
		titles.join();
		if (SUCCEEDED(com)) {
			CoUninitialize();
		}
	}
}

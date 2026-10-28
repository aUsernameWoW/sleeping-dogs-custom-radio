#include "app_device.hh"

#include <inspectable.h>
#include <mmdeviceapi.h>
#include <roapi.h>

#include <winrt/base.h>

namespace appdevice
{
	namespace
	{
		// IAudioPolicyConfigFactory: 19 methods we don't call after IInspectable's, then the three we do. Its IID
		// changed in Windows 10 21H2; the layout didn't.
		struct IAudioPolicyConfigFactory : IInspectable
		{
			virtual HRESULT STDMETHODCALLTYPE Unused01() = 0;
			virtual HRESULT STDMETHODCALLTYPE Unused02() = 0;
			virtual HRESULT STDMETHODCALLTYPE Unused03() = 0;
			virtual HRESULT STDMETHODCALLTYPE Unused04() = 0;
			virtual HRESULT STDMETHODCALLTYPE Unused05() = 0;
			virtual HRESULT STDMETHODCALLTYPE Unused06() = 0;
			virtual HRESULT STDMETHODCALLTYPE Unused07() = 0;
			virtual HRESULT STDMETHODCALLTYPE Unused08() = 0;
			virtual HRESULT STDMETHODCALLTYPE Unused09() = 0;
			virtual HRESULT STDMETHODCALLTYPE Unused10() = 0;
			virtual HRESULT STDMETHODCALLTYPE Unused11() = 0;
			virtual HRESULT STDMETHODCALLTYPE Unused12() = 0;
			virtual HRESULT STDMETHODCALLTYPE Unused13() = 0;
			virtual HRESULT STDMETHODCALLTYPE Unused14() = 0;
			virtual HRESULT STDMETHODCALLTYPE Unused15() = 0;
			virtual HRESULT STDMETHODCALLTYPE Unused16() = 0;
			virtual HRESULT STDMETHODCALLTYPE Unused17() = 0;
			virtual HRESULT STDMETHODCALLTYPE Unused18() = 0;
			virtual HRESULT STDMETHODCALLTYPE Unused19() = 0;
			virtual HRESULT STDMETHODCALLTYPE SetPersistedDefaultAudioEndpoint(UINT pid, EDataFlow flow, ERole role, HSTRING deviceId) = 0;
			virtual HRESULT STDMETHODCALLTYPE GetPersistedDefaultAudioEndpoint(UINT pid, EDataFlow flow, ERole role, HSTRING* deviceId) = 0;
			virtual HRESULT STDMETHODCALLTYPE ClearAllPersistedApplicationDefaultEndpoints() = 0;
		};

		constexpr IID kIid21H2 = { 0xab3d4648, 0xe242, 0x459f, { 0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24 } };
		constexpr IID kIidDownlevel = { 0x2a59116d, 0x6c4f, 0x45e0, { 0xa7, 0x4f, 0x70, 0x7e, 0x3f, 0xef, 0x92, 0x58 } };

		constexpr wchar_t kToken[] = L"\\\\?\\SWD#MMDEVAPI#";
		constexpr wchar_t kRender[] = L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}"; // DEVINTERFACE_AUDIO_RENDER

		std::string Hex(HRESULT hr)
		{
			char text[16];
			std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(hr));
			return text;
		}

		// The factory, once per process; null with the reason if the system has none.
		IAudioPolicyConfigFactory* Factory(std::string& error)
		{
			static std::string failure;
			static IAudioPolicyConfigFactory* const factory = [] () -> IAudioPolicyConfigFactory* {
				// RoGetActivationFactory is delay-loaded (missing under Wine and before Windows 8).
				if (!LoadLibraryW(L"api-ms-win-core-winrt-l1-1-0.dll") || !LoadLibraryW(L"api-ms-win-core-winrt-error-l1-1-1.dll")) {
					failure = "no WinRT here";
					return nullptr;
				}
				const winrt::hstring name = L"Windows.Media.Internal.AudioPolicyConfig";
				void* result = nullptr;
				HRESULT hr = RoGetActivationFactory(static_cast<HSTRING>(winrt::get_abi(name)), kIid21H2, &result);
				if (FAILED(hr)) {
					hr = RoGetActivationFactory(static_cast<HSTRING>(winrt::get_abi(name)), kIidDownlevel, &result);
				}
				if (FAILED(hr)) {
					failure = "the system's per-app audio device setting isn't available (" + Hex(hr) + ")";
					return nullptr;
				}
				return static_cast<IAudioPolicyConfigFactory*>(result); // kept for the process's life
			}();
			if (!factory) {
				error = failure;
			}
			return factory;
		}
	}

	std::wstring PolicyId(const std::wstring& deviceId)
	{
		return deviceId.empty() ? std::wstring() : kToken + deviceId + kRender;
	}

	std::wstring DeviceIdOf(const std::wstring& policyId)
	{
		std::wstring id = policyId;
		if (id.rfind(kToken, 0) == 0) {
			id.erase(0, std::size(kToken) - 1);
		}
		const size_t suffix = std::size(kRender) - 1;
		if (id.size() >= suffix && _wcsicmp(id.c_str() + id.size() - suffix, kRender) == 0) {
			id.resize(id.size() - suffix);
		}
		return id;
	}

	bool Get(DWORD pid, std::wstring& deviceId, std::string& error)
	{
		IAudioPolicyConfigFactory* factory = Factory(error);
		if (!factory) {
			return false;
		}
		winrt::hstring id;
		const HRESULT hr = factory->GetPersistedDefaultAudioEndpoint(pid, eRender, eMultimedia, reinterpret_cast<HSTRING*>(winrt::put_abi(id)));
		if (FAILED(hr)) {
			error = "reading the program's output device failed (" + Hex(hr) + ")";
			return false;
		}
		deviceId = DeviceIdOf(std::wstring(id));
		return true;
	}

	bool Set(DWORD pid, const std::wstring& deviceId, std::string& error)
	{
		IAudioPolicyConfigFactory* factory = Factory(error);
		if (!factory) {
			return false;
		}
		const winrt::hstring id{ PolicyId(deviceId) };
		// A null HSTRING is the empty string: back to the default device.
		const HSTRING abi = id.empty() ? nullptr : static_cast<HSTRING>(winrt::get_abi(id));
		for (const ERole role : { eMultimedia, eConsole }) {
			const HRESULT hr = factory->SetPersistedDefaultAudioEndpoint(pid, eRender, role, abi);
			if (FAILED(hr)) {
				error = "setting the program's output device failed (" + Hex(hr) + ")";
				return false;
			}
		}
		return true;
	}
}

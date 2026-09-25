#include "d3d.hh"

#include <Windows.h>
#include <d3d11.h>
#include <MinHook.h>

#include <atomic>
#include <cstring>
#include <mutex>

#include "cover.hh"
#include "log.hh"

namespace d3d
{
	namespace
	{
		// Logo_HKPDScanner as the installed build's UI.big has it: 128×64 DXT5, one level, and the FNV-1a-64 of
		// its 8192 bytes of blocks.
		constexpr UINT kLogoWidth = 128;
		constexpr UINT kLogoHeight = 64;
		constexpr size_t kLogoBytes = kLogoWidth / 4 * (kLogoHeight / 4) * 16;
		constexpr uint64_t kLogoHash = 0xB3D3AD52833FA849ull;

		// Vtable slots (d3d11.h declaration order): ID3D11Device::CreateTexture2D follows IUnknown's three,
		// CreateBuffer and CreateTexture1D; ID3D11DeviceContext::PSSetShaderResources follows IUnknown's three,
		// ID3D11DeviceChild's four and VSSetConstantBuffers.
		constexpr size_t kCreateTexture2DSlot = 5;
		constexpr size_t kPSSetShaderResourcesSlot = 8;

		using CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
			D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
		using CreateTexture2DFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
		using PSSetShaderResourcesFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);

		CreateDeviceFn gCreateDevice = nullptr;
		CreateTexture2DFn gCreateTexture2D = nullptr;
		PSSetShaderResourcesFn gPSSetShaderResources = nullptr;

		std::atomic<ID3D11DeviceContext*> gContext{ nullptr }; // the game's immediate context
		std::mutex gLock;
		ID3D11Texture2D* gTexture = nullptr;                 // the logo texture we made (our own reference)
		std::atomic<uint32_t> gShown{ 0xFFFFFFFF };           // cover generation gTexture holds (or was skipped)
		std::atomic<int> gOtherLogos{ 0 };                    // 128×64 BC3 textures that weren't it, logged up to a few

		uint64_t Fnv1a64(const void* data, size_t size)
		{
			uint64_t h = 0xCBF29CE484222325ull;
			for (size_t i = 0; i < size; ++i) {
				h = (h ^ static_cast<const uint8_t*>(data)[i]) * 0x100000001B3ull;
			}
			return h;
		}

		bool IsLogoCandidate(const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* initial)
		{
			return desc && initial && initial->pSysMem && desc->Width == kLogoWidth && desc->Height == kLogoHeight && desc->MipLevels == 1 &&
				desc->ArraySize == 1 && (desc->Format == DXGI_FORMAT_BC3_UNORM || desc->Format == DXGI_FORMAT_BC3_UNORM_SRGB);
		}

		HRESULT STDMETHODCALLTYPE CreateTexture2DHook(ID3D11Device* self, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* initial, ID3D11Texture2D** out)
		{
			if (!out || !IsLogoCandidate(desc, initial)) {
				return gCreateTexture2D(self, desc, initial, out);
			}
			const uint64_t hash = Fnv1a64(initial->pSysMem, kLogoBytes);
			if (hash != kLogoHash) {
				if (gOtherLogos.fetch_add(1) < 12) {
					LOG("d3d: 128x64 BC3 texture %016llX isn't the HKPD logo", static_cast<unsigned long long>(hash));
				}
				return gCreateTexture2D(self, desc, initial, out);
			}

			const cover::Current current = cover::Get();
			if (!current.mImage) {
				LOG("d3d: HKPD logo texture created before any cover was ready; left as it is");
				return gCreateTexture2D(self, desc, initial, out);
			}
			const coverimage::Image& image = *current.mImage;

			D3D11_TEXTURE2D_DESC ours = *desc;
			ours.Width = image.mWidth;
			ours.Height = image.mHeight;
			ours.MipLevels = static_cast<UINT>(image.mMips.size());
			ours.Usage = D3D11_USAGE_DEFAULT; // immutable in the game; UpdateSubresource needs DEFAULT
			ours.CPUAccessFlags = 0;
			D3D11_SUBRESOURCE_DATA levels[16] = {};
			for (size_t i = 0; i < image.mMips.size() && i < 16; ++i) {
				levels[i].pSysMem = image.mMips[i].data();
				levels[i].SysMemPitch = image.RowPitch(i);
			}

			const HRESULT hr = gCreateTexture2D(self, &ours, levels, out);
			if (FAILED(hr) || !*out) {
				LOG("d3d: creating the %ux%u logo failed (0x%08lX); the HKPD logo stays", ours.Width, ours.Height, static_cast<unsigned long>(hr));
				return gCreateTexture2D(self, desc, initial, out);
			}

			{
				std::lock_guard lock(gLock);
				if (gTexture) {
					gTexture->Release();
				}
				gTexture = *out;
				gTexture->AddRef();
				gShown = current.mGeneration;
			}
			LOG("d3d: HKPD logo texture replaced: %ux%u, %u mips (cover #%u)", ours.Width, ours.Height, ours.MipLevels, current.mGeneration);
			return hr;
		}

		// On the render thread: writes the current picture into our texture.
		void Upload(ID3D11DeviceContext* context)
		{
			const cover::Current current = cover::Get();
			std::lock_guard lock(gLock);
			if (gTexture && current.mImage) {
				const coverimage::Image& image = *current.mImage;
				D3D11_TEXTURE2D_DESC desc;
				gTexture->GetDesc(&desc);
				if (desc.Width == image.mWidth && desc.Height == image.mHeight && desc.MipLevels == image.mMips.size()) {
					for (size_t i = 0; i < image.mMips.size(); ++i) {
						context->UpdateSubresource(gTexture, static_cast<UINT>(i), nullptr, image.mMips[i].data(), image.RowPitch(i), 0);
					}
					LOG("d3d: logo updated to cover #%u", current.mGeneration);
				}
			}
			gShown = current.mGeneration; // with no texture yet, the next one is created with this picture anyway
		}

		void STDMETHODCALLTYPE PSSetShaderResourcesHook(ID3D11DeviceContext* self, UINT start, UINT count, ID3D11ShaderResourceView* const* views)
		{
			if (self == gContext.load(std::memory_order_relaxed) && gShown.load(std::memory_order_relaxed) != cover::Generation()) {
				Upload(self);
			}
			gPSSetShaderResources(self, start, count, views);
		}

		template <typename T>
		bool HookMethod(const char* name, void* object, size_t slot, void* detour, T& original)
		{
			void* target = (*static_cast<void***>(object))[slot];
			MH_STATUS status = MH_CreateHook(target, detour, reinterpret_cast<void**>(&original));
			if (status == MH_OK) {
				status = MH_EnableHook(target);
			}
			if (status != MH_OK) {
				LOG("d3d: hooking %s failed: %s", name, MH_StatusToString(status));
				return false;
			}
			return true;
		}

		HRESULT WINAPI CreateDeviceHook(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels,
			UINT levelCount, UINT sdkVersion, ID3D11Device** device, D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context)
		{
			const HRESULT hr = gCreateDevice(adapter, driverType, software, flags, levels, levelCount, sdkVersion, device, level, context);
			if (FAILED(hr) || !device || !*device) {
				return hr;
			}

			ID3D11DeviceContext* immediate = context ? *context : nullptr;
			if (!immediate) {
				(*device)->GetImmediateContext(&immediate);
				immediate->Release(); // the device keeps it alive; we only compare the pointer
			}
			gContext = immediate;

			static bool hooked = false;
			if (!hooked) {
				hooked = true;
				const bool ok = HookMethod("ID3D11Device::CreateTexture2D", *device, kCreateTexture2DSlot, &CreateTexture2DHook, gCreateTexture2D) &&
					HookMethod("ID3D11DeviceContext::PSSetShaderResources", immediate, kPSSetShaderResourcesSlot, &PSSetShaderResourcesHook, gPSSetShaderResources);
				LOG("d3d: device %p (flags 0x%X), context %p; logo hooks %s", *device, flags, immediate, ok ? "ready" : "FAILED");
			}
			return hr;
		}

		// Points the module's import of `dll!function` at `replacement`; the previous target goes to `original`.
		bool PatchImport(HMODULE module, const char* dll, const char* function, void* replacement, void** original)
		{
			auto* base = reinterpret_cast<uint8_t*>(module);
			auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
			auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
			if (!dir.VirtualAddress) {
				return false;
			}
			for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; ++desc) {
				if (_stricmp(reinterpret_cast<const char*>(base + desc->Name), dll) != 0 || !desc->OriginalFirstThunk) {
					continue;
				}
				auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
				auto* funcs = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
				for (; names->u1.AddressOfData; ++names, ++funcs) {
					if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
						continue;
					}
					auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
					if (std::strcmp(reinterpret_cast<const char*>(byName->Name), function) != 0) {
						continue;
					}
					DWORD protect;
					if (!VirtualProtect(&funcs->u1.Function, sizeof(funcs->u1.Function), PAGE_READWRITE, &protect)) {
						return false;
					}
					*original = reinterpret_cast<void*>(funcs->u1.Function);
					funcs->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
					VirtualProtect(&funcs->u1.Function, sizeof(funcs->u1.Function), protect, &protect);
					return true;
				}
			}
			return false;
		}
	}

	bool Install()
	{
		if (!PatchImport(GetModuleHandleW(nullptr), "d3d11.dll", "D3D11CreateDevice", reinterpret_cast<void*>(&CreateDeviceHook),
				reinterpret_cast<void**>(&gCreateDevice))) {
			LOG("d3d: no D3D11CreateDevice import in the game; the logo stays the borrowed one");
			return false;
		}
		LOG("d3d: D3D11CreateDevice import patched");
		return true;
	}
}

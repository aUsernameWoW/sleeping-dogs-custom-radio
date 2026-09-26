#include <Windows.h>

#include <string>

#include "core/config.hh"
#include "core/crash.hh"
#include "core/hooks.hh"
#include "core/library.hh"
#include "core/log.hh"
#include "core/logo.hh"

static std::wstring GetModuleDirectory(HMODULE module)
{
	wchar_t path[MAX_PATH] = {};
	const DWORD length = GetModuleFileNameW(module, path, ARRAYSIZE(path));
	if (length == 0 || length >= ARRAYSIZE(path)) {
		return L".";
	}

	std::wstring dir(path, length);
	const size_t slash = dir.find_last_of(L"\\/");
	return slash == std::wstring::npos ? L"." : dir.substr(0, slash);
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(module);

		// Pin ourselves: the hooks and the worker threads point into this module.
		HMODULE pinned;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&DllMain), &pinned);

		const std::wstring dir = GetModuleDirectory(module);
		config::Load(dir);

		if (gConfig.mLogging) {
			logger::Open(dir + L"\\SDRadio.log");
			crash::Install(dir);
		}

		LOG("SDRadio loaded, music folder %s", logger::ToUtf8(gConfig.mMusicFolder.c_str()).c_str());

		// So there's an obvious place to drop music into on first run.
		CreateDirectoryW(gConfig.mMusicFolder.c_str(), nullptr);

		// The scan and the logo start now (their threads run once the loader lock is released) and are
		// normally finished before the game reads Radios.xml, which the hook waits for. The hooks go in
		// before the game runs.
		library::StartScan(gConfig.mMusicFolder);
		if (gConfig.mCustomLogo) {
			logo::Start(dir, gConfig.mMusicFolder);
		}
		hooks::Install();
	}

	return TRUE;
}

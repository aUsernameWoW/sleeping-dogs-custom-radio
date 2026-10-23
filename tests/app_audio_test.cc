// App capture (core/app_audio.cc) without playing anything: finding a process tree's root by name, and opening a
// process loopback capture either working (Windows 11, Windows Server 2025) or failing with a reason, never
// crashing (Wine has no process loopback). The capture itself is tested by app_audio_manual. argv[1] is unused.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "../core/app_audio.cc"
#include "../core/live.cc"
#include "../core/log.cc"

#include <cstdio>

#pragma comment(lib, "ole32.lib")

namespace
{
	int gFailures = 0;

	void Check(bool ok, const char* what)
	{
		if (!ok) {
			std::printf("FAIL: %s\n", what);
			++gFailures;
		}
	}
}

int main()
{
	wchar_t path[MAX_PATH];
	GetModuleFileNameW(nullptr, path, MAX_PATH);
	std::wstring name = path;
	name = name.substr(name.find_last_of(L"\\/") + 1);
	for (wchar_t& c : name) c = static_cast<wchar_t>(towupper(c));

	Check(appaudio::FindProcess(name) == GetCurrentProcessId(), "finds this process by its name, case-insensitively");
	Check(appaudio::FindProcess(L"no-such-program-sdradio.exe") == 0, "a program that isn't running isn't found");

	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	appaudio::Capture capture;
	std::string error;
	if (capture.Open(GetCurrentProcessId(), error)) {
		std::vector<int16_t> samples;
		Check(capture.Read(samples, 200, error), "an open capture reads");
		std::printf("process loopback works here (%zu samples of this silent process)\n", samples.size());
	}
	else {
		Check(!error.empty(), "a capture that can't open says why");
		std::printf("process loopback isn't available here: %s\n", error.c_str());
	}

	if (gFailures) {
		return 1;
	}
	std::printf("PASS\n");
	return 0;
}

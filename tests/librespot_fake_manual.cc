// A stand-in for librespot, for spotify_test: checks the arguments SDRadio passes, logs a track_changed event the
// way librespot 0.8 does (env_logger header, then the {:#?} dump of the --onevent environment), then writes a
// 440 Hz sine as 44.1 kHz 16-bit stereo to stdout for as long as anyone reads (writes block, as librespot's pipe
// backend does). Compiled, not run by itself.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
	void Err(const std::string& text)
	{
		DWORD written;
		WriteFile(GetStdHandle(STD_ERROR_HANDLE), text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
	}
}

int wmain()
{
	const std::wstring command = GetCommandLineW();
	for (const wchar_t* required : { L"--backend pipe", L"--format S16", L"--name \"Test Device\"", L"--onevent \"cmd /c rem\"" }) {
		if (command.find(required) == std::wstring::npos) {
			Err("[2025-11-10T12:00:00Z ERROR librespot] missing argument\n");
			return 2;
		}
	}
	wchar_t rustLog[256] = {};
	GetEnvironmentVariableW(L"RUST_LOG", rustLog, 256);
	if (std::wstring(rustLog).find(L"librespot::player_event_handler=debug") == std::wstring::npos) {
		Err("[2025-11-10T12:00:00Z ERROR librespot] RUST_LOG not set\n");
		return 3;
	}

	Err("[2025-11-10T12:00:00Z INFO  librespot] librespot 0.8.0 (fake)\r\n");
	Err("[2025-11-10T12:00:01Z DEBUG librespot_core::session] noise that isn't logged\n");
	// UTF-8 as librespot writes it; \" and \n escaped as Rust's Debug does.
	Err("[2025-11-10T12:00:02Z DEBUG librespot::player_event_handler] Running cmd /c rem with environment variables:\n"
		"{\n"
		"    \"PLAYER_EVENT\": \"track_changed\",\n"
		"    \"NAME\": \"\xE8\x87\xAA\xE7\x94\xB1\xE7\x9A\x84\xE4\xBD\xA0 \\\"Live\\\"\",\n"
		"    \"ARTISTS\": \"G.E.M.\\nGuest\",\n"
		"    \"URI\": \"spotify:track:0\",\n"
		"}\n");

	std::vector<int16_t> chunk(4410 * 2);
	double phase = 0.0;
	HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
	for (;;) {
		for (size_t i = 0; i < chunk.size(); i += 2) {
			chunk[i] = chunk[i + 1] = static_cast<int16_t>(8000 * std::sin(phase));
			phase += 2.0 * 3.14159265358979 * 440.0 / 44100.0;
		}
		DWORD written;
		if (!WriteFile(out, chunk.data(), static_cast<DWORD>(chunk.size() * 2), &written, nullptr)) {
			return 0; // the reader is gone
		}
	}
}

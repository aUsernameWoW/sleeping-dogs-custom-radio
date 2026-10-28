#pragma once

// The output device an app plays on by its own choice: the per-app "Output device" in Settings > System > Sound >
// Volume mixer, which Windows keeps per program. Windows has no documented API for it: this is the WinRT class
// Windows.Media.Internal.AudioPolicyConfig that the Settings page uses, called the way EarTrumpet (MIT) does
// (its interface layouts; own code). Missing under Wine and before Windows 10 1803.
//
// Not used by the .asi (only tests/app_audio_manual.cc, `move` / `follow`): setting it changes where a program's
// next audio stream opens, not the one it plays on now (2026-10-28, Windows 11). Apple Music keeps one stream
// open through pause, resume, seeks and most track changes, so a move from the mod would only be heard after
// the program restarts, and moving it back after the game likewise. See CLAUDE.md, "App audio".

#include <Windows.h>

#include <string>

namespace appdevice
{
	// The device `pid`'s program chose, as an MMDevice ID ("{0.0.0.00000000}.{guid}"); empty while it follows the
	// default device. False (with `error`) if the system can't say.
	bool Get(DWORD pid, std::wstring& deviceId, std::string& error);
	// Sends `pid`'s program to that device (empty: back to the default device), for the console and multimedia
	// roles alike. Windows remembers the choice after the program exits.
	bool Set(DWORD pid, const std::wstring& deviceId, std::string& error);

	// The MMDevice ID wrapped as the policy config takes and returns it, and back. Pure.
	std::wstring PolicyId(const std::wstring& deviceId);
	std::wstring DeviceIdOf(const std::wstring& policyId);
}

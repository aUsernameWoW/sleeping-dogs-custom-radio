#pragma once

// The station logo: logo.png from the music folder if there is one (a player's own), else the built-in one
// (art\logo_512.png, the LOGO resource of the .asi, see SDRadio.rc), written at startup as a texture pack
// of our own next to the .asi (SDRadio-logo.perm.bin / .temp.bin, see logo_pack.hh) on a background
// thread; Radios.xml then names that pack.

#include <Windows.h>

#include <string>

namespace logo
{
	// The texture's name in the pack, for Radios.xml's TextureName.
	constexpr char kTextureName[] = "Logo_SDRadio";

	// `asiDir` is where the pack goes; the game folder is the executable's.
	void Start(const std::wstring& asiDir, const std::wstring& musicFolder);

	// The TexturePack for Radios.xml once the pack is written, or empty if there is none (it couldn't be
	// made, or it isn't ready within `timeoutMs`).
	std::string Wait(DWORD timeoutMs);
}

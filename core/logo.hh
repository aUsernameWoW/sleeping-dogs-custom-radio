#pragma once

// The station logo: logo.png from the music folder if there is one (a player's own), else the built-in one
// (art\logo_512.png, the LOGO resource of the .asi, see SDRadio.rc), written at startup as a texture pack
// of our own next to the .asi (SDRadio-logo.perm.bin / .temp.bin, see logo_pack.hh) on a background
// thread; Radios.xml then names that pack.

#include <Windows.h>

#include <string>
#include <vector>

namespace logo
{
	// The texture's name in the pack, for Radios.xml's TextureName.
	constexpr char kTextureName[] = "Logo_SDRadio";

	// `asiDir` is where the pack goes; the game folder is the executable's.
	void Start(const std::wstring& asiDir, const std::wstring& musicFolder);

	// The TexturePack for Radios.xml once the pack is written, or empty if there is none (it couldn't be
	// made, or it isn't ready within `timeoutMs`).
	std::string Wait(DWORD timeoutMs);

	// A pack of a station's own, SDRadio-logo-<suffix>, from `folder`'s logo.png or the built-in logo, with
	// texture Logo_SDRadio_<suffix> (returned in `textureName`): each station needs its own, as the HUD widget
	// misbehaves when two stations share a pack (hooks.cc, LoadTexturesHook). The TexturePack for Radios.xml,
	// or empty if there is none. After Wait.
	std::string StationPack(const std::wstring& folder, const std::string& suffix, std::string& textureName);

	// Deletes the SDRadio-logo-<suffix> packs next to the .asi whose suffix isn't in `keep` (a subfolder that
	// was a station before).
	void RemoveOtherStationPacks(const std::vector<std::string>& keep);
}

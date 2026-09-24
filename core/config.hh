#pragma once

#include <string>

struct Config
{
	// Shown on the HUD when switching stations (UTF-8, at most 63 bytes).
	std::string mStationName = "SDRADIO";

	// Where the music is (subfolders included). Empty = the SDRadio folder next to the .asi.
	std::wstring mMusicFolder;

	// HUD logo: any station's texture from UI.big until the mod ships its own.
	std::string mTextureName = "Logo_Softly";
	std::string mTexturePack = "Radio_Softly_TexturePack";

	// Write SDRadio.log next to the .asi.
	bool mLogging = true;
};

extern Config gConfig;

namespace config
{
	// Loads <dir>\SDRadio.ini (UTF-8), writing a default one if it doesn't exist, and resolves the music
	// folder against `dir`.
	void Load(const std::wstring& dir);
}

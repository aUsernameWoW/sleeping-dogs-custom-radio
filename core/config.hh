#pragma once

#include <string>

struct Config
{
	// Shown on the HUD when switching stations (UTF-8, at most 63 bytes).
	std::string mStationName = "SDRADIO";

	// Where the music is (subfolders included). Empty = the SDRadio folder next to the .asi.
	std::wstring mMusicFolder;

	// HUD logo: logo.png from the music folder, else the built-in one, as a texture pack of our own (logo.hh).
	// Off, or if the pack can't be made: the two settings below.
	bool mCustomLogo = true;

	// HUD logo borrowed from the game: any station's texture from UI.big.
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

#pragma once

#include <string>

struct Config
{
	// Shown on the HUD when switching stations (UTF-8, at most 63 bytes).
	std::string mStationName = "SDRADIO";

	// Where the music is (subfolders included). Empty = the SDRadio folder next to the .asi.
	std::wstring mMusicFolder;

	// HUD logo shows the playing track's cover art (see cover.hh, d3d.hh). Takes the HKPD scanner's logo slot
	// and overrides the two settings below.
	bool mCoverArt = true;

	// HUD logo without cover art: any station's texture from UI.big.
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

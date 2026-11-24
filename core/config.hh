#pragma once

#include <map>
#include <string>
#include <vector>

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

	// `[Car] Station`: what the radio plays when the player gets into another car (hooks.cc): "last" (the station
	// last tuned to, HKPD aside; SDRadio's first station until there is one), "sdradio" (our first music station,
	// else the live one), "live" (the live one, else the first music station) or "game" (the car's own).
	std::string mCarStation = "last";

	// A second station playing a live source (live.hh); `[Live] Source = off` (the default) turns it off.
	bool mLive = false;
	std::string mLiveName = "SDRADIO LIVE";
	// Which source: "test" (a beep each second), "radio" (internet radio at mLiveUrl, netradio.hh) or "app".
	std::string mLiveSource = "test";
	// The internet radio stream (or .m3u/.pls) for Source = radio.
	std::string mLiveUrl;
	// The program whose sound Source = app plays (process name, e.g. L"cloudmusic.exe"; app_audio.hh).
	std::wstring mLiveApp;
	// For Source = app: pause the program while the game doesn't play the live station, resume it after.
	bool mLivePauseApp = true;
	// For Source = app: how the program is heard, "auto", "direct" or "radio" (appaudio::ParseSound).
	std::string mLiveAppSound = "auto";
	// For Source = spotify (spotify.hh): librespot.exe (default: next to the .asi), the Spotify Connect device
	// name, and where librespot keeps its credentials (next to the .asi).
	std::wstring mLibrespot;
	std::string mSpotifyName = "SDRadio";
	std::wstring mSpotifyCache;

	// Write SDRadio.log next to the .asi.
	bool mLogging = true;

	// What Load did to the file, for the log (it's read before the log opens): empty, or e.g. "moved to the
	// new layout (the old file is SDRadio.ini.bak)".
	std::string mIniNote;
};

extern Config gConfig;

namespace config
{
	// "Section.Key" -> value (UTF-8, as written).
	using Values = std::map<std::string, std::string>;

	Values Parse(const std::string& text);
	// Moves the keys of the layout before 2026-11-24 ([Station], [Live] with every source's keys) to the current
	// one; true if there were any.
	bool Migrate(Values& values);
	// The default ini with `values` in place of its defaults; keys it doesn't have go at the end of their
	// section if it has that section, else they're dropped.
	std::string Render(const Values& values);
	// The default ini's "Section.Key"s, in order.
	std::vector<std::string> TemplateKeys();

	// Loads <dir>\SDRadio.ini (UTF-8): writes the default if there's none, rewrites it in the current layout
	// (keeping every value, the old file as SDRadio.ini.bak) when it has old keys or lacks current ones, and
	// resolves the music folder against `dir`.
	void Load(const std::wstring& dir);
}

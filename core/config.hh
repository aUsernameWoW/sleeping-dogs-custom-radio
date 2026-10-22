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

	// A second station playing a live source (live.hh); off by default.
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
	// For Source = spotify (spotify.hh): librespot.exe (default: next to the .asi), the Spotify Connect device
	// name, and where librespot keeps its credentials (next to the .asi).
	std::wstring mLibrespot;
	std::string mSpotifyName = "SDRadio";
	std::wstring mSpotifyCache;

	// Experimental external audio, diagnostics only for now (external.hh).
	bool mExternal = false;
	// Media session to read: a part of the app's ID ("spotify"); empty = the current one.
	std::string mMediaSession;
	// Executable to capture ("Spotify.exe"); empty = everything the playback device below plays.
	std::wstring mCaptureProcess;
	// Playback device for that: part of its name; empty = the default device.
	std::string mCaptureDevice;

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

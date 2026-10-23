#pragma once

// Spotify on the live station (`[Live] Source = spotify`): runs librespot (https://github.com/librespot-org/librespot,
// MIT; a separate program, not compiled in) as a Spotify Connect device the player picks in their Spotify app
// (Premium only; librespot is an unofficial client). Its pipe backend writes raw 44.1 kHz 16-bit stereo to
// stdout and blocks while nobody reads, so reading at real-time pace paces the player, and not reading while
// the game doesn't play the station holds it. The title comes from librespot's own debug log: for every
// player event it runs the `--onevent` program and logs that program's environment (NAME, ARTISTS...), which
// is parsed from its stderr; the program itself does nothing. The idea of librespot behind a game radio is
// FH6 Universal Radio's.

#include <atomic>
#include <string>
#include <string_view>

namespace spotify
{
	struct Options
	{
		std::wstring mLibrespot;  // librespot.exe
		std::string mDeviceName;  // shown in Spotify's device list
		std::wstring mCacheDir;   // credentials (so the device can be found again without signing in)
	};

	// One player event from librespot's log, as far as the HUD cares.
	struct Event
	{
		std::string mType;    // PLAYER_EVENT: "track_changed", "playing", "paused", ...
		std::string mName;    // NAME
		std::string mArtists; // ARTISTS, one per line
	};

	// Reads librespot's stderr line by line and picks out the logged environment of each event, which looks like
	//   [... DEBUG librespot::player_event_handler] Running cmd /c rem with environment variables:
	//   {
	//       "PLAYER_EVENT": "track_changed",
	//       "NAME": "...",
	//   }
	// (Rust's {:#?} of a HashMap: keys and values are Rust-escaped strings).
	class EventParser
	{
	public:
		// True when `line` ended an event's block; the event is in `out`.
		bool Feed(std::string_view line, Event& out);

	private:
		bool mInBlock = false;
		Event mCurrent;
	};

	// A Rust Debug-escaped string's content (without the quotes) back to UTF-8: \" \\ \n \r \t \' \0 \u{XXXX}.
	std::string Unescape(std::string_view escaped);
	// "Artist A, Artist B - Name", or the name alone; empty without a name.
	std::string Title(const Event& event);

	// The live source: starts librespot (again if it exits), feeds its audio into the live ring while the game
	// plays the station, and its titles to the HUD. Returns once `stop` is set, ending librespot.
	void Run(const Options& options, const std::atomic<bool>& stop);
}

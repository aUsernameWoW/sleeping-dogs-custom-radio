#pragma once

// Internet radio as a live source (`[Live] Source = radio`, `Url = ...`): an HTTP(S) stream (Icecast,
// SHOUTcast 2, or an .m3u/.pls pointing at one) fetched with WinHTTP (Schannel on Windows, Wine's own
// implementation elsewhere), ICY metadata split off for the title, MP3 or Ogg Vorbis decoded, resampled to
// 48 kHz and pushed into the live ring at real-time pace.
//
// Pacing: servers send a burst on connect (often several seconds) and then real time, and our clock and
// theirs drift apart a little. Decoded audio goes into a FIFO (pacer.hh) that a feeder thread empties into
// the ring by the local clock; the network side stops reading while kMaxBufferedMs are queued (TCP then
// slows the server down). Errors reconnect with backoff (2 s, doubling to 30 s).

#include <atomic>
#include <string>

namespace netradio
{
	// Plays `url` into live::Stream() until `stop` (reconnecting on errors).
	void Run(const std::string& url, const std::atomic<bool>& stop);
}

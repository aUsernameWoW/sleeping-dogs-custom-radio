#pragma once

// The parts of internet radio that are plain text and byte shuffling (pure, see icy_test):
// - ICY metadata: a client that sends "Icy-MetaData: 1" gets "icy-metaint: N", then every N audio bytes a
//   length byte L and L * 16 bytes of text such as StreamTitle='Artist - Title';StreamUrl='';
// - playlists: a station URL often names an .m3u or .pls file listing the real stream.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace icy
{
	// Splits an ICY stream into audio bytes and metadata blocks. With metaint 0 everything is audio.
	class Demuxer
	{
	public:
		explicit Demuxer(uint32_t metaint) : mMetaint(metaint), mUntilMeta(metaint) {}

		// Appends the audio bytes of `data` to `audio` and every complete, non-empty metadata block to `metadata`.
		void Feed(const uint8_t* data, size_t size, std::vector<uint8_t>& audio, std::vector<std::string>& metadata);

	private:
		uint32_t mMetaint;
		uint32_t mUntilMeta;     // audio bytes left before the next length byte
		bool mInMeta = false;
		uint32_t mMetaLeft = 0;  // metadata bytes still to come
		std::string mMeta;
	};

	// The StreamTitle value of a metadata block ("" if none), as UTF-8 (servers send UTF-8, Latin-1 or, for
	// Chinese stations, often GBK; see tags::LegacyToUtf8).
	std::string StreamTitle(std::string_view metadata);

	// Whether a response is a playlist rather than audio, from its Content-Type or, when that says nothing
	// useful, the URL's extension.
	bool IsPlaylist(std::string_view contentType, std::string_view url);

	// The first stream URL in an M3U or PLS playlist; "" if none.
	std::string PlaylistUrl(std::string_view body);

	enum class Format
	{
		Unknown,
		Mp3,
		Ogg,     // Ogg Vorbis
		Aac,     // not decoded yet
		Opus,    // in Ogg; not decoded yet
		OggFlac, // not decoded yet
	};

	// From the Content-Type, else the first bytes of the stream.
	Format DetectFormat(std::string_view contentType, const uint8_t* head, size_t size);
	const char* FormatName(Format format);
}

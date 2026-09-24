#pragma once

// Artist/title from the tag formats the bundled decoders hand over raw: ID3v2 (2.2-2.4) and ID3v1 in MP3,
// Vorbis comments in FLAC and Ogg, RIFF INFO in WAV. Parsed here rather than through the shell's property
// handlers so tags work the same under Wine/Proton, where those handlers are stubs.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace tags
{
	struct Tags
	{
		std::string mArtist; // UTF-8
		std::string mTitle;
		std::string mAlbumArtist; // fallback for a missing artist, see Finish
	};

	// Call once every tag source was read: the album artist stands in for a missing artist.
	void Finish(Tags& t);

	// A full ID3v2 tag, header included. Fills only what's still empty in `out`.
	void ParseId3v2(const uint8_t* data, size_t size, Tags& out);

	// The 128-byte ID3v1 tag ("TAG..."). Fills only what's still empty.
	void ParseId3v1(const uint8_t* data, size_t size, Tags& out);

	// One "KEY=value" Vorbis comment (UTF-8 by definition). Several ARTIST comments are joined with "; ".
	void AddVorbisComment(std::string_view comment, Tags& out);

	// Text in no declared encoding (ID3 "ISO-8859-1", ID3v1, RIFF INFO) to UTF-8. Much Chinese music carries
	// GBK there, so: valid UTF-8 as is, else GBK (code page 936) if it decodes cleanly, else Latin-1.
	std::string LegacyToUtf8(std::string_view bytes);
}

#pragma once

// Artist/title (and on request the cover art) from the tag formats the bundled decoders hand over raw:
// ID3v2 (2.2-2.4) and ID3v1 in MP3, Vorbis comments and PICTURE blocks in FLAC and Ogg, RIFF INFO in WAV. Parsed here rather than through the shell's property
// handlers so tags work the same under Wine/Proton, where those handlers are stubs.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tags
{
	constexpr int kFrontCover = 3; // ID3v2 / FLAC picture type

	struct Tags
	{
		std::string mArtist; // UTF-8
		std::string mTitle;
		std::string mAlbumArtist; // fallback for a missing artist, see Finish

		// Embedded cover art as stored (JPEG, PNG...), collected only when asked for: it can be megabytes
		// per file, and the library scan doesn't need it.
		bool mWantPicture = false;
		std::vector<uint8_t> mPicture;
		int mPictureType = -1; // of mPicture; -1 = none
	};

	// Call once every tag source was read: the album artist stands in for a missing artist.
	void Finish(Tags& t);

	// Offers a picture (if mWantPicture): the front cover wins over other types, else the first one stays.
	void AddPicture(int type, const uint8_t* data, size_t size, Tags& out);

	// A FLAC PICTURE block body (also what a Vorbis METADATA_BLOCK_PICTURE comment holds, base64-encoded).
	void ParseFlacPicture(const uint8_t* data, size_t size, Tags& out);

	// Standard base64 (whitespace skipped); decoding stops at the first invalid character.
	std::vector<uint8_t> Base64Decode(std::string_view text);

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

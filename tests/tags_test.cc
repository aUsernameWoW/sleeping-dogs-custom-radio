// Tag parsing (core/tags.cc) on hand-built tags: ID3v2.2/2.3/2.4 text frames in every encoding, the 2.4
// data-length flag, whole-tag unsynchronisation, album artist as fallback, ID3v1, Vorbis comments, GBK text
// where the format declares none, and cover art (APIC/PIC, FLAC PICTURE via base64). Compiles tags.cc
// directly; argv[1] is unused.

#include "../core/tags.cc"

#include <cstdio>
#include <cstring>

namespace
{
	int gFailures = 0;

	void Check(bool ok, const char* what)
	{
		if (!ok) {
			std::printf("FAIL: %s\n", what);
			++gFailures;
		}
	}

	using Bytes = std::vector<uint8_t>;

	void Append(Bytes& b, std::string_view s) { b.insert(b.end(), s.begin(), s.end()); }

	Bytes Frame(uint8_t version, std::string_view id, const Bytes& payload, uint8_t formatFlags = 0)
	{
		Bytes f;
		Append(f, id);
		const size_t n = payload.size();
		if (version == 2) {
			f.insert(f.end(), { uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n) });
		}
		else if (version == 3) {
			f.insert(f.end(), { uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n), 0, 0 });
		}
		else {
			f.insert(f.end(), { uint8_t(n >> 21 & 0x7F), uint8_t(n >> 14 & 0x7F), uint8_t(n >> 7 & 0x7F), uint8_t(n & 0x7F), 0, formatFlags });
		}
		f.insert(f.end(), payload.begin(), payload.end());
		return f;
	}

	Bytes Tag(uint8_t version, const Bytes& frames, uint8_t flags = 0)
	{
		Bytes t;
		Append(t, "ID3");
		const size_t n = frames.size() + 16; // some padding
		t.insert(t.end(), { version, 0, flags, uint8_t(n >> 21 & 0x7F), uint8_t(n >> 14 & 0x7F), uint8_t(n >> 7 & 0x7F), uint8_t(n & 0x7F) });
		t.insert(t.end(), frames.begin(), frames.end());
		t.resize(t.size() + 16, 0);
		return t;
	}

	Bytes Text(uint8_t encoding, const Bytes& raw)
	{
		Bytes b{ encoding };
		b.insert(b.end(), raw.begin(), raw.end());
		return b;
	}

	Bytes Str(std::string_view s) { return Bytes(s.begin(), s.end()); }

	// "月亮" = U+6708 U+4EAE; UTF-8 E6 9C 88 E4 BA AE; GBK D4 C2 C1 C1.
	constexpr char kMoonUtf8[] = "\xE6\x9C\x88\xE4\xBA\xAE";
}

int main()
{
	{
		tags::Tags t;
		Bytes frames = Frame(3, "TIT2", Text(1, { 0xFF, 0xFE, 0x08, 0x67, 0xAE, 0x4E, 0, 0 })); // UTF-16LE with BOM
		const Bytes artist = Frame(3, "TPE1", Text(0, Str("Beyonc\xE9")));                       // Latin-1
		frames.insert(frames.end(), artist.begin(), artist.end());
		tags::ParseId3v2(Tag(3, frames).data(), Tag(3, frames).size(), t);
		tags::Finish(t);
		Check(t.mTitle == kMoonUtf8, "2.3 UTF-16 title");
		Check(t.mArtist == "Beyonc\xC3\xA9", "2.3 Latin-1 artist stays Latin-1");
	}
	{
		tags::Tags t;
		Bytes frames = Frame(4, "TIT2", Text(3, Str(std::string_view("Song\0Remix", 10))));
		const Bytes album = Frame(4, "TPE2", Text(2, { 0x67, 0x08, 0x4E, 0xAE }));                // UTF-16BE, album artist
		const Bytes withLength = Frame(4, "TXXX", Bytes{ 0, 0, 0, 5, 3, 'a', 0, 'b', 0 }, 0x01);   // skipped cleanly
		frames.insert(frames.end(), withLength.begin(), withLength.end());
		frames.insert(frames.end(), album.begin(), album.end());
		const Bytes tag = Tag(4, frames);
		tags::ParseId3v2(tag.data(), tag.size(), t);
		tags::Finish(t);
		Check(t.mTitle == "Song; Remix", "2.4 UTF-8 list joined");
		Check(t.mArtist == kMoonUtf8, "album artist stands in for a missing artist");
	}
	{
		tags::Tags t;
		const Bytes tag = Tag(2, Frame(2, "TT2", Text(0, { 0xD4, 0xC2, 0xC1, 0xC1 }))); // GBK in a Latin-1 frame
		tags::ParseId3v2(tag.data(), tag.size(), t);
		Check(t.mTitle == kMoonUtf8, "2.2 frame, GBK recognized");
	}
	{
		// Whole-tag unsynchronisation (2.3): FF 00 pairs lose the 00.
		tags::Tags t;
		Bytes frames = Frame(3, "TPE1", Text(1, { 0xFF, 0xFE, 'A', 0, 'B', 0 }));
		Bytes unsynced;
		for (uint8_t b : frames) {
			unsynced.push_back(b);
			if (b == 0xFF) unsynced.push_back(0);
		}
		const Bytes tag = Tag(3, unsynced, 0x80);
		tags::ParseId3v2(tag.data(), tag.size(), t);
		Check(t.mArtist == "AB", "unsynchronised tag");
	}
	{
		tags::Tags t;
		Bytes v1(128, 0);
		std::memcpy(v1.data(), "TAG", 3);
		std::memcpy(v1.data() + 3, "Title One", 9);
		std::memcpy(v1.data() + 33, "\xD4\xC2\xC1\xC1", 4);
		t.mTitle = "from ID3v2";
		tags::ParseId3v1(v1.data(), v1.size(), t);
		Check(t.mTitle == "from ID3v2", "ID3v1 doesn't override");
		Check(t.mArtist == kMoonUtf8, "ID3v1 GBK artist");
	}
	{
		tags::Tags t;
		tags::AddVorbisComment("title=Night Drive", t);
		tags::AddVorbisComment("ALBUMARTIST=Various", t);
		tags::AddVorbisComment("ARTIST=A", t);
		tags::AddVorbisComment("Artist=B", t);
		tags::AddVorbisComment("GARBAGE", t);
		tags::Finish(t);
		Check(t.mTitle == "Night Drive", "Vorbis title");
		Check(t.mArtist == "A; B", "Vorbis artists joined, album artist ignored");
	}
	{
		// APIC (2.3): UTF-16 description (even-aligned double NUL), back cover first, then the front cover.
		const Bytes back = Frame(3, "APIC", Bytes{ 0, 'i', 'm', 'a', 'g', 'e', '/', 'p', 'n', 'g', 0, 4, 'b', 0, 0xB1, 0xB2 });
		const Bytes front = Frame(3, "APIC",
			Bytes{ 1, 'i', 'm', 'a', 'g', 'e', '/', 'j', 'p', 'e', 'g', 0, 3, 0xFF, 0xFE, 'x', 0, 0, 0, 0xFF, 0xD8, 0xFF, 0xE0 });
		Bytes frames = back;
		frames.insert(frames.end(), front.begin(), front.end());
		const Bytes tag = Tag(3, frames);

		tags::Tags skipped;
		tags::ParseId3v2(tag.data(), tag.size(), skipped);
		Check(skipped.mPicture.empty(), "pictures ignored unless asked for");

		tags::Tags t;
		t.mWantPicture = true;
		tags::ParseId3v2(tag.data(), tag.size(), t);
		Check(t.mPictureType == tags::kFrontCover && t.mPicture == (Bytes{ 0xFF, 0xD8, 0xFF, 0xE0 }), "APIC front cover wins");
	}
	{
		// PIC (2.2): 3-character format instead of a MIME type.
		tags::Tags t;
		t.mWantPicture = true;
		const Bytes tag = Tag(2, Frame(2, "PIC", Bytes{ 0, 'P', 'N', 'G', 0, 'd', 0, 0x89, 'P', 'N', 'G' }));
		tags::ParseId3v2(tag.data(), tag.size(), t);
		Check(t.mPictureType == 0 && t.mPicture == (Bytes{ 0x89, 'P', 'N', 'G' }), "PIC read");
	}
	{
		Check(tags::Base64Decode("TWFu") == Str("Man") && tags::Base64Decode("TWE=") == Str("Ma") && tags::Base64Decode("T Q==\n") == Str("M"),
			"base64");

		// METADATA_BLOCK_PICTURE: type 3, MIME "image/png", no description, 4 × u32, 3 bytes of data.
		Bytes block{ 0, 0, 0, 3, 0, 0, 0, 9 };
		Append(block, "image/png");
		block.insert(block.end(), { 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 32, 0, 0, 0, 0, 0, 0, 0, 3, 7, 8, 9 });
		static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string encoded = "metadata_block_picture=";
		for (size_t i = 0; i < block.size(); i += 3) {
			const uint32_t v = block[i] << 16 | (i + 1 < block.size() ? block[i + 1] << 8 : 0) | (i + 2 < block.size() ? block[i + 2] : 0);
			encoded += kAlphabet[v >> 18 & 63];
			encoded += kAlphabet[v >> 12 & 63];
			encoded += i + 1 < block.size() ? kAlphabet[v >> 6 & 63] : '=';
			encoded += i + 2 < block.size() ? kAlphabet[v & 63] : '=';
		}
		tags::Tags t;
		t.mWantPicture = true;
		tags::AddVorbisComment(encoded, t);
		tags::AddVorbisComment("TITLE=x", t);
		Check(t.mPictureType == 3 && t.mPicture == (Bytes{ 7, 8, 9 }), "Vorbis METADATA_BLOCK_PICTURE");
		Check(t.mTitle == "x", "comments after a picture still read");

		tags::Tags truncated;
		truncated.mWantPicture = true;
		tags::ParseFlacPicture(block.data(), block.size() - 1, truncated);
		Check(truncated.mPicture.empty(), "truncated PICTURE block rejected");
	}
	Check(tags::LegacyToUtf8("plain ascii  ") == "plain ascii", "ASCII trimmed");
	Check(tags::LegacyToUtf8(kMoonUtf8) == kMoonUtf8, "UTF-8 passes");

	if (gFailures) {
		std::printf("%d failure(s)\n", gFailures);
		return 1;
	}
	std::printf("PASS\n");
	return 0;
}

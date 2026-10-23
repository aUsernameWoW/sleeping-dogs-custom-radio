// Internet radio's text and byte handling (core/icy.cc): ICY metadata split from the audio across arbitrary
// chunk boundaries, StreamTitle parsing (quotes, legacy encodings), playlists (M3U, PLS) and format detection.
// Compiles icy.cc and tags.cc directly; argv[1] is unused.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "../core/icy.cc"
#include "../core/tags.cc"

#include <cstdio>

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

	// metaint 8: 8 audio bytes, a length byte, the block, and so on.
	std::vector<uint8_t> MakeStream(std::vector<std::string>& blocks)
	{
		std::vector<uint8_t> stream;
		uint8_t next = 0;
		for (int i = 0; i < 6; ++i) {
			for (int k = 0; k < 8; ++k) stream.push_back(next++);
			std::string block = i % 2 ? "" : "StreamTitle='Song " + std::to_string(i) + "';";
			const size_t padded = (block.size() + 15) / 16 * 16;
			stream.push_back(static_cast<uint8_t>(padded / 16));
			stream.insert(stream.end(), block.begin(), block.end());
			stream.insert(stream.end(), padded - block.size(), '\0');
			if (!block.empty()) blocks.push_back(block);
		}
		return stream;
	}

	void TestDemuxer()
	{
		std::vector<std::string> expected;
		const std::vector<uint8_t> stream = MakeStream(expected);
		for (size_t chunk : { size_t{ 1 }, size_t{ 3 }, size_t{ 7 }, size_t{ 9 }, size_t{ 64 }, stream.size() }) {
			icy::Demuxer demuxer(8);
			std::vector<uint8_t> audio;
			std::vector<std::string> metadata;
			for (size_t at = 0; at < stream.size(); at += chunk) {
				demuxer.Feed(stream.data() + at, (std::min)(chunk, stream.size() - at), audio, metadata);
			}
			bool inOrder = audio.size() == 48;
			for (size_t i = 0; inOrder && i < audio.size(); ++i) inOrder = audio[i] == i;
			Check(inOrder, "audio bytes come out whole and in order, whatever the chunking");
			Check(metadata == expected, "metadata blocks come out without padding, empty ones skipped");
		}

		icy::Demuxer none(0);
		std::vector<uint8_t> audio;
		std::vector<std::string> metadata;
		const uint8_t bytes[] = { 1, 2, 3 };
		none.Feed(bytes, 3, audio, metadata);
		Check(audio.size() == 3 && metadata.empty(), "metaint 0: everything is audio");
	}

	void TestTitles()
	{
		Check(icy::StreamTitle("StreamTitle='Artist - Title';StreamUrl='';") == "Artist - Title", "plain title");
		Check(icy::StreamTitle("StreamTitle='Guns N' Roses - Paradise City';") == "Guns N' Roses - Paradise City", "quote inside");
		Check(icy::StreamTitle("StreamTitle='No end") == "No end", "unterminated");
		Check(icy::StreamTitle("StreamUrl='x';").empty(), "no title");
		Check(icy::StreamTitle("StreamTitle='Caf\xE9';") == "Caf\xC3\xA9", "Latin-1 to UTF-8");
		Check(icy::StreamTitle("StreamTitle='\xC4\xE3\xBA\xC3';") == "\xE4\xBD\xA0\xE5\xA5\xBD", "GBK to UTF-8");
		Check(icy::StreamTitle("StreamTitle='\xE4\xBD\xA0\xE5\xA5\xBD';") == "\xE4\xBD\xA0\xE5\xA5\xBD", "UTF-8 kept");
	}

	void TestPlaylists()
	{
		Check(icy::IsPlaylist("audio/x-mpegurl", "http://a/b"), "m3u content type");
		Check(icy::IsPlaylist("audio/x-scpls", "http://a/b"), "pls content type");
		Check(icy::IsPlaylist("text/plain", "http://a/listen.pls?x=1"), "pls by extension");
		Check(!icy::IsPlaylist("audio/mpeg", "http://a/stream.m3u"), "an audio type wins over the extension");
		Check(!icy::IsPlaylist("audio/mpeg", "http://a/stream"), "audio");

		Check(icy::PlaylistUrl("#EXTM3U\r\n#EXTINF:-1,Station\r\nhttp://host:8000/live.mp3\r\n") == "http://host:8000/live.mp3", "M3U");
		Check(icy::PlaylistUrl("\xEF\xBB\xBFhttps://host/a\n") == "https://host/a", "M3U with BOM");
		Check(icy::PlaylistUrl("[playlist]\nNumberOfEntries=2\nFile1=http://one/\nTitle1=One\nFile2=http://two/\n") == "http://one/", "PLS");
		Check(icy::PlaylistUrl("[playlist]\nVersion=2\n").empty(), "empty PLS");
	}

	void TestFormats()
	{
		const uint8_t ogg[] = { 'O', 'g', 'g', 'S' };
		const uint8_t id3[] = { 'I', 'D', '3', 4 };
		const uint8_t mp3[] = { 0xFF, 0xFB, 0x90, 0x00 };
		const uint8_t adts[] = { 0xFF, 0xF1, 0x50, 0x80 };
		Check(icy::DetectFormat("audio/mpeg", nullptr, 0) == icy::Format::Mp3, "audio/mpeg");
		Check(icy::DetectFormat("application/ogg", nullptr, 0) == icy::Format::Ogg, "application/ogg");
		Check(icy::DetectFormat("audio/aacp", nullptr, 0) == icy::Format::Aac, "audio/aacp");
		Check(icy::DetectFormat("", ogg, 4) == icy::Format::Ogg, "OggS");
		Check(icy::DetectFormat("application/octet-stream", id3, 4) == icy::Format::Mp3, "ID3");
		Check(icy::DetectFormat("", mp3, 4) == icy::Format::Mp3, "MPEG frame sync");
		Check(icy::DetectFormat("", adts, 4) == icy::Format::Aac, "ADTS");
		const uint8_t opus[] = "OggS\0\2\0\0\0\0\0\0\0\0\1\2\3\4\0\0\0\0\5\6\7\x8\1\x13OpusHead";
		const uint8_t flac[] = "OggS\0\2\0\0\0\0\0\0\0\0\1\2\3\4\0\0\0\0\5\6\7\x8\1\x33\x7F" "FLAC";
		const uint8_t vorbis[] = "OggS\0\2\0\0\0\0\0\0\0\0\1\2\3\4\0\0\0\0\5\6\7\x8\1\x1E\1vorbis";
		Check(icy::DetectFormat("application/ogg", opus, sizeof(opus) - 1) == icy::Format::Opus, "Opus in Ogg");
		Check(icy::DetectFormat("application/ogg", flac, sizeof(flac) - 1) == icy::Format::OggFlac, "FLAC in Ogg");
		Check(icy::DetectFormat("application/ogg", vorbis, sizeof(vorbis) - 1) == icy::Format::Ogg, "Vorbis in Ogg");
		Check(icy::DetectFormat("audio/ogg; codecs=opus", nullptr, 0) == icy::Format::Opus, "opus content type");
	}
}

int main()
{
	TestDemuxer();
	TestTitles();
	TestPlaylists();
	TestFormats();
	if (gFailures) {
		return 1;
	}
	std::printf("PASS\n");
	return 0;
}

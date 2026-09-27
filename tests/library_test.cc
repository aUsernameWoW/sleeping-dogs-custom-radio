// Loads SDRadio.asi with music already in its folder, as on a player's first start: the library scan must find
// files in subfolders and under Chinese names, read WAV tags written in UTF-8 and in GBK, skip a file no
// backend can open (a broken .m4a goes to the Media Foundation fallback and fails there, or finds no Media
// Foundation at all) and ignore files that aren't music. Written for the CI run under Wine, where file names,
// code page 936 and Media Foundation are Wine's own implementations; runs on Windows too.
// argv[1] = path to the .asi (a sandbox copy).

#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
	void Put32(std::string& s, uint32_t v)
	{
		for (int i = 0; i < 4; ++i) s += static_cast<char>(v >> (8 * i));
	}

	void Put16(std::string& s, uint16_t v)
	{
		s += static_cast<char>(v);
		s += static_cast<char>(v >> 8);
	}

	std::string Chunk(const char* id, const std::string& body)
	{
		std::string c(id, 4);
		Put32(c, static_cast<uint32_t>(body.size()));
		c += body;
		if (body.size() % 2) c += '\0';
		return c;
	}

	// 16-bit PCM silence with an optional LIST/INFO chunk (IART, INAM: NUL-terminated as writers store them)
	// ahead of the data.
	std::string Wav(uint32_t rate, uint16_t channels, uint32_t frames, const std::string& artist, const std::string& title)
	{
		std::string fmt;
		Put16(fmt, 1);
		Put16(fmt, channels);
		Put32(fmt, rate);
		Put32(fmt, rate * channels * 2);
		Put16(fmt, static_cast<uint16_t>(channels * 2));
		Put16(fmt, 16);

		std::string body = "WAVE" + Chunk("fmt ", fmt);
		if (!artist.empty() || !title.empty()) {
			body += Chunk("LIST", "INFO" + Chunk("IART", artist + '\0') + Chunk("INAM", title + '\0'));
		}
		body += Chunk("data", std::string(static_cast<size_t>(frames) * channels * 2, '\0'));
		return Chunk("RIFF", body);
	}

	void Write(const std::filesystem::path& path, const std::string& bytes)
	{
		std::ofstream(path, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	}
}

int main(int argc, char** argv)
{
	if (argc < 2) {
		std::printf("usage: library_test <SDRadio.asi>\n");
		return 2;
	}

	const std::filesystem::path dir = std::filesystem::path(argv[1]).parent_path();
	const std::filesystem::path music = dir / L"SDRadio";
	std::filesystem::create_directories(music / L"华语");

	// Artist in GBK (测试歌手, as old Chinese taggers wrote it), title in UTF-8.
	Write(music / L"a.wav", Wav(44100, 2, 22050, "\xB2\xE2\xCA\xD4\xB8\xE8\xCA\xD6", "测试曲目"));
	// No tags: the title comes from the (Chinese) file name. Mono, as some rips are.
	Write(music / L"华语" / L"无标签.wav", Wav(48000, 1, 48000, "", ""));
	Write(music / L"broken.m4a", "not an MP4 file at all");
	Write(music / L"notes.txt", "not music");

	HMODULE module = LoadLibraryA(argv[1]);
	if (!module) {
		std::printf("FAIL: LoadLibrary error %lu\n", GetLastError());
		return 1;
	}

	// Media Foundation's first start can take a while under Wine.
	std::string contents;
	for (int i = 0; i < 200 && contents.find("tracks ready") == std::string::npos; ++i) {
		Sleep(100);
		std::ifstream log(dir / L"SDRadio.log");
		std::stringstream text;
		text << log.rdbuf();
		contents = text.str();
	}
	std::printf("%s", contents.c_str());

	const char* expected[] = { "library: 3 music files", "  测试歌手 - 测试曲目  (dr_wav, 44100 Hz, 0.5 s)",
		"broken.m4a (no decoder for it, or no length)", "   - 无标签  (dr_wav, 48000 Hz, 1.0 s)", "library: 2 tracks ready" };
	for (const char* line : expected) {
		if (contents.find(line) == std::string::npos) {
			std::printf("FAIL: log lacks \"%s\"\n", line);
			return 1;
		}
	}
	if (contents.find("notes.txt") != std::string::npos) {
		std::printf("FAIL: notes.txt was taken for music\n");
		return 1;
	}

	std::printf("PASS\n");
	return 0;
}

// Loads SDRadio.asi into a process that isn't the game: it must not crash, must write its default ini,
// create the music folder, report the missing game functions and hook nothing. argv[1] = path to the .asi
// (build.ps1 passes a sandbox copy).

#include <Windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

int main(int argc, char** argv)
{
	if (argc < 2) {
		std::printf("usage: load_test <SDRadio.asi>\n");
		return 2;
	}

	HMODULE module = LoadLibraryA(argv[1]);
	if (!module) {
		std::printf("FAIL: LoadLibrary error %lu\n", GetLastError());
		return 1;
	}

	std::string dir = argv[1];
	dir = dir.substr(0, dir.find_last_of("\\/") + 1);

	if (!std::ifstream(dir + "SDRadio.ini")) {
		std::printf("FAIL: no default SDRadio.ini written\n");
		return 1;
	}
	if (!(GetFileAttributesA((dir + "SDRadio").c_str()) & FILE_ATTRIBUTE_DIRECTORY)) {
		std::printf("FAIL: no SDRadio music folder created\n");
		return 1;
	}

	// The scan thread logs once it has looked at the (empty) folder, the logo thread once the pack is written.
	std::string contents;
	for (int i = 0; i < 50 && (contents.find("tracks ready") == std::string::npos || contents.find("logo: texture pack") == std::string::npos); ++i) {
		Sleep(100);
		std::ifstream log(dir + "SDRadio.log");
		std::stringstream text;
		text << log.rdbuf();
		contents = text.str();
	}
	std::printf("%s", contents.c_str());

	// The test's folder stands in for the game folder (the executable's), so the pack is one level down.
	const char* expected[] = { "SDRadio loaded", "SimpleXML::XMLCache::ExtractFromCache: 0 matches", "game functions missing, no station",
		"library: 0 music files", "library: 0 tracks ready", "using the built-in logo", "logo: texture pack ..\\..\\load_test\\SDRadio-logo" };
	for (const char* line : expected) {
		if (contents.find(line) == std::string::npos) {
			std::printf("FAIL: log lacks \"%s\"\n", line);
			return 1;
		}
	}
	for (const char* file : { "SDRadio-logo.perm.bin", "SDRadio-logo.temp.bin" }) {
		if (!std::ifstream(dir + file)) {
			std::printf("FAIL: no %s written\n", file);
			return 1;
		}
	}

	std::printf("PASS\n");
	return 0;
}

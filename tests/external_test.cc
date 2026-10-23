// Loads SDRadio.asi with [External] Enabled = 1, capturing a child process (ping): the diagnostics thread must
// start, report the media sessions (or why there are none, as under Wine), start or fail the capture with a
// reason (CI runners may have no audio device), and log a level line, without crashing, and the test must then
// exit promptly (build.ps1 and CI wait for it). argv[1] = path to the .asi (build.ps1 passes a sandbox copy).
// A child rather than itself, as the game captures another app.

#include <Windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

int main(int argc, char** argv)
{
	if (argc < 2) {
		std::printf("usage: external_test <SDRadio.asi>\n");
		return 2;
	}

	std::string dir = argv[1];
	dir = dir.substr(0, dir.find_last_of("\\/") + 1);
	{
		std::ofstream ini(dir + "SDRadio.ini", std::ios::binary);
		ini << "[External]\nEnabled = 1\nCaptureProcess = ping.exe\n[Debug]\nLogging = 1\n";
	}
	// A stale log would pass the checks below without this run writing anything.
	if (!DeleteFileA((dir + "SDRadio.log").c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
		std::printf("FAIL: can't delete the old SDRadio.log (error %lu; a previous run still holds it?)\n", GetLastError());
		return 1;
	}

	char command[] = "ping.exe -n 30 127.0.0.1";
	STARTUPINFOA startup = { sizeof(startup) };
	startup.dwFlags = STARTF_USESTDHANDLES; // no handles: keeps ping's output out of the test's
	PROCESS_INFORMATION child = {};
	if (!CreateProcessA(nullptr, command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) {
		std::printf("FAIL: can't start ping (error %lu)\n", GetLastError());
		return 1;
	}

	if (!LoadLibraryA(argv[1])) {
		std::printf("FAIL: LoadLibrary error %lu\n", GetLastError());
		return 1;
	}

	const char* expected[] = { "external: diagnostics on", "media: ", "capture: ", "level: " };
	std::string contents;
	for (int i = 0; i < 150; ++i) {
		Sleep(100);
		std::ifstream log(dir + "SDRadio.log");
		std::stringstream text;
		text << log.rdbuf();
		contents = text.str();
		bool all = true;
		for (const char* line : expected) {
			all = all && contents.find(line) != std::string::npos;
		}
		if (all) {
			break;
		}
	}
	std::printf("%s", contents.c_str());
	TerminateProcess(child.hProcess, 0);
	CloseHandle(child.hThread);
	CloseHandle(child.hProcess);

	for (const char* line : expected) {
		if (contents.find(line) == std::string::npos) {
			std::printf("FAIL: log lacks \"%s\"\n", line);
			return 1;
		}
	}
	if (contents.find("crash: ") != std::string::npos) {
		std::printf("FAIL: the log reports a crash\n");
		return 1;
	}

	std::printf("PASS\n");
	return 0;
}

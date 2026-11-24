// SDRadio.ini (core/config.cc): the layout before 2026-11-24 moves to the current one with every value kept, the
// rendered file reads back as what went in, and Load rewrites an old or incomplete file (keeping a .bak) but leaves
// a current one alone. Compiles config.cc directly; argv[1] is unused.

#include "../core/config.cc"

#include <cstdio>
#include <string>

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

	std::string Get(const config::Values& values, const char* key)
	{
		auto it = values.find(key);
		return it == values.end() ? "<missing>" : it->second;
	}

	std::string CarStation(const char* oldStationSection)
	{
		config::Values values = config::Parse(std::string("[Station]\n") + oldStationSection);
		config::Migrate(values);
		return Get(values, "Car.Station");
	}

	std::string ReadAll(const std::wstring& path)
	{
		std::string text;
		FILE* file = nullptr;
		if (_wfopen_s(&file, path.c_str(), L"rb") == 0 && file) {
			char buffer[4096];
			size_t n;
			while ((n = fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, n);
			fclose(file);
		}
		return text;
	}

	void WriteAll(const std::wstring& path, const std::string& text)
	{
		FILE* file = nullptr;
		if (_wfopen_s(&file, path.c_str(), L"wb") == 0 && file) {
			fwrite(text.data(), 1, text.size(), file);
			fclose(file);
		}
	}

	// An ini written by a build from October 2026 (the user's own), CRLF, a Chinese name, the live station on.
	constexpr char kOldIni[] =
		"; SDRadio 配置 / configuration (UTF-8)\r\n\r\n"
		"[Station]\r\n; 台名\r\nName = 私家台\r\n\r\nMusicFolder = D:\\Music\\电台\r\nCustomLogo = 1\r\n"
		"TextureName = Logo_Softly\r\nTexturePack = Radio_Softly_TexturePack\r\n\r\n"
		"[Live]\r\nEnabled = 1\r\nName = SDRADIO LIVE\r\nSource = app\r\nUrl =\r\nApp = auto\r\nPauseApp = 0\r\n"
		"Librespot =\r\nSpotifyName = SDRadio\r\n\r\n"
		"[Debug]\r\nLogging = 0\r\n";
}

int main()
{
	// The default file reads back as its own defaults and holds every key once.
	{
		const config::Values defaults = config::Parse(config::Render({}));
		Check(defaults.size() == config::TemplateKeys().size(), "default: every key once");
		Check(Get(defaults, "Car.Station") == "last", "default: last station");
		Check(Get(defaults, "Live.Source") == "off", "default: no live station");
		Check(Get(defaults, "Music.Folder").empty(), "default: empty folder");
		config::Values copy = defaults;
		Check(!config::Migrate(copy) && copy == defaults, "default: nothing to migrate");
	}

	// The old file: every value carried, the borrowed logo's defaults dropped, nothing of the old layout left.
	{
		config::Values values = config::Parse(kOldIni);
		Check(config::Migrate(values), "old: migrated");
		Check(Get(values, "Music.Name") == "私家台", "old: name");
		Check(Get(values, "Music.Folder") == "D:\\Music\\电台", "old: folder");
		Check(Get(values, "Live.Source") == "app", "old: live on, source kept");
		Check(Get(values, "App.Program") == "auto", "old: app");
		Check(Get(values, "App.Pause") == "0", "old: pause");
		Check(Get(values, "Spotify.Device") == "SDRadio", "old: spotify name");
		Check(Get(values, "Debug.Logging") == "0", "old: logging");
		Check(!values.count("Music.CustomLogo") && !values.count("Music.TextureName"), "old: default logo keys dropped");
		Check(!values.count("Car.Station"), "old: no Default/KeepLast, no car station (the default applies)");
		for (const auto& [key, value] : values) {
			Check(!key.starts_with("Station.") && key != "Live.Enabled" && key != "Live.Url" && key != "Live.App", "old: no old key left");
		}

		const std::string text = config::Render(values);
		config::Values back = config::Parse(text);
		Check(!config::Migrate(back), "rendered: current layout");
		Check(Get(back, "Music.Name") == "私家台" && Get(back, "App.Pause") == "0" && Get(back, "Live.Source") == "app", "rendered: values kept");
		Check(Get(back, "Car.Station") == "last", "rendered: default car station");
		Check(text.find("Folder = D:\\Music\\电台\n") != std::string::npos, "rendered: folder line");
	}

	// The live station was off unless Enabled = 1; Source defaulted to test.
	{
		config::Values values = config::Parse("[Live]\nEnabled = 0\nSource = radio\nUrl = http://x/\n");
		config::Migrate(values);
		Check(Get(values, "Live.Source") == "off" && Get(values, "Radio.Url") == "http://x/", "live off, url kept");
		values = config::Parse("[Live]\nEnabled = 1\n");
		config::Migrate(values);
		Check(Get(values, "Live.Source") == "test", "live on without a source: test");
		values = config::Parse("[Station]\nName = A\n[Live]\nSource = radio\n");
		config::Migrate(values);
		Check(Get(values, "Live.Source") == "off", "live without Enabled: off");
	}

	// Default and KeepLast become one key.
	Check(CarStation("Default = music\n") == "last", "music, KeepLast missing: last");
	Check(CarStation("Default = music\nKeepLast = 0\n") == "sdradio", "music, KeepLast 0: sdradio");
	Check(CarStation("Default = Live\nKeepLast = 0\n") == "live", "live, KeepLast 0: live");
	Check(CarStation("Default = live\nKeepLast = 1\n") == "last", "live, KeepLast 1: last");
	Check(CarStation("Default = off\n") == "game", "off: game");
	Check(CarStation("Default = off\nKeepLast = 1\n") == "game", "off, KeepLast 1: game");
	Check(CarStation("KeepLast = 0\n") == "sdradio", "KeepLast 0 alone: sdradio");

	// A borrowed logo that isn't the default goes to [Music], after its keys, and reads back.
	{
		config::Values values = config::Parse("[Station]\nCustomLogo = 0\nTextureName = Logo_Kerrang\nTexturePack = Radio_Kerrang_TexturePack\n");
		config::Migrate(values);
		const std::string text = config::Render(values);
		const config::Values back = config::Parse(text);
		Check(Get(back, "Music.CustomLogo") == "0" && Get(back, "Music.TextureName") == "Logo_Kerrang", "logo carried");
		Check(text.find("Name = SDRADIO\nCustomLogo = 0\n") != std::string::npos, "logo keys right after [Music]'s");
	}

	// A new key wins over the old one it replaces.
	{
		config::Values values = config::Parse("[Music]\nName = New\n[Station]\nName = Old\n");
		config::Migrate(values);
		Check(Get(values, "Music.Name") == "New", "new key wins");
	}

	// Load: an old file is rewritten with a backup, a current one is left alone, a missing one is written.
	{
		wchar_t temp[MAX_PATH];
		GetTempPathW(MAX_PATH, temp);
		const std::wstring dir = std::wstring(temp) + L"sdradio_config_test";
		CreateDirectoryW(dir.c_str(), nullptr);
		const std::wstring ini = dir + L"\\SDRadio.ini", bak = ini + L".bak";
		DeleteFileW(ini.c_str());
		DeleteFileW(bak.c_str());

		WriteAll(ini, kOldIni);
		gConfig = Config();
		config::Load(dir);
		Check(ReadAll(bak) == kOldIni, "load old: backup is the old file");
		Check(ReadAll(ini).find("[Car]") != std::string::npos, "load old: rewritten");
		Check(gConfig.mStationName == "私家台" && gConfig.mLive && gConfig.mLiveSource == "app" && !gConfig.mLivePauseApp && !gConfig.mLogging,
			"load old: settings applied");
		Check(gConfig.mCarStation == "last", "load old: car station");
		Check(gConfig.mIniNote.find("new layout") != std::string::npos, "load old: noted");

		const std::string current = ReadAll(ini);
		DeleteFileW(bak.c_str());
		gConfig = Config();
		config::Load(dir);
		Check(ReadAll(ini) == current && GetFileAttributesW(bak.c_str()) == INVALID_FILE_ATTRIBUTES && gConfig.mIniNote.empty(),
			"load current: untouched");
		Check(gConfig.mStationName == "私家台", "load current: settings applied");

		// A current file missing a key gets it, values kept.
		WriteAll(ini, "[Music]\nName = X\n[Car]\nStation = game\n");
		gConfig = Config();
		config::Load(dir);
		const config::Values filled = config::Parse(ReadAll(ini));
		Check(Get(filled, "Music.Name") == "X" && Get(filled, "Car.Station") == "game" && Get(filled, "Live.Source") == "off", "load incomplete: filled");
		Check(gConfig.mCarStation == "game" && !gConfig.mLive, "load incomplete: applied");

		DeleteFileW(ini.c_str());
		DeleteFileW(bak.c_str());
		gConfig = Config();
		config::Load(dir);
		Check(ReadAll(ini) == config::Render({}), "load missing: default written");
		DeleteFileW(ini.c_str());
		DeleteFileW(bak.c_str());
		RemoveDirectoryW(dir.c_str());
	}

	std::printf(gFailures ? "%d failure(s)\n" : "PASS\n", gFailures);
	return gFailures ? 1 : 0;
}

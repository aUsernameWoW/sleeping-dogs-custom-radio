#include "config.hh"

#include <Windows.h>

#include <cstdio>
#include <optional>
#include <string_view>

Config gConfig;

namespace config
{
	namespace
	{
		// The layout since 2026-11-24 (before: [Station] and [Live] held everything, see Migrate). Details are in
		// ADVANCED.md; keep this short. Render puts the player's values in place of these defaults.
		constexpr char kDefaultIni[] =
			"; SDRadio 设置 / settings (UTF-8). 详细说明 / details: ADVANCED.md\n"
			"\n"
			"[Music]\n"
			"; 音乐文件夹，留空 = .asi 旁边的 SDRadio 文件夹。直接放在里面的歌是一个台（台名 Name），每个子文件夹是一个台，\n"
			"; 台名就是文件夹名（最多 33 个台，每台 255 首）。文件夹里放一张透明背景、2:1 的 logo.png 就是那个台的图标。\n"
			"; Music folder, empty = SDRadio next to the .asi. Its own files are one station (Name), each subfolder another,\n"
			"; named after the folder (up to 33 stations, 255 tracks each). A 2:1 logo.png with a transparent background in a\n"
			"; folder is that station's logo.\n"
			"Folder =\n"
			"Name = SDRADIO\n"
			"\n"
			"[Car]\n"
			"; 上车时放的台 / the station when getting into a car:\n"
			"; last = 上次听的台（这局第一辆车放 SDRadio）/ the one you last listened to (SDRadio in the first car)\n"
			"; sdradio = 总是 SDRadio / always SDRadio; live = 总是直播台 / always the live station; game = 游戏原样 / as the game does\n"
			"; 警车的 HKPD、任务和脚本指定的台不受影响。/ A police car's HKPD and mission or script stations stay.\n"
			"Station = last\n"
			"\n"
			"[Live]\n"
			"; 直播电台 / live station: off; radio = 网络电台 / internet radio [Radio];\n"
			"; app = 另一个程序的声音 / another program's sound [App] (Windows 11); spotify = Spotify Premium [Spotify];\n"
			"; test = 测试信号 / a test signal\n"
			"Source = off\n"
			"Name = SDRADIO LIVE\n"
			"\n"
			"[Radio]\n"
			"; MP3 / Ogg Vorbis 流，或指向它的 .m3u / .pls / stream, or an .m3u / .pls. https://www.radio-browser.info\n"
			"Url =\n"
			"\n"
			"[App]\n"
			"; auto = 正在放歌的程序 / whichever plays music; 或进程名 / or a process name: AppleMusic.exe, Spotify.exe, cloudmusic.exe\n"
			"Program = auto\n"
			"; direct = 听程序自己的声音 / its own sound; radio = 通过游戏电台（要把程序的输出设备改到不用的设备）/ through the\n"
			"; radio (send the program to an output device you don't use); auto = 按程序的输出设备选 / by the program's device\n"
			"Sound = auto\n"
			"; 1 = 不在这个台时暂停程序 / pause the program while you're not on this station\n"
			"Pause = 1\n"
			"\n"
			"[Spotify]\n"
			"; Spotify 里的设备名 / the device name in Spotify; librespot.exe，留空 = .asi 旁边 / empty = next to the .asi\n"
			"Device = SDRadio\n"
			"Librespot =\n"
			"\n"
			"[External]\n"
			"; 实验功能（gpl/external-audio 分支），只写日志：系统媒体会话，以及抓到的程序（或播放设备）的声音电平。1 = 开启。\n"
			"; Experimental (gpl/external-audio branch), log only: the media sessions and the level captured from a program\n"
			"; (or a playback device). 1 = on.\n"
			"Enabled = 0\n"
			"; 媒体会话：App ID 的一部分（如 spotify），留空 = 当前的 / media session: part of the app ID, empty = the current one\n"
			"MediaSession =\n"
			"; 程序（如 Spotify.exe），留空 = 抓下面的播放设备 / program (e.g. Spotify.exe), empty = the playback device below\n"
			"CaptureProcess =\n"
			"; 播放设备名称的一部分，留空 = 默认设备 / part of a playback device's name, empty = the default device\n"
			"CaptureDevice =\n"
			"\n"
			"[Debug]\n"
			"; 在 .asi 旁边写 SDRadio.log / write SDRadio.log next to the .asi\n"
			"Logging = 1\n";

		std::string_view Trim(std::string_view s)
		{
			while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
			while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
			return s;
		}

		std::string Lower(std::string s)
		{
			for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
			return s;
		}

		// One line of an ini: a section header, a key, or neither (comment, blank).
		struct Line
		{
			std::string mSection; // set for a header
			std::string mKey, mValue;
		};

		Line Classify(std::string_view raw)
		{
			const std::string_view line = Trim(raw);
			Line out;
			if (line.empty() || line.front() == ';' || line.front() == '#') return out;
			if (line.front() == '[' && line.back() == ']') {
				out.mSection = std::string(Trim(line.substr(1, line.size() - 2)));
				return out;
			}
			if (const size_t eq = line.find('='); eq != std::string_view::npos) {
				out.mKey = std::string(Trim(line.substr(0, eq)));
				out.mValue = std::string(Trim(line.substr(eq + 1)));
			}
			return out;
		}

		std::vector<std::string_view> Lines(std::string_view text)
		{
			std::vector<std::string_view> lines;
			while (!text.empty()) {
				const size_t eol = text.find('\n');
				lines.push_back(text.substr(0, eol));
				text = eol == std::string_view::npos ? std::string_view() : text.substr(eol + 1);
			}
			return lines;
		}

		std::wstring Widen(const std::string& utf8)
		{
			if (utf8.empty()) {
				return {};
			}
			const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
			std::wstring result(static_cast<size_t>(size), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), result.data(), size);
			return result;
		}

		bool WriteFile(const std::wstring& path, const std::string& text)
		{
			// Through a temporary file, so a failed write never leaves a half-written ini.
			const std::wstring temp = path + L".tmp";
			FILE* file = nullptr;
			if (_wfopen_s(&file, temp.c_str(), L"wb") != 0 || !file) return false;
			const bool written = fwrite(text.data(), 1, text.size(), file) == text.size();
			if (fclose(file) != 0 || !written) {
				DeleteFileW(temp.c_str());
				return false;
			}
			if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
				DeleteFileW(temp.c_str());
				return false;
			}
			return true;
		}
	}

	// GetPrivateProfileString reads BOM-less files in the ANSI code page (1252 here), which would mangle a Chinese
	// station name or folder, so the (tiny) file is parsed as UTF-8 here. Keys are "Section.Key", case-sensitive.
	Values Parse(const std::string& text)
	{
		Values values;
		std::string section;
		std::string_view rest = text;
		if (rest.starts_with("\xEF\xBB\xBF")) {
			rest.remove_prefix(3);
		}
		for (std::string_view raw : Lines(rest)) {
			Line line = Classify(raw);
			if (!line.mSection.empty()) section = std::move(line.mSection);
			else if (!line.mKey.empty()) values[section + "." + line.mKey] = std::move(line.mValue);
		}
		return values;
	}

	bool Migrate(Values& values)
	{
		bool old = false;
		auto take = [&](const char* key) -> std::optional<std::string> {
			auto it = values.find(key);
			if (it == values.end()) return std::nullopt;
			old = true;
			std::string value = std::move(it->second);
			values.erase(it);
			return value;
		};
		// A key the file also has in the new layout keeps the new one.
		auto move = [&](const char* from, const char* to) {
			if (auto value = take(from); value && !values.count(to)) values[to] = *value;
		};
		// The borrowed logo isn't in the new default file: carried only when set to something else than its default.
		auto moveUnlessDefault = [&](const char* from, const char* to, const char* defaultValue) {
			if (auto value = take(from); value && !value->empty() && *value != defaultValue && !values.count(to)) values[to] = *value;
		};

		move("Station.Name", "Music.Name");
		move("Station.MusicFolder", "Music.Folder");
		moveUnlessDefault("Station.CustomLogo", "Music.CustomLogo", "1");
		moveUnlessDefault("Station.TextureName", "Music.TextureName", "Logo_Softly");
		moveUnlessDefault("Station.TexturePack", "Music.TexturePack", "Radio_Softly_TexturePack");

		// Default = music | live | off and KeepLast = 1 | 0 (default 1) became one key. Default = off is taken as
		// "leave it to the game", as it was written before KeepLast existed.
		const auto defaultStation = take("Station.Default");
		const auto keepLast = take("Station.KeepLast");
		if ((defaultStation || keepLast) && !values.count("Car.Station")) {
			const std::string d = Lower(defaultStation.value_or("music"));
			values["Car.Station"] = d == "off" || d == "0" ? "game" : keepLast.value_or("1") != "0" ? "last" : d == "live" ? "live" : "sdradio";
		}

		const auto enabled = take("Live.Enabled");
		move("Live.Url", "Radio.Url");
		move("Live.App", "App.Program");
		move("Live.AppSound", "App.Sound");
		move("Live.PauseApp", "App.Pause");
		move("Live.Librespot", "Spotify.Librespot");
		move("Live.SpotifyName", "Spotify.Device");

		// Any other key of the old [Station] (none known) has nowhere to go.
		for (auto it = values.begin(); it != values.end();) {
			if (it->first.starts_with("Station.")) {
				it = values.erase(it);
				old = true;
			}
			else {
				++it;
			}
		}

		// The live station was off unless Enabled = 1, and its Source defaulted to test.
		if (old) {
			if (enabled.value_or("0") != "1") values["Live.Source"] = "off";
			else if (values["Live.Source"].empty()) values["Live.Source"] = "test";
		}
		return old;
	}

	std::vector<std::string> TemplateKeys()
	{
		std::vector<std::string> keys;
		std::string section;
		for (std::string_view raw : Lines(kDefaultIni)) {
			Line line = Classify(raw);
			if (!line.mSection.empty()) section = std::move(line.mSection);
			else if (!line.mKey.empty()) keys.push_back(section + "." + line.mKey);
		}
		return keys;
	}

	std::string Render(const Values& values)
	{
		const Values defaults = Parse(kDefaultIni);
		std::vector<std::string> out;
		std::string section;
		// Keys of `section` the template doesn't have, inserted after its last non-blank line.
		auto flushExtras = [&] {
			if (section.empty()) return;
			size_t at = out.size();
			while (at > 0 && out[at - 1].empty()) --at;
			std::vector<std::string> extras;
			const std::string prefix = section + ".";
			for (auto it = values.lower_bound(prefix); it != values.end() && it->first.starts_with(prefix); ++it) {
				if (!defaults.count(it->first)) {
					const std::string key = it->first.substr(prefix.size());
					extras.push_back(it->second.empty() ? key + " =" : key + " = " + it->second);
				}
			}
			out.insert(out.begin() + static_cast<ptrdiff_t>(at), extras.begin(), extras.end());
		};
		for (std::string_view raw : Lines(kDefaultIni)) {
			Line line = Classify(raw);
			if (!line.mSection.empty()) {
				flushExtras();
				section = std::move(line.mSection);
				out.emplace_back(raw);
			}
			else if (!line.mKey.empty()) {
				const auto it = values.find(section + "." + line.mKey);
				const std::string& value = it != values.end() ? it->second : line.mValue;
				out.push_back(value.empty() ? line.mKey + " =" : line.mKey + " = " + value);
			}
			else {
				out.emplace_back(raw);
			}
		}
		flushExtras();
		std::string text;
		for (const std::string& line : out) text += line + "\n";
		return text;
	}

	void Load(const std::wstring& dir)
	{
		const std::wstring path = dir + L"\\SDRadio.ini";

		std::string text;
		bool exists = false;
		FILE* file = nullptr;
		if (_wfopen_s(&file, path.c_str(), L"rb") == 0 && file) {
			exists = true;
			char buffer[4096];
			size_t n;
			while ((n = fread(buffer, 1, sizeof(buffer), file)) > 0) {
				text.append(buffer, n);
			}
			fclose(file);
		}

		Values values = Parse(text);
		const bool migrated = Migrate(values);
		std::string missing;
		for (const std::string& key : TemplateKeys()) {
			if (!values.count(key)) missing += (missing.empty() ? "" : ", ") + key;
		}
		if (!exists) {
			gConfig.mIniNote = WriteFile(path, Render(values)) ? "wrote the default SDRadio.ini" : "couldn't write the default SDRadio.ini";
		}
		else if (migrated || !missing.empty()) {
			// Rewritten in the current layout with every value kept; the player's own comments go, so the old file
			// stays next to it.
			const bool backedUp = CopyFileW(path.c_str(), (path + L".bak").c_str(), FALSE) != 0;
			const bool written = backedUp && WriteFile(path, Render(values));
			const std::string what = migrated ? "moved to the new layout" : "added " + missing;
			gConfig.mIniNote = written ? "SDRadio.ini " + what + " (the old file is SDRadio.ini.bak)"
				: "SDRadio.ini should be " + what + ", but couldn't be rewritten (read as it is)";
		}

		auto get = [&](const char* key, std::string& out) {
			if (auto it = values.find(key); it != values.end() && !it->second.empty()) out = it->second;
		};
		auto flag = [&](const char* key, bool& out) {
			if (auto it = values.find(key); it != values.end() && !it->second.empty()) out = it->second != "0";
		};
		std::string folder;
		get("Music.Folder", folder);
		get("Music.Name", gConfig.mStationName);
		flag("Music.CustomLogo", gConfig.mCustomLogo);
		get("Music.TextureName", gConfig.mTextureName);
		get("Music.TexturePack", gConfig.mTexturePack);

		std::string car;
		get("Car.Station", car);
		car = Lower(car);
		gConfig.mCarStation = car == "sdradio" || car == "live" || car == "game" ? car : "last";

		std::string source;
		get("Live.Source", source);
		source = Lower(source);
		gConfig.mLive = !source.empty() && source != "off" && source != "0";
		if (gConfig.mLive) gConfig.mLiveSource = source;
		get("Live.Name", gConfig.mLiveName);
		get("Radio.Url", gConfig.mLiveUrl);
		std::string app;
		get("App.Program", app);
		gConfig.mLiveApp = Widen(app);
		get("App.Sound", gConfig.mLiveAppSound);
		flag("App.Pause", gConfig.mLivePauseApp);
		std::string librespot;
		get("Spotify.Librespot", librespot);
		gConfig.mLibrespot = librespot.empty() ? dir + L"\\librespot.exe" : Widen(librespot);
		get("Spotify.Device", gConfig.mSpotifyName);
		gConfig.mSpotifyCache = dir + L"\\SDRadio-spotify";
		if (auto it = values.find("External.Enabled"); it != values.end()) {
			gConfig.mExternal = it->second == "1";
		}
		get("External.MediaSession", gConfig.mMediaSession);
		get("External.CaptureDevice", gConfig.mCaptureDevice);
		std::string process;
		get("External.CaptureProcess", process);
		gConfig.mCaptureProcess = Widen(process);
		flag("Debug.Logging", gConfig.mLogging);

		gConfig.mMusicFolder = Widen(folder);
		if (gConfig.mMusicFolder.empty()) {
			gConfig.mMusicFolder = dir + L"\\SDRadio";
		}
		else if (gConfig.mMusicFolder.size() < 2 || (gConfig.mMusicFolder[1] != L':' && gConfig.mMusicFolder[0] != L'\\')) {
			gConfig.mMusicFolder = dir + L"\\" + gConfig.mMusicFolder; // relative to the plugins folder
		}
		while (gConfig.mMusicFolder.size() > 3 && (gConfig.mMusicFolder.back() == L'\\' || gConfig.mMusicFolder.back() == L'/')) {
			gConfig.mMusicFolder.pop_back();
		}
	}
}

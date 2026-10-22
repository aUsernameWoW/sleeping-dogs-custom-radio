#include "config.hh"

#include <Windows.h>

#include <cstdio>
#include <map>
#include <string_view>

Config gConfig;

namespace config
{
	namespace
	{
		constexpr char kDefaultIni[] =
			"; SDRadio 配置 / configuration (UTF-8)\n"
			"\n"
			"[Station]\n"
			"; 音乐文件夹里直接放的歌组成的电台的台名（最多 63 字节，中文约 21 个字）；子文件夹的台用文件夹名。\n"
			"; HUD name of the station of the files right in the music folder (at most 63 bytes); a subfolder's\n"
			"; station is named after the folder.\n"
			"Name = SDRADIO\n"
			"\n"
			"; 音乐文件夹。留空 = .asi 旁边的 SDRadio 文件夹。直接放在里面的歌是上面 Name 这个台；每个子文件夹\n"
			"; 是一个单独的电台，台名就是文件夹名（最多 33 个台，每台最多 255 首）。\n"
			"; 支持 Windows 能解码的格式：mp3、m4a/aac、flac、wav、wma 等。\n"
			"; Music folder. Empty = the SDRadio folder next to the .asi. The files right in it are the station above;\n"
			"; each subfolder is a station of its own, named after the folder (up to 33 stations, 255 tracks each).\n"
			"MusicFolder =\n"
			"\n"
			"; 电台图标：电台的文件夹里有 logo.png 就用它，否则用 SDRadio 自带的「私家台」（每次启动写成 .asi 旁边的\n"
			"; SDRadio-logo*.perm.bin / .temp.bin）。游戏会把电台图标染成白色，只有形状（透明度）会显示：\n"
			"; 请用透明背景、2:1（如 512x256）的 PNG。0 = 改用下面借来的游戏图标。\n"
			"; HUD logo: logo.png from the station's folder if there is one, else SDRadio's own (written at each start as\n"
			"; SDRadio-logo*.perm.bin / .temp.bin next to the .asi). The game tints station logos white, so only the\n"
			"; shape (transparency) shows: use a 2:1 PNG (e.g. 512x256) with a transparent background.\n"
			"; 0 = the game logo below.\n"
			"CustomLogo = 1\n"
			"\n"
			"; CustomLogo = 0 时借用的游戏电台图标：\n"
			"; With CustomLogo = 0, one of the game's station logos:\n"
			";   Logo_H_Klub / Radio_H_Klub_TexturePack, Logo_WarpRecords / Radio_WarpRecords_TexturePack,\n"
			";   Logo_NinjaTune / Radio_NinjaTune_TexturePack, Logo_RoadRunnerRecords / Radio_RoadRunnerRecords_TexturePack,\n"
			";   Logo_RealFM / Radio_RealFM_TexturePack, Logo_Kerrang / Radio_Kerrang_TexturePack,\n"
			";   Logo_Saggittarius / Radio_Saggittarius_TexturePack, Logo_DaptoneRecords / Radio_DaptoneRecords_TexturePack,\n"
			";   Logo_Softly / Radio_Softly_TexturePack, Logo_BooseyHawkes / Radio_BooseyHawkes_TexturePack\n"
			"TextureName = Logo_Softly\n"
			"TexturePack = Radio_Softly_TexturePack\n"
			"\n"
			"; 默认电台：换上另一辆车时，游戏会切到这辆车自己的电台；这里改成 SDRadio。music = 排在最前面的音乐电台，live = 直播电台，\n"
			"; off = 保持游戏原样。警车/救护车的 HKPD、回到刚下的车时恢复的电台、任务车和脚本指定的电台不受影响。\n"
			"; Default station: getting into another car, the game tunes to that car's own station; this tunes to SDRadio\n"
			"; instead. music = the first music station, live = the live station, off = as the game does. A police or emergency\n"
			"; car's HKPD, the station restored in the car you just left, and stations set by missions or scripts stay.\n"
			"Default = music\n"
			"\n"
			"[Live]\n"
			"; 实验功能：再加一个「直播」电台，播放实时音源（以后是网络电台等）。1 = 开启。\n"
			"; Experimental: a second, live station playing a real-time source (internet radio and more later). 1 = on.\n"
			"Enabled = 0\n"
			"; 直播电台的台名。/ The live station's name.\n"
			"Name = SDRADIO LIVE\n"
			"; 音源：test = 测试信号（每秒一声提示音，每四声一声高音）；radio = 网络电台（下面的 Url）；\n"
			"; app = 另一个程序的声音（下面的 App，需要 Windows 11）；spotify = Spotify（见下）。\n"
			"; Source: test = a test signal (a beep every second, every fourth one higher); radio = internet radio (Url);\n"
			"; app = another program's sound (App below; needs Windows 11); spotify = Spotify (below).\n"
			"Source = test\n"
			"; 网络电台的地址：MP3 或 Ogg Vorbis 的流，或指向它的 .m3u / .pls。可以在 https://www.radio-browser.info 找台。\n"
			"; Internet radio stream: MP3 or Ogg Vorbis, or an .m3u / .pls pointing at one. Find stations at\n"
			"; https://www.radio-browser.info\n"
			"Url =\n"
			"; Source = app 时要播放的程序。auto = 自动：正在放歌的那个程序（Windows 媒体浮窗里正在播放的，或者唯一在出声的程序）。\n"
			"; 也可以写进程名，例如 AppleMusic.exe（Apple Music）、Spotify.exe、cloudmusic.exe（网易云音乐）。\n"
			"; For Source = app: the program. auto = whichever plays music (its Windows media session plays, or it's the only\n"
			"; program making sound). Or its process name, e.g. AppleMusic.exe (Apple Music), Spotify.exe, cloudmusic.exe\n"
			"; (NetEase Cloud Music).\n"
			"App = auto\n"
			"; 1 = 游戏里没在放这个台时（换台、关电台、下车、暂停菜单）让程序暂停，回来时继续播放；0 = 不控制程序。\n"
			"; 1 = pause the program while the game doesn't play this station (another station, radio off, out of the car,\n"
			"; the pause menu) and resume it when it does; 0 = leave the program alone.\n"
			"PauseApp = 1\n"
			"; 怎么听到这个程序：direct = 直接听程序自己的声音，电台不出声，只是跟着电台暂停/继续（什么都不用设置）；\n"
			"; radio = 通过游戏电台听（带收音机效果），要在音量合成器里把程序的输出设备改成一个你不用的设备，不然会听到两份；\n"
			"; auto = 程序在默认输出设备上就用 direct，改到了别的设备就用 radio。\n"
			"; How the program is heard: direct = its own sound, the station stays silent and only pauses/resumes it (no setup);\n"
			"; radio = through the game's radio (its effect), with the program sent to an output device you don't listen to\n"
			"; (Volume mixer), else you hear it twice; auto = direct while it plays on the default output device, radio on another.\n"
			"AppSound = auto\n"
			"; Source = spotify：mod 启动 librespot，在 Spotify 里选择下面这个名字的设备即可在电台里播放（需要 Spotify Premium）。\n"
			"; librespot.exe 的位置，留空 = .asi 旁边的 librespot.exe。\n"
			"; For Source = spotify: the mod runs librespot, a Spotify Connect device you pick in the Spotify app (Premium).\n"
			"; Where librespot.exe is; empty = librespot.exe next to the .asi.\n"
			"Librespot =\n"
			"SpotifyName = SDRadio\n"
			"\n"
			"[External]\n"
			"; 实验功能（gpl/external-audio 分支），目前只写日志：读取系统媒体会话（正在播放什么），并抓取指定 App\n"
			"; 的声音、记录音量，用来验证在音量合成器里静音的 App 还能不能被抓到。还不会接进游戏。1 = 开启。\n"
			"; Experimental (gpl/external-audio branch), diagnostics only for now: logs the system media sessions\n"
			"; (what is playing) and captures an app's audio and logs its level, to find out whether an app muted in\n"
			"; the volume mixer can still be captured. Nothing reaches the game yet. 1 = on.\n"
			"Enabled = 0\n"
			"; 要读取的媒体会话：App ID 的一部分（如 spotify）。留空 = 系统当前的会话。\n"
			"; Media session to read: part of the app's ID (e.g. spotify). Empty = the current one.\n"
			"MediaSession =\n"
			"; 要抓取声音的程序（如 Spotify.exe、cloudmusic.exe、chrome.exe）。留空 = 抓取下面播放设备的全部声音。\n"
			"; Program to capture (e.g. Spotify.exe, chrome.exe). Empty = everything the playback device below plays.\n"
			"CaptureProcess =\n"
			"; CaptureProcess 留空时抓取的播放设备：名称的一部分。留空 = 默认设备。\n"
			"; With CaptureProcess empty, the playback device to capture: part of its name. Empty = the default device.\n"
			"CaptureDevice =\n"
			"\n"
			"[Debug]\n"
			"; 在 .asi 旁边写 SDRadio.log。\n"
			"; Write SDRadio.log next to the .asi.\n"
			"Logging = 1\n";

		std::string_view Trim(std::string_view s)
		{
			while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
			while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
			return s;
		}

		// GetPrivateProfileString reads BOM-less files in the ANSI code page (1252 here), which would mangle
		// a Chinese station name or folder, so the (tiny) file is parsed as UTF-8 here. Keys are
		// "Section.Key", case-sensitive.
		std::map<std::string, std::string> Parse(const std::string& text)
		{
			std::map<std::string, std::string> values;
			std::string section;
			std::string_view rest = text;
			if (rest.starts_with("\xEF\xBB\xBF")) {
				rest.remove_prefix(3);
			}
			while (!rest.empty()) {
				const size_t eol = rest.find('\n');
				std::string_view line = Trim(rest.substr(0, eol));
				rest = eol == std::string_view::npos ? std::string_view() : rest.substr(eol + 1);
				if (line.empty() || line.front() == ';' || line.front() == '#') {
					continue;
				}
				if (line.front() == '[' && line.back() == ']') {
					section = std::string(Trim(line.substr(1, line.size() - 2)));
					continue;
				}
				const size_t eq = line.find('=');
				if (eq != std::string_view::npos) {
					values[section + "." + std::string(Trim(line.substr(0, eq)))] = std::string(Trim(line.substr(eq + 1)));
				}
			}
			return values;
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
	}

	void Load(const std::wstring& dir)
	{
		const std::wstring path = dir + L"\\SDRadio.ini";

		if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
			FILE* file = nullptr;
			if (_wfopen_s(&file, path.c_str(), L"wb") == 0 && file) {
				fwrite(kDefaultIni, 1, sizeof(kDefaultIni) - 1, file);
				fclose(file);
			}
		}

		std::string text;
		FILE* file = nullptr;
		if (_wfopen_s(&file, path.c_str(), L"rb") == 0 && file) {
			char buffer[4096];
			size_t n;
			while ((n = fread(buffer, 1, sizeof(buffer), file)) > 0) {
				text.append(buffer, n);
			}
			fclose(file);
		}

		const auto values = Parse(text);
		auto get = [&](const char* key, std::string& out) {
			if (auto it = values.find(key); it != values.end() && !it->second.empty()) out = it->second;
		};
		get("Station.Name", gConfig.mStationName);
		get("Station.TextureName", gConfig.mTextureName);
		get("Station.TexturePack", gConfig.mTexturePack);
		std::string folder;
		get("Station.MusicFolder", folder);
		if (auto it = values.find("Station.CustomLogo"); it != values.end()) {
			gConfig.mCustomLogo = it->second != "0";
		}
		if (auto it = values.find("Station.Default"); it != values.end()) {
			std::string value = it->second;
			for (char& c : value) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
			gConfig.mDefaultStation = value == "live" || value == "off" || value == "0" ? (value == "0" ? "off" : value) : "music";
		}
		if (auto it = values.find("Live.Enabled"); it != values.end()) {
			gConfig.mLive = it->second == "1";
		}
		get("Live.Name", gConfig.mLiveName);
		get("Live.Source", gConfig.mLiveSource);
		get("Live.Url", gConfig.mLiveUrl);
		std::string app;
		get("Live.App", app);
		gConfig.mLiveApp = Widen(app);
		if (auto it = values.find("Live.PauseApp"); it != values.end()) {
			gConfig.mLivePauseApp = it->second != "0";
		}
		get("Live.AppSound", gConfig.mLiveAppSound);
		std::string librespot;
		get("Live.Librespot", librespot);
		gConfig.mLibrespot = librespot.empty() ? dir + L"\\librespot.exe" : Widen(librespot);
		get("Live.SpotifyName", gConfig.mSpotifyName);
		gConfig.mSpotifyCache = dir + L"\\SDRadio-spotify";
		if (auto it = values.find("External.Enabled"); it != values.end()) {
			gConfig.mExternal = it->second == "1";
		}
		get("External.MediaSession", gConfig.mMediaSession);
		get("External.CaptureDevice", gConfig.mCaptureDevice);
		std::string process;
		get("External.CaptureProcess", process);
		gConfig.mCaptureProcess = Widen(process);
		if (auto it = values.find("Debug.Logging"); it != values.end()) {
			gConfig.mLogging = it->second != "0";
		}

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

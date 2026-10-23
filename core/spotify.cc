#include "spotify.hh"

#include <Windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "live.hh"
#include "log.hh"
#include "stream_decoder.hh"

namespace spotify
{
	namespace
	{
		constexpr uint32_t kSourceRate = 44100; // librespot's output, whatever the bitrate
		// Read ahead at most this much (48 kHz) before leaving librespot blocked on its pipe: the delay between
		// the Spotify app's play/skip and the game's radio grows with it.
		constexpr uint64_t kMaxQueuedMs = 2'500;
		// Handed over at once when the game starts playing the station, so the live reader (which waits for
		// live::kResumeMs) starts right away.
		constexpr uint64_t kHeadMs = 2'000;
		// librespot runs this on every player event and logs its environment; it does nothing itself. cmd runs
		// in librespot's (hidden) console, so no window appears.
		constexpr wchar_t kOnEvent[] = L"cmd /c rem";
		// The env dump is logged at debug level by librespot's player_event_handler; the rest at info.
		constexpr wchar_t kRustLog[] = L"RUST_LOG=libmdns=info,librespot=info,librespot::player_event_handler=debug";

		constexpr uint64_t FramesForMs(uint64_t ms) { return ms * live::kRate / 1000; }

		std::wstring Widen(const std::string& utf8)
		{
			if (utf8.empty()) return {};
			const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
			std::wstring out(static_cast<size_t>(size), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), size);
			return out;
		}

		// 48 kHz stereo audio between the pipe reader and the feeder.
		class Queue
		{
		public:
			void Push(const std::vector<int16_t>& samples)
			{
				std::lock_guard lock(mLock);
				mSamples.insert(mSamples.end(), samples.begin(), samples.end());
			}
			uint64_t Frames()
			{
				std::lock_guard lock(mLock);
				return mSamples.size() / 2;
			}
			// Moves up to `frames` into `out` (cleared first); returns the frames moved.
			uint64_t Take(uint64_t frames, std::vector<int16_t>& out)
			{
				std::lock_guard lock(mLock);
				const size_t n = static_cast<size_t>((std::min)(frames, static_cast<uint64_t>(mSamples.size() / 2))) * 2;
				out.assign(mSamples.begin(), mSamples.begin() + static_cast<ptrdiff_t>(n));
				mSamples.erase(mSamples.begin(), mSamples.begin() + static_cast<ptrdiff_t>(n));
				return n / 2;
			}

		private:
			std::mutex mLock;
			std::deque<int16_t> mSamples;
		};

		void ReadAudio(HANDLE pipe, Queue& queue, const std::atomic<bool>& done)
		{
			streamdec::Resampler resampler;
			std::vector<uint8_t> buffer(16384 + 4);
			std::vector<int16_t> resampled;
			size_t carried = 0; // bytes of an incomplete frame kept at the buffer's start
			for (;;) {
				while (!done.load() && queue.Frames() >= FramesForMs(kMaxQueuedMs)) {
					Sleep(10);
				}
				if (done.load()) {
					return;
				}
				DWORD got = 0;
				if (!ReadFile(pipe, buffer.data() + carried, static_cast<DWORD>(buffer.size() - carried), &got, nullptr) || got == 0) {
					return; // librespot exited or was ended
				}
				const size_t bytes = carried + got;
				const size_t frames = bytes / 4;
				resampled.clear();
				resampler.Process(reinterpret_cast<const int16_t*>(buffer.data()), frames, kSourceRate, resampled);
				queue.Push(resampled);
				carried = bytes - frames * 4;
				std::memmove(buffer.data(), buffer.data() + frames * 4, carried);
			}
		}

		// "[2025-11-10T12:00:00Z INFO  librespot_core::session] Connecting to AP" -> level and message.
		bool SplitLogLine(std::string_view line, std::string_view& level, std::string_view& message)
		{
			if (line.empty() || line.front() != '[') return false;
			const size_t close = line.find("] ");
			if (close == std::string_view::npos) return false;
			const std::string_view header = line.substr(1, close - 1);
			const size_t a = header.find(' ');
			if (a == std::string_view::npos) return false;
			const size_t b = header.find_first_not_of(' ', a);
			const size_t c = header.find(' ', b);
			level = header.substr(b, c == std::string_view::npos ? std::string_view::npos : c - b);
			message = line.substr(close + 2);
			return true;
		}

		void ReadLog(HANDLE pipe)
		{
			EventParser parser;
			Event event;
			std::string pending;
			char buffer[4096];
			DWORD got = 0;
			while (ReadFile(pipe, buffer, sizeof(buffer), &got, nullptr) && got) {
				pending.append(buffer, got);
				size_t at;
				while ((at = pending.find('\n')) != std::string::npos) {
					std::string line = pending.substr(0, at);
					pending.erase(0, at + 1);
					if (!line.empty() && line.back() == '\r') line.pop_back();
					if (parser.Feed(line, event)) {
						if (event.mType == "track_changed") {
							const std::string title = Title(event);
							live::SetNowPlaying(title);
							LOG("spotify: now playing \"%s\"", title.c_str());
						}
						else if (event.mType == "playing" || event.mType == "paused" || event.mType == "stopped") {
							LOG("spotify: %s", event.mType.c_str());
						}
						continue;
					}
					std::string_view level, message;
					if (SplitLogLine(line, level, message) && level != "DEBUG" && level != "TRACE") {
						LOG("spotify: %s %.*s", std::string(level).c_str(), static_cast<int>(message.size()), message.data());
					}
				}
			}
		}

		// The environment librespot runs with: ours, with RUST_LOG set for the event dumps.
		std::vector<wchar_t> Environment()
		{
			std::vector<wchar_t> block;
			if (wchar_t* strings = GetEnvironmentStringsW()) {
				for (const wchar_t* s = strings; *s; s += wcslen(s) + 1) {
					if (_wcsnicmp(s, L"RUST_LOG=", 9) == 0) continue;
					block.insert(block.end(), s, s + wcslen(s) + 1);
				}
				FreeEnvironmentStringsW(strings);
			}
			block.insert(block.end(), kRustLog, kRustLog + wcslen(kRustLog) + 1);
			block.push_back(L'\0');
			return block;
		}

		struct Child
		{
			HANDLE mProcess = nullptr;
			HANDLE mJob = nullptr;
			HANDLE mAudio = nullptr;
			HANDLE mLog = nullptr;

			~Child()
			{
				if (mProcess) {
					TerminateProcess(mProcess, 0);
					WaitForSingleObject(mProcess, 2'000);
					CloseHandle(mProcess);
				}
				if (mJob) CloseHandle(mJob);
				if (mAudio) CloseHandle(mAudio);
				if (mLog) CloseHandle(mLog);
			}
		};

		// Starts librespot with stdout and stderr on pipes, in a job that ends it when the game exits.
		bool Start(const Options& options, Child& child, std::string& error)
		{
			SECURITY_ATTRIBUTES inherit = { sizeof(inherit), nullptr, TRUE };
			HANDLE audioWrite = nullptr, logWrite = nullptr;
			if (!CreatePipe(&child.mAudio, &audioWrite, &inherit, 1 << 16) || !CreatePipe(&child.mLog, &logWrite, &inherit, 1 << 16)) {
				error = "CreatePipe failed (" + std::to_string(GetLastError()) + ")";
				if (audioWrite) CloseHandle(audioWrite);
				return false;
			}
			SetHandleInformation(child.mAudio, HANDLE_FLAG_INHERIT, 0);
			SetHandleInformation(child.mLog, HANDLE_FLAG_INHERIT, 0);
			HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);

			std::wstring name;
			for (const wchar_t c : Widen(options.mDeviceName)) {
				if (c != L'"') name += c;
			}
			CreateDirectoryW(options.mCacheDir.c_str(), nullptr);
			std::wstring command = L"\"" + options.mLibrespot + L"\" --name \"" + name +
				L"\" --backend pipe --format S16 --bitrate 320 --initial-volume 100 --volume-ctrl fixed --disable-audio-cache"
				L" --cache \"" + options.mCacheDir + L"\" --onevent \"" + kOnEvent + L"\"";
			std::vector<wchar_t> environment = Environment();

			STARTUPINFOW startup = { sizeof(startup) };
			startup.dwFlags = STARTF_USESTDHANDLES;
			startup.hStdInput = nul;
			startup.hStdOutput = audioWrite;
			startup.hStdError = logWrite;
			PROCESS_INFORMATION info = {};
			const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
				CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, environment.data(), nullptr, &startup, &info);
			const DWORD createError = GetLastError();
			// Only the child holds the write ends now: its exit ends our reads.
			CloseHandle(audioWrite);
			CloseHandle(logWrite);
			if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
			if (!created) {
				error = "can't start " + logger::ToUtf8(options.mLibrespot.c_str()) + " (" + std::to_string(createError) + ")";
				return false;
			}
			child.mProcess = info.hProcess;
			child.mJob = CreateJobObjectW(nullptr, nullptr);
			if (child.mJob) {
				JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
				limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
				SetInformationJobObject(child.mJob, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
				AssignProcessToJobObject(child.mJob, info.hProcess);
			}
			ResumeThread(info.hThread);
			CloseHandle(info.hThread);
			return true;
		}
	}

	bool EventParser::Feed(std::string_view line, Event& out)
	{
		if (!mInBlock) {
			if (line.find("with environment variables:") != std::string_view::npos) {
				mInBlock = true;
				mCurrent = {};
			}
			return false;
		}
		const size_t first = line.find_first_not_of(" \t");
		if (first == std::string_view::npos) {
			return false;
		}
		line.remove_prefix(first);
		if (line == "{") {
			return false;
		}
		if (line.front() == '}') {
			mInBlock = false;
			out = mCurrent;
			return true;
		}
		// "KEY": "VALUE",
		if (line.front() != '"') {
			mInBlock = false; // not the shape we know: drop it
			return false;
		}
		const size_t keyEnd = line.find("\": \"");
		if (keyEnd == std::string_view::npos) {
			return false;
		}
		const std::string_view key = line.substr(1, keyEnd - 1);
		std::string_view value = line.substr(keyEnd + 4);
		if (!value.empty() && value.back() == ',') value.remove_suffix(1);
		if (!value.empty() && value.back() == '"') value.remove_suffix(1);
		if (key == "PLAYER_EVENT") mCurrent.mType = Unescape(value);
		else if (key == "NAME") mCurrent.mName = Unescape(value);
		else if (key == "ARTISTS") mCurrent.mArtists = Unescape(value);
		return false;
	}

	std::string Unescape(std::string_view escaped)
	{
		std::string out;
		for (size_t i = 0; i < escaped.size(); ++i) {
			const char c = escaped[i];
			if (c != '\\' || i + 1 == escaped.size()) {
				out += c;
				continue;
			}
			const char e = escaped[++i];
			switch (e) {
			case 'n': out += '\n'; break;
			case 'r': out += '\r'; break;
			case 't': out += '\t'; break;
			case '0': out += '\0'; break;
			case 'u': {
				// \u{XXXX}
				const size_t close = escaped.find('}', i);
				if (i + 1 < escaped.size() && escaped[i + 1] == '{' && close != std::string_view::npos) {
					const uint32_t cp = std::strtoul(std::string(escaped.substr(i + 2, close - i - 2)).c_str(), nullptr, 16);
					if (cp < 0x80) {
						out += static_cast<char>(cp);
					}
					else if (cp < 0x800) {
						out += static_cast<char>(0xC0 | (cp >> 6));
						out += static_cast<char>(0x80 | (cp & 0x3F));
					}
					else if (cp < 0x10000) {
						out += static_cast<char>(0xE0 | (cp >> 12));
						out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
						out += static_cast<char>(0x80 | (cp & 0x3F));
					}
					else {
						out += static_cast<char>(0xF0 | (cp >> 18));
						out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
						out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
						out += static_cast<char>(0x80 | (cp & 0x3F));
					}
					i = close;
				}
				else {
					out += "\\u";
				}
				break;
			}
			default: out += e; break; // \" \\ \'
			}
		}
		return out;
	}

	std::string Title(const Event& event)
	{
		if (event.mName.empty()) {
			return {};
		}
		std::string artists;
		size_t start = 0;
		while (start <= event.mArtists.size()) {
			size_t end = event.mArtists.find('\n', start);
			if (end == std::string::npos) end = event.mArtists.size();
			if (end > start) {
				artists += (artists.empty() ? "" : ", ") + event.mArtists.substr(start, end - start);
			}
			start = end + 1;
		}
		return artists.empty() ? event.mName : artists + " - " + event.mName;
	}

	void Run(const Options& options, const std::atomic<bool>& stop)
	{
		bool missingLogged = false;
		std::vector<int16_t> chunk;
		LARGE_INTEGER frequency;
		QueryPerformanceFrequency(&frequency);
		while (!stop.load()) {
			if (GetFileAttributesW(options.mLibrespot.c_str()) == INVALID_FILE_ATTRIBUTES) {
				if (!missingLogged) {
					LOG("spotify: no librespot at %s (see the README); the station stays silent", logger::ToUtf8(options.mLibrespot.c_str()).c_str());
					missingLogged = true;
				}
				for (int i = 0; i < 50 && !stop.load(); ++i) Sleep(100);
				continue;
			}
			missingLogged = false;

			Child child;
			std::string error;
			if (!Start(options, child, error)) {
				LOG("spotify: %s", error.c_str());
				for (int i = 0; i < 50 && !stop.load(); ++i) Sleep(100);
				continue;
			}
			LOG("spotify: librespot started as Spotify Connect device \"%s\"; pick it in the Spotify app (Premium)", options.mDeviceName.c_str());

			Queue queue;
			std::atomic<bool> done{ false };
			std::thread audio([&] { ReadAudio(child.mAudio, queue, done); });
			std::thread log([&] { ReadLog(child.mLog); });

			// Feeds the ring by the clock while the game plays the station; otherwise reads nothing more, which
			// soon blocks librespot's writes and so holds the music where it is.
			bool feeding = false;
			LARGE_INTEGER start = {};
			uint64_t due0 = 0, sent = 0;
			DWORD exitCode = STILL_ACTIVE;
			while (!stop.load()) {
				if (WaitForSingleObject(child.mProcess, 10) == WAIT_OBJECT_0) {
					GetExitCodeProcess(child.mProcess, &exitCode);
					break;
				}
				if (!live::Listening(3'000)) {
					feeding = false;
					continue;
				}
				LARGE_INTEGER now;
				QueryPerformanceCounter(&now);
				if (!feeding) {
					feeding = true;
					start = now;
					sent = 0;
					due0 = queue.Take(FramesForMs(kHeadMs), chunk);
					if (!chunk.empty()) live::Stream().Push(chunk.data(), chunk.size());
					sent = due0;
					continue;
				}
				const uint64_t due = due0 + static_cast<uint64_t>((now.QuadPart - start.QuadPart) * live::kRate / frequency.QuadPart);
				if (due <= sent) continue;
				const uint64_t taken = queue.Take(due - sent, chunk);
				if (taken) {
					live::Stream().Push(chunk.data(), chunk.size());
					sent += taken;
				}
				// Nothing for 200 ms (paused in Spotify, or between connections): silence keeps the ring moving.
				if (due - sent >= FramesForMs(200)) {
					chunk.assign(static_cast<size_t>(due - sent) * 2, 0);
					live::Stream().Push(chunk.data(), chunk.size());
					sent = due;
				}
			}

			done = true;
			if (child.mProcess) {
				TerminateProcess(child.mProcess, 0);
			}
			audio.join();
			log.join();
			if (!stop.load()) {
				LOG("spotify: librespot exited (code %lu); restarting in 5 s", exitCode);
				for (int i = 0; i < 50 && !stop.load(); ++i) Sleep(100);
			}
		}
	}
}

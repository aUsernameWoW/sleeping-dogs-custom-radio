#include "netradio.hh"
#include "pacer.hh"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <thread>

#include "icy.hh"
#include "live.hh"
#include "log.hh"
#include "stream_decoder.hh"

namespace netradio
{
	namespace
	{
		struct Handle
		{
			HINTERNET mHandle = nullptr;
			Handle() = default;
			explicit Handle(HINTERNET h) : mHandle(h) {}
			Handle(const Handle&) = delete;
			Handle& operator=(const Handle&) = delete;
			~Handle()
			{
				if (mHandle) WinHttpCloseHandle(mHandle);
			}
			operator HINTERNET() const { return mHandle; }
		};

		std::wstring Widen(const std::string& utf8)
		{
			if (utf8.empty()) return {};
			const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
			std::wstring out(static_cast<size_t>(size), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), size);
			return out;
		}

		std::string Header(HINTERNET request, DWORD query, const wchar_t* name = WINHTTP_HEADER_NAME_BY_INDEX)
		{
			wchar_t buffer[512];
			DWORD size = sizeof(buffer);
			if (!WinHttpQueryHeaders(request, query, name, buffer, &size, WINHTTP_NO_HEADER_INDEX)) {
				return {};
			}
			return logger::ToUtf8(buffer);
		}

		struct Response
		{
			Handle mSession, mConnection, mRequest;
			std::string mContentType;
		};

		// GET with ICY metadata asked for. False (and `error`) unless the server answers 200.
		bool Get(const std::string& url, Response& r, std::string& error)
		{
			const std::wstring wide = Widen(url);
			URL_COMPONENTS parts = { sizeof(parts) };
			wchar_t host[256] = {}, path[2048] = {};
			parts.lpszHostName = host;
			parts.dwHostNameLength = ARRAYSIZE(host);
			parts.lpszUrlPath = path;
			parts.dwUrlPathLength = ARRAYSIZE(path);
			parts.dwExtraInfoLength = 1; // the query string lands in the path buffer too
			if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts)) {
				error = "not a URL";
				return false;
			}
			std::wstring target = path;
			if (target.empty()) target = L"/";

			// The system's proxy settings where WinHTTP supports reading them (Windows 8.1+), else none.
			r.mSession.mHandle = WinHttpOpen(L"SDRadio", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
			if (!r.mSession) {
				r.mSession.mHandle = WinHttpOpen(L"SDRadio", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
			}
			if (!r.mSession) {
				error = "WinHttpOpen failed (" + std::to_string(GetLastError()) + ")";
				return false;
			}
			// Bounds every blocking call, so a stalled server can't hold the source past stop by much.
			WinHttpSetTimeouts(r.mSession, 10'000, 10'000, 10'000, 10'000);
			r.mConnection.mHandle = WinHttpConnect(r.mSession, host, parts.nPort, 0);
			const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
			r.mRequest.mHandle = r.mConnection ? WinHttpOpenRequest(r.mConnection, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags) : nullptr;
			if (!r.mRequest) {
				error = "connect failed (" + std::to_string(GetLastError()) + ")";
				return false;
			}
			if (!WinHttpSendRequest(r.mRequest, L"Icy-MetaData: 1\r\n", static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
				|| !WinHttpReceiveResponse(r.mRequest, nullptr)) {
				const DWORD code = GetLastError();
				// SHOUTcast 1 answers "ICY 200 OK", which isn't HTTP and which WinHTTP rejects.
				error = code == ERROR_WINHTTP_INVALID_SERVER_RESPONSE ? "the server's reply isn't HTTP (an old SHOUTcast 1 server?), not supported"
																	: "request failed (" + std::to_string(code) + ")";
				return false;
			}
			const std::string status = Header(r.mRequest, WINHTTP_QUERY_STATUS_CODE);
			if (status != "200") {
				error = "HTTP " + status;
				return false;
			}
			r.mContentType = Header(r.mRequest, WINHTTP_QUERY_CONTENT_TYPE);
			return true;
		}

		std::mutex gLock;
		std::string gNowPlaying;

		void SetTitle(const std::string& title)
		{
			std::lock_guard lock(gLock);
			if (title == gNowPlaying) {
				return;
			}
			gNowPlaying = title;
			LOG("netradio: now playing \"%s\"", title.c_str());
		}

		enum class Outcome
		{
			Decoded,     // played for a while, then lost the connection: reconnect soon
			Failed,      // nothing played: reconnect with growing backoff
			Unsupported, // a format we don't decode: reconnecting won't help
		};

		// One connection: resolves playlists, then decodes until an error or stop.
		Outcome Play(std::string url, Pacer& pacer, std::mutex& pacerLock, const std::atomic<bool>& stop)
		{
			std::unique_ptr<Response> response;
			std::string error;
			for (int hop = 0;; ++hop) {
				response = std::make_unique<Response>();
				if (!Get(url, *response, error)) {
					LOG("netradio: %s: %s", url.c_str(), error.c_str());
					return Outcome::Failed;
				}
				if (!icy::IsPlaylist(response->mContentType, url)) {
					break;
				}
				std::string body;
				char buffer[4096];
				DWORD got = 0;
				while (body.size() < 65536 && WinHttpReadData(response->mRequest, buffer, sizeof(buffer), &got) && got) {
					body.append(buffer, got);
				}
				const std::string next = icy::PlaylistUrl(body);
				if (next.empty() || hop == 3) {
					LOG("netradio: %s is a playlist (%s) without a stream URL in it", url.c_str(), response->mContentType.c_str());
					return Outcome::Failed;
				}
				LOG("netradio: playlist %s -> %s", url.c_str(), next.c_str());
				url = next;
			}
			Response& r = *response;

			const std::string metaint = Header(r.mRequest, WINHTTP_QUERY_CUSTOM, L"icy-metaint");
			const std::string name = Header(r.mRequest, WINHTTP_QUERY_CUSTOM, L"icy-name");
			const std::string bitrate = Header(r.mRequest, WINHTTP_QUERY_CUSTOM, L"icy-br");
			LOG("netradio: connected to %s: %s, \"%s\", %s kbit/s, metadata every %s bytes", url.c_str(), r.mContentType.c_str(),
				name.c_str(), bitrate.empty() ? "?" : bitrate.c_str(), metaint.empty() ? "(none)" : metaint.c_str());

			icy::Demuxer demuxer(static_cast<uint32_t>(strtoul(metaint.c_str(), nullptr, 10)));
			std::unique_ptr<streamdec::Decoder> decoder;
			streamdec::Resampler resampler;
			std::vector<uint8_t> audio;
			std::vector<std::string> metadata;
			std::vector<int16_t> resampled;
			uint32_t rate = 0;
			bool decoded = false;
			char buffer[16384];
			while (!stop.load()) {
				// Backpressure: don't read ahead of the pacer by more than kMaxBufferedMs.
				for (;;) {
					uint64_t buffered;
					{
						std::lock_guard lock(pacerLock);
						buffered = pacer.BufferedMs();
					}
					if (buffered < kMaxBufferedMs || stop.load()) break;
					Sleep(100);
				}
				DWORD got = 0;
				if (!WinHttpReadData(r.mRequest, buffer, sizeof(buffer), &got)) {
					LOG("netradio: read failed (%lu)", GetLastError());
					return decoded ? Outcome::Decoded : Outcome::Failed;
				}
				if (got == 0) {
					LOG("netradio: the server ended the stream");
					return decoded ? Outcome::Decoded : Outcome::Failed;
				}
				audio.clear();
				metadata.clear();
				demuxer.Feed(reinterpret_cast<const uint8_t*>(buffer), got, audio, metadata);
				for (const std::string& block : metadata) {
					std::string title = icy::StreamTitle(block);
					if (!title.empty()) {
						std::lock_guard lock(pacerLock);
						pacer.Mark(std::move(title));
					}
				}
				if (audio.empty()) {
					continue;
				}
				if (!decoder) {
					const icy::Format format = icy::DetectFormat(r.mContentType, audio.data(), audio.size());
					decoder = streamdec::Create(format);
					if (!decoder) {
						LOG("netradio: %s streams aren't supported (yet); many stations also offer MP3 or Ogg Vorbis", icy::FormatName(format));
						return Outcome::Unsupported;
					}
					LOG("netradio: decoding %s", icy::FormatName(format));
				}
				const bool ok = decoder->Push(audio.data(), audio.size(), [&](const int16_t* frames, size_t count, uint32_t frameRate) {
					if (frameRate != rate) {
						LOG("netradio: %u Hz%s", frameRate, frameRate == live::kRate ? "" : ", resampling to 48000");
						rate = frameRate;
					}
					decoded = true;
					resampled.clear();
					resampler.Process(frames, count, frameRate, resampled);
					std::lock_guard lock(pacerLock);
					pacer.Add(resampled.data(), resampled.size());
				});
				if (!ok) {
					LOG("netradio: the stream doesn't decode");
					return Outcome::Unsupported;
				}
				std::string title;
				if (decoder->TakeTitle(title)) {
					std::lock_guard lock(pacerLock);
					pacer.Mark(std::move(title));
				}
			}
			return decoded ? Outcome::Decoded : Outcome::Failed;
		}
	}

	void Run(const std::string& url, const std::atomic<bool>& stop)
	{
		if (url.empty()) {
			LOG("netradio: no [Live] Url set");
			return;
		}
		Pacer pacer;
		std::mutex pacerLock;
		std::atomic<bool> pacerStop{ false };

		// Feeds the ring by the clock.
		std::thread feeder([&] {
			std::vector<int16_t> chunk;
			bool wasPlaying = false;
			uint32_t underruns = 0;
			while (!pacerStop.load()) {
				Sleep(10);
				chunk.clear();
				bool playing;
				uint64_t buffered;
				std::string title;
				bool newTitle;
				{
					std::lock_guard lock(pacerLock);
					pacer.Take(GetTickCount64(), chunk);
					newTitle = pacer.TakeTitle(title);
					playing = pacer.Playing();
					buffered = pacer.BufferedMs();
					if (pacer.Underruns() != underruns) {
						underruns = pacer.Underruns();
						LOG("netradio: buffer ran dry (%u time(s)), waiting for %u ms", underruns, kRebufferMs);
					}
				}
				if (playing && !wasPlaying) {
					LOG("netradio: playing (%llu ms buffered)", buffered);
				}
				wasPlaying = playing;
				if (newTitle) {
					SetTitle(title);
				}
				if (!chunk.empty()) {
					live::Stream().Push(chunk.data(), chunk.size());
				}
			}
		});

		DWORD backoff = 2'000;
		while (!stop.load()) {
			const Outcome outcome = Play(url, pacer, pacerLock, stop);
			if (outcome == Outcome::Unsupported) {
				LOG("netradio: not retrying this station");
				break;
			}
			if (outcome == Outcome::Decoded) {
				backoff = 2'000;
			}
			if (stop.load()) break;
			LOG("netradio: reconnecting in %lu s", backoff / 1000);
			for (DWORD waited = 0; waited < backoff && !stop.load(); waited += 100) {
				Sleep(100);
			}
			backoff = (std::min)(backoff * 2, DWORD{ 30'000 });
		}
		pacerStop = true;
		feeder.join();
	}
}

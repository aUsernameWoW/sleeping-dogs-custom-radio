#include "icy.hh"

#include <algorithm>

#include "tags.hh"

namespace icy
{
	namespace
	{
		std::string Lower(std::string_view text)
		{
			std::string out(text);
			std::transform(out.begin(), out.end(), out.begin(), [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
			return out;
		}

		std::string_view Trim(std::string_view s)
		{
			while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\xEF' || s.front() == '\xBB' || s.front() == '\xBF')) {
				s.remove_prefix(1);
			}
			while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
				s.remove_suffix(1);
			}
			return s;
		}

		bool IsUrl(std::string_view s)
		{
			const std::string lower = Lower(s.substr(0, 8));
			return lower.starts_with("http://") || lower.starts_with("https://");
		}
	}

	void Demuxer::Feed(const uint8_t* data, size_t size, std::vector<uint8_t>& audio, std::vector<std::string>& metadata)
	{
		if (mMetaint == 0) {
			audio.insert(audio.end(), data, data + size);
			return;
		}
		while (size) {
			if (mInMeta) {
				const size_t n = (std::min)(size, static_cast<size_t>(mMetaLeft));
				mMeta.append(reinterpret_cast<const char*>(data), n);
				data += n;
				size -= n;
				mMetaLeft -= static_cast<uint32_t>(n);
				if (mMetaLeft == 0) {
					// Blocks are padded with NULs to a multiple of 16.
					const size_t end = mMeta.find('\0');
					if (end != std::string::npos) {
						mMeta.resize(end);
					}
					if (!mMeta.empty()) {
						metadata.push_back(std::move(mMeta));
					}
					mMeta.clear();
					mInMeta = false;
					mUntilMeta = mMetaint;
				}
			}
			else if (mUntilMeta == 0) {
				// The length byte; 0 means no metadata this time.
				mMetaLeft = static_cast<uint32_t>(*data) * 16;
				++data;
				--size;
				if (mMetaLeft) {
					mInMeta = true;
				}
				else {
					mUntilMeta = mMetaint;
				}
			}
			else {
				const size_t n = (std::min)(size, static_cast<size_t>(mUntilMeta));
				audio.insert(audio.end(), data, data + n);
				data += n;
				size -= n;
				mUntilMeta -= static_cast<uint32_t>(n);
			}
		}
	}

	std::string StreamTitle(std::string_view metadata)
	{
		const size_t key = metadata.find("StreamTitle='");
		if (key == std::string_view::npos) {
			return {};
		}
		const size_t begin = key + 13;
		// The value may contain quotes ("Guns N' Roses"); it ends at the quote before the field's ';' (or at the
		// end of the block).
		size_t end = metadata.find("';", begin);
		if (end == std::string_view::npos) {
			end = metadata.rfind('\'');
			if (end == std::string_view::npos || end < begin) {
				end = metadata.size();
			}
		}
		return tags::LegacyToUtf8(Trim(metadata.substr(begin, end - begin)));
	}

	bool IsPlaylist(std::string_view contentType, std::string_view url)
	{
		const std::string type = Lower(contentType);
		if (type.find("mpegurl") != std::string::npos || type.find("scpls") != std::string::npos || type.find("x-pls") != std::string::npos) {
			return true;
		}
		if (type.starts_with("audio/") && type.find("mpegurl") == std::string::npos) {
			return false;
		}
		std::string path = Lower(url.substr(0, url.find_first_of("?#")));
		return path.ends_with(".m3u") || path.ends_with(".m3u8") || path.ends_with(".pls");
	}

	std::string PlaylistUrl(std::string_view body)
	{
		// PLS: File1=http://...; M3U: the first line that isn't a comment.
		std::string_view rest = body;
		std::string firstPlain;
		while (!rest.empty()) {
			const size_t eol = rest.find('\n');
			const std::string_view line = Trim(rest.substr(0, eol));
			rest = eol == std::string_view::npos ? std::string_view() : rest.substr(eol + 1);
			if (line.empty() || line.front() == '#' || line.front() == '[') {
				continue;
			}
			const size_t eq = line.find('=');
			if (eq != std::string_view::npos && Lower(line.substr(0, 4)) == "file" && IsUrl(Trim(line.substr(eq + 1)))) {
				return std::string(Trim(line.substr(eq + 1)));
			}
			if (firstPlain.empty() && IsUrl(line)) {
				firstPlain = std::string(line);
			}
		}
		return firstPlain;
	}

	Format DetectFormat(std::string_view contentType, const uint8_t* head, size_t size)
	{
		// "application/ogg" says nothing about the codec: the first page's packet does.
		if (size >= 4 && head[0] == 'O' && head[1] == 'g' && head[2] == 'g' && head[3] == 'S') {
			const std::string_view page(reinterpret_cast<const char*>(head), (std::min)(size, size_t{ 128 }));
			if (page.find("OpusHead") != std::string_view::npos) return Format::Opus;
			if (page.find("\x7F" "FLAC") != std::string_view::npos) return Format::OggFlac;
			return Format::Ogg;
		}
		const std::string type = Lower(contentType);
		if (type.find("opus") != std::string::npos) {
			return Format::Opus;
		}
		if (type.find("mpeg") != std::string::npos || type.find("mp3") != std::string::npos) {
			return Format::Mp3;
		}
		if (type.find("ogg") != std::string::npos) {
			return Format::Ogg;
		}
		if (type.find("aac") != std::string::npos || type.find("mp4") != std::string::npos) {
			return Format::Aac;
		}
		if (size >= 3 && head[0] == 'I' && head[1] == 'D' && head[2] == '3') {
			return Format::Mp3;
		}
		if (size >= 2 && head[0] == 0xFF && (head[1] & 0xE0) == 0xE0) {
			// Frame sync: MPEG audio, unless the layer bits are 00, which is ADTS (AAC).
			return (head[1] & 0x06) == 0 ? Format::Aac : Format::Mp3;
		}
		return Format::Unknown;
	}

	const char* FormatName(Format format)
	{
		switch (format) {
		case Format::Mp3: return "MP3";
		case Format::Ogg: return "Ogg";
		case Format::Aac: return "AAC";
		case Format::Opus: return "Opus";
		case Format::OggFlac: return "Ogg FLAC";
		default: return "unknown";
		}
	}
}

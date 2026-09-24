#include "tags.hh"

#include <Windows.h>

#include <vector>

namespace tags
{
	namespace
	{
		bool IsUtf8(std::string_view s)
		{
			for (size_t i = 0; i < s.size();) {
				const auto c = static_cast<unsigned char>(s[i]);
				const size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
				if (n == 0 || i + n > s.size()) {
					return false;
				}
				for (size_t k = 1; k < n; ++k) {
					if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
				}
				i += n;
			}
			return true;
		}

		std::string Narrow(const wchar_t* text, int length)
		{
			const int size = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
			std::string out(static_cast<size_t>(size > 0 ? size : 0), '\0');
			if (size > 0) {
				WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), size, nullptr, nullptr);
			}
			return out;
		}

		std::string Utf16ToUtf8(const uint8_t* p, size_t size, bool bigEndian)
		{
			std::wstring w;
			for (size_t i = 0; i + 1 < size; i += 2) {
				w.push_back(static_cast<wchar_t>(bigEndian ? (p[i] << 8) | p[i + 1] : p[i] | (p[i + 1] << 8)));
			}
			return Narrow(w.data(), static_cast<int>(w.size()));
		}

		// Values separated by NULs (ID3v2.4 lists) become "a; b"; trailing NULs and blanks go.
		std::string Tidy(std::string s)
		{
			std::string out;
			for (size_t i = 0; i < s.size(); ++i) {
				if (s[i] == '\0') {
					while (i + 1 < s.size() && s[i + 1] == '\0') ++i;
					if (i + 1 < s.size() && !out.empty()) out += "; ";
				}
				else {
					out += s[i];
				}
			}
			while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
			while (!out.empty() && (out.front() == ' ' || out.front() == '\t')) out.erase(out.begin());
			return out;
		}

		std::string DecodeText(const uint8_t* p, size_t size)
		{
			if (size == 0) {
				return {};
			}
			const uint8_t encoding = p[0];
			++p;
			--size;
			switch (encoding) {
			case 1: { // UTF-16 with BOM (each value of a list has its own)
				std::string out;
				while (size >= 2) {
					bool big = false;
					if (p[0] == 0xFE && p[1] == 0xFF) { big = true; p += 2; size -= 2; }
					else if (p[0] == 0xFF && p[1] == 0xFE) { p += 2; size -= 2; }
					size_t end = 0;
					while (end + 1 < size && (p[end] || p[end + 1])) end += 2;
					if (!out.empty()) out.push_back('\0');
					out += Utf16ToUtf8(p, end, big);
					end = end + 2 <= size ? end + 2 : size;
					p += end;
					size -= end;
				}
				return Tidy(out);
			}
			case 2:
				return Tidy(Utf16ToUtf8(p, size, true));
			case 3:
				return Tidy(std::string(reinterpret_cast<const char*>(p), size));
			default:
				return Tidy(LegacyToUtf8(std::string_view(reinterpret_cast<const char*>(p), size)));
			}
		}

		uint32_t Syncsafe(const uint8_t* p) { return (p[0] & 0x7F) << 21 | (p[1] & 0x7F) << 14 | (p[2] & 0x7F) << 7 | (p[3] & 0x7F); }
		uint32_t Be32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

		// Unsynchronisation inserts a 00 after every FF; undo it.
		std::vector<uint8_t> Resync(const uint8_t* p, size_t size)
		{
			std::vector<uint8_t> out;
			out.reserve(size);
			for (size_t i = 0; i < size; ++i) {
				out.push_back(p[i]);
				if (p[i] == 0xFF && i + 1 < size && p[i + 1] == 0x00) ++i;
			}
			return out;
		}

		void Set(std::string& field, std::string value)
		{
			if (field.empty() && !value.empty()) field = std::move(value);
		}
	}

	std::string LegacyToUtf8(std::string_view bytes)
	{
		while (!bytes.empty() && (bytes.back() == '\0' || bytes.back() == ' ')) bytes.remove_suffix(1);
		if (IsUtf8(bytes)) {
			return std::string(bytes);
		}
		const int length = static_cast<int>(bytes.size());
		int size = MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, bytes.data(), length, nullptr, 0);
		const UINT codePage = size > 0 ? 936 : 28591; // GBK, else ISO-8859-1
		size = MultiByteToWideChar(codePage, 0, bytes.data(), length, nullptr, 0);
		std::wstring w(static_cast<size_t>(size > 0 ? size : 0), L'\0');
		if (size > 0) {
			MultiByteToWideChar(codePage, 0, bytes.data(), length, w.data(), size);
		}
		return Narrow(w.data(), static_cast<int>(w.size()));
	}

	void ParseId3v2(const uint8_t* data, size_t size, Tags& out)
	{
		if (size < 10 || data[0] != 'I' || data[1] != 'D' || data[2] != '3') {
			return;
		}
		const uint8_t version = data[3];
		const uint8_t flags = data[5];
		size_t tagSize = Syncsafe(data + 6);
		if (tagSize > size - 10) tagSize = size - 10;

		std::vector<uint8_t> body(data + 10, data + 10 + tagSize);
		if ((flags & 0x80) && version < 4) {
			body = Resync(body.data(), body.size()); // whole-tag unsynchronisation (2.2/2.3)
		}
		size_t at = 0;
		if ((flags & 0x40) && version >= 3 && body.size() >= 4) {
			at = version == 3 ? Be32(body.data()) + 4 : Syncsafe(body.data()); // extended header
		}

		const size_t headerSize = version == 2 ? 6 : 10;
		while (at + headerSize <= body.size()) {
			const uint8_t* h = body.data() + at;
			if (h[0] == 0) break; // padding
			std::string_view id(reinterpret_cast<const char*>(h), version == 2 ? 3 : 4);
			size_t frameSize = version == 2 ? (h[3] << 16 | h[4] << 8 | h[5]) : version == 3 ? Be32(h + 4) : Syncsafe(h + 4);
			at += headerSize;
			if (frameSize > body.size() - at) break;

			const uint8_t* p = body.data() + at;
			size_t n = frameSize;
			std::vector<uint8_t> resynced;
			if (version == 4) {
				const uint8_t formatFlags = h[9];
				if (formatFlags & 0x01) { // data length indicator
					if (n < 4) { at += frameSize; continue; }
					p += 4;
					n -= 4;
				}
				if (formatFlags & 0x02) { // frame-level unsynchronisation
					resynced = Resync(p, n);
					p = resynced.data();
					n = resynced.size();
				}
			}

			if (id == "TIT2" || id == "TT2") {
				Set(out.mTitle, DecodeText(p, n));
			}
			else if (id == "TPE1" || id == "TP1") {
				Set(out.mArtist, DecodeText(p, n));
			}
			else if (id == "TPE2" || id == "TP2") {
				Set(out.mAlbumArtist, DecodeText(p, n));
			}
			at += frameSize;
		}
	}

	void ParseId3v1(const uint8_t* data, size_t size, Tags& out)
	{
		if (size < 128 || data[0] != 'T' || data[1] != 'A' || data[2] != 'G') {
			return;
		}
		auto field = [&](size_t offset) {
			std::string_view raw(reinterpret_cast<const char*>(data + offset), 30);
			raw = raw.substr(0, raw.find('\0'));
			return Tidy(LegacyToUtf8(raw));
		};
		Set(out.mTitle, field(3));
		Set(out.mArtist, field(33));
	}

	void AddVorbisComment(std::string_view comment, Tags& out)
	{
		const size_t eq = comment.find('=');
		if (eq == std::string_view::npos) {
			return;
		}
		std::string key(comment.substr(0, eq));
		for (char& c : key) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
		const std::string value = Tidy(std::string(comment.substr(eq + 1)));
		if (value.empty()) {
			return;
		}
		if (key == "TITLE") {
			Set(out.mTitle, value);
		}
		else if (key == "ARTIST") {
			out.mArtist = out.mArtist.empty() ? value : out.mArtist + "; " + value;
		}
		else if (key == "ALBUMARTIST" || key == "ALBUM ARTIST") {
			Set(out.mAlbumArtist, value);
		}
	}

	void Finish(Tags& t)
	{
		if (t.mArtist.empty()) {
			t.mArtist = t.mAlbumArtist;
		}
	}
}

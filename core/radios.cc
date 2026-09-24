#include "radios.hh"

#include <cstdio>

namespace radios
{
	namespace
	{
		bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

		// Value of attribute `name` inside one tag (between '<' and '>'), or npos.
		size_t FindAttribute(std::string_view tag, std::string_view name, std::string_view& value)
		{
			for (size_t at = tag.find(name); at != std::string_view::npos; at = tag.find(name, at + 1)) {
				if (at == 0 || !IsSpace(tag[at - 1])) {
					continue;
				}
				size_t p = at + name.size();
				while (p < tag.size() && IsSpace(tag[p])) ++p;
				if (p >= tag.size() || tag[p] != '=') {
					continue;
				}
				++p;
				while (p < tag.size() && IsSpace(tag[p])) ++p;
				if (p >= tag.size() || (tag[p] != '"' && tag[p] != '\'')) {
					continue;
				}
				const size_t end = tag.find(tag[p], p + 1);
				if (end == std::string_view::npos) {
					return std::string_view::npos;
				}
				value = tag.substr(p + 1, end - p - 1);
				return at;
			}
			return std::string_view::npos;
		}

		void AppendEscaped(std::string& out, std::string_view text)
		{
			for (char c : text) {
				switch (c) {
				case '&': out += "&amp;"; break;
				case '<': out += "&lt;"; break;
				case '>': out += "&gt;"; break;
				case '"': out += "&quot;"; break;
				case '\'': out += "&apos;"; break;
				default:
					// Control characters have no business in a title and pugixml would keep them verbatim.
					if (static_cast<unsigned char>(c) >= 0x20) out += c;
					break;
				}
			}
		}
	}

	uint32_t MaxStationId(std::string_view xml)
	{
		uint32_t best = 0;
		for (size_t at = xml.find("<Station"); at != std::string_view::npos; at = xml.find("<Station", at + 1)) {
			const size_t end = xml.find('>', at);
			if (end == std::string_view::npos) {
				break;
			}
			std::string_view value;
			if (FindAttribute(xml.substr(at, end - at), "id", value) != std::string_view::npos) {
				uint32_t id = 0;
				for (char c : value) {
					if (c < '0' || c > '9') break;
					id = id * 10 + static_cast<uint32_t>(c - '0');
				}
				if (id > best) best = id;
			}
		}
		return best;
	}

	std::string TruncateUtf8(std::string_view text, size_t bytes)
	{
		if (text.size() <= bytes) {
			return std::string(text);
		}
		size_t cut = bytes;
		// Back up over continuation bytes (10xxxxxx) to the start of the character that doesn't fit.
		while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
			--cut;
		}
		return std::string(text.substr(0, cut));
	}

	std::string Append(std::string_view xml, const Station& station, uint32_t id)
	{
		const size_t close = xml.rfind("</Radios>");
		if (close == std::string_view::npos) {
			return {};
		}

		// Only tracks: chanceTrack 100 makes DetermineAssetType always pick a track, and with no ads or DJs
		// the other asset kinds have nothing to play anyway. The ident asset every station gets is never
		// chosen for the same reason.
		std::string s;
		char head[128];
		std::snprintf(head, sizeof(head), "\t<Station name=\"");
		s += head;
		AppendEscaped(s, TruncateUtf8(station.mName, 63));
		std::snprintf(head, sizeof(head), "\" id=\"%02u\" numConsecutiveTracks=\"01\" chanceDJ=\"0\" chanceAd=\"0\" chanceTrack=\"100\">\r\n", id);
		s += head;
		std::snprintf(head, sizeof(head), "\t\t<Tracks count=\"%zu\">\r\n", station.mTracks.size());
		s += head;
		for (const Track& t : station.mTracks) {
			s += "\t\t\t<Track artist=\"";
			AppendEscaped(s, t.mArtist);
			s += "\" name=\"";
			AppendEscaped(s, t.mTitle);
			s += "\"></Track>\r\n";
		}
		s += "\t\t</Tracks>\r\n\t\t<Ads count=\"0\"></Ads>\r\n\t\t<DJs count=\"0\"></DJs>\r\n\t\t<TextureName name=\"";
		AppendEscaped(s, station.mTextureName);
		s += "\"></TextureName>\r\n\t\t<TexturePack name=\"";
		AppendEscaped(s, station.mTexturePack);
		s += "\"></TexturePack>\r\n\t</Station>\r\n";

		std::string out;
		out.reserve(xml.size() + s.size());
		out.append(xml.substr(0, close));
		out += s;
		out.append(xml.substr(close));
		return out;
	}
}

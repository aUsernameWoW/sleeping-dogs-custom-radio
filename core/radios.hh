#pragma once

// Data\Audio\Radios.xml: the station list UFG::Radio::LoadRadioStationData reads at startup (name, id,
// chances, tracks with artist/name, ads, DJs, HUD logo). The HUD widget and the station cycling are built
// from the resulting list, so appending a <Station> is all it takes for a new station to exist.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace radios
{
	struct Track
	{
		std::string mArtist; // UTF-8
		std::string mTitle;
	};

	struct Station
	{
		std::string mName;        // UTF-8; RadioStation::m_name is a char[64], so it is cut to 63 bytes
		std::string mTextureName; // HUD logo, e.g. Logo_Softly
		std::string mTexturePack; // e.g. Radio_Softly_TexturePack (Data\UI\<pack>.perm.bin)
		std::vector<Track> mTracks;
	};

	// Highest id="..." among the <Station> elements (0 if none).
	uint32_t MaxStationId(std::string_view xml);

	// The document with `station` added as the last station under id `id`; empty if there's no </Radios>.
	std::string Append(std::string_view xml, const Station& station, uint32_t id);

	// Cuts a UTF-8 string to at most `bytes` bytes without splitting a character.
	std::string TruncateUtf8(std::string_view text, size_t bytes);
}

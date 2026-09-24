// Radios.xml editing (core/radios.cc): the next station ID comes from the existing ones, the station goes
// in right before </Radios> with everything escaped, and the name is cut to fit RadioStation::m_name
// (char[64]) without splitting a UTF-8 character. Compiles radios.cc directly; argv[1] is unused.

#include "../core/radios.cc"

#include <cstdio>

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

	// Shaped like the game's file: BOM, odd spacing around '=', mixed indentation, a trailing cop scanner.
	constexpr char kXml[] =
		"\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<Radios>\r\n"
		"\t  <Station name=\"H-KLUB\" id=\"01\"  numConsecutiveTracks=\"01\" chanceDJ=\"20\">\r\n"
		"\t\t  <Tracks count =\"1\"><Track artist=\"24HERBS\" name=\"Sin City\"></Track></Tracks>\r\n"
		"\t  </Station>\r\n"
		"<Station name=\"BOOSEY and HAWKES RADIO\" id=\"10\"  numConsecutiveTracks=\"01\">\r\n</Station>\r\n"
		"<Station name=\"HKPD\" id=\"11\" copScanner=\"true\">\r\n</Station>\r\n"
		"</Radios>\r\n";
}

int main()
{
	Check(radios::MaxStationId(kXml) == 11, "max station id");
	Check(radios::MaxStationId("<Radios></Radios>") == 0, "no stations");
	Check(radios::MaxStationId("<Station name=\"x\" myid=\"99\" id = '7'>") == 7, "only the real id attribute");

	radios::Station s;
	s.mName = "My <Radio> & \"Friends\"";
	s.mTextureName = "Logo_Softly";
	s.mTexturePack = "Radio_Softly_TexturePack";
	s.mTracks = { { "AC/DC", "It's a Long Way" }, { "", "\xE6\x9C\x88\xE4\xBA\xAE\xE4\xBB\xA3\xE8\xA1\xA8\xE6\x88\x91\xE7\x9A\x84\xE5\xBF\x83" } };
	const std::string out = radios::Append(kXml, s, 12);

	const size_t station = out.find("<Station name=\"My &lt;Radio&gt; &amp; &quot;Friends&quot;\" id=\"12\"");
	Check(station != std::string::npos, "escaped station element with id 12");
	Check(station != std::string::npos && station > out.find("id=\"11\""), "after the last station");
	Check(out.find("</Radios>") > station, "before </Radios>");
	Check(out.find("chanceDJ=\"0\" chanceAd=\"0\" chanceTrack=\"100\"") != std::string::npos, "tracks only");
	Check(out.find("<Tracks count=\"2\">") != std::string::npos, "track count");
	Check(out.find("artist=\"AC/DC\" name=\"It&apos;s a Long Way\"") != std::string::npos, "escaped track");
	Check(out.find("name=\"\xE6\x9C\x88\xE4\xBA\xAE") != std::string::npos, "UTF-8 title kept as is");
	Check(out.find("<TexturePack name=\"Radio_Softly_TexturePack\">") != std::string::npos, "logo");
	Check(out.compare(0, sizeof(kXml) - 12, kXml, sizeof(kXml) - 12) == 0, "original stations untouched");
	Check(radios::Append("<Radios>", s, 12).empty(), "no </Radios> -> empty");

	// 22 three-byte characters = 66 bytes: cut to 21 (63 bytes), never mid-character.
	std::string cjk;
	for (int i = 0; i < 22; ++i) cjk += "\xE7\x94\xB5";
	Check(radios::TruncateUtf8(cjk, 63).size() == 63, "63 bytes of CJK kept");
	Check(radios::TruncateUtf8(cjk, 62).size() == 60, "a partial character is dropped");
	Check(radios::TruncateUtf8("short", 63) == "short", "short names untouched");

	if (gFailures) {
		std::printf("%d failure(s)\n", gFailures);
		return 1;
	}
	std::printf("PASS\n");
	return 0;
}

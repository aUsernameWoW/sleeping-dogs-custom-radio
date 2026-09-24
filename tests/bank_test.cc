// The generated station bank (core/bank.cc), read back the way Wwise 2012.2's bank loader reads it (the
// readers are ported from the legacy exe: LoadSource, SetNodeBaseParams, CAkActorMixer, CAkAction/Play,
// CAkEvent): every object must be consumed exactly, IDs must match the names the game hashes, and the
// actor-mixer's node parameters must be byte-identical to the radio's top node in the station banks.
// Also writes the bank next to argv[1] (station12.bnk) for offline inspection. Compiles bank.cc directly.

#include "../core/bank.cc"

#include <cstdio>
#include <fstream>
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

	struct Reader
	{
		const uint8_t* p;
		size_t size;
		size_t at = 0;

		bool Has(size_t n) const { return at + n <= size; }
		uint8_t U8() { return Has(1) ? p[at++] : 0; }
		uint16_t U16() { uint16_t v = 0; if (Has(2)) std::memcpy(&v, p + at, 2); at += 2; return v; }
		uint32_t U32() { uint32_t v = 0; if (Has(4)) std::memcpy(&v, p + at, 4); at += 4; return v; }
		void Skip(size_t n) { at += n; }
	};

	// CAkParameterNodeBase::SetNodeBaseParams (feedback data present). Returns the parent ID.
	uint32_t NodeBase(Reader& r, uint32_t* bus = nullptr)
	{
		r.U8();                                   // override parent FX
		const uint8_t fx = r.U8();
		if (fx) {
			r.U8();                               // bypass bits
			r.Skip(fx * 7u);                      // index, fx ID, share set, rendered
		}
		const uint32_t outputBus = r.U32();
		if (bus) *bus = outputBus;
		const uint32_t parent = r.U32();
		r.Skip(2);                                // priority flags
		const uint8_t props = r.U8();
		r.Skip(props * 5u);
		const uint8_t ranges = r.U8();
		r.Skip(ranges * 9u);
		if (r.U8()) {                             // positioning override
			const uint8_t has2d = r.U8();
			const uint8_t has3d = r.U8();
			if (has2d) r.U8();
			Check(!has3d, "no 3D positioning expected");
		}
		r.Skip(3);                                // aux overrides
		if (r.U8()) r.Skip(16);                   // user aux sends
		r.Skip(13);                               // advanced settings
		const uint32_t states = r.U32();
		Check(states == 0, "no state groups expected");
		const uint16_t rtpcs = r.U16();
		for (uint16_t i = 0; i < rtpcs; ++i) {
			r.Skip(13);                           // RTPC ID, parameter, curve ID, scaling
			const uint16_t points = r.U16();
			r.Skip(points * 12u);
		}
		r.U32();                                  // feedback bus
		return parent;
	}

	// The radio's top node (music switch 992991330) as stored in every mus_radio_station_N bank of SFX.pck,
	// SetNodeBaseParams part: effect 287224143, bus radio_car, priority props, 2D, advanced, three RTPCs.
	constexpr uint8_t kTopNode[204] = {
		0x00, 0x01, 0x00, 0x00, 0x4f, 0xb1, 0x1e, 0x11, 0x01, 0x00, 0xcd, 0x94, 0xb6, 0xd3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x02, 0x05, 0x06, 0x00, 0x00, 0xc8, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x01, 0x00, 0x00, 0x02, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x1e,
		0x75, 0x67, 0xb4, 0x1c, 0x00, 0x00, 0x00, 0xe5, 0x84, 0x95, 0x2f, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x33, 0x33, 0x73, 0x3f, 0x00, 0x00, 0x80, 0x3f, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0xd3, 0x22, 0x42, 0x90, 0x00, 0x00, 0x00, 0x00, 0x7e, 0x66,
		0x67, 0x23, 0x02, 0x02, 0x00, 0x9a, 0x99, 0xc0, 0xc2, 0xff, 0xfe, 0x7f, 0xbf, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x48, 0x22, 0xf3, 0x49, 0x00, 0x00, 0x00, 0x00, 0x13, 0x3a, 0x52,
		0x14, 0x02, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0xaa, 0x42,
		0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc8, 0x42, 0x19, 0xfb, 0x7e, 0xbf, 0x04, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
	};
}

int main(int argc, char** argv)
{
	constexpr uint32_t kStation = 12;
	constexpr uint32_t kTracks = 3;
	const std::vector<uint8_t> image = bank::Build(kStation, kTracks);

	if (argc > 1) {
		std::string out = argv[1];
		out = out.substr(0, out.find_last_of("\\/") + 1) + "station12.bnk";
		std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
	}

	Check(bank::BankId(kStation) == bank::Fnv("mus_radio_station_12"), "bank ID = FNV(mus_radio_station_12)");
	Check(bank::TrackEventId(kStation, 2) == bank::Fnv("play_station_12_track_02"), "event ID = FNV(play_station_12_track_02)");
	Check(bank::TrackEventId(1, 1) == 3665015614u, "the game's own event ID for station 1 track 1");

	Reader r{ image.data(), image.size() };
	Check(std::memcmp(image.data(), "BKHD", 4) == 0, "BKHD first");
	r.Skip(4);
	Check(r.U32() == 24, "BKHD size");
	Check(r.U32() == 88, "bank version 88");
	Check(r.U32() == bank::BankId(kStation), "BKHD bank ID");
	r.Skip(16);
	Check(std::memcmp(image.data() + r.at, "HIRC", 4) == 0, "HIRC second");
	r.Skip(4);
	const uint32_t hircSize = r.U32();
	Check(r.at + hircSize == image.size(), "HIRC runs to the end");

	const uint32_t count = r.U32();
	Check(count == 1 + kTracks * 3 + 1, "object count: effect, sounds, mixer, actions, events");

	uint32_t sounds = 0, actions = 0, events = 0, mixers = 0, effects = 0;
	uint32_t mixerId = 0;
	for (uint32_t i = 0; i < count && r.Has(9); ++i) {
		const uint8_t type = r.U8();
		const uint32_t size = r.U32();
		const size_t end = r.at + size;
		const uint32_t id = r.U32();
		Reader o{ image.data(), end, r.at };

		switch (type) {
		case 18: // effect custom, copied verbatim
			++effects;
			Check(id == 287224143u, "effect ID");
			o.at = end;
			break;
		case 2: { // sound: LoadSource, then node params
			++sounds;
			Check(o.U32() == 0x00010001, "PCM codec plugin");
			Check(o.U32() == 1, "streamed");
			const uint32_t source = o.U32();
			const uint32_t file = o.U32();
			Check(source == file && file == bank::TrackFileId(kStation, sounds), "source/file ID of track");
			o.U8();
			const uint32_t parent = NodeBase(o);
			Check(parent == bank::Fnv("sdradio_station_12_mixer"), "sound's parent is the mixer");
			break;
		}
		case 7: { // actor-mixer
			++mixers;
			mixerId = id;
			Check(end - o.at >= sizeof(kTopNode) && std::memcmp(image.data() + o.at, kTopNode, sizeof(kTopNode)) == 0,
				"mixer node params identical to the radio top node");
			uint32_t bus = 0;
			Check(NodeBase(o, &bus) == 0 && bus == 3551958221u, "mixer: no parent, bus radio_car");
			const uint32_t children = o.U32();
			Check(children == kTracks, "mixer children");
			o.Skip(children * 4u);
			Check(sounds == kTracks, "sounds before their parent");
			break;
		}
		case 3: // action: type, target, is-bus, props, ranges, fade curve, bank ID
			++actions;
			Check(o.U16() == 0x0403, "Play action");
			o.U32();
			o.Skip(3);
			o.U8();
			Check(o.U32() == bank::BankId(kStation), "Play action's bank ID");
			Check(mixers == 1, "actions after the mixer");
			break;
		case 4: // event: action count, action IDs
			++events;
			Check(o.U32() == 1, "one action per event");
			o.U32();
			Check(id == bank::TrackEventId(kStation, events), "event IDs in track order");
			Check(actions == kTracks, "events after all actions");
			break;
		default:
			Check(false, "unexpected object type");
			o.at = end;
		}
		Check(o.at == end, "object consumed exactly");
		r.at = end;
	}
	Check(effects == 1 && sounds == kTracks && mixers == 1 && actions == kTracks && events == kTracks, "object counts");
	Check(mixerId == bank::Fnv("sdradio_station_12_mixer"), "mixer ID");
	Check(r.at == image.size(), "nothing after the last object");

	if (gFailures) {
		std::printf("%d failure(s)\n", gFailures);
		return 1;
	}
	std::printf("PASS (%zu bytes)\n", image.size());
	return 0;
}

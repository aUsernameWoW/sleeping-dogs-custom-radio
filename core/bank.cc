#include "bank.hh"

#include <bit>
#include <cstdio>
#include <cstring>

namespace bank
{
	namespace
	{
		constexpr uint32_t kGeneratorVersion = 88; // Wwise 2012.2, as in every bank of SFX.pck
		constexpr uint32_t kProjectId = 295;       // BKHD field copied from the station banks

		// HIRC object types.
		constexpr uint8_t kSound = 2;
		constexpr uint8_t kAction = 3;
		constexpr uint8_t kEvent = 4;
		constexpr uint8_t kActorMixer = 7;
		constexpr uint8_t kFxCustom = 18;

		constexpr uint32_t kPluginPcm = 0x00010001; // codec plugin (type 1), AKCODECID_PCM
		constexpr uint32_t kStreamed = 1;           // no prefetch, no in-bank data
		constexpr uint16_t kActionPlay = 0x0403;
		constexpr uint8_t kCurveLinear = 4;

		constexpr uint8_t kPropPriority = 5;
		constexpr uint8_t kPropPriorityDistanceOffset = 6;
		constexpr uint32_t kParamVolume = 0;
		constexpr uint32_t kParamBypassAllFx = 28;
		constexpr uint8_t kScalingNone = 0;
		constexpr uint8_t kScalingDb = 2;
		constexpr uint32_t kInterpLinear = 4;
		constexpr uint32_t kInterpSCurve = 5;
		constexpr uint32_t kInterpConstant = 9;

		// What the radio's shared top node (music switch 992991330 in every mus_radio_station_N bank) holds.
		constexpr uint32_t kBusRadioCar = 3551958221;  // FNV "radio_car", under master_music (Init.bnk)
		constexpr uint32_t kRadioEffect = 287224143;   // effect custom object below, shared by the stations
		constexpr uint32_t kRtpcRadioOnOff = 3026679070; // DoStartTrack sets the radio entity's "effect_off_on" to 0.95
		constexpr uint32_t kRtpcRadioVolume = 2420253395;
		constexpr uint32_t kRtpcRadioDuck = 1240670792;

		// The effect object as stored in the station banks (plugin 0x00820003, its parameter block and an
		// RTPC on one of its parameters). Every station bank carries the same bytes; ours needs its own copy
		// because the other station's bank is unloaded while ours plays, and the actor-mixer's effect slot
		// must resolve at load time.
		constexpr uint8_t kRadioEffectBody[] = {
			0x03, 0x00, 0x82, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0xc8, 0x42,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x1e, 0x75, 0x67, 0xb4, 0x01,
			0x00, 0x00, 0x00, 0x34, 0xe4, 0x90, 0x13, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0xc8, 0x44, 0x04, 0x00, 0x00, 0x00, 0x33, 0x33, 0x73, 0x3f, 0x00, 0x00, 0xc8, 0x42, 0x09, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0xc8, 0x42, 0x04, 0x00, 0x00, 0x00,
		};

		// Advanced settings (13 bytes: virtual voice behavior, max-instance limit and its flags, below-
		// threshold behavior, override flags, HDR envelope flags) of the top node and of a track segment.
		constexpr uint8_t kTopAdvanced[] = { 0x01, 0x00, 0x00, 0x02, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
		constexpr uint8_t kTrackAdvanced[] = { 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

		class Writer
		{
		public:
			std::vector<uint8_t> mBytes;

			void U8(uint8_t v) { mBytes.push_back(v); }
			void U16(uint16_t v) { Raw(&v, sizeof(v)); }
			void U32(uint32_t v) { Raw(&v, sizeof(v)); }
			void F32(float v) { U32(std::bit_cast<uint32_t>(v)); }
			void Raw(const void* data, size_t size)
			{
				const auto* p = static_cast<const uint8_t*>(data);
				mBytes.insert(mBytes.end(), p, p + size);
			}
			void Patch32(size_t at, uint32_t v) { std::memcpy(mBytes.data() + at, &v, sizeof(v)); }
			size_t Size() const { return mBytes.size(); }
		};

		struct Point
		{
			float mFrom;
			float mTo;
			uint32_t mInterp;
		};

		void Rtpc(Writer& w, uint32_t rtpc, uint32_t param, uint32_t curve, uint8_t scaling, std::initializer_list<Point> points)
		{
			w.U32(rtpc);
			w.U32(param);
			w.U32(curve);
			w.U8(scaling);
			w.U16(static_cast<uint16_t>(points.size()));
			for (const Point& p : points) {
				w.F32(p.mFrom);
				w.F32(p.mTo);
				w.U32(p.mInterp);
			}
		}

		// SetNodeBaseParams of the top node, with its parent (0) and bus unchanged.
		void TopNodeParams(Writer& w)
		{
			w.U8(0);            // don't override the parent's effects: there is no parent, this is the top
			w.U8(1);            // one effect
			w.U8(0);            // bypass bits
			w.U8(0);            //   slot 0
			w.U32(kRadioEffect);
			w.U8(1);            //   share set
			w.U8(0);            //   not rendered
			w.U32(kBusRadioCar);
			w.U32(0);           // no parent
			w.U8(0);            // priority: don't override parent
			w.U8(0);            // priority: no distance factor
			w.U8(2);            // props
			w.U8(kPropPriority);
			w.U8(kPropPriorityDistanceOffset);
			w.F32(100.0f);
			w.F32(0.0f);
			w.U8(0);            // ranged props
			w.U8(1);            // positioning: override
			w.U8(1);            //   2D
			w.U8(0);            //   no 3D
			w.U8(0);            //   2D panner off
			w.U8(0);            // aux: override game-defined
			w.U8(0);            //      use game-defined
			w.U8(0);            //      override user-defined
			w.U8(0);            //      no user aux sends
			w.Raw(kTopAdvanced, sizeof(kTopAdvanced));
			w.U32(0);           // state groups
			w.U16(3);           // RTPCs, exact copies (curve IDs and points)
			Rtpc(w, kRtpcRadioOnOff, kParamBypassAllFx, 798328037, kScalingNone,
				{ { 0.0f, 0.0f, kInterpConstant }, { 0.95f, 1.0f, kInterpConstant }, { 2.0f, 0.0f, kInterpConstant } });
			Rtpc(w, kRtpcRadioVolume, kParamVolume, 593979006, kScalingDb,
				{ { -96.3f, std::bit_cast<float>(0xBF7FFEFFu), kInterpLinear }, { 0.0f, 0.0f, kInterpLinear } });
			Rtpc(w, kRtpcRadioDuck, kParamVolume, 340933139, kScalingDb,
				{ { 0.0f, 0.0f, kInterpConstant }, { 85.0f, 0.0f, kInterpSCurve }, { 100.0f, std::bit_cast<float>(0xBF7EFB19u), kInterpLinear } });
			w.U32(0);           // feedback bus (the banks declare feedback data, so every node has this field)
		}

		void TrackNodeParams(Writer& w, uint32_t parent)
		{
			w.U8(0);            // effects from the parent
			w.U8(0);
			w.U32(0);           // bus from the parent
			w.U32(parent);
			w.U8(0);
			w.U8(0);
			w.U8(1);            // props: priority 100, like the original track segments
			w.U8(kPropPriority);
			w.F32(100.0f);
			w.U8(0);            // ranged props
			w.U8(0);            // positioning from the parent
			w.U8(0);            // aux from the parent, no sends
			w.U8(0);
			w.U8(0);
			w.U8(0);
			w.Raw(kTrackAdvanced, sizeof(kTrackAdvanced));
			w.U32(0);           // state groups
			w.U16(0);           // RTPCs
			w.U32(0);           // feedback bus
		}

		uint32_t NamedId(const char* format, uint32_t station, uint32_t index = 0)
		{
			char name[64];
			std::snprintf(name, sizeof(name), format, station, index);
			return Fnv(name);
		}

		uint32_t MixerId(uint32_t station) { return NamedId("sdradio_station_%02u_mixer", station); }
		uint32_t SoundId(uint32_t station, uint32_t track) { return NamedId("sdradio_station_%02u_sound_%03u", station, track); }
		uint32_t ActionId(uint32_t station, uint32_t track) { return NamedId("sdradio_station_%02u_action_%03u", station, track); }

		class Hirc
		{
		public:
			explicit Hirc(Writer& w) : mW(w)
			{
				mCountAt = w.Size();
				w.U32(0);
			}

			// Starts an object; returns the offset of its size field for End().
			size_t Begin(uint8_t type, uint32_t id)
			{
				mW.U8(type);
				const size_t at = mW.Size();
				mW.U32(0);
				mW.U32(id);
				++mCount;
				return at;
			}

			void End(size_t sizeAt) { mW.Patch32(sizeAt, static_cast<uint32_t>(mW.Size() - sizeAt - 4)); }
			void Finish() { mW.Patch32(mCountAt, mCount); }

		private:
			Writer& mW;
			size_t mCountAt = 0;
			uint32_t mCount = 0;
		};
	}

	uint32_t BankId(uint32_t station) { return NamedId("mus_radio_station_%u", station); }
	uint32_t TrackEventId(uint32_t station, uint32_t track) { return NamedId("play_station_%02u_track_%02u", station, track); }
	uint32_t TrackFileId(uint32_t station, uint32_t track) { return NamedId("sdradio_station_%02u_track_%03u", station, track); }

	std::vector<uint8_t> Build(uint32_t station, uint32_t trackCount)
	{
		if (trackCount > kMaxTracks) {
			trackCount = kMaxTracks;
		}

		const uint32_t bankId = BankId(station);
		const uint32_t mixer = MixerId(station);
		Writer w;

		w.Raw("BKHD", 4);
		w.U32(24);
		w.U32(kGeneratorVersion);
		w.U32(bankId);
		w.U32(0);  // language: SFX
		w.U32(1);  // feedback data in bank
		w.U32(kProjectId);
		w.U32(0);

		w.Raw("HIRC", 4);
		const size_t hircSizeAt = w.Size();
		w.U32(0);
		Hirc hirc(w);

		// Children before parents and actions before events, the order the Wwise bank generator uses: a
		// node links to an already loaded parent, a parent links its already loaded children, and an event
		// only resolves loaded actions.
		size_t at = hirc.Begin(kFxCustom, kRadioEffect);
		w.Raw(kRadioEffectBody, sizeof(kRadioEffectBody));
		hirc.End(at);

		for (uint32_t k = 1; k <= trackCount; ++k) {
			at = hirc.Begin(kSound, SoundId(station, k));
			const uint32_t file = TrackFileId(station, k);
			w.U32(kPluginPcm);
			w.U32(kStreamed);
			w.U32(file); // source ID
			w.U32(file); // file ID the stream manager opens
			w.U8(0);     // not language-specific
			TrackNodeParams(w, mixer);
			hirc.End(at);
		}

		at = hirc.Begin(kActorMixer, mixer);
		TopNodeParams(w);
		w.U32(trackCount);
		for (uint32_t k = 1; k <= trackCount; ++k) {
			w.U32(SoundId(station, k));
		}
		hirc.End(at);

		for (uint32_t k = 1; k <= trackCount; ++k) {
			at = hirc.Begin(kAction, ActionId(station, k));
			w.U16(kActionPlay);
			w.U32(SoundId(station, k));
			w.U8(0);  // target is not a bus
			w.U8(0);  // props
			w.U8(0);  // ranged props
			w.U8(kCurveLinear);
			w.U32(bankId);
			hirc.End(at);
		}

		for (uint32_t k = 1; k <= trackCount; ++k) {
			at = hirc.Begin(kEvent, TrackEventId(station, k));
			w.U32(1);
			w.U32(ActionId(station, k));
			hirc.End(at);
		}

		hirc.Finish();
		w.Patch32(hircSizeAt, static_cast<uint32_t>(w.Size() - hircSizeAt - 4));
		return std::move(w.mBytes);
	}
}

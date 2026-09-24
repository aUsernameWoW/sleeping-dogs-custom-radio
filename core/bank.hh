#pragma once

// The sound bank the game loads for our station ("mus_radio_station_<id>"), generated at runtime.
//
// The game's radio code is data-driven: RadioStation loads bank mus_radio_station_<id> and plays track k by
// posting event play_station_<id>_track_<k> (TrackAsset, %02u). The original stations are interactive
// music (segment -> random/sequence -> switch) under one shared top node, music switch 992991330, which
// carries everything that makes a station sound like the car radio: output bus radio_car, the tuning
// effect and three RTPCs. Our bank has, per track, a Play action + event and a Sound streaming PCM from a
// file ID that only the mod's I/O hook serves; the sounds sit under an actor-mixer that repeats the top
// node's parameters (effect, bus, RTPCs, 2D positioning) field for field.
//
// Format: bank generator version 88 (Wwise 2012.2). Object layouts follow the loaders in the legacy exe:
// CAkSound::SetInitialValues, CAkBankMgr::LoadSource, CAkParameterNodeBase::SetNodeBaseParams (FX, bus,
// parent, priority flags, props, ranges, positioning, aux, advanced settings, states, RTPCs, feedback bus),
// CAkActorMixer (node params + children), CAkAction (type, target, props, ranges, then Play's fade curve
// and bank ID), CAkEvent (action IDs, which must already be loaded).

#include <cstdint>
#include <string_view>
#include <vector>

namespace bank
{
	// Wwise short IDs: FNV-1 32-bit over the lowercased name (AK::SoundEngine::GetIDFromString; the game's
	// TiDo::CalcWwiseUid and qWiseSymbol use the same). mus_radio_station_1 -> 116370779.
	constexpr uint32_t Fnv(std::string_view name)
	{
		uint32_t hash = 2166136261u;
		for (char c : name) {
			const unsigned char lower = (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c - 'A' + 'a') : static_cast<unsigned char>(c);
			hash = hash * 16777619u;
			hash ^= lower;
		}
		return hash;
	}
	static_assert(Fnv("mus_radio_station_1") == 116370779u);
	static_assert(Fnv("play_station_01_track_01") == 3665015614u);

	uint32_t BankId(uint32_t station);                     // mus_radio_station_<id>
	uint32_t TrackEventId(uint32_t station, uint32_t track); // play_station_%02u_track_%02u, track from 1
	uint32_t TrackFileId(uint32_t station, uint32_t track);  // our streamed file, checked against the game's

	// Most tracks a station can have: TrackAsset stores the index in a byte.
	constexpr uint32_t kMaxTracks = 255;

	// The whole .bnk image (BKHD + HIRC) for tracks 1..trackCount.
	std::vector<uint8_t> Build(uint32_t station, uint32_t trackCount);
}

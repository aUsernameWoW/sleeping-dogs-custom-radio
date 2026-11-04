#pragma once

// Channel layouts of music files, and the ones the stations pass on to Wwise as they are.
//
// Every channel goes to Wwise (2026-11-04): the radio is a 2D sound without the panner, and Wwise 2012 maps
// such a source's channels straight onto the matching speakers (CAkSpeakerPan::GetSpeakerVolumes2DPan; a 5.1
// source's back pair lands on the side pair of a 7.1 output), then folds them into whatever the output is at
// mix time with ITU coefficients (AkDownmix::ComputeVolumes: centre and surrounds at -3 dB into stereo, LFE
// dropped). So a 5.1 track plays 5.1 under SDAtmos (whose bed is 7.1) or on surround speakers, and Wwise's
// own downmix on stereo. GetSpeakerVolumes2DPan only knows these layouts (LFE aside): stereo, 3.0, quad,
// 5.0 (back pair), 6.0 (back + side), 7.0; for any other mask it leaves the volumes unset. So 5.1 with side
// surrounds (0x60F, common in FLAC/WAV/MF output) is relabelled as back (same channel order), and layouts
// still outside that set are folded to stereo here, with the same coefficients. Mono stays duplicated to
// stereo as before (0 dB on both sides, not -3 dB through the centre).
//
// Masks and channel order are WAVEFORMATEXTENSIBLE's (SPEAKER_* bits; interleaved in ascending bit order).

#include <cstddef>
#include <cstdint>

namespace layout
{
	constexpr uint32_t kFL = 0x1, kFR = 0x2, kFC = 0x4, kLFE = 0x8, kBL = 0x10, kBR = 0x20, kFLC = 0x40, kFRC = 0x80, kBC = 0x100,
		kSL = 0x200, kSR = 0x400;
	constexpr uint32_t kStereo = kFL | kFR;
	constexpr uint32_t kMaxChannels = 8;

	struct Layout
	{
		uint32_t mChannels = 2;
		uint32_t mMask = kStereo;

		uint32_t BlockAlign() const { return mChannels * sizeof(int16_t); }
		bool operator==(const Layout&) const = default;
	};

	// The usual mask for a channel count with no mask of its own (WAVE, FLAC and Media Foundation order);
	// 0 for counts without one.
	uint32_t DefaultMask(uint32_t channels);

	// What a decoder delivers: `mask` if it names `channels` speakers, else the default for the count.
	Layout FromDecoder(uint32_t channels, uint32_t mask);

	// What the track's WAV stream carries: the source's own layout when Wwise maps it, else stereo.
	Layout ForWwise(const Layout& source);

	// Interleaved `frames` from one layout to another: the same speakers copied, side and back pairs standing
	// in for each other, the rest folded into the front pair (centres and surrounds at -3 dB, LFE dropped).
	void Convert(const int16_t* in, const Layout& from, size_t frames, const Layout& to, int16_t* out);

	// Vorbis interleaves in its own order (FL C FR ... LFE last). The permutation into WAVE order (out[k] =
	// in[perm[k]]) and the mask; false for counts Vorbis doesn't define (> 8).
	bool VorbisOrder(uint32_t channels, uint8_t (&perm)[kMaxChannels], uint32_t& mask);

	// "mono", "stereo", "5.1", ... or the mask in hex, for the log.
	const char* Name(const Layout& l, char (&buffer)[16]);
}

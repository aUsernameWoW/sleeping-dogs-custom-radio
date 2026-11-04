#include "layout.hh"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>

namespace layout
{
	namespace
	{
		constexpr float kMinus3dB = 0.70710677f;

		// Base masks (LFE aside) CAkSpeakerPan::GetSpeakerVolumes2DPan handles.
		constexpr uint32_t kWwiseBases[] = { 0x3, 0x7, 0x33, 0x37, 0x633, 0x637 };

		uint32_t Count(uint32_t mask) { return static_cast<uint32_t>(std::popcount(mask)); }

		// Index of speaker `bit` among the channels of `mask` (interleaved in ascending bit order).
		uint32_t IndexOf(uint32_t mask, uint32_t bit) { return Count(mask & (bit - 1)); }

		// A side pair with no back pair plays as the back pair (and the other way round), as Wwise itself does
		// between 5.1 and 7.1. Keeps the channel order, since nothing in a mapped layout sits between them.
		uint32_t SurroundsAs(uint32_t mask, uint32_t target)
		{
			const uint32_t side = kSL | kSR, back = kBL | kBR;
			if ((mask & side) == side && !(mask & back) && !(target & side) && (target & back) == back) {
				return (mask & ~side) | back;
			}
			if ((mask & back) == back && !(mask & side) && !(target & back) && (target & side) == side) {
				return (mask & ~back) | side;
			}
			return mask;
		}

		int16_t Clamp(float v) { return static_cast<int16_t>(std::clamp(v, -32768.0f, 32767.0f)); }
	}

	uint32_t DefaultMask(uint32_t channels)
	{
		switch (channels) {
		case 1: return kFC;
		case 2: return kStereo;
		case 3: return kStereo | kFC;
		case 4: return kStereo | kBL | kBR;
		case 5: return kStereo | kFC | kBL | kBR;
		case 6: return kStereo | kFC | kLFE | kBL | kBR;
		case 7: return kStereo | kFC | kLFE | kBC | kSL | kSR;
		case 8: return kStereo | kFC | kLFE | kBL | kBR | kSL | kSR;
		default: return 0;
		}
	}

	Layout FromDecoder(uint32_t channels, uint32_t mask)
	{
		return { channels, Count(mask) == channels ? mask : DefaultMask(channels) };
	}

	Layout ForWwise(const Layout& source)
	{
		if (source.mChannels > kMaxChannels || source.mMask == 0) {
			return {};
		}
		const uint32_t mask = SurroundsAs(source.mMask, kBL | kBR);
		const uint32_t base = mask & ~kLFE;
		for (uint32_t ok : kWwiseBases) {
			if (base == ok) {
				return { Count(mask), mask };
			}
		}
		return {}; // mono included: duplicated to stereo
	}

	void Convert(const int16_t* in, const Layout& from, size_t frames, const Layout& to, int16_t* out)
	{
		if (from.mChannels == to.mChannels && SurroundsAs(from.mMask, to.mMask) == to.mMask) {
			std::memcpy(out, in, frames * to.BlockAlign());
			return;
		}

		// gain[o][i]: input channel i into output channel o.
		constexpr uint32_t kMaxIn = 32;
		const uint32_t inCount = (std::min)(from.mChannels, kMaxIn);
		float gain[kMaxChannels][kMaxIn] = {};
		const uint32_t fromMask = SurroundsAs(from.mMask, to.mMask);
		auto add = [&](uint32_t bit, uint32_t i, float g) {
			if (to.mMask & bit) gain[IndexOf(to.mMask, bit)][i] += g;
		};
		uint32_t rest = fromMask;
		for (uint32_t i = 0; i < inCount; ++i) {
			const uint32_t bit = rest & (0u - rest); // i-th speaker of the input; 0 past the mask's last
			rest &= rest - 1;
			if (from.mChannels == 1) {
				add(kFL, i, 1.0f);
				add(kFR, i, 1.0f);
			}
			else if (bit && (to.mMask & bit)) {
				add(bit, i, 1.0f);
			}
			else if (bit == kFC) {
				add(kFL, i, kMinus3dB);
				add(kFR, i, kMinus3dB);
			}
			else if (bit == kBC) {
				if ((to.mMask & (kBL | kBR)) == (kBL | kBR)) {
					add(kBL, i, kMinus3dB);
					add(kBR, i, kMinus3dB);
				}
				else {
					add(kFL, i, 0.5f);
					add(kFR, i, 0.5f);
				}
			}
			else if (bit == kFLC) {
				add(kFL, i, 1.0f);
			}
			else if (bit == kFRC) {
				add(kFR, i, 1.0f);
			}
			else if (bit == kBL || bit == kSL) {
				add(kFL, i, kMinus3dB);
			}
			else if (bit == kBR || bit == kSR) {
				add(kFR, i, kMinus3dB);
			}
			else if (bit == 0 && i < 2) {
				add(i == 0 ? kFL : kFR, i, 1.0f); // no mask: the first two are taken as left and right
			}
			// LFE and speakers this doesn't know: dropped
		}

		for (size_t f = 0; f < frames; ++f) {
			const int16_t* s = in + f * from.mChannels;
			int16_t* d = out + f * to.mChannels;
			for (uint32_t o = 0; o < to.mChannels; ++o) {
				float v = 0;
				for (uint32_t i = 0; i < inCount; ++i) {
					v += gain[o][i] * s[i];
				}
				d[o] = Clamp(v);
			}
		}
	}

	bool VorbisOrder(uint32_t channels, uint8_t (&perm)[kMaxChannels], uint32_t& mask)
	{
		// Vorbis I spec, 4.3.9: 3 = L C R; 5 = FL C FR BL BR; 6 = + LFE; 7 = FL C FR SL SR BC LFE;
		// 8 = FL C FR SL SR BL BR LFE.
		static constexpr uint8_t kPerm[9][kMaxChannels] = {
			{},
			{ 0 },
			{ 0, 1 },
			{ 0, 2, 1 },
			{ 0, 1, 2, 3 },
			{ 0, 2, 1, 3, 4 },
			{ 0, 2, 1, 5, 3, 4 },
			{ 0, 2, 1, 6, 5, 3, 4 },
			{ 0, 2, 1, 7, 5, 6, 3, 4 },
		};
		if (channels == 0 || channels > kMaxChannels) {
			return false;
		}
		std::memcpy(perm, kPerm[channels], kMaxChannels);
		mask = DefaultMask(channels);
		return true;
	}

	const char* Name(const Layout& l, char (&buffer)[16])
	{
		switch (l.mMask) {
		case kFC: return "mono";
		case 0x3: return "stereo";
		case 0xB: return "2.1";
		case 0x7: return "3.0";
		case 0xF: return "3.1";
		case 0x33: return "quad";
		case 0x3B: return "4.1";
		case 0x37: return "5.0";
		case 0x3F: return "5.1";
		case 0x607: return "5.0 side";
		case 0x60F: return "5.1 side";
		case 0x70F: return "6.1";
		case 0x633: return "6.0";
		case 0x637: return "7.0";
		case 0x63F: return "7.1";
		default:
			std::snprintf(buffer, sizeof buffer, "%u ch 0x%X", l.mChannels, l.mMask);
			return buffer;
		}
	}
}

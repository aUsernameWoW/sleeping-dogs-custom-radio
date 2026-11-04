// Channel layouts (core/layout.cc): which layouts go to Wwise as they are, the side/back relabel, folding to
// stereo, mono, Vorbis order. Pure; argv[1] (the .asi) is unused.

#include <cstdio>
#include <cstring>

#include "../core/layout.cc"

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

	bool Is(const layout::Layout& l, uint32_t channels, uint32_t mask) { return l.mChannels == channels && l.mMask == mask; }
}

int main()
{
	using namespace layout;

	// Decoders' masks.
	Check(Is(FromDecoder(6, 0), 6, 0x3F), "6 channels without a mask are 5.1");
	Check(Is(FromDecoder(6, 0x60F), 6, 0x60F), "a mask that fits is kept");
	Check(Is(FromDecoder(6, 0x3), 6, 0x3F), "a mask with the wrong count is replaced");
	Check(Is(FromDecoder(12, 0), 12, 0), "no default past 8");

	// What goes to Wwise.
	Check(Is(ForWwise({ 2, kStereo }), 2, kStereo), "stereo");
	Check(Is(ForWwise({ 1, kFC }), 2, kStereo), "mono plays as stereo");
	Check(Is(ForWwise({ 6, 0x3F }), 6, 0x3F), "5.1");
	Check(Is(ForWwise({ 6, 0x60F }), 6, 0x3F), "5.1 side as 5.1");
	Check(Is(ForWwise({ 5, 0x607 }), 5, 0x37), "5.0 side as 5.0");
	Check(Is(ForWwise({ 8, 0x63F }), 8, 0x63F), "7.1");
	Check(Is(ForWwise({ 4, 0x33 }), 4, 0x33), "quad");
	Check(Is(ForWwise({ 3, 0xB }), 3, 0xB), "2.1");
	Check(Is(ForWwise({ 7, 0x70F }), 2, kStereo), "6.1 folds to stereo");
	Check(Is(ForWwise({ 4, 0x107 }), 2, kStereo), "4.0 with a back centre folds to stereo");
	Check(Is(ForWwise({ 12, 0 }), 2, kStereo), "unknown layouts fold to stereo");

	// Same layout and the side/back relabel: a copy.
	{
		const int16_t in[6] = { 1, 2, 3, 4, 5, 6 };
		int16_t out[6] = {};
		Convert(in, { 6, 0x60F }, 1, { 6, 0x3F }, out);
		Check(std::memcmp(in, out, sizeof in) == 0, "5.1 side to 5.1 copies");
	}
	// 5.1 to stereo: centre and surrounds at -3 dB, LFE dropped (Wwise's own downmix).
	{
		const int16_t in[6] = { 1000, 2000, 1000, 30000, 1000, 2000 };
		int16_t out[2] = {};
		Convert(in, { 6, 0x3F }, 1, {}, out);
		Check(out[0] == static_cast<int16_t>(1000 + 2 * 707.10677f) && out[1] == static_cast<int16_t>(2000 + 707.10677f + 1414.2135f),
			"5.1 folds to stereo with -3 dB centre and surrounds, no LFE");
	}
	// 6.1 to stereo: back centre at -6 dB on both sides.
	{
		const int16_t in[7] = { 0, 0, 0, 0, 1000, 0, 0 };
		int16_t out[2] = {};
		Convert(in, { 7, 0x70F }, 1, {}, out);
		Check(out[0] == 500 && out[1] == 500, "6.1's back centre at -6 dB");
	}
	// Mono to stereo: duplicated at 0 dB.
	{
		const int16_t in[2] = { 1234, -5 };
		int16_t out[4] = {};
		Convert(in, { 1, kFC }, 2, {}, out);
		Check(out[0] == 1234 && out[1] == 1234 && out[2] == -5 && out[3] == -5, "mono duplicated");
	}
	// Clipping clamps.
	{
		const int16_t in[6] = { 32000, 0, 32000, 0, 32000, 0 };
		int16_t out[2] = {};
		Convert(in, { 6, 0x3F }, 1, {}, out);
		Check(out[0] == 32767, "clamped");
	}

	// Vorbis 5.1 (FL C FR BL BR LFE) to WAVE order (FL FR C LFE BL BR).
	{
		uint8_t perm[kMaxChannels];
		uint32_t mask = 0;
		Check(VorbisOrder(6, perm, mask) && mask == 0x3F, "Vorbis 6 channels are 5.1");
		const int16_t vorbis[6] = { 10, 30, 20, 50, 60, 40 }; // FL C FR BL BR LFE
		int16_t wave[6];
		for (int k = 0; k < 6; ++k) wave[k] = vorbis[perm[k]];
		const int16_t expected[6] = { 10, 20, 30, 40, 50, 60 };
		Check(std::memcmp(wave, expected, sizeof wave) == 0, "Vorbis 5.1 reordered");
		Check(VorbisOrder(8, perm, mask) && mask == 0x63F && perm[3] == 7 && perm[6] == 3, "Vorbis 7.1");
		Check(!VorbisOrder(9, perm, mask), "no Vorbis order past 8");
	}

	char buffer[16];
	Check(std::strcmp(Name({ 6, 0x60F }, buffer), "5.1 side") == 0, "names");
	Check(std::strcmp(Name({ 3, 0x13 }, buffer), "3 ch 0x13") == 0, "unknown names");

	if (gFailures) {
		return 1;
	}
	std::printf("PASS\n");
	return 0;
}

// The logo image (core/cover_image.cc): hand-made BMPs decoded, fitted into the middle square of the
// 512×256 logo and compressed to a full BC3 mip chain, checked by decoding blocks back; transparent sides;
// a portrait picture pillarboxed; garbage rejected. Compiles cover_image.cc and the stb implementations
// directly (they're in reference\stb, which the test build has no include path for); argv[1] is unused.

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_BMP
#include "../../../reference/stb/stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "../../../reference/stb/stb_image_resize2.h"
#define STB_DXT_IMPLEMENTATION
#include "../../../reference/stb/stb_dxt.h"

#include "../core/cover_image.cc"

#include <cstdio>
#include <cstdlib>
#include <cstring>

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

	using Bytes = std::vector<uint8_t>;

	void Put32(Bytes& b, uint32_t v)
	{
		for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (i * 8)));
	}

	// A 24-bit bottom-up BMP of one color.
	Bytes Bmp(uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b)
	{
		const uint32_t stride = (w * 3 + 3) & ~3u;
		Bytes out{ 'B', 'M' };
		Put32(out, 54 + stride * h);
		Put32(out, 0);
		Put32(out, 54);
		Put32(out, 40);
		Put32(out, w);
		Put32(out, h);
		out.insert(out.end(), { 1, 0, 24, 0 });
		Put32(out, 0);
		Put32(out, stride * h);
		Put32(out, 2835);
		Put32(out, 2835);
		Put32(out, 0);
		Put32(out, 0);
		for (uint32_t y = 0; y < h; ++y) {
			for (uint32_t x = 0; x < w; ++x) out.insert(out.end(), { b, g, r });
			out.resize(out.size() + (stride - w * 3), 0);
		}
		return out;
	}

	struct Rgba
	{
		int r, g, b, a;
	};

	// One pixel of a BC3 level: 8-byte alpha block (2 endpoints, 3-bit indices), then the BC1 color block.
	Rgba Pixel(const coverimage::Image& image, size_t level, uint32_t x, uint32_t y)
	{
		const uint8_t* block = image.mMips[level].data() + (y / 4) * image.RowPitch(level) + (x / 4) * 16;
		const uint32_t i = (y % 4) * 4 + x % 4;

		const int a0 = block[0], a1 = block[1];
		uint64_t alphaBits = 0;
		for (int k = 0; k < 6; ++k) alphaBits |= static_cast<uint64_t>(block[2 + k]) << (8 * k);
		const int ai = static_cast<int>(alphaBits >> (3 * i) & 7);
		int a;
		if (ai == 0) a = a0;
		else if (ai == 1) a = a1;
		else if (a0 > a1) a = ((8 - ai) * a0 + (ai - 1) * a1) / 7;
		else a = ai == 6 ? 0 : ai == 7 ? 255 : ((6 - ai) * a0 + (ai - 1) * a1) / 5;

		const int c0 = block[8] | block[9] << 8, c1 = block[10] | block[11] << 8;
		const uint32_t bits = block[12] | block[13] << 8 | block[14] << 16 | static_cast<uint32_t>(block[15]) << 24;
		const int ci = static_cast<int>(bits >> (2 * i) & 3);
		auto expand = [](int c, Rgba& out) {
			out.r = (c >> 11 & 31) * 255 / 31;
			out.g = (c >> 5 & 63) * 255 / 63;
			out.b = (c & 31) * 255 / 31;
		};
		Rgba e0{}, e1{};
		expand(c0, e0);
		expand(c1, e1);
		Rgba p{};
		const int w0 = ci == 0 ? 3 : ci == 1 ? 0 : ci == 2 ? 2 : 1; // BC3 always uses the 4-color mode
		p.r = (w0 * e0.r + (3 - w0) * e1.r) / 3;
		p.g = (w0 * e0.g + (3 - w0) * e1.g) / 3;
		p.b = (w0 * e0.b + (3 - w0) * e1.b) / 3;
		p.a = a;
		return p;
	}

	bool Near(Rgba p, int r, int g, int b, int a)
	{
		return std::abs(p.r - r) <= 8 && std::abs(p.g - g) <= 8 && std::abs(p.b - b) <= 8 && std::abs(p.a - a) <= 8;
	}
}

int main()
{
	{
		const Bytes bmp = Bmp(300, 300, 255, 0, 0);
		const auto image = coverimage::FromEncoded(bmp.data(), bmp.size());
		Check(image != nullptr, "square BMP decodes");
		if (image) {
			Check(image->mWidth == 512 && image->mHeight == 256 && image->mSourceWidth == 300 && image->mSourceHeight == 300, "sizes");
			Check(image->mMips.size() == 10, "mip chain 512x256 .. 1x1");
			Check(image->mMips[0].size() == 128 * 64 * 16 && image->mMips.back().size() == 16, "level sizes");
			Check(Near(Pixel(*image, 0, 256, 128), 255, 0, 0, 255), "center is the picture");
			Check(Near(Pixel(*image, 0, 128, 0), 255, 0, 0, 255) && Near(Pixel(*image, 0, 383, 255), 255, 0, 0, 255), "square spans x 128..383");
			Check(Pixel(*image, 0, 127, 100).a == 0 && Pixel(*image, 0, 384, 100).a == 0 && Pixel(*image, 0, 0, 0).a == 0, "sides transparent");
			Check(Pixel(*image, 2, 64, 32).a > 200 && Pixel(*image, 2, 0, 32).a == 0, "mips keep the layout");
		}
	}
	{
		const Bytes bmp = Bmp(100, 200, 0, 0, 255);
		const auto image = coverimage::FromEncoded(bmp.data(), bmp.size());
		Check(image != nullptr, "portrait BMP decodes");
		if (image) {
			Check(Near(Pixel(*image, 0, 256, 10), 0, 0, 255, 255) && Near(Pixel(*image, 0, 192, 250), 0, 0, 255, 255), "portrait fills 192..319");
			Check(Pixel(*image, 0, 188, 128).a == 0 && Pixel(*image, 0, 324, 128).a == 0, "portrait pillarboxed");
		}
	}
	{
		const Bytes junk{ 'B', 'M', 1, 2, 3 };
		Check(coverimage::FromEncoded(junk.data(), junk.size()) == nullptr, "garbage rejected");
		Check(coverimage::FromEncoded(nullptr, 0) == nullptr, "empty rejected");
	}
	{
		const auto blank = coverimage::Blank();
		Check(blank == coverimage::Blank(), "one blank image");
		Check(blank->mMips.size() == 10 && Pixel(*blank, 0, 256, 128).a == 0 && Pixel(*blank, 0, 0, 0).a == 0, "blank is transparent");
	}
	{
		const uint8_t jpeg[] = { 0xFF, 0xD8, 0xFF, 0xE0 };
		const uint8_t png[] = { 0x89, 'P', 'N', 'G' };
		Check(std::strcmp(coverimage::FormatName(jpeg, 4), "JPEG") == 0 && std::strcmp(coverimage::FormatName(png, 4), "PNG") == 0, "format names");
	}

	if (gFailures) {
		std::printf("%d failure(s)\n", gFailures);
		return 1;
	}
	std::printf("PASS\n");
	return 0;
}

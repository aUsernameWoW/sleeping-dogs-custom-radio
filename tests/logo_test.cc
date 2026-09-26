// The HUD logo: core/logo_image.cc, core/logo_pack.cc, and the built-in logo embedded in the .asi (argv[1],
// read as a resource without running it). The built-in PNG must come out as a 512×256 BC3 mip chain whose
// alpha matches the PNG's, packed as the game packs its UI textures; pictures without transparency become
// silhouettes of what differs from their border; other aspect ratios are fitted and centered.

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#include "../../../reference/stb/stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "../../../reference/stb/stb_image_resize2.h"
#define STB_DXT_IMPLEMENTATION
#include "../../../reference/stb/stb_dxt.h"

#include "../core/logo_image.cc"
#include "../core/logo_pack.cc"

#include <Windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace logoimage;

static int gFailures = 0;

static void Check(bool ok, const char* what)
{
	std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
	gFailures += ok ? 0 : 1;
}

// The alpha of pixel i (0..15) of a BC3 block.
static int Bc3Alpha(const uint8_t* block, int i)
{
	const int a0 = block[0], a1 = block[1];
	uint64_t bits = 0;
	for (int k = 0; k < 6; ++k) {
		bits |= static_cast<uint64_t>(block[2 + k]) << (8 * k);
	}
	int palette[8] = { a0, a1 };
	if (a0 > a1) {
		for (int k = 1; k < 7; ++k) palette[k + 1] = ((7 - k) * a0 + k * a1) / 7;
	}
	else {
		for (int k = 1; k < 5; ++k) palette[k + 1] = ((5 - k) * a0 + k * a1) / 5;
		palette[6] = 0;
		palette[7] = 255;
	}
	return palette[(bits >> (3 * i)) & 7];
}

static int AlphaAt(const Image& image, size_t level, uint32_t x, uint32_t y)
{
	const uint8_t* block = &image.mMips[level][(y / 4) * image.RowPitch(level) + (x / 4) * 16];
	return Bc3Alpha(block, static_cast<int>((y % 4) * 4 + x % 4));
}

struct Picture
{
	uint32_t mWidth, mHeight;
	std::vector<uint8_t> mRgba;

	Picture(uint32_t w, uint32_t h, uint8_t gray) : mWidth(w), mHeight(h), mRgba(static_cast<size_t>(w) * h * 4, 255)
	{
		for (size_t i = 0; i < mRgba.size(); i += 4) mRgba[i] = mRgba[i + 1] = mRgba[i + 2] = gray;
	}
	void Fill(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint8_t gray, uint8_t alpha = 255)
	{
		for (uint32_t y = y0; y < y1; ++y) {
			for (uint32_t x = x0; x < x1; ++x) {
				uint8_t* p = &mRgba[(static_cast<size_t>(y) * mWidth + x) * 4];
				p[0] = p[1] = p[2] = gray;
				p[3] = alpha;
			}
		}
	}
};

template <typename T>
static T Get(const std::vector<uint8_t>& data, size_t at)
{
	T value;
	std::memcpy(&value, &data[at], sizeof(T));
	return value;
}

// The texture pack, read back the way tools\extract's textures.py reads the game's.
static void Pack(const Image& image)
{
	// Facts from the game's own packs: name UIDs and the temp.bin UIDs their textures point at.
	Check(logopack::HashUpper32("LOGO_HKPDSCANNER") == 0x174F50FA && logopack::HashUpper32("Logo_HKPDScanner") == 0x174F50FA, "name UID");
	const struct { const char* pack; uint32_t uid; } known[] = { { "Radio_HKPDScanner_TexturePack", 0x8626FA83 },
		{ "Icons_Weapon_BATON_TP", 0x1E927749 }, { "Loading_Generic5_TP", 0x02AA5676 }, { "RaceCountdown_TexturePack", 0x8F1A9038 },
		{ "face_charm_texturepack", 0x9D61FBAB } };
	bool uids = true;
	for (const auto& k : known) {
		uids &= logopack::TextureFileUid(std::string("Data\\UI\\") + k.pack + ".temp.bin") == k.uid;
	}
	Check(uids, "temp.bin UIDs of 5 game packs reproduced");

	const char* permPath = "Data\\UI\\..\\..\\plugins\\SDRadio-logo.perm.bin";
	const logopack::Files files = logopack::Build(image, "Logo_SDRadio", permPath);
	const std::vector<uint8_t>& perm = files.mPerm;
	const std::vector<uint8_t>& temp = files.mTemp;
	Check(perm.size() == 480 && Get<uint32_t>(perm, 0) == 0xCDBFA090 && Get<uint32_t>(perm, 4) == 0x1D0 && Get<uint32_t>(perm, 8) == 0x1D0,
		"perm.bin: one 0x1D0-byte texture chunk, like the game's radio logos");
	const size_t t = 16;
	Check(Get<uint32_t>(perm, t + 0x18) == logopack::HashUpper32("LOGO_SDRADIO") && std::strcmp(reinterpret_cast<const char*>(&perm[t + 0x34]), "LOGO_SDRADIO") == 0 &&
			Get<uint32_t>(perm, t + 0x30) == 0x8B43FABF,
		"texture named LOGO_SDRADIO");
	Check(perm[t + 0x5C] == 3 && Get<uint16_t>(perm, t + 0x64) == 512 && Get<uint16_t>(perm, t + 0x66) == 256 && perm[t + 0x68] == 7 &&
			Get<uint16_t>(perm, t + 0x6A) == 1 && Get<uint32_t>(perm, t + 0x6C) == 0xA3833FDE,
		"DXT5 512x256, 7 mips, the UI alpha state");
	uint32_t pixels = 0;
	for (const auto& level : image.mMips) pixels += static_cast<uint32_t>(level.size());
	Check(Get<uint32_t>(perm, t + 0x78) == pixels && Get<uint64_t>(perm, t + 0x80) == 16, "pixel size and position");
	Check(Get<uint32_t>(perm, t + 0xB0) == logopack::TextureFileUid("Data\\UI\\..\\..\\plugins\\SDRadio-logo.temp.bin"), "points at its temp.bin");
	Check(temp.size() == 16 + pixels && Get<uint32_t>(temp, 0) == 0x5E73CDD7 && Get<uint32_t>(temp, 4) == pixels &&
			std::memcmp(&temp[16], image.mMips[0].data(), image.mMips[0].size()) == 0 &&
			std::memcmp(&temp[temp.size() - 32], image.mMips[6].data(), 32) == 0,
		"temp.bin: one chunk, the mips back to back");
}

static void BuiltIn(const char* asi)
{
	HMODULE module = LoadLibraryExA(asi, nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
	HRSRC resource = module ? FindResourceA(module, "LOGO", RT_RCDATA) : nullptr;
	HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
	const auto* png = loaded ? static_cast<const uint8_t*>(LockResource(loaded)) : nullptr;
	Check(png != nullptr, "the .asi has the LOGO resource");
	if (!png) {
		return;
	}
	const int size = static_cast<int>(SizeofResource(module, resource));

	const Result result = FromPng(png, size);
	Check(result.mImage && result.mImage->mWidth == 512 && result.mImage->mHeight == 256, "built-in logo: 512x256 texture");
	Check(result.mSourceWidth == 512 && result.mSourceHeight == 256 && result.mShape == Shape::Alpha, "built-in logo: a 512x256 PNG with transparency");
	if (!result.mImage) {
		return;
	}
	const Image& image = *result.mImage;
	Check(image.mMips.size() == 7 && image.mMips[0].size() == 128 * 64 * 16 && image.mMips[6].size() == 2 * 16, "7 BC3 mip levels down to 8x4");

	int w = 0, h = 0, channels = 0;
	stbi_uc* pixels = stbi_load_from_memory(png, size, &w, &h, &channels, 4);
	long long total = 0, worst = 0, covered = 0;
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const int expected = pixels[(y * w + x) * 4 + 3];
			const long long diff = std::abs(AlphaAt(image, 0, x, y) - expected);
			total += diff;
			worst = diff > worst ? diff : worst;
			covered += expected;
		}
	}
	stbi_image_free(pixels);
	const double mean = static_cast<double>(total) / (w * h);
	std::printf("      alpha error: mean %.2f, worst %lld; coverage %.1f%%\n", mean, worst, 100.0 * covered / (255.0 * w * h));
	Check(mean < 1.5 && worst <= 24, "level 0 alpha matches the PNG (BC3 error only)");
	Check(covered > 255LL * w * h / 10, "the logo isn't blank");
	Check(AlphaAt(image, 0, 0, 0) == 0 && AlphaAt(image, 0, 511, 255) == 0, "corners transparent");
	Pack(image);
}

static void Opaque()
{
	// Black ink on slightly gray paper: the paper must vanish, the ink be (almost) fully opaque.
	Picture drawing(300, 100, 230);
	drawing.Fill(100, 25, 200, 75, 20);
	Result result = FromRgba(drawing.mRgba.data(), drawing.mWidth, drawing.mHeight);
	Check(result.mImage && result.mShape == Shape::DarkOnLight, "opaque drawing on light paper: dark on light");
	if (result.mImage) {
		// Fitted 512x171 at y 42 (scale 1.707): the ink covers x 171..341, y 85..170.
		Check(AlphaAt(*result.mImage, 0, 256, 128) >= 225, "ink opaque");
		Check(AlphaAt(*result.mImage, 0, 40, 128) == 0 && AlphaAt(*result.mImage, 0, 256, 60) == 0, "paper transparent");
		Check(AlphaAt(*result.mImage, 0, 256, 10) == 0 && AlphaAt(*result.mImage, 0, 256, 250) == 0, "outside the fitted picture transparent");
	}

	Picture negative(300, 100, 10);
	negative.Fill(100, 25, 200, 75, 250);
	result = FromRgba(negative.mRgba.data(), negative.mWidth, negative.mHeight);
	Check(result.mImage && result.mShape == Shape::LightOnDark, "opaque light drawing on black: light on dark");
	if (result.mImage) {
		Check(AlphaAt(*result.mImage, 0, 256, 128) >= 240 && AlphaAt(*result.mImage, 0, 40, 128) == 0, "light ink opaque, dark ground transparent");
	}
}

static void Fit()
{
	// A tall picture with a transparent frame fits the height: 128 wide, centered at x 192..320.
	Picture tall(50, 100, 0);
	tall.Fill(0, 0, 50, 100, 0, 0);
	tall.Fill(2, 2, 48, 98, 0, 255);
	const Result result = FromRgba(tall.mRgba.data(), tall.mWidth, tall.mHeight);
	Check(result.mImage && result.mShape == Shape::Alpha, "picture with transparency: its alpha");
	if (result.mImage) {
		Check(AlphaAt(*result.mImage, 0, 256, 128) >= 250, "tall picture: middle opaque");
		Check(AlphaAt(*result.mImage, 0, 150, 128) == 0 && AlphaAt(*result.mImage, 0, 360, 128) == 0, "tall picture: sides transparent");
		Check(AlphaAt(*result.mImage, 0, 256, 20) >= 250, "tall picture: fills the height");
	}

	const uint8_t junk[] = "not a picture at all";
	const Result bad = FromPng(junk, sizeof(junk));
	Check(!bad.mImage && bad.mSourceWidth == 0, "junk doesn't decode");
}

int main(int argc, char** argv)
{
	if (argc < 2) {
		std::printf("usage: logo_test <SDRadio.asi>\n");
		return 2;
	}
	BuiltIn(argv[1]);
	Opaque();
	Fit();
	std::printf(gFailures ? "FAIL (%d)\n" : "PASS\n", gFailures);
	return gFailures ? 1 : 0;
}

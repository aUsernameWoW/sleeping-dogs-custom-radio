#include "logo_image.hh"

#include <algorithm>
#include <climits>
#include <cmath>

// Implementations in third_party.c (stb_image: PNG only, no stdio). logo_test compiles this file without
// include paths and includes stb itself first.
#if __has_include(<stb_image.h>)
#include <stb_dxt.h>
#include <stb_image.h>
#include <stb_image_resize2.h>
#endif

namespace logoimage
{
	namespace
	{
		constexpr int kMaxSide = 16384; // a bigger picture is a mistake; don't decode hundreds of MB for it

		// Rec. 709 weights on the stored (sRGB) values: enough to tell ink from paper.
		int Luma(const uint8_t* p)
		{
			return (54 * p[0] + 183 * p[1] + 19 * p[2] + 128) >> 8;
		}

		// Each pixel's coverage: its alpha if the picture has any transparency, else how far its luminance is
		// from the border's average, scaled so the border itself is 0 and black (or white) paint is 255.
		std::vector<uint8_t> Coverage(const uint8_t* rgba, uint32_t w, uint32_t h, Shape& shape)
		{
			const size_t n = static_cast<size_t>(w) * h;
			std::vector<uint8_t> a(n);
			bool transparent = false;
			for (size_t i = 0; i < n; ++i) {
				a[i] = rgba[i * 4 + 3];
				transparent |= a[i] < 255;
			}
			if (transparent) {
				shape = Shape::Alpha;
				return a;
			}

			uint64_t sum = 0, count = 0;
			auto add = [&](uint32_t x, uint32_t y) {
				sum += Luma(&rgba[(static_cast<size_t>(y) * w + x) * 4]);
				++count;
			};
			for (uint32_t x = 0; x < w; ++x) {
				add(x, 0);
				add(x, h - 1);
			}
			for (uint32_t y = 1; y + 1 < h; ++y) {
				add(0, y);
				add(w - 1, y);
			}
			const int paper = static_cast<int>(sum / count);
			shape = paper >= 128 ? Shape::DarkOnLight : Shape::LightOnDark;
			const int range = std::max(1, shape == Shape::DarkOnLight ? paper : 255 - paper);
			for (size_t i = 0; i < n; ++i) {
				const int luma = Luma(&rgba[i * 4]);
				const int ink = shape == Shape::DarkOnLight ? paper - luma : luma - paper;
				a[i] = static_cast<uint8_t>(std::clamp(ink * 255 / range, 0, 255));
			}
			return a;
		}

		// Scaled to fit (never cropped) and centered on a transparent kWidth×kHeight canvas; empty on failure.
		std::vector<uint8_t> Fit(const std::vector<uint8_t>& a, uint32_t w, uint32_t h)
		{
			const double scale = std::min(static_cast<double>(kWidth) / w, static_cast<double>(kHeight) / h);
			const int fw = std::clamp(static_cast<int>(std::lround(w * scale)), 1, static_cast<int>(kWidth));
			const int fh = std::clamp(static_cast<int>(std::lround(h * scale)), 1, static_cast<int>(kHeight));
			std::vector<uint8_t> fitted(static_cast<size_t>(fw) * fh);
			if (!stbir_resize_uint8_linear(a.data(), static_cast<int>(w), static_cast<int>(h), 0, fitted.data(), fw, fh, 0, STBIR_1CHANNEL)) {
				return {};
			}
			std::vector<uint8_t> canvas(static_cast<size_t>(kWidth) * kHeight, 0);
			const uint32_t left = (kWidth - fw) / 2;
			const uint32_t top = (kHeight - fh) / 2;
			for (int y = 0; y < fh; ++y) {
				std::copy_n(&fitted[static_cast<size_t>(y) * fw], fw, &canvas[(top + y) * static_cast<size_t>(kWidth) + left]);
			}
			return canvas;
		}

		// One level down: 2×2 average (a 1-pixel side stays 1).
		std::vector<uint8_t> HalfSize(const std::vector<uint8_t>& src, uint32_t w, uint32_t h, uint32_t& outW, uint32_t& outH)
		{
			outW = std::max(1u, w / 2);
			outH = std::max(1u, h / 2);
			std::vector<uint8_t> dst(static_cast<size_t>(outW) * outH);
			for (uint32_t y = 0; y < outH; ++y) {
				const size_t y0 = std::min(y * 2, h - 1) * static_cast<size_t>(w), y1 = std::min(y * 2 + 1, h - 1) * static_cast<size_t>(w);
				for (uint32_t x = 0; x < outW; ++x) {
					const uint32_t x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
					dst[static_cast<size_t>(y) * outW + x] = static_cast<uint8_t>((src[y0 + x0] + src[y0 + x1] + src[y1 + x0] + src[y1 + x1] + 2) / 4);
				}
			}
			return dst;
		}

		// Black with the given alpha, as the game's logos are.
		std::vector<uint8_t> ToBc3(const std::vector<uint8_t>& a, uint32_t w, uint32_t h)
		{
			const uint32_t bw = std::max(1u, (w + 3) / 4);
			const uint32_t bh = std::max(1u, (h + 3) / 4);
			std::vector<uint8_t> out(static_cast<size_t>(bw) * bh * 16);
			uint8_t block[64] = {};
			for (uint32_t by = 0; by < bh; ++by) {
				for (uint32_t bx = 0; bx < bw; ++bx) {
					// Levels smaller than a block repeat their edge pixels.
					for (uint32_t i = 0; i < 16; ++i) {
						const uint32_t x = std::min(bx * 4 + i % 4, w - 1);
						const uint32_t y = std::min(by * 4 + i / 4, h - 1);
						block[i * 4 + 3] = a[static_cast<size_t>(y) * w + x];
					}
					stb_compress_dxt_block(&out[(static_cast<size_t>(by) * bw + bx) * 16], block, 1, STB_DXT_HIGHQUAL);
				}
			}
			return out;
		}

		Image Compress(std::vector<uint8_t> a, uint32_t width, uint32_t height)
		{
			Image image;
			image.mWidth = width;
			image.mHeight = height;
			uint32_t w = width, h = height;
			// Down to a side of 4 (one block), as the game's own mipmapped textures go.
			for (;;) {
				image.mMips.push_back(ToBc3(a, w, h));
				if (w <= 4 || h <= 4) {
					break;
				}
				a = HalfSize(a, w, h, w, h);
			}
			return image;
		}
	}

	uint32_t Image::RowPitch(size_t level) const
	{
		const uint32_t w = std::max(1u, mWidth >> level);
		return std::max(1u, (w + 3) / 4) * 16;
	}

	Result FromRgba(const uint8_t* rgba, uint32_t width, uint32_t height)
	{
		Result result;
		result.mSourceWidth = width;
		result.mSourceHeight = height;
		if (!rgba || width == 0 || height == 0) {
			return result;
		}
		std::vector<uint8_t> canvas = Fit(Coverage(rgba, width, height, result.mShape), width, height);
		if (!canvas.empty()) {
			result.mImage = std::make_shared<Image>(Compress(std::move(canvas), kWidth, kHeight));
		}
		return result;
	}

	Result FromPng(const uint8_t* data, size_t size)
	{
		Result result;
		int w = 0, h = 0, channels = 0;
		if (!data || size == 0 || size > INT_MAX || !stbi_info_from_memory(data, static_cast<int>(size), &w, &h, &channels)) {
			return result;
		}
		result.mSourceWidth = static_cast<uint32_t>(w);
		result.mSourceHeight = static_cast<uint32_t>(h);
		if (w > kMaxSide || h > kMaxSide) {
			return result;
		}
		stbi_uc* pixels = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &channels, 4);
		if (!pixels) {
			return result;
		}
		result = FromRgba(pixels, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
		stbi_image_free(pixels);
		return result;
	}

	const char* ShapeName(Shape shape)
	{
		switch (shape) {
		case Shape::Alpha: return "its transparency";
		case Shape::DarkOnLight: return "dark on light (no transparency)";
		case Shape::LightOnDark: return "light on dark (no transparency)";
		}
		return "?";
	}
}

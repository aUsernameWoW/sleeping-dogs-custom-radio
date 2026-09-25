#include "cover_image.hh"

#include <algorithm>
#include <cmath>

// Implementations in third_party.c (stb_image: JPEG/PNG/BMP/GIF only, no stdio). cover_test compiles this
// file without include paths and includes stb itself first.
#if __has_include(<stb_image.h>)
#include <stb_dxt.h>
#include <stb_image.h>
#include <stb_image_resize2.h>
#endif

namespace coverimage
{
	namespace
	{
		// One level down: 2×2 average (a 1-pixel side stays 1). Plain averaging of R,G,B next to A = 0 is fine
		// here: transparent pixels are black and only border the opaque square.
		std::vector<uint8_t> HalfSize(const std::vector<uint8_t>& src, uint32_t w, uint32_t h, uint32_t& outW, uint32_t& outH)
		{
			outW = std::max(1u, w / 2);
			outH = std::max(1u, h / 2);
			std::vector<uint8_t> dst(static_cast<size_t>(outW) * outH * 4);
			for (uint32_t y = 0; y < outH; ++y) {
				for (uint32_t x = 0; x < outW; ++x) {
					const uint32_t x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
					const uint32_t y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
					for (uint32_t c = 0; c < 4; ++c) {
						const uint32_t sum = src[(static_cast<size_t>(y0) * w + x0) * 4 + c] + src[(static_cast<size_t>(y0) * w + x1) * 4 + c] +
							src[(static_cast<size_t>(y1) * w + x0) * 4 + c] + src[(static_cast<size_t>(y1) * w + x1) * 4 + c];
						dst[(static_cast<size_t>(y) * outW + x) * 4 + c] = static_cast<uint8_t>((sum + 2) / 4);
					}
				}
			}
			return dst;
		}

		std::vector<uint8_t> ToBc3(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h)
		{
			const uint32_t bw = std::max(1u, (w + 3) / 4);
			const uint32_t bh = std::max(1u, (h + 3) / 4);
			std::vector<uint8_t> out(static_cast<size_t>(bw) * bh * 16);
			uint8_t block[64];
			for (uint32_t by = 0; by < bh; ++by) {
				for (uint32_t bx = 0; bx < bw; ++bx) {
					// Levels smaller than a block repeat their edge pixels.
					for (uint32_t i = 0; i < 16; ++i) {
						const uint32_t x = std::min(bx * 4 + i % 4, w - 1);
						const uint32_t y = std::min(by * 4 + i / 4, h - 1);
						std::copy_n(&rgba[(static_cast<size_t>(y) * w + x) * 4], 4, &block[i * 4]);
					}
					stb_compress_dxt_block(&out[(static_cast<size_t>(by) * bw + bx) * 16], block, 1, STB_DXT_HIGHQUAL);
				}
			}
			return out;
		}
	}

	uint32_t Image::RowPitch(size_t level) const
	{
		const uint32_t w = std::max(1u, mWidth >> level);
		return std::max(1u, (w + 3) / 4) * 16;
	}

	Image Compress(std::vector<uint8_t> rgba, uint32_t width, uint32_t height)
	{
		Image image;
		image.mWidth = width;
		image.mHeight = height;
		uint32_t w = width, h = height;
		for (;;) {
			image.mMips.push_back(ToBc3(rgba, w, h));
			if (w == 1 && h == 1) {
				break;
			}
			rgba = HalfSize(rgba, w, h, w, h);
		}
		return image;
	}

	std::shared_ptr<const Image> FromEncoded(const uint8_t* data, size_t size)
	{
		if (!data || size == 0 || size > 0x7FFFFFFF) {
			return nullptr;
		}
		int w = 0, h = 0, channels = 0;
		stbi_uc* pixels = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &channels, 4);
		if (!pixels) {
			return nullptr;
		}

		// Fit (never crop) into the square in the middle; covers are nearly always square anyway.
		const double scale = std::min(static_cast<double>(kHeight) / w, static_cast<double>(kHeight) / h);
		const int fw = std::clamp(static_cast<int>(std::lround(w * scale)), 1, static_cast<int>(kHeight));
		const int fh = std::clamp(static_cast<int>(std::lround(h * scale)), 1, static_cast<int>(kHeight));
		std::vector<uint8_t> fitted(static_cast<size_t>(fw) * fh * 4);
		const bool resized = stbir_resize_uint8_srgb(pixels, w, h, 0, fitted.data(), fw, fh, 0, STBIR_RGBA) != nullptr;
		stbi_image_free(pixels);
		if (!resized) {
			return nullptr;
		}

		std::vector<uint8_t> canvas(static_cast<size_t>(kWidth) * kHeight * 4, 0);
		const uint32_t left = (kWidth - fw) / 2;
		const uint32_t top = (kHeight - fh) / 2;
		for (int y = 0; y < fh; ++y) {
			std::copy_n(&fitted[static_cast<size_t>(y) * fw * 4], static_cast<size_t>(fw) * 4, &canvas[((top + y) * static_cast<size_t>(kWidth) + left) * 4]);
		}

		auto image = std::make_shared<Image>(Compress(std::move(canvas), kWidth, kHeight));
		image->mSourceWidth = static_cast<uint32_t>(w);
		image->mSourceHeight = static_cast<uint32_t>(h);
		return image;
	}

	std::shared_ptr<const Image> Blank()
	{
		static const std::shared_ptr<const Image> blank =
			std::make_shared<Image>(Compress(std::vector<uint8_t>(static_cast<size_t>(kWidth) * kHeight * 4, 0), kWidth, kHeight));
		return blank;
	}

	const char* FormatName(const uint8_t* data, size_t size)
	{
		if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) return "JPEG";
		if (size >= 4 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') return "PNG";
		if (size >= 2 && data[0] == 'B' && data[1] == 'M') return "BMP";
		if (size >= 3 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F') return "GIF";
		if (size >= 12 && std::equal(data + 8, data + 12, "WEBP")) return "WebP";
		return "unknown format";
	}
}

#pragma once

// The station logo made from a picture (a track's cover art, or an image file). RadioStations.swf loads the
// logo into its slot and then forces the slot to 128×64 (onLoadInit sets _width/_height), whatever size the
// texture has, and Scaleform takes that size from the D3D texture itself. So the logo can be sharper than
// the game's own 128×64 ones: 512×256 here (the slot is ~3× larger on a 4K screen), the picture fitted into
// the 256×256 square in the middle, transparent at the sides where the widget's backing plate shows through.
// BC3 (DXT5) like the game's logos, with a full mip chain for small screens.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace coverimage
{
	constexpr uint32_t kWidth = 512;
	constexpr uint32_t kHeight = 256;

	struct Image
	{
		uint32_t mWidth = 0;
		uint32_t mHeight = 0;
		std::vector<std::vector<uint8_t>> mMips; // BC3 blocks of each level, level 0 first
		uint32_t mSourceWidth = 0;               // the picture as decoded, for the log
		uint32_t mSourceHeight = 0;

		// Bytes per row of 4×4 blocks at the given level.
		uint32_t RowPitch(size_t level) const;
	};

	// Decodes a JPEG, PNG, BMP or GIF and lays it out as the logo; nullptr if it doesn't decode.
	std::shared_ptr<const Image> FromEncoded(const uint8_t* data, size_t size);

	// Fully transparent: no art, only the widget's backing plate shows.
	std::shared_ptr<const Image> Blank();

	// R,G,B,A pixels of `width`×`height` to a BC3 mip chain down to 1×1.
	Image Compress(std::vector<uint8_t> rgba, uint32_t width, uint32_t height);

	// "JPEG", "PNG"... from the leading bytes, for the log.
	const char* FormatName(const uint8_t* data, size_t size);
}

#pragma once

// The station logo as the HUD draws it. RadioStations.swf places the logo holder with a color transform
// that multiplies R,G,B by 0 and adds 255, so every station logo shows as a white silhouette of its alpha
// channel; the game's own are black on transparent. A logo is therefore only its alpha: from a picture with
// transparency, its alpha; from an opaque one, how much darker (or lighter) each pixel is than the
// picture's border, so a drawing on white paper works too.
//
// The widget's onLoadInit forces the holder to 128×64 whatever the texture's size, and Scaleform takes the
// size from the D3D texture itself, so the logo can be sharper than the game's 128×64 ones: 512×256 here
// (the slot is ~3× larger on a 4K screen), the picture fitted in without cropping, transparent around it.
// BC3 (DXT5) like the game's logos, with mips down to 8×4 for small screens.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace logoimage
{
	constexpr uint32_t kWidth = 512;
	constexpr uint32_t kHeight = 256;

	struct Image
	{
		uint32_t mWidth = 0;
		uint32_t mHeight = 0;
		std::vector<std::vector<uint8_t>> mMips; // BC3 blocks of each level, level 0 first

		// Bytes per row of 4×4 blocks at the given level.
		uint32_t RowPitch(size_t level) const;
	};

	// Where the silhouette came from, for the log.
	enum class Shape { Alpha, DarkOnLight, LightOnDark };

	struct Result
	{
		std::shared_ptr<const Image> mImage; // null if the picture didn't decode
		uint32_t mSourceWidth = 0;
		uint32_t mSourceHeight = 0;
		Shape mShape = Shape::Alpha;
	};

	// Decodes a PNG and makes the logo from it.
	Result FromPng(const uint8_t* data, size_t size);

	// The logo from R,G,B,A pixels.
	Result FromRgba(const uint8_t* rgba, uint32_t width, uint32_t height);

	const char* ShapeName(Shape shape);
}

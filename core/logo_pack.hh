#pragma once

// A UI texture pack of our own holding the station logo, which the game loads like its own radio logos.
// The radio widget streams Data\UI\<TexturePack>.perm.bin (UFG::DataStreamer::OpenFiles, which also opens
// the matching .temp.bin), and UFG::StreamFileWrapper::Open reads a path that no .big archive has from disk
// (UFG::qOpen → CreateFileA, relative to the game folder). So Radios.xml can name a pack such as
// ..\..\plugins\SDRadio-logo, a pair of files the mod writes.
//
// perm.bin: one 0xCDBFA090 chunk, an Illusion::Texture resource (0x1D0 bytes) laid out like the game's
// radio logos (TheoryEngine illusion/texture.hh); temp.bin: one 0x5E73CDD7 chunk, the pixels. The texture
// finds its pixels by mTextureDataHandle's UID (+0xB0), which must be UFG::GenerateResourceFileUID(Texture,
// <the temp.bin path the game derives>), the UID DataStreamer::LoadStreamResources registers the loaded
// temp.bin under. Mip levels are packed tightly, level 0 first, as in the game's mipmapped textures.

#include <cstdint>
#include <string_view>
#include <vector>

#include "logo_image.hh"

namespace logopack
{
	// UFG::qStringHashUpper32: MSB-first CRC-32 (poly 0x04C11DB7) of the ASCII-uppercased bytes, no final xor.
	uint32_t HashUpper32(std::string_view text, uint32_t seed = 0xFFFFFFFF);

	// UFG::GenerateResourceFileUID(ResourceFileContentType_Texture, path).
	uint32_t TextureFileUid(std::string_view path);

	struct Files
	{
		std::vector<uint8_t> mPerm;
		std::vector<uint8_t> mTemp;
	};

	// The pack with one BC3 texture `name` holding `image`, for the game to load from `permPath` (the path
	// it builds, Data\UI\<TexturePack>.perm.bin).
	Files Build(const logoimage::Image& image, std::string_view name, std::string_view permPath);
}

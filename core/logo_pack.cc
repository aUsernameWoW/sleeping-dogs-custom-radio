#include "logo_pack.hh"

#include <algorithm>
#include <cstring>
#include <string>

namespace logopack
{
	namespace
	{
		constexpr uint32_t kTextureChunk = 0xCDBFA090;
		constexpr uint32_t kTextureDataChunk = 0x5E73CDD7;
		constexpr uint32_t kTextureTypeUid = 0x8B43FABF;   // RTypeUID_Texture
		constexpr uint32_t kUiAlphaStateUid = 0xA3833FDE;  // every DXT5 UI texture's, the radio logos' included
		constexpr uint32_t kTextureSize = 0x1D0;           // Illusion::Texture + TexturePlat, as in the file
		constexpr uint8_t kFormatDxt5 = 3;

		// Offsets in the Illusion::Texture resource.
		constexpr size_t kNameUid = 0x18;       // qResourceData::mNode.mUID
		constexpr size_t kTypeUid = 0x30;       // qResourceData::mTypeUID
		constexpr size_t kDebugName = 0x34;     // qResourceData::mDebugName[36]
		constexpr size_t kFormat = 0x5C;
		constexpr size_t kWidth = 0x64;
		constexpr size_t kHeight = 0x66;
		constexpr size_t kNumMipMaps = 0x68;
		constexpr size_t kDepth = 0x6A;
		constexpr size_t kAlphaStateUid = 0x6C;
		constexpr size_t kTextureUser = 0x70;   // qOffset64 to the TextureUser, 0x110 in every texture
		constexpr size_t kImageDataByteSize = 0x78;
		constexpr size_t kImageDataPosition = 0x80; // offset of the pixels in temp.bin, after its chunk header
		constexpr size_t kTextureDataUid = 0xB0;    // mTextureDataHandle's name UID
		constexpr size_t kTextureUserPlat = 0xD0;   // TexturePlat::mTextureUserPlat, 0xF0 in every texture
		constexpr size_t kUserPlat = 0x1C0;         // what it points at: the same two words in every texture

		struct CrcTable
		{
			uint32_t mEntries[256];
			CrcTable()
			{
				for (uint32_t i = 0; i < 256; ++i) {
					uint32_t c = i << 24;
					for (int k = 0; k < 8; ++k) {
						c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
					}
					mEntries[i] = c;
				}
			}
		};

		template <typename T>
		void Put(std::vector<uint8_t>& out, size_t at, T value)
		{
			std::memcpy(&out[at], &value, sizeof(T));
		}

		void PutChunkHeader(std::vector<uint8_t>& out, uint32_t uid, uint32_t size)
		{
			Put<uint32_t>(out, 0, uid);
			Put<uint32_t>(out, 4, size); // chunk size
			Put<uint32_t>(out, 8, size); // data size
			Put<uint32_t>(out, 12, 0);   // data offset
		}
	}

	uint32_t HashUpper32(std::string_view text, uint32_t seed)
	{
		static const CrcTable table;
		uint32_t h = seed;
		for (char c : text) {
			const uint8_t b = static_cast<uint8_t>(c >= 'a' && c <= 'z' ? c - 32 : c);
			h = (h << 8) ^ table.mEntries[((h >> 24) ^ b) & 0xFF];
		}
		return h;
	}

	uint32_t TextureFileUid(std::string_view path)
	{
		// The game hashes from the last "data\" (else "data/"), slashes dropped, after the content prefix.
		std::string lower(path);
		std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; });
		size_t at = lower.rfind("data\\");
		if (at == std::string::npos) {
			at = lower.rfind("data/");
		}
		std::string tail;
		for (char c : path.substr(at == std::string::npos ? 0 : at)) {
			if (c != '/' && c != '\\') {
				tail += c;
			}
		}
		return HashUpper32(tail, HashUpper32("Illusion:Texture:"));
	}

	Files Build(const logoimage::Image& image, std::string_view name, std::string_view permPath)
	{
		uint32_t pixels = 0;
		for (const std::vector<uint8_t>& level : image.mMips) {
			pixels += static_cast<uint32_t>(level.size());
		}

		Files files;
		files.mTemp.assign(16 + pixels, 0);
		PutChunkHeader(files.mTemp, kTextureDataChunk, pixels);
		size_t at = 16;
		for (const std::vector<uint8_t>& level : image.mMips) {
			std::memcpy(&files.mTemp[at], level.data(), level.size());
			at += level.size();
		}

		std::string tempPath(permPath);
		if (const size_t perm = tempPath.rfind("perm.bin"); perm != std::string::npos) {
			tempPath.replace(perm, 8, "temp.bin");
		}

		files.mPerm.assign(16 + kTextureSize, 0);
		PutChunkHeader(files.mPerm, kTextureChunk, kTextureSize);
		const size_t t = 16;
		Put<uint32_t>(files.mPerm, t + kNameUid, HashUpper32(name));
		Put<uint32_t>(files.mPerm, t + kTypeUid, kTextureTypeUid);
		for (size_t i = 0; i < name.size() && i < 35; ++i) {
			const char c = name[i];
			files.mPerm[t + kDebugName + i] = static_cast<uint8_t>(c >= 'a' && c <= 'z' ? c - 32 : c);
		}
		files.mPerm[t + kFormat] = kFormatDxt5;
		Put<uint16_t>(files.mPerm, t + kWidth, static_cast<uint16_t>(image.mWidth));
		Put<uint16_t>(files.mPerm, t + kHeight, static_cast<uint16_t>(image.mHeight));
		files.mPerm[t + kNumMipMaps] = static_cast<uint8_t>(image.mMips.size());
		Put<uint16_t>(files.mPerm, t + kDepth, 1);
		Put<uint32_t>(files.mPerm, t + kAlphaStateUid, kUiAlphaStateUid);
		Put<uint64_t>(files.mPerm, t + kTextureUser, 0x110);
		Put<uint32_t>(files.mPerm, t + kImageDataByteSize, pixels);
		Put<uint64_t>(files.mPerm, t + kImageDataPosition, 16);
		Put<uint32_t>(files.mPerm, t + kTextureDataUid, TextureFileUid(tempPath));
		Put<uint64_t>(files.mPerm, t + kTextureUserPlat, 0xF0);
		Put<uint32_t>(files.mPerm, t + kUserPlat, 0x42);
		Put<uint32_t>(files.mPerm, t + kUserPlat + 4, 0x41300000);
		return files;
	}
}

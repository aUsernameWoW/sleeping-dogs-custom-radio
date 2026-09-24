#pragma once

// The few Wwise 2012.2 low-level I/O types the hooks touch, as laid out in the legacy PDB
// (reference\SDmodding\game-itself). Wwise is statically linked into the game; there is no SDK header.

#include <cstdint>

namespace ak
{
	using AKRESULT = int32_t;
	constexpr AKRESULT kSuccess = 1;
	constexpr AKRESULT kFail = 2;

	struct FileSystemFlags
	{
		uint32_t mCompanyID;
		uint32_t mCodecID;        // 0 = sound bank, else the media's codec (1 = PCM)
		uint32_t mCustomParamSize;
		void* mCustomParam;
		bool mIsLanguageSpecific;
		bool mIsFromRSX;
		bool mIsAutomaticStream;
		uint32_t mCacheID;
	};
	static_assert(sizeof(FileSystemFlags) == 0x20);

	struct FileDesc
	{
		int64_t mFileSize;
		uint32_t mSector;
		// The game's package I/O stores the package's block size here: GetBlockSize returns it, and Close
		// treats any non-zero value as "a file inside a .pck" (hFile is then its StreamFileWrapper handle).
		uint32_t mCustomParamSize;
		void* mCustomParam;
		void* mFile;
		uint32_t mDeviceID;
	};
	static_assert(sizeof(FileDesc) == 0x28);

	struct AsyncIOTransferInfo;
	using IOCallback = void(__fastcall*)(AsyncIOTransferInfo* info, AKRESULT result);

	struct AsyncIOTransferInfo
	{
		uint64_t mFilePosition;
		uint32_t mBufferSize;
		uint32_t mRequestedSize;
		void* mBuffer;
		IOCallback mCallback; // must be called exactly once per Read that returned kSuccess
		void* mCookie;
		void* mUserData;
	};
	static_assert(sizeof(AsyncIOTransferInfo) == 0x30);
}

#include "logo.hh"

#include <cerrno>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <memory>
#include <thread>
#include <vector>

#include "log.hh"
#include "logo_image.hh"
#include "logo_pack.hh"

namespace logo
{
	namespace
	{
		HANDLE gDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		std::string gTexturePack; // set before gDone
		std::wstring gAsiDir;     // set before gDone, with gPackPrefix
		std::string gPackPrefix;  // the .asi folder as a TexturePack prefix: ..\..\<relative>\ (empty: no packs possible)

		bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& data)
		{
			FILE* file = nullptr;
			if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) {
				return false;
			}
			uint8_t buffer[65536];
			size_t n;
			while ((n = fread(buffer, 1, sizeof(buffer), file)) > 0 && data.size() < (64u << 20)) {
				data.insert(data.end(), buffer, buffer + n);
			}
			fclose(file);
			return true;
		}

		// 0 or the errno of the failure.
		int WriteIfChanged(const std::wstring& path, const std::vector<uint8_t>& data, bool& written)
		{
			written = false;
			if (std::vector<uint8_t> old; ReadWholeFile(path, old) && old == data) {
				return 0;
			}
			FILE* file = nullptr;
			if (const errno_t error = _wfopen_s(&file, path.c_str(), L"wb"); error != 0 || !file) {
				return error ? error : EIO;
			}
			const bool ok = fwrite(data.data(), 1, data.size(), file) == data.size();
			const bool closed = fclose(file) == 0;
			written = true;
			return ok && closed ? 0 : EIO;
		}

		logoimage::Result DecodeBuiltIn();

		// Decoded once: every station without a logo.png of its own shows it.
		const logoimage::Result& BuiltIn()
		{
			static const logoimage::Result result = DecodeBuiltIn();
			return result;
		}

		logoimage::Result DecodeBuiltIn()
		{
			HMODULE self = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&DecodeBuiltIn),
				&self);
			HRSRC resource = FindResourceW(self, L"LOGO", RT_RCDATA);
			HGLOBAL loaded = resource ? LoadResource(self, resource) : nullptr;
			const void* data = loaded ? LockResource(loaded) : nullptr;
			return data ? logoimage::FromPng(static_cast<const uint8_t*>(data), SizeofResource(self, resource)) : logoimage::Result{};
		}

		// logo.png from the music folder, else the built-in picture.
		std::shared_ptr<const logoimage::Image> Picture(const std::wstring& folder)
		{
			const std::wstring path = folder + L"\\logo.png";
			const std::string name = logger::ToUtf8(path.c_str());
			std::vector<uint8_t> file;
			if (!ReadWholeFile(path, file)) {
				LOG("logo: no %s, using the built-in logo", name.c_str());
			}
			else if (const logoimage::Result own = logoimage::FromPng(file.data(), file.size()); own.mImage) {
				LOG("logo: %s, %ux%u, silhouette from %s", name.c_str(), own.mSourceWidth, own.mSourceHeight, logoimage::ShapeName(own.mShape));
				return own.mImage;
			}
			else if (own.mSourceWidth) {
				LOG("logo: %s is %ux%u, too large to use; using the built-in logo", name.c_str(), own.mSourceWidth, own.mSourceHeight);
			}
			else {
				LOG("logo: %s doesn't decode as a PNG; using the built-in logo", name.c_str());
			}
			const logoimage::Result& builtIn = BuiltIn();
			if (!builtIn.mImage) {
				LOG("logo: the built-in logo is missing from the .asi");
			}
			return builtIn.mImage;
		}

		std::wstring GameFolder()
		{
			wchar_t path[MAX_PATH] = {};
			const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
			std::wstring dir(path, length < MAX_PATH ? length : 0);
			const size_t slash = dir.find_last_of(L"\\/");
			return slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash);
		}

		// `dir` relative to the game folder, in the ASCII the game's paths are made of (they reach CreateFileA
		// through Radios.xml); false if it's outside the game folder or has other characters.
		bool RelativeToGame(const std::wstring& dir, const std::wstring& game, std::string& out)
		{
			if (game.empty() || dir.size() < game.size() || _wcsnicmp(dir.c_str(), game.c_str(), game.size()) != 0 ||
				(dir.size() > game.size() && dir[game.size()] != L'\\' && dir[game.size()] != L'/')) {
				return false;
			}
			for (size_t i = game.size(); i < dir.size(); ++i) {
				const wchar_t c = dir[i];
				if (c < 0x20 || c > 0x7E) {
					return false;
				}
				if ((c == L'\\' || c == L'/') && (out.empty() || i + 1 == dir.size())) {
					continue; // no leading or trailing separator
				}
				out += c == L'/' ? '\\' : static_cast<char>(c);
			}
			return true;
		}

		// Writes the pack <asiDir>\<file>.perm.bin / .temp.bin holding `image` as texture `texture`; the
		// TexturePack for Radios.xml, or empty after a write error (logged).
		std::string WritePack(const logoimage::Image& image, const std::string& file, const char* texture, DWORD start)
		{
			// The widget loads Data\UI\<TexturePack>.perm.bin; two levels up is the game folder.
			const std::string pack = gPackPrefix + file;
			const logopack::Files files = logopack::Build(image, texture, "Data\\UI\\" + pack + ".perm.bin");
			const std::wstring base = gAsiDir + L"\\" + std::wstring(file.begin(), file.end());
			bool tempWritten = false, permWritten = false;
			int error = WriteIfChanged(base + L".temp.bin", files.mTemp, tempWritten);
			if (!error) {
				error = WriteIfChanged(base + L".perm.bin", files.mPerm, permWritten);
			}
			if (error) {
				LOG("logo: writing %s.perm.bin / .temp.bin failed (errno %d); the station borrows a game logo", logger::ToUtf8(base.c_str()).c_str(),
					error);
				return {};
			}
			LOG("logo: texture pack %s (%ux%u, %zu mips) %s in %lu ms", pack.c_str(), image.mWidth, image.mHeight, image.mMips.size(),
				tempWritten || permWritten ? "written" : "unchanged", GetTickCount() - start);
			return pack;
		}

		void Make(std::wstring asiDir, std::wstring musicFolder)
		{
			const DWORD start = GetTickCount();
			const std::wstring game = GameFolder();
			std::string relative;
			if (!RelativeToGame(asiDir, game, relative)) {
				LOG("logo: %s isn't an ASCII path inside the game folder %s, so the game can't load a texture pack from there; the station "
					"borrows a game logo", logger::ToUtf8(asiDir.c_str()).c_str(), logger::ToUtf8(game.c_str()).c_str());
				SetEvent(gDone);
				return;
			}
			wchar_t cwd[MAX_PATH] = {};
			if (GetCurrentDirectoryW(MAX_PATH, cwd) && _wcsicmp(cwd, game.c_str()) != 0) {
				LOG("logo: the working folder %s isn't the game folder; the game may look for the pack in the wrong place",
					logger::ToUtf8(cwd).c_str());
			}

			gAsiDir = asiDir;
			gPackPrefix = "..\\..\\" + (relative.empty() ? std::string() : relative + "\\");
			if (const std::shared_ptr<const logoimage::Image> image = Picture(musicFolder)) {
				gTexturePack = WritePack(*image, "SDRadio-logo", kTextureName, start);
			}
			SetEvent(gDone);
		}

		// SDRadio-logo-<suffix>.perm.bin / .temp.bin: true and the suffix if `file` is one.
		bool StationPackFile(const std::wstring& file, std::wstring& suffix)
		{
			static constexpr wchar_t kPrefix[] = L"SDRadio-logo-";
			constexpr size_t prefix = std::size(kPrefix) - 1;
			for (const wchar_t* ext : { L".perm.bin", L".temp.bin" }) {
				const size_t length = wcslen(ext);
				if (file.size() > prefix + length && _wcsnicmp(file.c_str(), kPrefix, prefix) == 0 &&
					_wcsicmp(file.c_str() + file.size() - length, ext) == 0) {
					suffix = file.substr(prefix, file.size() - prefix - length);
					return true;
				}
			}
			return false;
		}
	}

	std::string StationPack(const std::wstring& folder, const std::string& suffix, std::string& textureName)
	{
		if (WaitForSingleObject(gDone, 0) != WAIT_OBJECT_0 || gPackPrefix.empty()) {
			return {};
		}
		const DWORD start = GetTickCount();
		const std::shared_ptr<const logoimage::Image> image = Picture(folder);
		if (!image) {
			return {};
		}
		const std::string texture = std::string(kTextureName) + "_" + suffix;
		std::string pack = WritePack(*image, "SDRadio-logo-" + suffix, texture.c_str(), start);
		if (!pack.empty()) {
			textureName = texture;
		}
		return pack;
	}

	void RemoveOtherStationPacks(const std::vector<std::string>& keep)
	{
		if (gAsiDir.empty()) {
			return;
		}
		WIN32_FIND_DATAW data;
		HANDLE find = FindFirstFileW((gAsiDir + L"\\SDRadio-logo-*").c_str(), &data);
		if (find == INVALID_HANDLE_VALUE) {
			return;
		}
		do {
			std::wstring suffix;
			if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !StationPackFile(data.cFileName, suffix)) {
				continue;
			}
			bool kept = false;
			for (const std::string& k : keep) {
				kept |= _wcsicmp(suffix.c_str(), std::wstring(k.begin(), k.end()).c_str()) == 0;
			}
			if (!kept) {
				const std::wstring path = gAsiDir + L"\\" + data.cFileName;
				LOG("logo: removing %s (no station uses it now)", logger::ToUtf8(path.c_str()).c_str());
				DeleteFileW(path.c_str());
			}
		} while (FindNextFileW(find, &data));
		FindClose(find);
	}

	void Start(const std::wstring& asiDir, const std::wstring& musicFolder)
	{
		std::thread(Make, asiDir, musicFolder).detach();
	}

	std::string Wait(DWORD timeoutMs)
	{
		return WaitForSingleObject(gDone, timeoutMs) == WAIT_OBJECT_0 ? gTexturePack : std::string();
	}
}

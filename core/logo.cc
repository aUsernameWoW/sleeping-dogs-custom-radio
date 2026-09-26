#include "logo.hh"

#include <cerrno>
#include <cstdio>
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

		logoimage::Result BuiltIn()
		{
			HMODULE self = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&BuiltIn),
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
			const logoimage::Result builtIn = BuiltIn();
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

			if (const std::shared_ptr<const logoimage::Image> image = Picture(musicFolder)) {
				// The widget loads Data\UI\<TexturePack>.perm.bin; two levels up is the game folder.
				const std::string pack = "..\\..\\" + (relative.empty() ? std::string() : relative + "\\") + "SDRadio-logo";
				const logopack::Files files = logopack::Build(*image, kTextureName, "Data\\UI\\" + pack + ".perm.bin");
				const std::wstring base = asiDir + L"\\SDRadio-logo";
				bool tempWritten = false, permWritten = false;
				int error = WriteIfChanged(base + L".temp.bin", files.mTemp, tempWritten);
				if (!error) {
					error = WriteIfChanged(base + L".perm.bin", files.mPerm, permWritten);
				}
				if (error) {
					LOG("logo: writing %s.perm.bin / .temp.bin failed (errno %d); the station borrows a game logo",
						logger::ToUtf8(base.c_str()).c_str(), error);
				}
				else {
					gTexturePack = pack;
					LOG("logo: texture pack %s (%ux%u, %zu mips) %s in %lu ms", pack.c_str(), image->mWidth, image->mHeight, image->mMips.size(),
						tempWritten || permWritten ? "written" : "unchanged", GetTickCount() - start);
				}
			}
			SetEvent(gDone);
		}
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

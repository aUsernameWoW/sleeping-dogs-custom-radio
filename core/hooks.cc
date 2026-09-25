#include "hooks.hh"

#include <Windows.h>
#include <MinHook.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "ak.hh"
#include "bank.hh"
#include "config.hh"
#include "cover.hh"
#include "d3d.hh"
#include "hud.hh"
#include "library.hh"
#include "log.hh"
#include "radios.hh"
#include "scan.hh"
#include "stream_io.hh"

namespace hooks
{
	namespace
	{
		// Signatures from the legacy exe (IDA, legacy PDB names), each checked unique in both builds.

		// SimpleXML::XMLCache::ExtractFromCache(const char* filename): the game's XML files live LZ-compressed
		// in Global.big (data\global\xmlcache\XML_CacheList.bin); this returns a qMalloc'd, NUL-terminated
		// copy that pugixml parses and later frees. Radios.xml goes through here before qOpen is ever tried.
		constexpr char kSigExtractFromCache[] =
			"40 57 48 83 EC 60 48 C7 44 24 30 FE FF FF FF 48 89 5C 24 70 48 89 74 24 78 0F 1F 80 00 00 00 00 0F B6 11 8D 42 D2 3C 01 76 05 80 FA 5C 75 05 48 FF C1 EB EC";
		// Inside it: mov ebx, [rax+58h]; lea ecx, [rbx+80h]; xor r8d, r8d; lea rdx, "uncompressedXMLBuffer"; call UFG::qMalloc
		constexpr char kSigQMallocCall[] = "8B 58 58 8D 8B 80 00 00 00 45 33 C0 48 8D 15 ? ? ? ? E8";
		constexpr size_t kQMallocCallDisp = 20;

		// UFG::LowLevelIODispatcher::Open(AkFileID, AkOpenMode, AkFileSystemFlags*, bool&, AkFileDesc&): the
		// file location resolver the stream manager asks for every bank and streamed file by ID.
		constexpr char kSigDispatcherOpen[] =
			"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 83 EC 30 48 8B 74 24 70 4C 8B 64 24 78 49 8B E9 45 8B F0 44 8B FA B8 42 00 00 00 C6 06 01";
		// UFG::WwiseDefaultIOHookDeferred::Read(AkFileDesc&, const AkIoHeuristics&, AkAsyncIOTransferInfo&)
		constexpr char kSigDeferredRead[] = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 41 8B 41 08 49 8B D9 49 8B F8 48 8B F2 41 39 41 0C 76 15";
		// UFG::WwiseFilePackageLowLevelIO<WwiseDefaultIOHookDeferred>::Close(AkFileDesc&)
		constexpr char kSigPackageClose[] = "48 83 EC 28 83 7A 0C 00 77 09 48 83 C4 28 E9 ? ? ? ? 48 8B 4A 18 E8";

		// Diagnostics only: UFG::SoundBankManager::BankLoadCallback(bankId, pInMemory, AKRESULT, poolId,
		// cookie) and UFG::AudioEntity::CreateAndPlayEvent(eventId, controller, params, fadeMs, extSources).
		constexpr char kSigBankLoadCallback[] = "40 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 4C 8B 5C 24 60 4D 8B 13 33 D2 41 83 F8 42 74 11";
		constexpr char kSigCreateAndPlayEvent[] =
			"40 53 48 83 EC 30 80 B9 28 01 00 00 00 49 8B D8 74 24 48 8B 44 24 68 48 89 44 24 20 E8 ? ? ? ? 84 C0 74 11 8B 54 24 60 48 8B CB";

		using ExtractFromCacheFn = char*(__fastcall*)(const char* filename);
		using QMallocFn = void*(__fastcall*)(uint64_t size, const char* name, uint64_t allocationParams);
		using OpenFn = ak::AKRESULT(__fastcall*)(void* self, uint32_t fileId, uint32_t openMode, ak::FileSystemFlags* flags, bool* syncOpen, ak::FileDesc* desc);
		using ReadFn = ak::AKRESULT(__fastcall*)(void* self, ak::FileDesc* desc, const void* heuristics, ak::AsyncIOTransferInfo* info);
		using CloseFn = ak::AKRESULT(__fastcall*)(void* self, ak::FileDesc* desc);
		using BankLoadCallbackFn = void(__fastcall*)(uint32_t bankId, const void* inMemory, ak::AKRESULT result, int32_t poolId, void* cookie);
		using CreateAndPlayEventFn = bool(__fastcall*)(void* entity, uint32_t eventId, void* controller, const void* params, uint32_t fadeMs, void* externalSources);

		ExtractFromCacheFn gExtractFromCache = nullptr;
		QMallocFn gQMalloc = nullptr;
		OpenFn gOpen = nullptr;
		ReadFn gRead = nullptr;
		CloseFn gClose = nullptr;
		BankLoadCallbackFn gBankLoadCallback = nullptr;
		CreateAndPlayEventFn gCreateAndPlayEvent = nullptr;

		// The streaming device every file of SFX.pck is opened on; learnt from the first successful open
		// (Init.bnk loads long before any station), since our files must be read by the same device.
		std::atomic<bool> gHaveDevice{ false };
		std::atomic<uint32_t> gDeviceId{ 0 };

		std::mutex gStationLock;
		std::string gPatchedXml;        // Radios.xml with our station, built once
		uint32_t gStation = 0;
		uint32_t gBankId = 0;
		std::unordered_map<uint32_t, uint32_t> gEventTracks; // play_station_<id>_track_<k> -> k
		std::atomic<bool> gStationReady{ false };
		bool gCoverArt = false; // configured, and the D3D hook is in

		bool IsRadiosXml(const char* filename)
		{
			// The same normalization ExtractFromCache applies: skip leading '.', '/', '\'.
			while (*filename == '.' || *filename == '/' || *filename == '\\') {
				++filename;
			}
			static constexpr char kName[] = "data\\audio\\radios.xml";
			for (size_t i = 0; i < sizeof(kName); ++i) {
				char c = filename[i];
				if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
				if (c == '/') c = '\\';
				if (c != kName[i]) return false;
			}
			return true;
		}

		// Builds the station (XML, bank, streams) from the scanned library. Returns false if there's no station.
		bool BuildStation(const char* original)
		{
			// Radios.xml is read during the game's startup; the scan runs from DllMain, normally done by then.
			const std::vector<library::Track>* tracks = library::Wait(30'000);
			if (!tracks) {
				LOG("radios: music scan still running after 30 s, starting without the station");
				return false;
			}
			if (tracks->empty()) {
				LOG("radios: no music found in %s, no station", logger::ToUtf8(gConfig.mMusicFolder.c_str()).c_str());
				return false;
			}

			gStation = radios::MaxStationId(original) + 1;
			radios::Station station;
			station.mName = gConfig.mStationName;
			station.mTextureName = gCoverArt ? "Logo_HKPDScanner" : gConfig.mTextureName;
			station.mTexturePack = gCoverArt ? "Radio_HKPDScanner_TexturePack" : gConfig.mTexturePack;
			std::vector<streamio::TrackSource> sources;
			for (const library::Track& t : *tracks) {
				station.mTracks.push_back({ t.mArtist, t.mTitle });
				const uint32_t k = static_cast<uint32_t>(sources.size() + 1);
				sources.push_back({ bank::TrackFileId(gStation, k), t.mPath, t.mSampleRate, t.mFrames });
				gEventTracks[bank::TrackEventId(gStation, k)] = k;
			}

			gPatchedXml = radios::Append(original, station, gStation);
			if (gPatchedXml.empty()) {
				LOG("radios: no </Radios> in Radios.xml, no station");
				return false;
			}

			gBankId = bank::BankId(gStation);
			std::vector<uint8_t> image = bank::Build(gStation, static_cast<uint32_t>(sources.size()));
			LOG("radios: station %u \"%s\" with %zu tracks, logo %s / %s; bank mus_radio_station_%u = %08X (%zu bytes)", gStation,
				radios::TruncateUtf8(station.mName, 63).c_str(), sources.size(), station.mTextureName.c_str(), station.mTexturePack.c_str(), gStation,
				gBankId, image.size());
			if (gCoverArt) {
				std::vector<std::wstring> paths;
				for (const library::Track& t : *tracks) {
					paths.push_back(t.mPath);
				}
				cover::Start(std::move(paths), gConfig.mMusicFolder);
			}
			streamio::Register(gBankId, std::move(image), std::move(sources));
			return true;
		}

		char* __fastcall ExtractFromCacheHook(const char* filename)
		{
			char* xml = gExtractFromCache(filename);
			if (!xml || !filename || !IsRadiosXml(filename)) {
				return xml;
			}

			std::lock_guard lock(gStationLock);
			static bool tried = false;
			if (!tried) {
				tried = true;
				gStationReady = BuildStation(xml);
			}
			if (!gStationReady) {
				return xml;
			}

			// pugixml frees the buffer through the game's allocator, so the replacement must come from it too.
			// The original (~20 KB, once per load of the station list) is leaked: the matching free isn't
			// reachable from here, and this happens once at startup.
			auto* copy = static_cast<char*>(gQMalloc(gPatchedXml.size() + 1, "SDRadio.Radios.xml", 0));
			if (!copy) {
				LOG("radios: qMalloc failed, keeping the original station list");
				return xml;
			}
			std::memcpy(copy, gPatchedXml.c_str(), gPatchedXml.size() + 1);
			LOG("radios: Radios.xml %zu -> %zu bytes", std::strlen(xml), gPatchedXml.size());
			return copy;
		}

		ak::AKRESULT __fastcall OpenHook(void* self, uint32_t fileId, uint32_t openMode, ak::FileSystemFlags* flags, bool* syncOpen, ak::FileDesc* desc)
		{
			if (gStationReady && streamio::IsOurs(fileId)) {
				if (!gHaveDevice) {
					LOG("io: %08X requested before any game file was opened; don't know the device", fileId);
					return ak::kFail;
				}
				if (streamio::Open(fileId, gDeviceId, *desc)) {
					*syncOpen = true;
					return ak::kSuccess;
				}
				return ak::kFail;
			}

			const ak::AKRESULT result = gOpen(self, fileId, openMode, flags, syncOpen, desc);
			if (result == ak::kSuccess && !gHaveDevice.exchange(true)) {
				gDeviceId = desc->mDeviceID;
				LOG("io: game streaming device %u (first open: %08X)", desc->mDeviceID, fileId);
			}
			return result;
		}

		ak::AKRESULT __fastcall ReadHook(void* self, ak::FileDesc* desc, const void* heuristics, ak::AsyncIOTransferInfo* info)
		{
			if (gStationReady && streamio::Read(desc->mFile, *info)) {
				return ak::kSuccess;
			}
			return gRead(self, desc, heuristics, info);
		}

		ak::AKRESULT __fastcall CloseHook(void* self, ak::FileDesc* desc)
		{
			if (gStationReady && streamio::Close(desc->mFile)) {
				return ak::kSuccess;
			}
			return gClose(self, desc);
		}

		void __fastcall BankLoadCallbackHook(uint32_t bankId, const void* inMemory, ak::AKRESULT result, int32_t poolId, void* cookie)
		{
			if (gStationReady && bankId == gBankId) {
				LOG("bank: mus_radio_station_%u loaded with result %d (1 = success) in pool %d", gStation, result, poolId);
			}
			gBankLoadCallback(bankId, inMemory, result, poolId, cookie);
		}

		bool __fastcall CreateAndPlayEventHook(void* entity, uint32_t eventId, void* controller, const void* params, uint32_t fadeMs, void* externalSources)
		{
			const bool played = gCreateAndPlayEvent(entity, eventId, controller, params, fadeMs, externalSources);
			if (!gStationReady) {
				return played;
			}
			if (auto it = gEventTracks.find(eventId); it != gEventTracks.end()) {
				LOG("radio: post event %08X (track %u) on entity %p: %s", eventId, it->second, entity, played ? "playing" : "FAILED");
				if (played && gCoverArt) {
					cover::Request(it->second);
				}
			}
			return played;
		}

		template <typename T>
		bool Hook(const char* name, void* target, void* detour, T& original)
		{
			if (!target) {
				return false;
			}
			MH_STATUS status = MH_CreateHook(target, detour, reinterpret_cast<void**>(&original));
			if (status == MH_OK) {
				status = MH_EnableHook(target);
				if (status != MH_OK) {
					MH_RemoveHook(target);
					original = nullptr;
				}
			}
			if (status != MH_OK) {
				LOG("hook: %s: %s", name, MH_StatusToString(status));
				return false;
			}
			return true;
		}
	}

	void Install()
	{
		if (const MH_STATUS status = MH_Initialize(); status != MH_OK) {
			LOG("hook: MH_Initialize failed (%s), nothing hooked", MH_StatusToString(status));
			return;
		}

		uint8_t* extract = scan::FindUnique("SimpleXML::XMLCache::ExtractFromCache", kSigExtractFromCache);
		if (uint8_t* call = scan::FindUnique("qMalloc call in ExtractFromCache", kSigQMallocCall)) {
			gQMalloc = reinterpret_cast<QMallocFn>(scan::RipTarget(call + kQMallocCallDisp));
		}
		uint8_t* open = scan::FindUnique("UFG::LowLevelIODispatcher::Open", kSigDispatcherOpen);
		uint8_t* read = scan::FindUnique("UFG::WwiseDefaultIOHookDeferred::Read", kSigDeferredRead);
		uint8_t* close = scan::FindUnique("UFG::WwiseFilePackageLowLevelIO::Close", kSigPackageClose);

		// All or nothing: a station in the list whose bank can't be served would just be silent, and serving
		// files without the station is pointless.
		if (!extract || !gQMalloc || !open || !read || !close) {
			LOG("hook: game functions missing, no station");
			return;
		}
		const bool ok = Hook("Open", open, &OpenHook, gOpen) && Hook("Read", read, &ReadHook, gRead) &&
			Hook("Close", close, &CloseHook, gClose) && Hook("ExtractFromCache", extract, &ExtractFromCacheHook, gExtractFromCache);
		if (!ok) {
			// The XML hook goes last, so a failure before it leaves the station list untouched; the I/O hooks
			// that did install only pass through while no station exists.
			LOG("hook: install failed, no station");
			return;
		}

		uint8_t* bankLoaded = scan::FindUnique("UFG::SoundBankManager::BankLoadCallback", kSigBankLoadCallback);
		uint8_t* playEvent = scan::FindUnique("UFG::AudioEntity::CreateAndPlayEvent", kSigCreateAndPlayEvent);
		const bool diagnostics = Hook("BankLoadCallback", bankLoaded, &BankLoadCallbackHook, gBankLoadCallback) &
			Hook("CreateAndPlayEvent", playEvent, &CreateAndPlayEventHook, gCreateAndPlayEvent);
		LOG("hook: station hooks ready%s", diagnostics ? "" : " (diagnostic hooks missing)");

		// Cover art needs the track starts, which CreateAndPlayEvent reports.
		if (gConfig.mCoverArt && gCreateAndPlayEvent) {
			// The Flash hook first: our texture without it would show as a white square.
			gCoverArt = hud::Install("img://Logo_HKPDScanner") && d3d::Install();
		}
		else if (gConfig.mCoverArt) {
			LOG("hook: no CreateAndPlayEvent hook, so no cover art");
		}
	}
}

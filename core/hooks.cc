#include "hooks.hh"

#include <Windows.h>
#include <MinHook.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "ak.hh"
#include "app_audio.hh"
#include "bank.hh"
#include "config.hh"
#include "library.hh"
#include "live.hh"
#include "netradio.hh"
#include "log.hh"
#include "logo.hh"
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

		// The live station's song title on the HUD. UFG::UIHKRadioStationWidget::Flash_SetSongTitle(UIScreen*)
		// shows LocalizeText(RadioFullyControlled::GetCurrentlyPlayingSong(sm_playerRadio)), the current track's
		// name from Radios.xml; its first call to it (rel32 at kCurrentSongCallDisp) leads to the getter, which
		// starts `mov rax, [rcx+m_curStation]` (the offset is read from there). UIHKRadioStationWidget::Update
		// (widget, UIScreen*, elapsed) runs every frame and refreshes the texts while `mChanged` is set.
		constexpr char kSigSetSongTitle[] =
			"48 8B C4 57 48 81 EC 90 00 00 00 48 C7 40 98 FE FF FF FF 48 89 58 08 48 89 70 10 48 8B 42 60 48 8B 98 40 01 00 00 48 85 DB 0F 84 ? ? ? ? 48 8B 0D ? ? ? ? 48 85 C9 0F 84 ? ? ? ? E8";
		constexpr size_t kCurrentSongCallDisp = 64;
		constexpr char kSigWidgetUpdate[] = "48 89 5C 24 08 57 48 83 EC 40 83 39 02 0F 29 74 24 30";
		constexpr size_t kStationIdOffset = 0x24; // RadioStation::m_id
		constexpr size_t kStationNameOffset = 0x60; // RadioStation::m_name, char[64]
		constexpr size_t kWidgetChangedOffset = 4; // UIHKRadioStationWidget::mChanged

		// Diagnostics for station switches (a report of the live station not switching away, 2026-10-23):
		// UFG::Radio::NextStation / PrevStation(bool displayStationId) do nothing unless flag 0x01 (on) is set and
		// 0x20 / 0x40 (a switch waiting for its HUD update) are clear; Radio::Update clears those only once the new
		// station's bank reports loaded, the player is in a vehicle and 0.3 s have passed. UFG::Radio::SetStation
		// (index) is the vehicle's or a script's choice. Flags are the byte at +0x158; m_curStation at +0x148.
		constexpr char kSigNextStation[] =
			"48 89 74 24 10 57 48 83 EC 20 80 3D ? ? ? ? 00 0F B6 F2 48 8B F9 0F 84 ? ? ? ? 0F B6 81 58 01 00 00 A8 01 0F 84 ? ? ? ? A8 20";
		constexpr char kSigPrevStation[] =
			"48 89 74 24 10 57 48 83 EC 20 80 3D ? ? ? ? 00 0F B6 F2 48 8B F9 0F 84 ? ? ? ? 0F B6 81 58 01 00 00 A8 01 0F 84 ? ? ? ? A8 40";
		constexpr char kSigSetStation[] = "48 89 5C 24 08 57 48 83 EC 20 4C 8B 81 48 01 00 00";
		// UFG::UIHKRadioStationWidget::LoadTextures(oldIndex, newIndex), from ChangeStation(id) (index = id - 1).
		// When the new station's texture pack is the one already loaded it only activates the widget if newIndex
		// is already mCurrentStation, and otherwise does nothing, leaving mCurrentStation behind. The game's
		// stations each have their own pack, so it never mattered; ours share one (both stations, our logo or a
		// borrowed one). After a switch between them the widget never activated again, and a press on a hidden
		// widget only clicks and calls ChangeStation to show it: the radio couldn't switch until the player
		// re-entered the car (2026-10-23). The hook sets mCurrentStation first unless a pack load is in flight
		// (its callback matches its index against mCurrentStation); with a different pack the game sets it anyway.
		constexpr char kSigLoadTextures[] = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 49 63 F0";
		constexpr size_t kWidgetWaitingForTextureOffset = 7; // UIHKRadioStationWidget::mWaitingForTexture
		constexpr size_t kWidgetCurrentStationOffset = 8;    // UIHKRadioStationWidget::mCurrentStation
		constexpr size_t kRadioCurStationOffset = 0x148;
		constexpr size_t kRadioFlagsOffset = 0x158;
		constexpr size_t kWidgetVisibleOffset = 5; // UIHKRadioStationWidget::mVisible

		using ExtractFromCacheFn = char*(__fastcall*)(const char* filename);
		using QMallocFn = void*(__fastcall*)(uint64_t size, const char* name, uint64_t allocationParams);
		using OpenFn = ak::AKRESULT(__fastcall*)(void* self, uint32_t fileId, uint32_t openMode, ak::FileSystemFlags* flags, bool* syncOpen, ak::FileDesc* desc);
		using ReadFn = ak::AKRESULT(__fastcall*)(void* self, ak::FileDesc* desc, const void* heuristics, ak::AsyncIOTransferInfo* info);
		using CloseFn = ak::AKRESULT(__fastcall*)(void* self, ak::FileDesc* desc);
		using BankLoadCallbackFn = void(__fastcall*)(uint32_t bankId, const void* inMemory, ak::AKRESULT result, int32_t poolId, void* cookie);
		using CreateAndPlayEventFn = bool(__fastcall*)(void* entity, uint32_t eventId, void* controller, const void* params, uint32_t fadeMs, void* externalSources);
		using CurrentSongFn = const char*(__fastcall*)(void* radio);
		using WidgetUpdateFn = void(__fastcall*)(void* widget, void* screen, float elapsed);
		using StepStationFn = void(__fastcall*)(void* radio, bool displayStationId);
		using SetStationFn = void(__fastcall*)(void* radio, uint32_t index);
		using LoadTexturesFn = void(__fastcall*)(void* widget, int oldIndex, int newIndex);

		ExtractFromCacheFn gExtractFromCache = nullptr;
		QMallocFn gQMalloc = nullptr;
		OpenFn gOpen = nullptr;
		ReadFn gRead = nullptr;
		CloseFn gClose = nullptr;
		BankLoadCallbackFn gBankLoadCallback = nullptr;
		CreateAndPlayEventFn gCreateAndPlayEvent = nullptr;
		CurrentSongFn gCurrentSong = nullptr;
		WidgetUpdateFn gWidgetUpdate = nullptr;
		size_t gCurStationOffset = 0; // RadioFullyControlled::m_curStation, from the getter's first instruction
		std::atomic<uint32_t> gLiveStationId{ 0 }; // 0: no live station
		StepStationFn gNextStation = nullptr;
		StepStationFn gPrevStation = nullptr;
		SetStationFn gSetStation = nullptr;
		LoadTexturesFn gLoadTextures = nullptr;
		std::unordered_map<uint32_t, uint32_t> gStationBanks; // every station's bank ID -> station ID, for the log

		// The streaming device every file of SFX.pck is opened on; learnt from the first successful open
		// (Init.bnk loads long before any station), since our files must be read by the same device.
		std::atomic<bool> gHaveDevice{ false };
		std::atomic<uint32_t> gDeviceId{ 0 };

		std::mutex gStationLock;
		std::string gPatchedXml;        // Radios.xml with our stations, built once
		std::unordered_set<uint32_t> gBankIds;
		std::unordered_set<uint32_t> gEventIds;
		std::atomic<bool> gStationReady{ false };

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

		// Appends one station to gPatchedXml and registers its bank and streams.
		bool AddStation(radios::Station station, uint32_t id, std::vector<streamio::TrackSource> sources)
		{
			// Our logo's texture pack, written at startup (long done by now); without one, a game station's logo.
			const std::string pack = gConfig.mCustomLogo ? logo::Wait(10'000) : std::string();
			station.mTextureName = pack.empty() ? gConfig.mTextureName : logo::kTextureName;
			station.mTexturePack = pack.empty() ? gConfig.mTexturePack : pack;

			std::string xml = radios::Append(gPatchedXml, station, id);
			if (xml.empty()) {
				LOG("radios: no </Radios> in Radios.xml, no station");
				return false;
			}
			gPatchedXml = std::move(xml);

			for (uint32_t k = 1; k <= sources.size(); ++k) {
				gEventIds.insert(bank::TrackEventId(id, k));
			}
			const uint32_t bankId = bank::BankId(id);
			gBankIds.insert(bankId);
			std::vector<uint8_t> image = bank::Build(id, static_cast<uint32_t>(sources.size()));
			LOG("radios: station %u \"%s\" with %zu tracks, logo %s / %s; bank mus_radio_station_%u = %08X (%zu bytes)", id,
				radios::TruncateUtf8(station.mName, 63).c_str(), sources.size(), station.mTextureName.c_str(), station.mTexturePack.c_str(), id,
				bankId, image.size());
			streamio::Register(bankId, std::move(image), std::move(sources));
			return true;
		}

		// Builds the stations (XML, banks, streams): the music folder's, and the live one if enabled. Returns
		// false if there's neither.
		bool BuildStations(const char* original)
		{
			gPatchedXml = original;
			uint32_t next = radios::MaxStationId(original) + 1;
			bool any = false;
			// Before any bank can load; one past ours is harmless.
			for (uint32_t id = 1; id <= next + 1; ++id) {
				gStationBanks[bank::BankId(id)] = id;
			}

			// Radios.xml is read during the game's startup; the scan runs from DllMain, normally done by then.
			const std::vector<library::Track>* tracks = library::Wait(30'000);
			if (!tracks) {
				LOG("radios: music scan still running after 30 s, starting without the music station");
			}
			else if (tracks->empty()) {
				LOG("radios: no music found in %s, no music station", logger::ToUtf8(gConfig.mMusicFolder.c_str()).c_str());
			}
			else {
				radios::Station station;
				station.mName = gConfig.mStationName;
				std::vector<streamio::TrackSource> sources;
				for (const library::Track& t : *tracks) {
					station.mTracks.push_back({ t.mArtist, t.mTitle });
					const uint32_t k = static_cast<uint32_t>(sources.size() + 1);
					sources.push_back({ bank::TrackFileId(next, k), t.mPath, t.mSampleRate, t.mFrames });
				}
				if (AddStation(std::move(station), next, std::move(sources))) {
					++next;
					any = true;
				}
			}

			if (gConfig.mLive) {
				// The HUD shows the track's title, fixed at startup: the source, or the radio's host.
				std::string title = gConfig.mLiveSource;
				if (gConfig.mLiveSource == "test") {
					live::SetSource("test", &live::TestSignal);
				}
				else if (gConfig.mLiveSource == "radio") {
					live::SetSource("radio " + gConfig.mLiveUrl, [url = gConfig.mLiveUrl](const std::atomic<bool>& stop) { netradio::Run(url, stop); });
					const size_t host = gConfig.mLiveUrl.find("://");
					if (host != std::string::npos) {
						title = gConfig.mLiveUrl.substr(host + 3, gConfig.mLiveUrl.find_first_of("/:?", host + 3) - host - 3);
					}
				}
				else if (gConfig.mLiveSource == "app") {
					const std::string app = logger::ToUtf8(gConfig.mLiveApp.c_str());
					live::SetSource("app " + app, [exe = gConfig.mLiveApp](const std::atomic<bool>& stop) { appaudio::Run(exe, stop); });
					title = app.empty() ? "app" : app;
				}
				else {
					title.clear();
				}
				if (title.empty()) {
					LOG("radios: unknown live source \"%s\", no live station", gConfig.mLiveSource.c_str());
				}
				else {
					// Two tracks on the same stream: GetNextTrack avoids recently played tracks, and with only one
					// it might find none when the "endless" track does end (after an hour).
					radios::Station station;
					station.mName = gConfig.mLiveName;
					std::vector<streamio::TrackSource> sources;
					for (uint32_t k = 1; k <= 2; ++k) {
						station.mTracks.push_back({ "SDRadio", title });
						sources.push_back({ bank::TrackFileId(next, k), L"", live::kRate, live::kFrames, true });
					}
					if (AddStation(std::move(station), next, std::move(sources))) {
						gLiveStationId = next;
						any = true;
					}
				}
			}
			return any;
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
				gStationReady = BuildStations(xml);
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
			// Every station's bank, the game's too (the switch diagnostics need to see them).
			if (gStationReady) {
				if (const auto it = gStationBanks.find(bankId); it != gStationBanks.end()) {
					LOG("bank: %08X (station %u) callback with result %d (1 = success) in pool %d", bankId, it->second, result, poolId);
				}
			}
			gBankLoadCallback(bankId, inMemory, result, poolId, cookie);
		}

		std::string DescribeStation(const void* radio)
		{
			const auto* station = *reinterpret_cast<const uint8_t* const*>(static_cast<const uint8_t*>(radio) + kRadioCurStationOffset);
			if (!station) {
				return "(none)";
			}
			char name[64];
			std::memcpy(name, station + kStationNameOffset, sizeof(name));
			name[63] = '\0';
			return std::to_string(*reinterpret_cast<const uint32_t*>(station + kStationIdOffset)) + " \"" + name + "\"";
		}

		uint8_t RadioFlags(const void* radio)
		{
			return static_cast<const uint8_t*>(radio)[kRadioFlagsOffset];
		}

		void LogStep(const char* what, void* radio, StepStationFn original, bool displayStationId)
		{
			const std::string from = DescribeStation(radio);
			const uint8_t flags = RadioFlags(radio);
			original(radio, displayStationId);
			const std::string to = DescribeStation(radio);
			if (to == from) {
				LOG("radio: %s station ignored on %s (flags 0x%02X: on %d, waiting for the HUD %d)", what, from.c_str(), flags, flags & 1,
					(flags & 0x60) != 0);
			}
			else {
				LOG("radio: %s station %s -> %s (flags 0x%02X -> 0x%02X)", what, from.c_str(), to.c_str(), flags, RadioFlags(radio));
			}
		}

		void __fastcall NextStationHook(void* radio, bool displayStationId)
		{
			LogStep("next", radio, gNextStation, displayStationId);
		}

		void __fastcall PrevStationHook(void* radio, bool displayStationId)
		{
			LogStep("previous", radio, gPrevStation, displayStationId);
		}

		void __fastcall LoadTexturesHook(void* widget, int oldIndex, int newIndex)
		{
			auto* bytes = static_cast<uint8_t*>(widget);
			uint32_t current;
			std::memcpy(&current, bytes + kWidgetCurrentStationOffset, sizeof(current));
			if (!bytes[kWidgetWaitingForTextureOffset] && current != static_cast<uint32_t>(newIndex)) {
				const uint32_t index = static_cast<uint32_t>(newIndex);
				std::memcpy(bytes + kWidgetCurrentStationOffset, &index, sizeof(index));
			}
			gLoadTextures(widget, oldIndex, newIndex);
		}

		void __fastcall SetStationHook(void* radio, uint32_t index)
		{
			const std::string from = DescribeStation(radio);
			gSetStation(radio, index);
			LOG("radio: set station %u: %s -> %s (flags 0x%02X)", index, from.c_str(), DescribeStation(radio).c_str(), RadioFlags(radio));
		}

		bool __fastcall CreateAndPlayEventHook(void* entity, uint32_t eventId, void* controller, const void* params, uint32_t fadeMs, void* externalSources)
		{
			const bool played = gCreateAndPlayEvent(entity, eventId, controller, params, fadeMs, externalSources);
			if (gStationReady && gEventIds.count(eventId)) {
				LOG("radio: post event %08X on entity %p: %s", eventId, entity, played ? "playing" : "FAILED");
			}
			return played;
		}

		// The HUD's song line: on the live station, what the source plays now (the track's fixed title, the
		// radio's host, until the source names one). Called twice per refresh by Flash_SetSongTitle, which
		// copies the text before anything else runs, so one buffer per thread is enough.
		const char* __fastcall CurrentSongHook(void* radio)
		{
			const uint32_t live = gLiveStationId.load();
			const auto* station = radio ? *reinterpret_cast<const uint8_t* const*>(static_cast<const uint8_t*>(radio) + gCurStationOffset) : nullptr;
			if (live && station && *reinterpret_cast<const uint32_t*>(station + kStationIdOffset) == live) {
				thread_local std::string title;
				title = live::NowPlaying();
				if (!title.empty()) {
					static std::string logged;
					if (title != logged) {
						logged = title;
						LOG("hud: song title \"%s\"", title.c_str());
					}
					return title.c_str();
				}
			}
			return gCurrentSong(radio);
		}

		// A new title while the widget shows: have it refresh its texts, as the game does when a track starts
		// (Radio::DoStartTrack -> HandleNewSong). It only refreshes while visible, so the title waits otherwise
		// and shows when the widget next opens (which reads it anew anyway).
		void __fastcall WidgetUpdateHook(void* widget, void* screen, float elapsed)
		{
			static uint32_t shown = 0;
			auto* bytes = static_cast<uint8_t*>(widget);
			const uint32_t generation = live::NowPlayingGeneration();
			if (generation != shown && bytes[kWidgetVisibleOffset]) {
				shown = generation;
				bytes[kWidgetChangedOffset] = 1;
			}
			gWidgetUpdate(widget, screen, elapsed);
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
		// Finds and hooks the two functions behind the HUD's song line. Optional: without them the live station
		// shows its fixed title.
		bool InstallSongTitle()
		{
			uint8_t* setTitle = scan::FindUnique("UFG::UIHKRadioStationWidget::Flash_SetSongTitle", kSigSetSongTitle);
			uint8_t* update = scan::FindUnique("UFG::UIHKRadioStationWidget::Update", kSigWidgetUpdate);
			if (!setTitle || !update) {
				return false;
			}
			auto* getter = static_cast<uint8_t*>(scan::RipTarget(setTitle + kCurrentSongCallDisp));
			// mov rax, [rcx+disp32]; cmp qword ptr [rax+120h], 0 (RadioStation::m_currentlyPlaying)
			static constexpr uint8_t kStart[] = { 0x48, 0x8B, 0x81 };
			static constexpr uint8_t kCompare[] = { 0x48, 0x83, 0xB8, 0x20, 0x01, 0x00, 0x00, 0x00 };
			if (std::memcmp(getter, kStart, sizeof(kStart)) != 0 || std::memcmp(getter + 7, kCompare, sizeof(kCompare)) != 0) {
				LOG("hook: GetCurrentlyPlayingSong at +0x%llX doesn't look as expected", static_cast<unsigned long long>(getter - reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr))));
				return false;
			}
			int32_t offset;
			std::memcpy(&offset, getter + 3, sizeof(offset));
			gCurStationOffset = static_cast<size_t>(offset);
			LOG("scan: RadioFullyControlled::GetCurrentlyPlayingSong at +0x%llX (m_curStation +0x%X)",
				static_cast<unsigned long long>(getter - reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr))), offset);
			return Hook("GetCurrentlyPlayingSong", getter, &CurrentSongHook, gCurrentSong) &&
				Hook("UIHKRadioStationWidget::Update", update, &WidgetUpdateHook, gWidgetUpdate);
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

		// Needed as soon as two of our stations share a texture pack (the music and the live one).
		uint8_t* loadTextures = scan::FindUnique("UFG::UIHKRadioStationWidget::LoadTextures", kSigLoadTextures);
		if (!Hook("UIHKRadioStationWidget::LoadTextures", loadTextures, &LoadTexturesHook, gLoadTextures)) {
			LOG("hook: switching between SDRadio's stations may leave the radio stuck until the car is re-entered");
		}

		uint8_t* bankLoaded = scan::FindUnique("UFG::SoundBankManager::BankLoadCallback", kSigBankLoadCallback);
		uint8_t* playEvent = scan::FindUnique("UFG::AudioEntity::CreateAndPlayEvent", kSigCreateAndPlayEvent);
		uint8_t* nextStation = scan::FindUnique("UFG::Radio::NextStation", kSigNextStation);
		uint8_t* prevStation = scan::FindUnique("UFG::Radio::PrevStation", kSigPrevStation);
		uint8_t* setStation = scan::FindUnique("UFG::Radio::SetStation", kSigSetStation);
		const bool diagnostics = Hook("BankLoadCallback", bankLoaded, &BankLoadCallbackHook, gBankLoadCallback) &
			Hook("CreateAndPlayEvent", playEvent, &CreateAndPlayEventHook, gCreateAndPlayEvent) &
			Hook("NextStation", nextStation, &NextStationHook, gNextStation) & Hook("PrevStation", prevStation, &PrevStationHook, gPrevStation) &
			Hook("SetStation", setStation, &SetStationHook, gSetStation);
		LOG("hook: station hooks ready%s", diagnostics ? "" : " (diagnostic hooks missing)");
		if (gConfig.mLive && !InstallSongTitle()) {
			LOG("hook: the live station's song title can't be shown on the HUD (it shows the fixed title)");
		}
	}
}

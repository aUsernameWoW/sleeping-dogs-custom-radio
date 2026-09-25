# The game's radio and what SDRadio feeds it

Addresses are from the legacy Steam v1.0 build (`reference\SDmodding\game-itself`, full PDB); the installed
build is found by signature (see `core/hooks.cc`).

## Station data: Radios.xml

`UFG::Radio::LoadRadioStationData` (0x14067f730) opens `Data\Audio\Radios.xml` through
`SimpleXML::XMLDocument::Open` → `pugi::xml_document::load_file`, which first asks
`SimpleXML::XMLCache::ExtractFromCache` (0x14008a580) and only falls back to `qOpen`.

- The cache: `Global.big` → `data\global\xmlcache\XML_CacheList.bin`, 217 resources. Chunk magic
  `a0 c3 d0 24`, size at +4, body at +16; in the body the name (e.g. `data\audio\radios.xml`) at +0x34,
  uncompressed/compressed size at +0x58/+0x5C, a PMCQ LZ block at +0x68. Lookup key: `qStringHashUpper32`
  of the lowercased path with `/` → `\` (`gXMLFileInventory`). `ExtractFromCache` returns
  `qMalloc(usize + 0x80, "uncompressedXMLBuffer")`; pugixml frees it through its memory pool.
- Per `<Station name id numConsecutiveTracks chanceDJ chanceAd chanceTrack [copScanner]>`: `RadioStation`
  ctor, `SetupIdentAsset`, then children `Tracks/Track(artist, name)` → `SetupTrackAsset(index from 1)`,
  `Ads count` → ad assets, `DJs/DJ(track1..3)`, `TextureName name`, `TexturePack name`. Stations are kept in
  `Radio::sm_stationList` in file order; `sm_numStations`.
- The shipped file: 11 stations, ids 01-11 (11 = HKPD, `copScanner="true"`). Read it with
  `tools\extract.ps1 xml 'radios\.xml$' OUT` (the cache holds two identical copies).

## Playing

- `TrackAsset` ctor: `m_assetId = TiDo::CalcWwiseUid("play_station_%02u_track_%02u")` (FNV-1 of the
  lowercased name; station 1 track 1 → 3665015614), artist/name copied, `m_trackUID = qStringHash32(name)`.
- `RadioStation` ctor: `m_bankId = qWiseSymbol("mus_radio_station_%d")`. `Radio::GetNext/PrevStationId`
  unload the old station's bank and load the new one (`TiDo::LoadWwiseBank` → `SoundBankManager` →
  `AK::SoundEngine::LoadBank(id, BankLoadCallback, cookie, pool)`).
- `Radio::Update` (0x1406932b0) starts a track only once `SoundBankManager::BankLoaded(m_bankId)` (or the
  station is the cop scanner) and the player is in a vehicle.
- `Radio::DoStartTrack` (0x140676f00): sets RTPC `effect_off_on` = 0.95 on the radio entity, then
  `AudioEntity::CreateAndPlayEvent(GetNextTrack(), ..., TrackFinishCallback)`; on the first start after init
  it seeks (`AudioEvent::SeekMS(m_currentTrackTimer)`) so a station "kept playing" while away.
- `TrackFinishCallback`: callback type 2 (end of event) → `StartTrack` again.
- `GetNextTrack`: queued DJ follow-ups first, else `DetermineAssetType` (track if rand(100) ≤ chanceTrack),
  a random asset of that type not in the recently played list.
- HUD: `UIHKRadioStationWidget` reads the station list (`img://%s`, `Data\UI\%s.perm.bin`), shows
  `LocalizeText(m_name)` and `LocalizeText(GetCurrentlyPlayingSong())`.

## The HUD logo

- `UIHKRadioStationWidget::LoadTextures(old, new)` (0x1405f1870): if the new station's pack name differs
  from `mLoadedTexturePack`, `UIScreenTextureManager::ReleaseTexturePack(old)` +
  `QueueTexturePackLoad(new, TextureLoadedCallback)` and wait (`mWaitingForTexture`, 2 s timeout → empty
  texture); same name → just refresh. `HandleNewSong` (0x1405eb540) only sets `mChanged`. `Update`
  (0x140616710): `Deactivate` 4 s after the last input; on `mChanged` → `Flash_SetVisible`, then
  `Flash_SetTexture(texture)` only if `mShouldRefreshTextures` (station change), `Flash_Intro` if it was
  inactive, and always `Flash_SetSongTitle`/`Flash_SetStationName`. So a new song never re-shows the widget.
- `Flash_SetTexture` → `mc_RadioStations.SetTexture("img://Logo_X")`. Screens\RadioStations.bin (UI.big) is a
  `CFX` (zlib) GFx 8 movie, AS2 class `RadioStations`: `SetTexture` does `m_loader.loadClip(texture,
  slot.holder)` (5 slots, center 2); `onLoadInit` sets `holder._height = 64; holder._width = 128`. The slot
  sits on `Backing_9Slice_Metal_1`. Sprite 12 places `holder` (depth 1) with color transform mult (0,0,0,1)
  add (255,255,255,0): whatever loads into it shows white with its own alpha, which is how the black logos
  appear white in game (and why an untouched cover is a white square). Scaleform keeps a target clip's
  transform across `loadClip`. Changing it at runtime: `Movie::Invoke` (0x1408e6de0, `mov rcx,[rcx+18h];
  mov r10,[rcx]; jmp [r10+1C0h]`) → `ASMovieRootBase` vtable: `GetVariable` +0x188, `Invoke_2` +0x1C0;
  `Value` (0x30: list node, `pObjectInterface` +0x10, `Type` +0x18, payload +0x20; VT_String 6,
  VT_DisplayObject 10, managed bit 0x40) → `Value::ObjectInterface` vtable: `ObjectRelease` +0x10,
  `GetCxform` +0x110, `SetCxform` +0x118 (`AS2ValueObjectInterface::SetCxform` also stops timeline moves
  from overriding it). `Render::Cxform` = float[2][4], multiply row then add row, add in 0..1.
- The pack's texture: `Illusion::TexturePlat::CreateResources` (0x140a18fc0) builds a D3D11 desc from the
  Illusion texture (usage IMMUTABLE when there's initial data and no CPU access/UAV/RT flags; mips from the
  resource, pixels contiguous in `mInitialImageData`/`mVRamHandle`) → `Illusion::CreateTexture` →
  `ID3D11Device::CreateTexture2D`, SRV with a null desc. Scaleform wraps it with
  `D3D1x::Texture::Initialize(ID3D11Texture2D*)` (0x140a0efe0), which takes size and mip count from
  `GetDesc`.
- All radio logos: 128×64 DXT5, 1 level, 8192 bytes. FNV-1a-64 of the blocks (installed build): HKPD scanner
  `B3D3AD52833FA849`, Softly `D69A728F575CA4C2`, Kerrang `9C012F9F33FAF74E`
  (`tools\extract.ps1 hash '^LOGO_'`; the movie and its AS2: `tools\extract.ps1 gfx 'Screens.RadioStations' OUT`).
- The installed exe imports `D3D11CreateDevice` (d3d11.dll) and `CreateDXGIFactory1` (dxgi.dll).

## The station banks (SFX.pck)

AKPK v1; bank and stream tables have 24-byte entries (id, block size, size, 0, start block, language).
`mus_radio_station_1` = 116370779: BKHD (v88, feedback 1, project 295) + HIRC: per asset a music segment +
music track (Vorbis, streamed), ranseq/switch containers, and one shared top node: **music switch
992991330**, output bus `radio_car` (3551958221), effect 287224143 (type 18, plugin 0x00820003, shared by all
ten station banks, byte-identical), props priority 100 / distance offset 0, 2D positioning, advanced settings
`01 00 00 02 00 00 02 00 00 00 00 00 00`, RTPCs:

| RTPC | parameter | curve |
|---|---|---|
| 3026679070 | 28 (BypassAllFX) | 0→0, 0.95→1, 2→0 (constant) — the "effect_off_on" value DoStartTrack sets |
| 2420253395 | 0 (volume, dB) | -96.3→-1, 0→0 (linear) |
| 1240670792 | 0 (volume, dB) | 0→0, 85→0 (S-curve), 100→-0.996 |

## Wwise 2012.2 bank objects used (v88)

- Sound (2): `LoadSource` = plugin u32 (0x00010001 PCM, 0x00040001 Vorbis, 0x00080001 external), stream type
  u32 (0 data, 1 streamed, 2 prefetch), source ID u32, file ID u32, [offset u32, size u32 unless streamed],
  flags u8, [params if plugin type 2/5]; then `SetNodeBaseParams`.
- `SetNodeBaseParams`: override-FX u8, FX count u8, [bypass u8, count × (index u8, id u32, shareset u8,
  rendered u8)], override bus u32 (must exist), parent u32 (skipped if not loaded), 2 × u8 priority flags,
  props (count u8, ids, u32 values), ranged props (count u8, ids, 2 × u32), positioning (override u8, [2D u8,
  3D u8, [panner u8], [3D block]]), aux (4 × u8, [4 × u32]), advanced 13 bytes, states (u32 count, ...),
  RTPCs (u16 count, each id u32, param u32, curve u32, scaling u8, u16 points × (from f32, to f32, interp
  u32)), feedback bus u32 if the bank has feedback data. Validated against 14 069 Sounds in SFX.pck.
- Actor-mixer (7): node params + children (u32 count, ids). Action (3): type u16 (Play 0x0403), target
  u32, is-bus u8, props, ranged props, fade curve u8, bank ID u32. Event (4): count u32, action IDs (must be
  loaded already).
- Streamed PCM: `CAkSrcFilePCM::ParseHeader` → `AkFileParser::Parse` (RIFF/WAVE or RIFX, fmt, optional
  cue/smpl/akd, data) and requires `wFormatTag == 0xFFFE`.

## Low-level I/O

`UFG::LowLevelIODispatcher` (file location resolver, up to 3 devices) → `WwiseFilePackageLowLevelIO<
WwiseDefaultIOHookDeferred>::Open(id)`: looks the ID up in the loaded packages' bank/stream tables, opens
it through `StreamFileWrapper`, fills `AkFileDesc` (size, start block, `uCustomParamSize` = block size,
`pCustomParam` = priority, device). Reads: `WwiseDefaultIOHookDeferred::Read` queues a UFG stream read whose
completion calls `AkAsyncIOTransferInfo::pCallback(info, AK_Success)`. `GetBlockSize` returns
`uCustomParamSize`; `Close` treats a non-zero `uCustomParamSize` as a packaged file. The deferred device
calls Read from `CAkDeviceDeferredLinedUp::ExecuteTask` → `CAkLowLevelTransferDeferred::Execute`, which
marks the transfer after Read returns, so completions must come later from another thread.

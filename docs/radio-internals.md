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
- The shipped file: 11 stations, ids 01-11 (11 = HKPD, `copScanner="true"`).

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

## Switching stations and the HUD widget (installed build, 2026-10-23)

- Station order is `sm_stationList` (file order): 01-11, then SDRadio's 12 and 13. `GetNextStationId` /
  `GetPrevStationId` wrap around the list and skip cop-scanner stations (`RadioStation` +0x169) unless the radio
  is a scanner (`m_radioType`), and vice versa. From 13, "next" goes to 01 H-KLUB; from 01, "previous" to 13.
- `Radio::HandleInputs` (each `Radio::Update`): on `NextRadioStationReleased` / `PrevRadioStationReleased`, if
  the HUD widget `IsActive()` (`mState` not 0 and not fading out) it calls the virtual `NextStation(true)` /
  `PrevStation(true)`; otherwise it only plays the click (`PlayClick`) and calls
  `UIHKRadioStationWidget::ChangeStation(m_curStation->m_id)` to show the widget. Holding the buttons (> 0.4 s)
  changes the volume instead.
- `Radio::NextStation` / `PrevStation(displayStationId)` act only while flag 0x01 (on) is set and 0x20 / 0x40
  are clear (radio flags byte +0x158): `StopTrack(500 ms)`, Get{Next,Prev}StationId (unload the old bank, load
  the new), click, `StartTrack` (flag 0x08), then set 0x20 (next) or 0x40 (previous). `Radio::Update` clears
  0x20 / 0x40 with a `ChangeStation` on the widget, and starts the track (0x08), once the new station's bank is
  loaded (or it is the scanner), the player is in a vehicle and 0.3 s have passed. Every
  `sm_numStations - 1` consecutive steps one way, `AdvanceAllStations(180 s)`.
- `Radio::SetStation(index)` (callers: `VehicleInteractTask::Begin`, the Skookum methods
  `TSVehicle::set_radio_station` / `set_player_radio_station`, `VehicleAudioComponent::SetRadioStation` from its
  `Init`): entering a car logs two, the car's station then the player's last one. `RadioFullyControlled` overrides
  Next/PrevStation to call the base and `SetupScannerAndRadio` (police scanner audio on/off).
- `UIHKRadioStationWidget` (+0 `mState`: 0 hidden, 1 `STATE_SHOULD_ACTIVATE`, 2 shown, 3 `STATE_FADING_OUT`; 0 and 2 are unnamed in the PDB; +4 `mChanged`, +5
  `mVisible`, +6 `mShouldRefreshTextures`, +7 `mWaitingForTexture`, +8 `mCurrentStation` (index = id - 1),
  +0xC `mTimeSinceLastInput`, +0x18 `mStationData` (texture, texturePack per station), +0x28
  `mLoadedTexturePack`). `ChangeStation(id)` resets the input timer and calls
  `LoadTextures(mCurrentStation, id - 1)`: a different pack is released and the new one queued
  (`mCurrentStation` = new index, `mWaitingForTexture`; `TextureLoadedCallback(handle, index)` activates the
  widget if the index still is `mCurrentStation`); the same pack activates the widget **only if the new index
  already is `mCurrentStation`**, else nothing happens. With one pack per game station that never mattered;
  SDRadio's two stations share one, which stuck the radio (SDRadio hooks LoadTextures, see CLAUDE.md).
- `UIHKRadioStationWidget::Update`: in state 2, `Deactivate` (fade out) 4 s after the last input; a texture
  wait over 2 s gives up with an empty texture; while `mChanged`, `Flash_SetVisible(mVisible)` and, if
  visible, texture, `Flash_Intro` (state 1 -> 2), `Flash_SetSongTitle`, `Flash_SetStationName`.
  `HandleNewSong` (from `Radio::DoStartTrack`) only sets `mChanged`, so a new song shows while the widget is up.
- `RadioFullyControlled::GetCurrentlyPlayingSong`: `m_curStation` (+0x148) -> `m_currentlyPlaying` (+0x120)
  ? `m_currentlyPlayingAssetName` (qString at +0xF0) : "". Only `Flash_SetSongTitle` calls it.

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

The names, recovered 2026-11-02 by hashing the exe's strings: 3026679070 is **`game_sim_time_scale`** (so the
radio effect is bypassed at normal speed and only applies in slow motion, below 0.95), 1240670792 is **`distance`**,
2420253395 unknown. Volume curves with scaling 2 store gain - 1: -1 is silence (-0.996 ≈ -48 dB), which fits
`master_radio_volume` 0 muting; read that way, not checked in Wwise's code.

## The radio's volume outside the car (installed build, 2026-11-02)

What a player hears getting out and in; `AppSound = direct` follows the first three (CLAUDE.md).

- **The crossfades getting out and in** (what the user meant, 2026-11-02; first missed by looking at `ms_vehicle`
  only): `VehicleAudioComponent::Update` stops the engine (`eEVENT_STOP`) and calls `TurnOffRadio` once the vehicle
  isn't driven (flag 0x02 of +636 clear while 0x01 set); `Radio::TurnOffRadio` sets the global state
  `ms_player_radio` = `radio_off` (radio_car: -96 dB, transition 1.5 s) and `StopTrack(500)` (the event's own 0.5 s
  stop fade). `HumanDriverComponent::Update` (also `RemoteDriverComponent`, Skookum `start_radio`) calls
  `VehicleAudioComponent::TurnOnRadio` -> `Radio::TurnOnRadio`: `radio_on` (2.5 s back from -96 dB), the volume
  option re-applied, `StartTrack`. Every game wrapper ends in `AK::SoundEngine::SetState(group, state)` (the
  2-argument overload; it maps state 748895195, "none", to 0). Wwise interpolating the state's volume in dB is assumed.
- **`distance`**: `AudioEntity::AudioEntityUpdate` sets it on every audio entity (`WwiseInterface::SetWwiseRtpcValue
  (s_uDistanceUID, d, this)`): the listener-to-entity distance in meters, every frame below 25 m, then every
  0..0.5 s (25-50 m) or 0.5 s. `RadioFullyControlled::AudioEntityUpdate` moves the radio entity to the car
  (`m_pVehAudComponent`'s matrix), and turns the radio off itself only once the car component is gone. The top
  node's curve: 0 dB up to 85 m, S-curve to ≈ -48 dB at 100 m; it matters only while the radio plays with nobody
  driving (the car is off the player otherwise).
- **`master_radio_volume`** (1290242000, `Radio::sm_radioVolumeRtpcId`): `Radio::SetRadioVolume` clamps the option
  to 0..10 and sets it on the radio entity and globally (object ~0); radio_car's curve: (0, -1) to (10, 0) linear.
- **radio_car** (Init.bnk, bus 3551958221, parent master_music): RTPC `health` (3677180323) on the LPF, 75 at 0 to
  0 from 60 on (low health muffles the radio); state groups `ms_vehicle` (an unnamed state 770684599 -3 dB,
  `in_vehicle_ramming` LPF 25 / -3 dB; **`on_foot` changes nothing**), 3538277082 (unnamed state 1875627670: +3 dB),
  3813406269 (no states), `ms_player_radio` (`radio_off` -96 dB; `Radio::TurnOffRadio`/`TurnOnRadio`). STMG
  transition times: `ms_vehicle` in_vehicle <-> on_foot 2 s, `ms_player_radio` on -> off 1.5 s, off -> on 2.5 s.
  `ms_vehicle` is set by `VehicleAudioComponent::SetDriverType` (`on_foot` when the last player driver leaves);
  for the radio, `ms_player_radio` is the one that fades.
- **Obstruction / occlusion**: `AudioEntityUpdate` calls `AK::SoundEngine::SetObjectObstructionAndOcclusion` for each
  entity from `SetObsOccValues` (raycasts), easing at `TiDo::sm_occlusionRate`. Whether the radio gets any is not
  known yet: SDRadio logs it for the radio's object (`radio: obstruction`).
- **The pause menu** (2026-11-02): `TidoGame::EnterAudioState` / `ExitAudioState` post Wwise events through
  `AudioEntity::CreateAndPlayEvent` (`SetMix` and `PlayDLCHudEvent` do too; the decompiler shows their string
  literals shifted, the events' names come from the banks, bank 1777182227): **`pause_game`** = Pause All Except
  (action 0x0208, transition 1000 ms, curve 4 linear; exceptions: Sound 343881207 in bank 887576358 and an object
  1048063247 in no bank) + SetState group 3813406269 -> 319258907; **`resume_game`** = Resume All (0x0304, no
  transition) + that group -> 984691642 (also `loadfinish`); `resume_all` = Resume All (0x0305); `insta_pause` /
  `insta_resume` pause/resume only bus 3444197610. Group 3813406269 silences (-96 dB) master_aux, ambient_music,
  karaoke_player and other buses per state, transition 1 s by default; radio_car has the group with no states. So
  the radio fades out over 1 s and is paused (its stream's reads stop once Wwise's buffer is full, which in direct
  mode, where nothing is heard from it, never paused the app), and resumes at once while the state's buses come back
  over 1 s.
- Not looked at: HDR / ducking of master_music under dialogue.

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
  cue/smpl/akd, data) and requires `wFormatTag == 0xFFFE`. Any `dwChannelMask` is taken; how a 2D source's
  channels reach the speakers, and which masks that works for, is in CLAUDE.md, "Multichannel tracks".

## Low-level I/O

`UFG::LowLevelIODispatcher` (file location resolver, up to 3 devices) → `WwiseFilePackageLowLevelIO<
WwiseDefaultIOHookDeferred>::Open(id)`: looks the ID up in the loaded packages' bank/stream tables, opens
it through `StreamFileWrapper`, fills `AkFileDesc` (size, start block, `uCustomParamSize` = block size,
`pCustomParam` = priority, device). Reads: `WwiseDefaultIOHookDeferred::Read` queues a UFG stream read whose
completion calls `AkAsyncIOTransferInfo::pCallback(info, AK_Success)`. `GetBlockSize` returns
`uCustomParamSize`; `Close` treats a non-zero `uCustomParamSize` as a packaged file. The deferred device
calls Read from `CAkDeviceDeferredLinedUp::ExecuteTask` → `CAkLowLevelTransferDeferred::Execute`, which
marks the transfer after Read returns, so completions must come later from another thread.

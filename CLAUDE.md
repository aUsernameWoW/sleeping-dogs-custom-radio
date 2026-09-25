# SDRadio

Goal: a new car-radio station in Sleeping Dogs DE that plays the user's own music from a folder, and that
behaves like the game's stations in every way the player can notice: appears in the station cycle with a
HUD logo and name, shows "artist - title", advances to the next track by itself, goes through the car radio's
bus/effect/volume RTPCs, pauses/ducks with the game. Started 2026-09-24 ("route B": inside Wwise, not a
separate audio output). Long-form reverse-engineering notes are in `docs/radio-internals.md`.

**Must also run under Wine/Proton/CrossOver** (Linux, macOS; the user tests on both): no Windows-only
service on the main path. Decoding is bundled (dr_libs, stb_vorbis), tags are parsed by hand; Media
Foundation and the shell property store are only a fallback for formats nothing bundled handles, and the
log names the backend of each track.

Status (2026-09-24): **works in game on Windows** (user: "works as expected"). First test log (two FLACs,
48 and 44.1 kHz): all hooks found in the installed build, bank loads with result 1 (default pool -1) on every
switch to the station, events play, a whole 3.5-min FLAC decodes in ~110 ms, longest read wait 47 ms (only
at stream start), tracks alternate on end-of-track, and resuming the station starts mid-track (the game's
`SeekMS(m_currentTrackTimer)`: the station "kept playing" meanwhile) without trouble. Not yet tried: MP3/OGG/
M4A in game, Chinese titles on the HUD, Linux (Proton) and macOS (CrossOver/Wine).

## How it works

The game's radio is data-driven end to end, so the mod adds data and serves files; no game logic is patched.

1. **Station list** — `UFG::Radio::LoadRadioStationData` parses `Data\Audio\Radios.xml`, which lives
   LZ-compressed in `Global.big` → `data\global\xmlcache\XML_CacheList.bin` and is handed out by
   `SimpleXML::XMLCache::ExtractFromCache` (hooked). The hook appends a `<Station>` (id = max + 1 = 12,
   chanceTrack 100, no ads/DJs, borrowed logo) with one `<Track>` per music file and returns a buffer from
   the game's own `qMalloc` (pugixml frees it). The HUD widget and station cycling follow automatically.
2. **Bank** — `RadioStation` loads `mus_radio_station_<id>` (FNV-1 ID) and `Radio::Update` waits for
   `SoundBankManager::BankLoaded` before playing. The bank is generated at runtime (`core/bank.cc`) and
   served by the I/O hooks: per track a streamed PCM Sound + Play action + event
   `play_station_<id>_track_<k>` (what `TrackAsset` posts), under an actor-mixer that repeats the radio's
   shared top node (music switch 992991330 in every station bank) field for field: effect 287224143 (copied
   verbatim), bus `radio_car` 3551958221, priority, 2D, advanced settings, three RTPCs.
3. **Streams** — `UFG::LowLevelIODispatcher::Open` (by file ID), `WwiseDefaultIOHookDeferred::Read` and
   `WwiseFilePackageLowLevelIO::Close` are hooked; our IDs get a virtual file (`core/stream_io.cc`): the
   bank bytes, or a RIFF/WAVE_FORMAT_EXTENSIBLE 16-bit stereo image of the track at its own rate, decoded on
   a thread per open file. Reads are queued and completed from a completion thread once the decoder has the
   bytes (the transfer callback must not run inside Read: `CAkLowLevelTransferDeferred::Execute` sets a
   flag on the transfer after Read returns).
4. `TrackFinishCallback` (end of event) starts the next track; `GetNextTrack` picks randomly, avoiding
   recently played ones. Nothing mod-side.

## Files

- `dllmain.cc` — config, log, starts the library scan (thread), `hooks::Install()`.
- `core/hooks.*` — signatures + hooks (ExtractFromCache, qMalloc via its call site, dispatcher Open,
  deferred Read, package Close) and diagnostic hooks (BankLoadCallback, CreateAndPlayEvent: log only for
  our bank/events). Learns the streaming device ID from the first successful open.
- `core/radios.*` — Radios.xml: next station ID, append the station (escaping, name ≤ 63 bytes).
- `core/bank.*` — the generated bank (IDs, object layouts, top-node parameters). Pure.
- `core/library.*` — scan the music folder (sorted, ≤ 255 tracks: `TrackAsset` index is a byte), probe
  each file (format, exact length, tags).
- `core/decoder.*` — bundled backends (dr_mp3/dr_flac/dr_wav/stb_vorbis) + `ToStereo`;
  `core/decoder_mf.cc` — Media Foundation fallback (delay-loaded) with shell-property tags.
- `core/tags.*` — ID3v2.2-2.4/ID3v1, Vorbis comments, RIFF INFO; legacy text: UTF-8, else GBK, else Latin-1.
- `core/stream_io.*` — virtual files, decoder threads, completion thread.
- `core/third_party.c` — dr_libs implementations (C, warnings off); `stb_vorbis.c` is compiled directly.
- `core/ak.hh` — Wwise low-level I/O structs (legacy PDB layouts). `core/scan.*`, `core/log.*` from SDAtmos.
- `core/config.*` — `SDRadio.ini`, parsed as UTF-8 by hand (GetPrivateProfileString would read a BOM-less
  file as ANSI and mangle a Chinese station name or path).
- Tests: `bank_test` (reads the bank back with ported Wwise readers; mixer params byte-identical to the
  game's top node; writes `station12.bnk`), `radios_test`, `tags_test`, `load_test`.

## Design decisions (don't undo without reason)

- **Inside Wwise via files, not voices**: serving a bank + PCM streams keeps every game behavior (bus,
  effects, RTPCs, pause, ducking, end-of-track callback, SDAtmos routing) without touching the render path.
- **Actor-mixer + Sounds, not interactive music**: music segments need their duration in the bank; plain
  streamed Sounds take it from the WAV header. Music nodes can't parent Sounds, hence our own mixer with the
  top node's parameters instead of reusing 992991330.
- **Object order** in HIRC: effect, sounds, mixer, actions, events (children before parents, actions before
  events), as the bank generator does. A node's override bus must exist at load time (radio_car is in
  Init.bnk); a missing parent is skipped silently.
- **Exact lengths**: the WAV header (and so the stream size Wwise knows) is written at Open from the scanned
  length; dr_mp3's frame count matches its decode exactly. Short decode → silence, long → cut.
- **Memory**: a track is one `new[]` of its PCM size (~42 MB for 4 min), committed as the decoder writes;
  freed when Wwise closes the stream.
- The original Radios.xml buffer is leaked once (~20 KB): its free isn't reachable from the hook.
- IDs: bank `mus_radio_station_<id>`, files/sounds/actions `sdradio_station_%02u_*` (FNV-1); none collide with
  the 45k IDs in SFX.pck + English(US).pck (checked for station 12, tracks 1-255).

## Facts established (legacy addresses; see docs/radio-internals.md)

- Stations: `RadioStation` 0x170 bytes, `m_name` char[64] (qSPrintf, unbounded → we cut to 63), `m_id` +0x24,
  `m_bankId` +0x118 (`mus_radio_station_%d`), `m_bIsCopScannerStation` +0x169 (HKPD, id 11, scanner mode only).
- Asset kinds: track (`play_station_%02u_track_%02u`), DJ, ad, ident (`play_station_%02u_ident`);
  `DetermineAssetType` returns track whenever rand(100) ≤ chanceTrack.
- HUD: `UIHKRadioStationWidget::ReadStationList` → `img://<TextureName>`, `Data\UI\<TexturePack>.perm.bin`;
  station name and song title go through `UI::LocalizeText` (unknown strings expected to pass through).
- Wwise bank format v88 details (LoadSource, SetNodeBaseParams, action/event layouts) are in the bank.hh
  header comment and docs; streamed PCM requires `wFormatTag == 0xFFFE` (`CAkSrcFilePCM::ParseHeader`).
- The game's banks declare feedback data (BKHD +12 = 1), so every node ends with a feedback-bus u32.
- External sources exist only in dialogue banks (`dlg_external`); not used.
- Signatures are unique in both builds (installed: ExtractFromCache +0x8A920, dispatcher Open +0x149B10,
  deferred Read +0xA36270, package Close +0x143B60, BankLoadCallback +0x143190, CreateAndPlayEvent +0x143EE0).

## Plan

1. First in-game test — **passed** 2026-09-24 (Windows).
2. Next: Linux/macOS runs (the user has both), Chinese titles on the HUD, own HUD logo (build a texture pack), long-track seek behavior (`SeekMS` on resume reads far ahead:
   decode-to-position latency), Chinese titles on the HUD (font glyphs), Wine/Proton/CrossOver runs.
3. Maybe: several stations (one per subfolder), shuffle/order option, M3U playlists.

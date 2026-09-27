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
   chanceTrack 100, no ads/DJs, our logo pack, see 5) with one `<Track>` per music file and returns a buffer from
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
5. **Logo** (`CustomLogo = 1`, default; 2026-09-26) — a UI texture pack of our own. At startup a thread
   (`logo.cc`) takes `logo.png` from the music folder, else the built-in `art/logo_512.png` (resource
   `LOGO`, `SDRadio.rc`), makes the silhouette the HUD shows (`logo_image.cc`: alpha, or luminance against
   the border for opaque pictures; 512×256 BC3, 7 mips) and writes it as `SDRadio-logo.perm.bin` /
   `.temp.bin` next to the .asi (`logo_pack.cc`, only if changed). Radios.xml names texture `Logo_SDRadio`
   in pack `..\..\plugins\SDRadio-logo` (the .asi folder relative to the game folder), so the widget loads
   `Data\UI\..\..\plugins\SDRadio-logo.perm.bin`: no archive has that path, so the game reads the loose
   files (see Facts). No hooks at all. With `CustomLogo = 0`, or if the pack can't be written (the .asi
   outside the game folder or in a non-ASCII path, a write error), the station borrows `TextureName` /
   `TexturePack` (a game station's logo).

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
  `core/decoder_mf.cc` — Media Foundation fallback (delay-loaded) with shell-property tags; logs once
  whether it started (under Wine it may be missing, or start but open nothing without GStreamer plugins).
- `core/tags.*` — ID3v2.2-2.4/ID3v1, Vorbis comments, RIFF INFO; legacy text: UTF-8, else GBK, else Latin-1.
- `core/stream_io.*` — virtual files, decoder threads, completion thread.
- `core/crash.*` — vectored exception handler (installed with logging on): logs the first 4 access
  violations as `crash: access violation (read/write/execute 0x…) at <module>+0x…` plus the stack as
  module+offset frames, and writes the first 2 as `SDRadio-crash-<n>.dmp` next to the .asi (open with
  `tools\dump.ps1`, needs the .pdb of that exact build). Observes only; handlers after it still run. It also
  catches the game's pre-existing exit crash (see the workspace CLAUDE.md), so one `crash:` block at exit is
  expected. Came from the shelved `abandoned/cover-art` branch, where it pinned a fault in one round.
- `core/logo.*` — which picture (music folder's `logo.png`, else the `LOGO` resource), the pack's path
  relative to the game folder, writing the pack; a thread from DllMain, which the Radios.xml hook waits for
  (≤ 10 s). `core/logo_image.*` — PNG → silhouette → 512×256 BC3 mip chain. `core/logo_pack.*` — the
  perm.bin/temp.bin bytes, the game's name hash and resource-file UID. Both pure. `SDRadio.rc` embeds
  `art/logo_512.png`.
- `core/third_party.c` — dr_libs and stb_image (PNG only) / stb_image_resize2 / stb_dxt implementations
  (C, warnings off); `stb_vorbis.c` is compiled directly.
- `core/ak.hh` — Wwise low-level I/O structs (legacy PDB layouts). `core/scan.*`, `core/log.*` from SDAtmos.
- `core/config.*` — `SDRadio.ini`, parsed as UTF-8 by hand (GetPrivateProfileString would read a BOM-less
  file as ANSI and mangle a Chinese station name or path).
- Tests: `bank_test` (reads the bank back with ported Wwise readers; mixer params byte-identical to the
  game's top node; writes `station12.bnk`), `radios_test`, `tags_test`, `load_test` (also: the pack is
  written, one folder down from the test's "game folder"), `logo_test` (the `LOGO` resource of the built
  .asi decodes to 7 BC3 levels whose alpha matches the PNG; the pack's fields; the UID functions against
  5 game packs; opaque pictures, fitting; includes stb by relative path since tests get no include paths),
  `library_test` (the .asi with music already in its folder: a Chinese subfolder and file name, WAV INFO tags
  in GBK and UTF-8, mono, a broken `.m4a` that the Media Foundation fallback must reject, a `.txt` to ignore;
  written for the Wine run below).
- `.github/workflows/build.yml` — CI like SDIMEFix's (documented in `mods\SDIMEFix\CLAUDE.md`), including
  its last job, `nexus` (each prerelease → the "SDRadio GitHub CI Build" file on Nexus). The
  `package` job builds `SDRadio.zip` for players: Ultimate ASI Loader as `dinput8.dll` (pinned in
  `.github/asi-loader.env`, `asi-loader.yml` opens a PR for a new release), `plugins\SDRadio.asi`, notices,
  and the music folder `plugins\SDRadio\` with `.github/PUT-YOUR-MUSIC-HERE.txt` (ignored by the scan: not
  an audio extension). Prereleases attach it under its plain name for README.md's
  `releases/latest/download/SDRadio.zip` link. `reference.yml` (also SDIMEFix's) proposes newer pins in
  `.github/reference.env`: MinHook by release tag, dr_libs and stb by commits that change the files we compile
  (the decoders read player-supplied files, so their fixes matter). The `reference\` cache key includes the
  hash of `reference.env`, so adding a file to a `_FILES` list (and to the sparse checkout) refetches.
  The `wine` job (2026-09-27) reruns the built `*_test.exe` on `ubuntu-24.04` under WineHQ's stable Wine
  (11.0) and Ubuntu's Wine 9, each test next to its own `.asi` copy as on Windows; the prerelease waits for
  it. Wine's `fixme:`/`err:` lines go to the job summary per test (the CRT's `AppPolicyGet*` fixmes are
  dropped). Its Wine has no GStreamer plugins, so Media Foundation starts but opens nothing
  (`0xC00D36BB`): a real M4A test would need `gstreamer1.0-plugins-base/good` + `gstreamer1.0-libav` and a
  fixture. No X server: fine while no test creates a window (bank_test's first start logs explorer's
  `nodrv_CreateWindow` errors, harmless); a window-creating test needs `xvfb-run`.
  First run: all 6 tests pass on both; the scan, GBK (code page 936), Chinese paths and the logo pack behave
  as on Windows.
- `.github/workflows/nexus-release.yml` — a GitHub **release** (a `build-<N>` prerelease un-ticked as
  prerelease) goes to the main file on [Nexus Mods](https://www.nexusmods.com/sleepingdogsdefinitiveedition/mods/175)
  (page published 2026-09-27). Both Nexus jobs are copies of SDIMEFix's, which documents how they work; keep
  them in step with SDIMEFix's and SDAtmos's (only the names differ, and SDRadio's file descriptions also name
  the music folder). File descriptions write paths with `/`: the Files tab drops backslashes. Settings: repo
  variables `NEXUS_MOD_ID`, `NEXUS_CI_FILE_ID`, `NEXUS_RELEASE_FILE_ID` (v3 IDs from a file's "Advanced"
  dialog, not the `175` in the URL) and secret `NEXUSMODS_API_KEY`. Current values: mod `14933601288367`,
  release file `8038689` (uploaded by hand with build-12 as "SDRadio build 12 (1414af9)"; the first release
  through the workflow renames it "SDRadio"), CI file not created yet (the job is skipped until
  `NEXUS_CI_FILE_ID` is set). The public GraphQL API (`api.nexusmods.com/v2/graphql`, no key) lists a mod's
  files (`modFiles(modId: 175, gameId: 3477)`) when the site sits behind a bot check.
- `art/` — the station logo **私家台** (neon tubes; 私家 as in 私家車, the player's own car and music), final
  2026-09-26; `logo_512.png` is embedded as the default logo, so rerun `neon.py` before building after a
  change to it. `neon.py` generates `logo.svg` from tube skeletons (traced over Noto Sans SC
  Bold; straight glass with fixed-radius bends; ends marked 'S'/'E' stop a GAP short of the tube in front,
  branches and crossings join; equal letter gaps; reports hairline slits, keep that empty) and renders it
  through `render.py`: `logo_512.png`, `logo_128.png` (black RGB, alpha = coverage, the game's logo
  convention: the HUD tints logos white, so only alpha shows; the glow is a soft alpha fringe) and
  `build\logo_preview.png` (the logo at 3x plus a mock of the RadioStations widget, with the metal backing
  from the workspace's `extracted\` if present). `neon.py debug` overlays each character on the Noto glyph.
  Needs Pillow and Edge/Chrome (headless screenshots): `..\..\tools\extract\build\venv\Scripts\python.exe
  art\neon.py` from the mod folder.
- `assets/` — `banner.png` (README header and the GitHub social preview, 1280×640, keep under 1 MB) and
  `icon.png` (512×512, transparent corners), both rendered from `assets/branding/logo.html` like SDIMEFix's
  and SDAtmos's (`?export=banner` / `?export=icon` in headless Edge with an absolute `--screenshot` path,
  `--window-size=W,H --default-background-color=00000000 --virtual-time-budget=10000`; the output is
  byte-identical run to run). Same family look with neon magenta. The title is `art/logo.svg` itself, an
  `<img>` lit by an SVG filter (thresholds away the logo's alpha halo, then glass, eroded core, glows);
  the icon is its 台 alone, cropped by a window computed from `neon.py`'s layout (the numbers are in the
  page: redo them if the lettering or letter gaps change). Re-render both after any logo change. The right
  side is the music folder feeding the RadioStations widget (backing redrawn in CSS, logo tinted white);
  its song title stays Latin until Chinese titles are verified on the HUD. Chosen 2026-09-27 over a
  spectrum, a font title and green neon.
- `README.md` — for players with no modding experience (step-by-step install, where the music goes, FAQ incl.
  the Proton launch option); keep build/internals out of it. `ADVANCED.md` — everything else (how it works,
  formats/backends, downloads, settings table, building, CI). Both bilingual (Chinese first).

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
- **Logo as a texture pack of our own, not a borrowed slot**: a first version (2026-09-26, worked in game)
  swapped the HKPD scanner's logo texture in `ID3D11Device::CreateTexture2D`, but the HKPD station is a
  regular choice in any police car (and players get one from the story), so it showed our logo too. The
  pack needs no hooks at all and borrows nothing. Don't go back to a D3D swap.
- **Pack files next to the .asi, reached with `..\..\`**, not in the game's `Data\UI`: the mod's files stay
  in `plugins\`. Rewritten only when their bytes change.
- **Silhouette only, no color**: the holder's white tint is in RadioStations.swf's PlaceObject; untinting it
  through Scaleform's Value API crashed on the cover-art branch. The user chose white-only (2026-09-26).
- **Bigger texture than the slot**: `onLoadInit` sets the holder to 128×64 whatever the image size, and
  Scaleform takes the size from the D3D texture, so 512×256 just renders sharper.

## Facts established (legacy addresses; see docs/radio-internals.md)

- Stations: `RadioStation` 0x170 bytes, `m_name` char[64] (qSPrintf, unbounded → we cut to 63), `m_id` +0x24,
  `m_bankId` +0x118 (`mus_radio_station_%d`), `m_bIsCopScannerStation` +0x169 (HKPD, id 11: selectable in police cars).
- Asset kinds: track (`play_station_%02u_track_%02u`), DJ, ad, ident (`play_station_%02u_ident`);
  `DetermineAssetType` returns track whenever rand(100) ≤ chanceTrack.
- HUD: `UIHKRadioStationWidget::ReadStationList` → `img://<TextureName>`, `Data\UI\<TexturePack>.perm.bin`;
  station name and song title go through `UI::LocalizeText` (unknown strings expected to pass through).
  Logos are 128×64 DXT5, black on transparent; RadioStations.swf places the holder clip with a color
  transform mult (0,0,0,1) add (255,255,255,0), so they show white. The widget sits on
  `Backing_9Slice_Metal_1`: logo at (0,0), name at (140,6), title at (140,30.4); it fades out 4 s after the
  last input. `tools\extract.ps1 gfx 'Screens.RadioStations'` shows the placements and cxforms.
- UI texture packs (legacy names): `UIHKRadioStationWidget::LoadTextures` → `UIScreenTextureManager::
  QueueTexturePackLoad("Data\UI\<pack>.perm.bin")` → `DataStreamer::QueueStream` → `OpenFiles` opens it and
  the `temp.bin` of the same name through `StreamFileWrapper::Open`: `BigFileSystem::GetFileInfoFromBigFile`
  (hash of the path), else `qOpen` → `PCFileDevice::FileOpen` → `CreateFileA` (loose file, relative to the
  game folder). `LoadStreamResources` registers the loaded temp.bin under `GenerateResourceFileUID(Texture,
  path)` = CRC-upper of the path from its last `data\` without slashes, seeded with the hash of
  `Illusion:Texture:`; `TexturePlat::OnLoad` finds the pixels by the texture's `mTextureDataHandle` UID
  (+0xB0) and `mImageDataPosition`. `img://<name>` resolves by `qStringHashUpper32(name)` (case-insensitive).
  Every UI texture has 1 mip, alpha state `A3833FDE`, and the same constant words (`logo_pack.cc`);
  mipmapped game textures pack levels tightly down to a 4-pixel side. `logo_test` has the verified UIDs.
- Wwise bank format v88 details (LoadSource, SetNodeBaseParams, action/event layouts) are in the bank.hh
  header comment and docs; streamed PCM requires `wFormatTag == 0xFFFE` (`CAkSrcFilePCM::ParseHeader`).
- The game's banks declare feedback data (BKHD +12 = 1), so every node ends with a feedback-bus u32.
- External sources exist only in dialogue banks (`dlg_external`); not used.
- Signatures are unique in both builds (installed: ExtractFromCache +0x8A920, dispatcher Open +0x149B10,
  deferred Read +0xA36270, package Close +0x143B60, BankLoadCallback +0x143190, CreateAndPlayEvent +0x143EE0).

## Plan

1. First in-game test — **passed** 2026-09-24 (Windows).
2. Own HUD logo (5) — **passed** 2026-09-26 (Windows, with FileRedirector.asi installed: no conflict): 私家台
   on our station, HKPD keeps its own. A player `logo.png` in game not yet tried (the code path is tested).
3. Next: Linux/macOS runs in game (the user has both; Wine/Proton/CrossOver; the tests already pass under
   Wine in CI), Chinese titles on the HUD (font
   glyphs), long-track seek behavior (`SeekMS` on resume reads far ahead: decode-to-position latency).
4. Maybe: several stations (one per subfolder), shuffle/order option, M3U playlists.

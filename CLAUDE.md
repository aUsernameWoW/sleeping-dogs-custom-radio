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
M4A in game, Linux (Proton) and macOS (CrossOver/Wine). Chinese titles show on the HUD (2026-10-23, live
station, "G.E.M.邓紫棋 - 自由的你") with the Chinese translation pack's SDCNLoc.asi installed; without it,
unknown (the glyphs may come from the pack's font).

Since 2026-10-23 also a **live station** (`[Live]`, off by default) that plays a real-time source: a test
signal or **internet radio** (MP3 / Ogg Vorbis over WinHTTP). Both play in game on Windows, with the
current song title on the HUD. Two bugs found in game on 2026-10-23 are fixed and confirmed there: game hitches
(reads that waited for the source) and the radio sticking after a switch between our two stations (a game HUD
bug our shared logo triggers). See "Live station" and "Internet radio" below. Since 2026-10-23 the live station can also play
**another app's sound** (`Source = app`, own code on `main`, Windows 11; works in game): see "App audio".
The older GPL experiment is parked on branch `gpl/external-audio` (GPL,
not to be merged without the user's licensing decision; its CLAUDE.md has the details).

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
- `core/stream_io.*` — virtual files, decoder threads, completion thread. `core/live.*` — the live station's
  ring, reader and sources (see "Live station"). `core/app_audio.*`, `core/now_playing.*` — another app's sound and its song title (see "App audio").
  `core/netradio.*`, `core/icy.*`, `core/stream_decoder.*`,
  `core/pacer.*` — internet radio (see "Internet radio").
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
  written for the Wine run below), `live_test` (the live ring and reader: preroll start, padding, rebuffering,
  jumps, frame phase), `icy_test` and `pacer_test` (internet radio's pure parts, see there), and
  `netradio_manual` (compiled only: a real station with Wwise's reads emulated; needs the network).
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
  them in step with SDIMEFix's and SDAtmos's (only the names differ). File descriptions write paths with `/`
  (the Files tab drops backslashes) and stay at most 255 characters like the other mods' (Nexus' limit is
  unverified; naming the music folder too would make the CI one 274). Settings: repo
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

## Live station (2026-10-22, `[Live] Enabled = 1`; works in game since 2026-10-23)

The path every real-time source (internet radio next, then Jellyfin, another app's audio) will share, so far
fed by a test signal (`Source = test`: a 0.15 s beep each second, 880 Hz on every fourth, -12 dBFS).

- A second station appended after the music one (id max + 1 if there's no music), same logo, **two tracks** on
  the same stream: `GetNextTrack` avoids recently played tracks and might find none with one. `BuildStations`
  /`AddStation` in hooks.cc; `streamio::Register` now takes one call per station (banks by ID).
- `core/live.*`: the source pushes 48 kHz 16-bit stereo into a 10 s `Ring` (absolute byte positions). A live
  track's WAV header declares an hour of data (`live::kFrames`, see why there). Wwise's reads
  ignore the file position and take the next ring bytes (`Reader`), starting 5 s behind the newest audio
  (`kPrerollMs`: Wwise's opening burst takes ~3.4 s, leaving ~1.6 s for the steady reads). **Reads never wait**
  (see "Hitches and a stuck station" below): one the ring can't fill gets what there is plus silence, and the
  reader rebuffers (silence) until 2 s (`kResumeMs`) are there, then goes on where it stopped (the cursor
  moves only over real audio); a fresh source reads as silence until 2 s have arrived, then starts at its
  oldest audio; a reader more than 8 s behind jumps back to the preroll distance; after a seek (the game's
  resume `SeekMS`) the cursor takes the position's frame phase. Live reads complete in issue order per file
  (the completer fills them under its lock, logs after unlocking). Each open logs its first 20
  requests/reads, every 100th, and every underrun or jump; close logs reads, padded silence, underruns,
  jumps and seeks.
- The source runs while a live file is open and stops 10 s after the last close (station switches and track
  starts reopen it); `Ring::Reset` on start. `live_test` covers ring and reader.
- **In game (2026-10-23, Windows)**: the first build declared ~4 GB (~6.2 h) of data and played a looping,
  dial-up-like noise; Wwise read only 9 times in 87 s. With one hour it plays clean beeps through the radio
  effect, switching away and back works. How Wwise reads: 128 KB (682 ms) per read; the header (position 0),
  then a seek to the game's track timer, a burst of 4 reads (~2.7 s), then one every ~0.69 s, each issued
  while ~2 s are still buffered; 0 ms padded in steady state, ~2.7 s at each start (the source starts with
  the open, so the burst finds an empty ring). With the old 250 ms wait every steady-state read waited it out
  and just made it, hence 1.5 s now. Station 13's file IDs weren't checked against SFX.pck (12's were).
- **Stutter with internet radio (2026-10-23)**: the preroll was 1 s, and a source that starts with the open
  had nothing for Wwise's burst anyway, so the stream ran at the live edge: every steady-state read waited a
  whole chunk (671-704 ms), Wwise had ~no cushion, and playback stuttered (no pacer underruns: the network was
  fine). The test signal had hidden it: its first reads padded ~2.7 s of silence, which became the cushion.
  First fixed by a 3 s preroll the first read waited for, plus the pacer handing its first 3 s over at once;
  that didn't hold (next item).
- **Hitches (2026-10-23)**: the user reported the game hitching now and then and, after listening a while, the
  station not switching (that turned out to be something else: next item). The log showed the 3 s preroll
  eaten by the burst (reads 2-4 at once), then every steady read waiting ~0.4 s at the live edge again. Waiting itself was the bug: the game's
  streaming device is a `CAkDeviceDeferredLinedUp` shared by every game stream and bank load (ours included),
  and each pending transfer holds one of its `uMaxConcurrentIO` slots (`CAkIOThread::IncrementIOCount` resets
  the max-IO gate event, which `IOSchedThread` waits on before scheduling anything). So a read waiting 0.4 s
  (10 s at a start, 1.5 s each while the network is down, with Wwise queueing more) delays the game's own
  streams; enough of them could even keep the next station's bank from loading (`Radio::Update` waits for
  it), which was first taken for the stuck switch, wrongly. Now reads complete
  at once and the cushion lives in the ring. The pacer keeps 1 s (`kRebufferMs`) of its own when it starts, so
  the first late packet isn't an underrun. A stopped source's feeder no longer pushes while it winds down
  (it could mix into the next source's ring after a quick switch back). `netradio_manual` with Radio
  Paradise, 40 s: no wait, no underrun, no jump, lowest Wwise buffer 2.7 s; ~4 s of silence at a cold start
  (connecting + 2 s), none when the source is already running. In game afterwards: 0 ms waits, ~2.2 s behind
  live, no underruns, no hitches (user). One expected edge: back on the station within a second or two of its
  source starting (12 -> 13 -> 1 -> 13 quickly), the ring holds less than the preroll, Wwise's burst takes all
  of it and the next read rebuffers once (~0.4 s of silence, logged as "source late: rebuffering").
- **Stuck after switching between our stations (2026-10-23, fix confirmed in game)**: the radio sometimes
  wouldn't leave the live station; the user heard the click on every press, and re-entering the car fixed it. Station-switch logging
  (hooks on `Radio::NextStation` / `PrevStation` / `SetStation`, every station's bank callback) showed only a few
  presses reaching NextStation/PrevStation. `Radio::HandleInputs` switches only while the HUD widget is active
  (`IsActive`: state not 0 or fading out); on a hidden widget a press plays the click and calls
  `UIHKRadioStationWidget::ChangeStation(id)` to show it, which calls `LoadTextures(mCurrentStation, id - 1)`.
  When the new station's texture pack equals the loaded one, LoadTextures activates the widget only if the new
  index already is `mCurrentStation`, else does nothing and leaves `mCurrentStation` behind. Every game station
  has its own pack; ours share one (our logo pack, or the borrowed `TexturePack`), so after a switch between
  them (12 <-> 13) the widget never activated again until the car was re-entered. hooks.cc hooks LoadTextures:
  unless a pack load is in flight (`mWaitingForTexture`, whose callback matches its index against
  `mCurrentStation`) it sets `mCurrentStation` to the new index first; with a different pack the game sets it
  anyway, so game stations are unaffected. The switch logging stays (diagnostics). Lesson: the first
  explanation (waiting reads) was inferred without the log and sounded right; the switch log disproved it in one
  round. Log first, then conclude. Details of switching and the widget: docs/radio-internals.md.

## Internet radio (2026-10-23, `[Live] Source = radio`, `Url = ...`; plays in game on Windows)

In game (2026-10-23, Radio Paradise MP3): plays and switches fine, but stuttered now and then, then made the
game hitch and the station stick; the causes and the fix (not yet confirmed in game) are under "Live station". README's FAQ explains the ini for players.

The live station's first real source. All own code (no GPL), works under Wine as far as WinHTTP does.

- `core/netradio.*`: WinHTTP GET with `Icy-MetaData: 1` (system proxy via `WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY`,
  falling back to the default; 10 s timeouts bound every blocking call), playlists followed (≤ 3 hops), then
  ICY demux → decoder → resampler → `Pacer`. A feeder thread empties the pacer into the live ring every 10 ms by
  the clock. The network side stops reading while 15 s are queued. Reconnects with backoff (2 s doubling to
  30 s, reset after a connection that decoded); an unsupported format gives up for good.
- `core/icy.*` (pure, `icy_test`): ICY metadata demux, `StreamTitle` (to UTF-8 via `tags::LegacyToUtf8`: Latin-1
  and GBK titles exist), M3U/PLS, format detection (Ogg's first page tells Vorbis from Opus / FLAC).
- `core/stream_decoder.*`: MP3 through dr_mp3's low-level push API (`drmp3dec_decode_frame`, 16 KB before the
  first sync), Ogg Vorbis through stb_vorbis pushdata (reopened at each new logical stream's BOS page, where
  Icecast puts the next song's comments: the title for Ogg). A decoder that hasn't produced a sample after
  256 KB refuses the stream. `Resampler`: linear, to 48 kHz. Its own `ToStereo` (no decoder.cc in tests).
- `core/pacer.*` (pure, `pacer_test`, also the resampler and junk refusal): starts after 3 s buffered (1 s
  after an underrun) and hands over all but 1 s, up to the live preroll, at once (servers burst on connect, so
  a fresh reader's 2 s are there ~1 s after connecting), then by the clock; titles are marked at the audio
  position where they arrived and released when that audio goes out (up to 15 s queued, so ICY titles would
  otherwise be early).
- **HUD song title** (2026-10-23, works in game): Radios.xml fixes each track's title at startup (the live
  tracks': the URL's host). `UIHKRadioStationWidget::Flash_SetSongTitle` shows
  `LocalizeText(RadioFullyControlled::GetCurrentlyPlayingSong(sm_playerRadio))` (the current asset's name,
  `RadioStation::m_currentlyPlayingAssetName`) and invokes `mc_RadioStations.SetSongTitle`; hooks.cc hooks the
  getter (reached through Flash_SetSongTitle's first call; `m_curStation` +0x148 read from its first
  instruction, which is checked) and returns `live::NowPlaying()` on the live station (`RadioStation::m_id`
  +0x24), else the original. The widget refreshes its texts in `Update` only while `mChanged` (+4) is set and
  `mVisible` (+5); the game sets it from `Radio::DoStartTrack` via `HandleNewSong`. A hook on `Update` sets it
  when the title's generation changed while visible. So, as for game stations, a new song shows if the widget is
  up (it fades 4 s after the last input) or the next time it opens; it doesn't pop up by itself. The title is
  UTF-8 from ICY/Vorbis or the media session; Chinese shows (with SDCNLoc installed, see the top). Log:
  `hud: song title`.
- `tests/netradio_manual.cc <url> [s]` (compiled, not run; needs the network): plays a station into the ring and
  prints the log and the fill rate, with Wwise emulated as the game reads (128 KB, one at a time, ~2.7 s kept):
  reads, waits, padded silence, starvation, the lowest buffer. After the stutter fix, Radio Paradise for 40 s:
  first audio after 1.3 s, no silence, no starvation, never under 2.3 s buffered. 2026-10-23 on Windows: Radio
  Paradise MP3 128k (44.1 kHz, ICY titles), Radio
  Paradise Ogg Vorbis 192k (title from the comments), NPO Radio 1 MP3 (HTTP/1.0, 48 kHz), Chinese Music World
  Ogg: all at rate 1.000 ± 0.004; a walmradio Opus stream says Opus isn't supported and stops. Run it outside the
  Bash tool's sandbox: inside it direct connections fail (the tool's proxy is in env vars, which WinHTTP doesn't
  read). SomaFM refused this machine (403 / redirect to an alert MP3, its exit IP behind the user's proxy).
- Not supported yet: AAC (many stations), Opus, Ogg FLAC; SHOUTcast 1's `ICY 200 OK` reply (not HTTP, WinHTTP
  rejects it; would need a raw socket).

## App audio (2026-10-23, `[Live] Source = app`, `App = <process name>`; works in game)

The sound of another program (NetEase Cloud Music, Spotify, a browser) on the live station. The user chose
(2026-10-23) to write it on `main` as own code rather than merge the GPL branch, which stays parked. Someday
probably also a Spotify Connect receiver like FH6's, through librespot (MIT; needs Spotify Premium, and is an
unofficial client, against Spotify's terms: weigh that then).

- `core/app_audio.*`, written from Microsoft's documentation of the API only (not from the GPL branch's code):
  `FindProcess` takes the root of the process tree with that exe name (a same-named process whose parent isn't
  one; Electron apps play from a child); `Capture` activates process loopback
  (`ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK)` with `AUDIOCLIENT_ACTIVATION_PARAMS`,
  include the target's tree; the function looked up in mmdevapi.dll at run time, the handler agile) and
  initializes it shared, loopback + event + autoconvert, asking for 48 kHz 16-bit stereo (process loopback has
  no mix format of its own), 200 ms buffer; `Run` (the live source) waits for the program (retries every 2 s,
  5 s after a failed capture), captures until it exits or the capture fails, and pads silence by the clock
  once no packet has come for 200 ms (packets only come while the app renders). Every 10 s the log has the
  captured ms, peak dBFS and padded silence.
- **HUD title** (`core/now_playing.*`, 2026-10-23): a thread started by `appaudio::Run` polls the system media
  sessions every 2 s (`GlobalSystemMediaTransportControlsSessionManager`, C++/WinRT from the Windows SDK, MIT,
  in THIRD-PARTY-NOTICES), takes the session whose `SourceAppUserModelId` contains the process name without
  `.exe` (case-insensitive; one that plays wins), and sets `live::SetNowPlaying("Artist - Title")`, which the
  HUD hooks already show; no match leaves the process name. Every async call waits at most 5 s (a broken
  media service has hung `RequestAsync`); `REGDB_E_CLASSNOTREG` (Wine, pre-1809) ends the thread after one log
  line. The sessions are logged when they change; no matching one logs a hint about NetEase's "SMTC" setting
  (off by default there; Spotify and browsers always publish). The manager is released on that thread before
  `CoUninitialize` (the GPL branch saw a hang releasing one at DLL detach). `.asi`: `runtimeobject.lib`, and
  `api-ms-win-core-winrt-l1-1-0` / `-winrt-error-l1-1-1` delay-loaded (dumpbin: the only WinRT API sets it
  imports; `now_playing` checks both load before any WinRT call). Tests link `oleaut32.lib` too (C++/WinRT's
  error info) and delay-load the same. Offline with NetEase (SMTC on): "G.E.M.邓紫棋 - 自由的你"
  (`app_audio_manual titles cloudmusic.exe`); in game the same on the HUD, Chinese included (2026-10-23).
- **NetEase Cloud Music ignores Windows' per-app output device** (2026-10-23, user): it plays to the device
  chosen in its own settings, so it had to be set there to "WASAPI: Steam Streaming Speakers"; the volume
  mixer's choice alone didn't move it. Other apps (Spotify, browsers) follow the mixer's choice as far as
  known. So setting the device from the mod (policy config, see Next) wouldn't help NetEase either.
- **Requirement: Windows 10 build 20348+** (`AUDIOCLIENT_ACTIVATION_PARAMS`), so in practice Windows 11 (or
  Server 2022+). Wine: activation fails (the GPL branch's CI saw `0x80070002`); logged, the station stays silent.
- **The mute question, answered 2026-10-23 on Windows 11 (`tests/app_audio_manual.cc`, no game)**: a child
  playing a -20 dBFS sine on a spare output device (Steam Streaming Speakers) was captured at -20.0 dBFS, and
  still at -20.0 with that **device muted** (`IAudioEndpointVolume::SetMute`, restored after). Process loopback
  taps after the app's session volume (the GPL branch found a muted session captures silence) but before the
  endpoint. So players send the app to an unused output device (Settings > System > Sound > Volume mixer, the
  app's output device), or mute that device, and hear it only through the game's radio. Don't mute the app.
  `app_audio_manual list` shows the output devices, `<index> mute` reruns the experiment.
- **In game (2026-10-23, Windows 11, NetEase Cloud Music sent to Steam Streaming Speakers)**: plays through
  the radio, 79 reads in a row without padded silence; the quick-return rebuffer edge as for internet radio.
  The first try was silent because the user, unsure which of the two outputs they heard, had muted
  cloudmusic in the volume mixer: the log then shows a capture at ~-90 dBFS (no padded silence: the app keeps
  rendering, just muted). The docs now say it louder; a log/HUD warning when the app's session is muted
  (`IAudioSessionManager2` -> the process's `ISimpleAudioVolume::GetMute`) would catch it at once.
- `tests/app_audio_test.cc`: `FindProcess` (its own exe, case-insensitive; a missing one), and a capture that
  opens and reads or fails with a reason (Wine). On Windows 11 a silent process still delivers a packet.
- **Pausing the app while the station isn't played** (`[Live] PauseApp`, default 1; 2026-10-23, not yet tried
  in game; FH6 Universal Radio's idea): `live::Listening(3 s)` (a live file open and opened or read within
  3 s; `stream_io` calls `live::NoteRead` per live read; the pause menu stops reads but keeps the file open,
  as a 55 s gap in a log showed) drives `nowplaying`'s `Control`, every 250 ms on the session kept between
  refreshes: not listening for 1.5 s while the app plays -> `TryPauseAsync`, flag `gPausedByUs` (a static:
  the source run stops 10 s after the station is left, the next run must resume); listening again with the
  flag set -> clear it, and `TryPlayAsync` if still paused. A pause the player made is never undone; an app
  the player restarted meanwhile isn't paused again until the station is left again. The session's playback
  status is logged on change (4 playing, 5 paused). `app_audio_manual control <exe>`: 5 s not listening, 5 s
  listening (offline, NetEase was paused, so nothing to do: verify with it playing).
- Next: a warning when the app is muted or plays on the default device; maybe next/previous track from the
  radio (SMTC `TrySkipNextAsync`...); maybe set the app's output device from the mod (only through undocumented policy-config interfaces,
  as EarTrumpet does: risky).

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
   Wine in CI), Chinese titles on the HUD without SDCNLoc,
   long-track seek behavior (`SeekMS` on resume reads far ahead: decode-to-position latency).
4. Maybe: several stations (one per subfolder), shuffle/order option, M3U playlists.
5. Live station / internet radio (2026-10-23, see their sections; hitches, stuck switch and HUD title done and
   confirmed in game): next AAC streams
   (many stations; needs a decoder: Media Foundation can't take a byte stream as simply, a bundled one would
   need a license check), several stations or a station list, SHOUTcast 1's `ICY 200 OK` (raw socket). A
   browser dashboard was discussed (2026-10-22, like FH6 Universal Radio's): own code, localhost by default
   with a token and a Host check, LAN only as an option, no API that sets paths or starts programs (FH6's
   allows any origin and sets executable paths); `ShellExecute` to open it (Wine hands that to the Linux
   browser). WebView2: not needed, and it doesn't run under Wine.
6. Another app's audio: own code on `main` since 2026-10-23, works in game (see "App audio"); the HUD title
   from its media session too; next a warning when the app is muted in the mixer. The GPL branch `gpl/external-audio` (code from FH6 Universal Radio) is parked;
   keep rebasing it. Later maybe librespot (Spotify Connect).

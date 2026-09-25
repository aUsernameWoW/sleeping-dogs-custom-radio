# SDRadio

Goal: a new car-radio station in Sleeping Dogs DE that plays the user's own music from a folder, and that
behaves like the game's stations in every way the player can notice: appears in the station cycle with a
HUD logo and name, shows "artist - title", advances to the next track by itself, goes through the car radio's
bus/effect/volume RTPCs, pauses/ducks with the game. Started 2026-09-24 ("route B": inside Wwise, not a
separate audio output). Long-form reverse-engineering notes are in `docs/radio-internals.md`.

**Must also run under Wine/Proton/CrossOver** (Linux, macOS; the user tests on both): no Windows-only
service on the main path. Decoding is bundled (dr_libs, stb_vorbis; stb_image for cover art), tags and
embedded pictures are parsed by hand; Media Foundation and the shell property store are only a fallback
for formats nothing bundled handles, and the log names the backend of each track.

Status (2026-09-24): **works in game on Windows** (user: "works as expected"). First test log (two FLACs,
48 and 44.1 kHz): all hooks found in the installed build, bank loads with result 1 (default pool -1) on every
switch to the station, events play, a whole 3.5-min FLAC decodes in ~110 ms, longest read wait 47 ms (only
at stream start), tracks alternate on end-of-track, and resuming the station starts mid-track (the game's
`SeekMS(m_currentTrackTimer)`: the station "kept playing" meanwhile) without trouble. Not yet tried: MP3/OGG/
M4A in game, Chinese titles on the HUD, Linux (Proton) and macOS (CrossOver/Wine).

**Branch `abandoned/cover-art` (2026-09-25): shelved.** Cover art as the HUD logo (item 5 below). The user
dropped it: the logo is only on screen for a few seconds after switching stations, not worth touching
Scaleform internals for. What stands, from three in-game tests:

- Works: the D3D side (texture caught on every switch, 512×256 cover created, covers decoded and uploaded
  on the render thread), tags/pictures, `cover_image` (tested), the `crash.cc` diagnostics.
- Test 1: white square: RadioStations.swf's PlaceObject paints the holder white (see hud.hh).
- Test 2: the game died right at the first `SetTexture` after the swap, before any `hud:` line.
- Test 3 (SEH guard + crash.cc): read AV at `0xFFFFFFFFFFFFFFFF` in `SDHDShip.exe+0x6EEF5E`, called straight
  from `hud.cc`: that is inside `AS2ValueObjectInterface::GetCxform` (legacy 0x1406EF100, installed RVA
  0x6EEF00), although both vtables match the legacy PDB slot for slot. The guard caught it, but swallowing a
  fault inside Scaleform left it broken: 20 s later execution jumped to a stack address. So the Value /
  GetCxform call contract is wrong somehow (next step would have been reading the dump,
  `SDRadio-crash-1.dmp`, against the installed exe), and SEH guards around Scaleform are no fix.
- Less invasive ideas not tried: skip GetCxform and only SetCxform a hard-coded original; or replace the
  tint where it's defined (serve a patched RadioStations movie through the game's file layer, together with
  our own texture pack instead of the HKPD slot).

## How it works

The game's radio is data-driven end to end, so the mod adds data and serves files; no game logic is patched.
The one addition outside that is the cover-art logo (5): D3D11 calls plus Scaleform's public Value API,
no game code.

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
5. **Cover art as the logo** (`CoverArt = 1`, default; added 2026-09-25, see Status) — the
   station borrows `Logo_HKPDScanner` / `Radio_HKPDScanner_TexturePack` (the cop scanner's, seen only in
   scanner mode). `CreateAndPlayEvent` of our track k → `cover::Request(k)`: a worker takes the embedded
   picture (APIC/PIC, FLAC PICTURE, Vorbis METADATA_BLOCK_PICTURE), else cover/folder/front/logo.* in the
   track's folder or a parent up to the music folder, else blank, and makes a 512×256 BC3 image with mips
   (`cover_image.cc`: stb_image → stb_image_resize2 → stb_dxt). `d3d.cc` patches the game's
   `D3D11CreateDevice` import, then MinHooks the device's `CreateTexture2D` (recognizes the HKPD logo:
   128×64 BC3, 1 level, FNV-1a-64 of its blocks `B3D3AD52833FA849`, and creates ours instead, DEFAULT usage)
   and the immediate context's `PSSetShaderResources` (render thread: `UpdateSubresource` when the cover
   generation changed). No game RVAs; ReShade/DXVK see ordinary D3D11 calls. `hud.cc` hooks Scaleform's
   `Movie::Invoke` wrapper (signature): after `mc_RadioStations.SetTexture(t)` it sets the color transform
   of `mc_RadioStations.slot.holder` to identity if `t` is our logo, else back to its original (read once).

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
- `core/cover_image.*` — picture → logo image (fit into the middle 256² square, transparent sides, BC3 mip
  chain). Pure. `core/cover.*` — worker: which picture for which track, folder cache, generation counter.
  `core/d3d.*` — the D3D11 hooks above. `core/hud.*` — the holder's color transform through Scaleform's
  Value API (vtable offsets from the legacy PDB, in the header comment).
- `core/crash.*` — vectored exception handler: logs the first 4 access violations (kind, address, stack as
  module+offset) and writes the first 2 as `SDRadio-crash-<n>.dmp` next to the .asi (`tools\dump.ps1`).
  Observes only; installed with logging on.
- `core/third_party.c` — dr_libs and stb_image/stb_image_resize2/stb_dxt implementations (C, warnings off);
  `stb_vorbis.c` is compiled directly.
- `core/ak.hh` — Wwise low-level I/O structs (legacy PDB layouts). `core/scan.*`, `core/log.*` from SDAtmos.
- `core/config.*` — `SDRadio.ini`, parsed as UTF-8 by hand (GetPrivateProfileString would read a BOM-less
  file as ANSI and mangle a Chinese station name or path).
- Tests: `bank_test` (reads the bank back with ported Wwise readers; mixer params byte-identical to the
  game's top node; writes `station12.bnk`), `radios_test`, `tags_test` (also pictures), `cover_test`
  (BMP → logo, decodes the BC3 back; includes stb by relative path since tests get no include paths),
  `load_test`.

## Reading SDRadio.log

The user tests in game and sends `plugins\SDRadio.log`; every step leaves a line, so one round should tell
where things stop. In order:

| Line | Means | If it's missing or different |
|---|---|---|
| `scan: <function> at +0x…` (8×) | each signature found once | `0 matches` / `N matches`: the exe changed. Find the function again in IDA by the legacy name in `hooks.cc` and make a new signature (unique in both builds). |
| `hook: station hooks ready` | Wwise + XML hooks in | `game functions missing, no station`: see above |
| `d3d: D3D11CreateDevice import patched` | cover art armed | `CoverArt = 0`, or the import is gone |
| `library: k  artist - title  (backend, Hz, s)`, `library: N tracks ready` | the music folder | `skipping …`: no decoder for the file |
| `radios: station 12 "…" … logo Logo_HKPDScanner / …`, `radios: Radios.xml a -> b bytes` | station added to the list | `no music found`, or the scan took > 30 s |
| `cover: station logo <path>` | a logo image in the music folder | optional |
| `d3d: device … (flags …), context …; logo hooks ready` | once, at startup | **missing**: the game made its device before the .asi loaded, or not through its import; cover art can't work that way. `FAILED`: MinHook couldn't patch the method. |
| `io: game streaming device …` | Wwise I/O working | |
| `bank: mus_radio_station_12 loaded with result 1` | each switch to the station | result ≠ 1: bank rejected, see `bank_test` / docs |
| `d3d: HKPD logo texture replaced: 512x256, 10 mips (cover #n)` | each switch to the station (the widget reloads the pack) | `128x64 BC3 texture … isn't the HKPD logo` lines instead: the logo's bytes changed; update `kLogoHash` from `tools\extract.ps1 hash '^LOGO_HKPD'`. Neither line: the pack didn't load (check the TextureName/TexturePack line above). |
| `radio: post event … (track k) … playing` | the game starts track k | `FAILED`: event not in the loaded bank |
| `stream: open track …`, `stream: … decoded … in … ms`, `stream: close … longest wait …` | the track's PCM | a long wait at start: decoding slower than playback started |
| `cover: track k: embedded PNG, 1400x1400, ready in … ms` (or a folder file, or `none (blank logo)`) | the picture for track k | `doesn't decode`: a format stb_image lacks (WebP, 12-bit JPEG…); the folder is tried next |
| `hud: holder's own color transform: mult 0.00 0.00 0.00 1.00, add 1.00 1.00 1.00 0.00` | first logo change after startup | other values: the movie changed; `GetCxform FAILED` / `not a display object`: the path or the Value API offsets are wrong |
| `hud: logo untinted (cover art)` / `hud: logo tint restored` | each logo change (ours / another station's) | `SetCxform FAILED`: see above. White square although "untinted": something else re-tints the holder |
| `crash: access violation (read/write/execute 0x…) at <module>+0x…`, `crash:   #n <module>+0x…`, `crash: dump written` | a fault (handled or not) | read the frames; open the dump with `tools\dump.ps1 plugins\SDRadio-crash-1.dmp` (needs the .pdb of that exact build) |
| `hud: exception in the color-transform code …` | the SEH guard caught a fault in hud.cc | the `crash:` lines above it say where |
| `d3d: logo updated to cover #n` | written into the texture on the render thread | missing after a `cover:` line: no `PSSetShaderResources` on the immediate context since (unlikely), or the texture doesn't exist yet (it's created with the latest picture then) |

## Tools for re-deriving facts

All in the workspace (`tools\extract.ps1`, reads the installed game's archives):

- `xml 'radios\.xml$' OUTDIR` — the real Radios.xml from the XML cache (the cache holds it twice).
- `hash '^LOGO_'` — size, mips and FNV-1a-64 of level 0 of the radio logos (what `d3d.cc` matches).
- `gfx 'Screens.RadioStations' OUTDIR` — the widget's movie inflated plus an AS2 disassembly (`SetTexture`,
  `onLoadInit`) and its placements (the holder's white-tint color transform).
- `export '^LOGO_' OUTDIR` — the logos as PNG.
- The legacy exe + PDB + IDA database are in `reference\SDmodding\game-itself` (ida-pro-mcp); addresses in
  these docs are legacy ones, the signatures work on both builds.

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
- **Cover art by swapping pixels, not names**: the widget only re-reads the texture on a station switch
  (`LoadTextures` reloads the pack only if its name changes; `HandleNewSong` just refreshes the title), but
  Scaleform samples the same D3D texture every frame, so writing into it changes the logo at once. The
  HKPD slot because a borrowed regular station's logo would also show our cover on that station (same
  pack name → no reload). A texture pack of our own (served through the game's file layer) is the clean
  successor.
- **Untint the holder only for our logo**: the game's logos need the white tint (they're black), so the
  holder's color transform is switched per `SetTexture`, not removed once.
- **Bigger texture than the slot**: RadioStations.swf's `onLoadInit` sets the holder to 128×64 whatever the
  image size, and `Scaleform::Render::D3D1x::Texture::Initialize` takes its size from `GetDesc`, so 512×256
  just renders sharper.
- IDs: bank `mus_radio_station_<id>`, files/sounds/actions `sdradio_station_%02u_*` (FNV-1); none collide with
  the 45k IDs in SFX.pck + English(US).pck (checked for station 12, tracks 1-255).

## Facts established (legacy addresses; see docs/radio-internals.md)

- Stations: `RadioStation` 0x170 bytes, `m_name` char[64] (qSPrintf, unbounded → we cut to 63), `m_id` +0x24,
  `m_bankId` +0x118 (`mus_radio_station_%d`), `m_bIsCopScannerStation` +0x169 (HKPD, id 11, scanner mode only).
- Asset kinds: track (`play_station_%02u_track_%02u`), DJ, ad, ident (`play_station_%02u_ident`);
  `DetermineAssetType` returns track whenever rand(100) ≤ chanceTrack.
- HUD: `UIHKRadioStationWidget::ReadStationList` → `img://<TextureName>`, `Data\UI\<TexturePack>.perm.bin`;
  station name and song title go through `UI::LocalizeText` (unknown strings expected to pass through).
  Logos are 128×64 DXT5, black on transparent; RadioStations.swf places the holder clip with a color
  transform mult (0,0,0,1) add (255,255,255,0), so they show white (and so would our cover: hence hud.cc).
  It's in the PlaceObject tag, not the AS2: `tools\extract.ps1 gfx` lists placements with their cxforms.
  The widget fades out 4 s after the last input and doesn't pop up on a new song. Details in
  docs/radio-internals.md.
- Wwise bank format v88 details (LoadSource, SetNodeBaseParams, action/event layouts) are in the bank.hh
  header comment and docs; streamed PCM requires `wFormatTag == 0xFFFE` (`CAkSrcFilePCM::ParseHeader`).
- The game's banks declare feedback data (BKHD +12 = 1), so every node ends with a feedback-bus u32.
- External sources exist only in dialogue banks (`dlg_external`); not used.
- Signatures are unique in both builds (installed: ExtractFromCache +0x8A920, dispatcher Open +0x149B10,
  deferred Read +0xA36270, package Close +0x143B60, BankLoadCallback +0x143190, CreateAndPlayEvent +0x143EE0;
  Scaleform `Movie::Invoke` is unique in both too).

## Plan

1. First in-game test — **passed** 2026-09-24 (Windows).
2. Next: second in-game test of the cover-art logo (colors after the untint; does a track change update a
   visible logo at once); Linux/macOS runs (the user has both); Chinese titles on the HUD (font
   glyphs); long-track seek behavior (`SeekMS` on resume reads far ahead: decode-to-position latency).
   Later: our own texture pack instead of the HKPD slot; M4A `covr` art; popping the widget up on a new song.
3. Maybe: several stations (one per subfolder), shuffle/order option, M3U playlists.

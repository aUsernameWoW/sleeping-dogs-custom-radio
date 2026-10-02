# Sleeping Dogs: Definitive Edition — custom radio station (SDRadio)

[![Build](https://github.com/aUsernameWoW/sleeping-dogs-custom-radio/actions/workflows/build.yml/badge.svg)](https://github.com/aUsernameWoW/sleeping-dogs-custom-radio/actions/workflows/build.yml)

[中文](#中文) | [English](#english)

## 中文

给《热血无赖：终极版》的车载电台加一个新电台，播放你自己文件夹里的音乐。它和游戏自带的电台走同一套系统：
切台时显示台名、图标和"歌手 - 歌名"，一首放完自动换下一首（随机、避开刚放过的），音量、车内电台效果、
暂停和对话压低都和原版电台一致。

状态：Windows 上已在游戏内验证（FLAC）；Linux / macOS（Proton、Wine、CrossOver）待测。

### 需求

- 《热血无赖：终极版》（Steam），Windows 10/11 x64；也面向 Linux / macOS 上的 Proton、Wine、CrossOver。
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)（例如作为 `dinput8.dll`）。

### 安装与使用

1. 把 `SDRadio.asi` 放进游戏目录的 `plugins` 文件夹。
2. 把音乐放进 `plugins\SDRadio`（第一次运行游戏时会自动创建；可含子文件夹，最多 255 首）。
   - MP3、FLAC、WAV、OGG Vorbis：由 mod 自带的解码器处理，任何平台都一样。
   - M4A/AAC、WMA 等：借助 Windows 的 Media Foundation，在 Wine/Proton 下是否可用取决于其版本。
   - 歌名和歌手取自文件标签（ID3、Vorbis comment），没有标签时用文件名；GBK 编码的老式中文标签也能识别。
3. 开车时切台，新电台排在最后。台名、音乐文件夹和图标可在 `plugins\SDRadio.ini` 里修改。
4. 电台图标显示正在播放歌曲的封面：优先用音乐文件内嵌的封面（MP3、FLAC、OGG），否则找同一文件夹
   （或上级文件夹，直到音乐文件夹）里的 `cover`/`folder`/`front`/`logo` 图片（jpg、png、bmp、gif）。
   在音乐文件夹里放一张 `logo.png`，没有封面的歌就显示它。这个功能借用了 HKPD 警用频道的图标位置
   （它只在警用扫描模式下出现，届时可能显示最后一张封面）；`CoverArt = 0` 可关闭。
5. 出问题时请附上 `plugins\SDRadio.log`。

### 致谢

这个分支用到或参考了下面这些人和项目的成果，在此致谢。

- [SDmodding](https://github.com/SDmodding)，几乎全部出自 [sneakyevil](https://github.com/sneakyevil) 一人之手：SDmodding 分享的
  游戏 v1.0 版 exe 和调试符号（PDB，Steam 首发版自带），游戏的电台系统、Wwise 的文件读取和界面都是从这里查到的；
  读取游戏资源包（`.big`）的工具照 [BigFileSystem](https://github.com/SDmodding/BigFileSystem)、
  [TheoryEngine](https://github.com/SDmodding/TheoryEngine)，以及 sneakyevil 的
  [SD-BigFileExplorer](https://github.com/sneakyevil/SD-BigFileExplorer) 和 [Ekey](https://github.com/Ekey) 的 SDDEUnpacker
  里的文件名列表写成，电台列表和电台图标都是用它找到的。
- Audiokinetic 的 [Wwise](https://www.audiokinetic.com)（游戏用的是 Wwise 2012.2，音频库格式是对它逆向分析得到的）和
  Autodesk 的 Scaleform（游戏界面用的中间件，封面图要经过它显示）。
- mod 里包含的代码（许可证全文见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)）：
  [MinHook](https://github.com/TsudaKageyu/minhook)（Tsuda Kageyu，内含 Vyacheslav Patkov 的 Hacker Disassembler Engine）、
  [dr_libs](https://github.com/mackron/dr_libs)（David Reid；dr_mp3 含 lieff 的 [minimp3](https://github.com/lieff/minimp3)）、
  [stb](https://github.com/nothings/stb)（Sean Barrett 等：stb_vorbis、stb_image、stb_image_resize2、stb_dxt）。
- 需要另外安装的 [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)（ThirteenAG）。
- 工具：[IDA Pro](https://hex-rays.com/ida-pro)（Hex-Rays）和 [ida-pro-mcp](https://github.com/mrexodia/ida-pro-mcp)（mrexodia）；
  [Claude Code](https://claude.com/claude-code)（Anthropic）：这个 mod 的代码、文档和逆向分析几乎全部由 Claude 完成；作者负责提出需求、把握方向和在游戏里测试，代码审查得很少。

《热血无赖：终极版》由 United Front Games 开发、Square Enix 发行，游戏及其内容的版权归 Square Enix 所有。Wwise 是
Audiokinetic 的商标，Scaleform 是 Autodesk 的商标。与 Square Enix、United Front Games、Audiokinetic、Autodesk 均无关联。

## English

Adds a station to the car radio that plays your own music from a folder. It runs through the game's own
radio system: name, logo and "artist - title" on the HUD when switching, the next track starts by itself
(random, avoiding recent ones), and volume, the car-radio effect, pausing and dialogue ducking behave like
the original stations.

Status: verified in game on Windows (FLAC); Linux / macOS (Proton, Wine, CrossOver) not yet tested.

### Requirements

- Sleeping Dogs: Definitive Edition (Steam), Windows 10/11 x64; Proton, Wine and CrossOver on Linux/macOS are
  intended to work too.
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (e.g. as `dinput8.dll`).

### Install and use

1. Copy `SDRadio.asi` into the game's `plugins` folder.
2. Put music into `plugins\SDRadio` (created on the first run; subfolders included, up to 255 tracks).
   - MP3, FLAC, WAV, Ogg Vorbis: decoded by the mod itself, identical on every platform.
   - M4A/AAC, WMA and others: through Windows Media Foundation; under Wine/Proton it depends on the build.
   - Titles and artists come from the tags (ID3, Vorbis comments), else the file name.
3. Switch stations while driving; the new one comes last. Name, folder and logo are in `plugins\SDRadio.ini`.
4. The station logo shows the playing track's cover art: embedded in the file (MP3, FLAC, Ogg), else a
   `cover`/`folder`/`front`/`logo` image (jpg, png, bmp, gif) in its folder or a parent up to the music
   folder. A `logo.png` in the music folder shows for tracks without art. This takes over the HKPD
   scanner's logo slot (only seen in cop-scanner mode, where it may then show the last cover);
   `CoverArt = 0` turns it off.
5. When reporting a problem, attach `plugins\SDRadio.log`.

### Credits

This branch uses or builds on the work of these people and projects. Thank you.

- [SDmodding](https://github.com/SDmodding), almost all of it the work of one person,
  [sneakyevil](https://github.com/sneakyevil): the game's v1.0 exe and its debug symbols (PDB, shipped with the
  original Steam release), shared by SDmodding, from which the game's radio system, Wwise's file I/O and the UI were
  worked out; our tool for reading the game's `.big` archives follows
  [BigFileSystem](https://github.com/SDmodding/BigFileSystem), [TheoryEngine](https://github.com/SDmodding/TheoryEngine)
  and the file name lists in sneakyevil's [SD-BigFileExplorer](https://github.com/sneakyevil/SD-BigFileExplorer) and in
  [Ekey](https://github.com/Ekey)'s SDDEUnpacker; the station list and the station logos were found with it.
- Audiokinetic's [Wwise](https://www.audiokinetic.com) (the game uses Wwise 2012.2; the bank format was
  reverse-engineered from it) and Autodesk's Scaleform (the game's UI middleware, which the cover art goes through).
- Code in the mod (full license texts in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)):
  [MinHook](https://github.com/TsudaKageyu/minhook) (Tsuda Kageyu, with Vyacheslav Patkov's Hacker Disassembler Engine),
  [dr_libs](https://github.com/mackron/dr_libs) (David Reid; dr_mp3 contains lieff's
  [minimp3](https://github.com/lieff/minimp3)), [stb](https://github.com/nothings/stb) (Sean Barrett and others:
  stb_vorbis, stb_image, stb_image_resize2, stb_dxt).
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (ThirteenAG), installed separately.
- Tools: [IDA Pro](https://hex-rays.com/ida-pro) (Hex-Rays) and [ida-pro-mcp](https://github.com/mrexodia/ida-pro-mcp)
  (mrexodia); [Claude Code](https://claude.com/claude-code) (Anthropic): almost all of this mod's code, documentation and reverse
  engineering was done by Claude; the author set the goals, steered and tested in game, and reviewed little of the
  code.

Sleeping Dogs: Definitive Edition was developed by United Front Games and published by Square Enix; the game and its
content are © Square Enix. Wwise is a trademark of Audiokinetic, Scaleform a trademark of Autodesk. Not affiliated
with Square Enix, United Front Games, Audiokinetic or Autodesk.

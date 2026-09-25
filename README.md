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
4. 出问题时请附上 `plugins\SDRadio.log`。

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
4. When reporting a problem, attach `plugins\SDRadio.log`.

Third-party code and licenses: [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

# SDRadio — advanced users and developers

[![Build](https://github.com/aUsernameWoW/sleeping-dogs-custom-radio/actions/workflows/build.yml/badge.svg)](https://github.com/aUsernameWoW/sleeping-dogs-custom-radio/actions/workflows/build.yml)

[中文](#中文) | [English](#english)

新手安装说明见 [README.md](README.md)。 · Step-by-step install for players: [README.md](README.md).

## 中文

### 原理

游戏的电台完全由数据驱动，所以 mod 只添加数据、提供文件，不改游戏逻辑：

1. **电台列表**：游戏从 `Global.big` 的 XML 缓存里读 `Radios.xml`。mod hook 了读取函数，在末尾加一个
   `<Station>`，每个音乐文件对应一个 `<Track>`。HUD 和切台逻辑自动跟上。
2. **音频库（bank）**：mod 在运行时生成这个电台的 Wwise bank（每首歌一个流式 Sound + Play 事件），挂在车载电台
   原本的总线、效果和 RTPC 下面，所以音量、车内音效、暂停和对话压低都和原版一样。
3. **音频流**：hook Wwise 的底层文件 I/O，把我们的文件 ID 变成虚拟文件：bank 本身，或者每首歌实时解码成的
   16 位立体声 WAV。
4. 一首结束后由游戏自己的逻辑随机挑下一首。
5. **图标**：mod 启动时把图标做成游戏格式的 UI 贴图包，写成 `.asi` 旁边的 `SDRadio-logo.perm.bin` 和
   `.temp.bin`，电台列表里写上这个包的相对路径。游戏的归档里没有这个路径，就会从磁盘读取，和加载自带电台的
   图标完全一样，不需要任何 hook，也不占用别的电台的图标。见下面的[自定义图标](#自定义图标)。

实现细节和逆向笔记见 [CLAUDE.md](CLAUDE.md) 和 [docs/radio-internals.md](docs/radio-internals.md)（英文）。

**格式**：MP3、FLAC、WAV、OGG Vorbis 由 mod 自带的解码器（dr_libs、stb_vorbis）处理，任何平台都一样；M4A/AAC、
WMA 等借助 Windows 的 Media Foundation，在 Wine/Proton 下是否可用取决于其版本。歌名和歌手取自 ID3、Vorbis
comment、RIFF INFO 标签，没有标签时用文件名；GBK 编码的老式中文标签也能识别。日志里每首歌都写明用的是哪个解码器。

状态：Windows 上已在游戏内验证（FLAC）；MP3/OGG/M4A、HUD 上的中文歌名、Linux / macOS（Proton、Wine、
CrossOver）上的游戏内表现待测（自动测试在 Wine 11 和 Wine 9 下都通过）。

### 需求

- 《热血无赖：终极版》的两个发行版本（当前版本在游戏里验证过；旧版 v1.0 里特征码同样唯一匹配），Windows
  10/11 x64；也面向 Linux / macOS 上的 Proton、Wine、CrossOver。mod 用字节特征码定位游戏函数，找不到时日志写
  “game functions missing, no station”，不做任何改动。
- 任意 ASI 加载器，例如 [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)（`SDRadio.zip`
  里自带一份，作为 `dinput8.dll`）。Wine/Proton 下需要启动选项 `WINEDLLOVERRIDES="dinput8=n,b" %command%`。

### 下载

[Releases](https://github.com/aUsernameWoW/sleeping-dogs-custom-radio/releases) 里每个版本都有：

| 文件 | 内容 |
| --- | --- |
| `SDRadio.zip` | 解压到游戏目录：`dinput8.dll`（Ultimate ASI Loader）+ `plugins\SDRadio.asi` + 音乐文件夹 `plugins\SDRadio\` + 许可声明 |
| `SDRadio.asi` | 只有 mod 本体，放进已有加载器的 `plugins\` |
| `SDRadio.pdb` | 调试符号，只在分析崩溃转储时需要 |
| `THIRD-PARTY-NOTICES.md` | 第三方代码的许可证 |

`main` 上每次提交都会自动编译、测试并发布为预发布版 `build-<N>`（没有在游戏里测过）。在游戏里验证过的构建会被
转为正式版；README 里的下载链接指向最新的正式版。

已经有 ASI 加载器时，只需要把 `SDRadio.asi` 放进它加载插件的目录（通常是 `plugins\`）。`SDRadio.ini`、
`SDRadio.log` 和默认的音乐文件夹 `SDRadio\` 都在 `.asi` 旁边。

### 设置（`SDRadio.ini`，UTF-8）

| 段 | 键 | 默认 | 作用 |
| --- | --- | --- | --- |
| `[Station]` | `Name` | `SDRADIO` | 切台时显示的台名，最多 63 字节 |
| `[Station]` | `MusicFolder` | 空 | 音乐文件夹；空 = `.asi` 旁边的 `SDRadio`，相对路径相对于 `plugins` |
| `[Station]` | `CustomLogo` | 1 | HUD 图标：1 = 音乐文件夹里的 `logo.png`，没有就用自带的「私家台」；0 = 借用下面的游戏图标 |
| `[Station]` | `TextureName`, `TexturePack` | `Logo_Softly`, `Radio_Softly_TexturePack` | `CustomLogo = 0` 时借用的游戏电台图标（可选值见 ini 注释） |
| `[Debug]` | `Logging` | 1 | 写 `SDRadio.log`，出错时记录调用栈并写 `SDRadio-crash-<n>.dmp` |

### 自定义图标

把 PNG 命名为 `logo.png` 放进音乐文件夹（默认 `plugins\SDRadio`），重启游戏生效；删掉就回到自带的「私家台」。

- HUD 会把所有电台图标染成白色（原版的彩色图标也一样），所以只有**透明度**会显示：不透明的部分是白色，半透明
  的部分是半透明的白（可以做光晕），颜色会被忽略；
- 比例 2:1，推荐 512×256；其他尺寸会等比缩放进 512×256 并居中，不裁切（每边最多 16384 像素）；
- 没有透明背景的图会自动转成剪影：与图片边缘的明暗差越大越不透明，所以白纸上的黑色图案、黑底上的白色图案都可以；
- 可以参考自带图标 [`art/logo_512.png`](art/logo_512.png)，它由 [`art/neon.py`](art/neon.py) 生成。
- 日志里的 `logo:` 行写明用了哪张图、从透明度还是明暗得到的剪影，以及贴图包是否写好；
- `.asi` 必须在游戏文件夹里面（通常是 `plugins\`），而且路径只含英文字符，游戏才能读到贴图包；否则电台借用
  `TextureName` / `TexturePack` 指定的游戏图标，日志里会说明原因。

### 编译

Visual Studio 2022（v143），Windows SDK 10.0.26100。项目需要放在工作区的 `mods\SDRadio`，工作区里还要有
`reference\minhook`（MinHook v1.3.4）、`reference\dr_libs` 和 `reference\stb`，它们的源码都随项目一起编译。

GitHub Actions 会对推送和 PR 按同样的布局编译（`-warnAsError`）并运行自动测试，依赖的确切版本见
`.github/reference.env`；同样的测试还会在 Linux 上用 Wine 再跑一遍（WineHQ 最新稳定版和 Ubuntu 24.04 自带的
Wine 9），Wine 报告的未实现函数汇总在运行摘要里。然后打包 `SDRadio.zip`，其中 Ultimate ASI Loader 的版本和
SHA-256 固定在 `.github/asi-loader.env`。推送到 `main` 且测试（包括 Wine 下的）通过的构建会发布为预发布版
`build-<N>`。`asi-loader.yml` 每月检查
一次 Ultimate ASI Loader 的新版本，有新版时开 PR 更新 `asi-loader.env`；`reference.yml` 对编译所用的依赖做
同样的检查，开 PR 更新 `reference.env`；Dependabot 每月更新 Actions 的版本。

第三方代码及其许可证见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。

与 Square Enix、United Front Games、Audiokinetic 均无关联。

## English

### How it works

The game's radio is data-driven end to end, so the mod adds data and serves files; no game logic is patched:

1. **Station list**: the game reads `Radios.xml` from the XML cache in `Global.big`. The mod hooks that read
   and appends a `<Station>` with one `<Track>` per music file. The HUD and station cycling follow by
   themselves.
2. **Bank**: the mod generates the station's Wwise bank at runtime (per track a streamed Sound + Play event),
   under the car radio's own bus, effect and RTPCs, so volume, the car-radio sound, pausing and dialogue
   ducking behave like the original stations.
3. **Streams**: Wwise's low-level file I/O is hooked; our file IDs become virtual files: the bank, or each
   track decoded on the fly into a 16-bit stereo WAV.
4. At the end of a track the game's own logic picks the next one at random.
5. **Logo**: at startup the mod turns the logo into a UI texture pack in the game's format,
   `SDRadio-logo.perm.bin` and `.temp.bin` next to the `.asi`, and the station list names that pack by a
   relative path. No archive has that path, so the game reads the files from disk, just as it loads its own
   stations' logos: no hooks, and no other station's logo is touched. See [Custom logo](#custom-logo) below.

Details and reverse-engineering notes: [CLAUDE.md](CLAUDE.md) and
[docs/radio-internals.md](docs/radio-internals.md).

**Formats**: MP3, FLAC, WAV and Ogg Vorbis are decoded by the mod itself (dr_libs, stb_vorbis), identical on
every platform; M4A/AAC, WMA and others go through Windows Media Foundation, which under Wine/Proton depends
on the build. Titles and artists come from ID3, Vorbis comments or RIFF INFO, else the file name; legacy GBK
Chinese tags are recognized. The log names the decoder used for each track.

Status: verified in game on Windows (FLAC); MP3/OGG/M4A, Chinese titles on the HUD and the game on Linux /
macOS (Proton, Wine, CrossOver) not yet tested (the automated tests pass under Wine 11 and Wine 9).

### Requirements

- Both released builds of Sleeping Dogs: Definitive Edition (the current one verified in game; the signatures
  match uniquely in the legacy v1.0 too), Windows 10/11 x64; Proton, Wine and CrossOver on Linux/macOS are
  intended to work too. The mod finds the game's functions by byte signatures; if they aren't found, the log
  says "game functions missing, no station" and nothing is changed.
- Any ASI loader, e.g. [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
  (`SDRadio.zip` includes it as `dinput8.dll`). Under Wine/Proton it needs the launch option
  `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

### Downloads

Every version on [Releases](https://github.com/aUsernameWoW/sleeping-dogs-custom-radio/releases) has:

| File | Contents |
| --- | --- |
| `SDRadio.zip` | Unpacks into the game folder: `dinput8.dll` (Ultimate ASI Loader) + `plugins\SDRadio.asi` + the music folder `plugins\SDRadio\` + notices |
| `SDRadio.asi` | The mod alone, for the `plugins\` folder of an existing loader |
| `SDRadio.pdb` | Debug symbols, only needed to read crash dumps |
| `THIRD-PARTY-NOTICES.md` | Licenses of the third-party code |

Every commit on `main` is built, tested and published as a prerelease `build-<N>` (not tested in game).
Builds verified in game are promoted to full releases; the README's download link points to the newest one.

If you already have an ASI loader, just put `SDRadio.asi` where it loads plugins from (usually `plugins\`).
`SDRadio.ini`, `SDRadio.log` and the default music folder `SDRadio\` are next to the `.asi`.

### Settings (`SDRadio.ini`, UTF-8)

| Section | Key | Default | Effect |
| --- | --- | --- | --- |
| `[Station]` | `Name` | `SDRADIO` | Station name on the HUD, at most 63 bytes |
| `[Station]` | `MusicFolder` | empty | Music folder; empty = `SDRadio` next to the `.asi`, relative paths are relative to `plugins` |
| `[Station]` | `CustomLogo` | 1 | HUD logo: 1 = `logo.png` from the music folder, else SDRadio's own (私家台); 0 = the game logo below |
| `[Station]` | `TextureName`, `TexturePack` | `Logo_Softly`, `Radio_Softly_TexturePack` | With `CustomLogo = 0`, the game station logo to borrow (choices in the ini's comments) |
| `[Debug]` | `Logging` | 1 | Write `SDRadio.log`; on a crash, log the stack and write `SDRadio-crash-<n>.dmp` |

### Custom logo

Name a PNG `logo.png` and put it into the music folder (`plugins\SDRadio` by default), then restart the
game; delete it to get SDRadio's own logo (私家台) back.

- The HUD tints every station logo white (the game's colored ones too), so only **transparency** shows:
  opaque parts are white, translucent parts translucent white (a glow works), colors are ignored;
- 2:1, 512×256 recommended; other sizes are scaled into 512×256 and centered, never cropped (at most 16384
  pixels a side);
- a picture without transparency becomes a silhouette of what differs from its border in brightness, so
  black on white paper and white on black both work;
- SDRadio's own logo is [`art/logo_512.png`](art/logo_512.png), made by [`art/neon.py`](art/neon.py).
- The log's `logo:` lines name the picture used, whether the silhouette came from its transparency or its
  brightness, and whether the pack was written;
- the `.asi` must be inside the game folder (usually `plugins\`) in a path of English characters for the game
  to read the pack; otherwise the station borrows the game logo named by `TextureName` / `TexturePack`, and the
  log says why.

### Building

Visual Studio 2022 (v143), Windows SDK 10.0.26100. The project expects to sit at `mods\SDRadio` in a
workspace that also has `reference\minhook` (MinHook v1.3.4), `reference\dr_libs` and `reference\stb`, all
compiled from source with the project.

GitHub Actions builds pushes and pull requests in that same layout (with `-warnAsError`) and runs the
automated tests; `.github/reference.env` lists the exact dependency versions. The same tests then run again
on Linux under Wine (WineHQ's newest stable and Ubuntu 24.04's Wine 9), with the functions Wine reports as
unimplemented summarized in the run summary. It then packages `SDRadio.zip`, with the Ultimate ASI Loader
version and SHA-256 pinned in `.github/asi-loader.env`. Builds of `main` that pass (under Wine too) are
published as prereleases `build-<N>`. `asi-loader.yml` checks monthly for a new Ultimate
ASI Loader release and opens a PR that updates `asi-loader.env`, `reference.yml` does the same for the
libraries the build compiles against (`reference.env`), and Dependabot updates the Actions monthly.

The third-party code and its licenses are listed in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

Not affiliated with Square Enix, United Front Games or Audiokinetic.

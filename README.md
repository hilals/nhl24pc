# NHL24 PC

**NHL 24 (PS4, CUSA37933) running natively on Windows** – a single-game fork of the
[shadPS4](https://github.com/shadps4-emu/shadPS4) emulator, tuned and fixed for one title, with a launcher
that extracts the game from your own PKG and starts it. No console, no CPU emulation: the game's x86-64
code runs directly on your processor, the PS4 system libraries are replaced by native Windows
implementations, and the GPU work goes through Vulkan.

> **Not the same as the other NHL 24 forks.** This one started from [HajDEO/nhl24pc](https://github.com/HajDEO/nhl24pc) v0.8.3 and adds
> **NVIDIA DLSS (Super Resolution and DLAA) with ultrawide support, a frame rate cap up to 144 FPS, an experimental PS4 Pro mode,
> a fix for the black bars in cutscenes, the Utah Mammoth with a full roster**, plus many crash and freeze fixes and a start-up that
> went from about 105 s to about 15 s on the test PC. The table below lists everything that is different.

| | |
|---|---|
| Download | **[Latest release](https://github.com/hilals/nhl24pc/releases/latest)** – the ready-to-run zip has `launcher.exe`, `nhl24.exe`, the DLSS files and a clean `user\` template |
| Current build | **v1.0.1** (2026-11-04), Windows 10/11 x64 |
| Needs | your own NHL 24 PKG (base v01.00, optionally the 1.60 update), a Vulkan 1.3 GPU; an NVIDIA RTX card for DLSS |
| Tested on | mainly one PC (Core i9-13900KF, RTX 4090, 3440x1440, 120 FPS cap); reports from other hardware are welcome |
| License | GPL-2.0-or-later (same as shadPS4) |

## What this fork adds (vs. HajDEO/nhl24pc v0.8.3)

| Area | What you get |
|---|---|
| **NVIDIA DLSS** | DLSS Super Resolution and DLAA on RTX 20/30/40/50 cards (NVIDIA NGX, `nvngx_dlss.dll` 310.9.16 in the package). Quality modes DLAA, Quality, Balanced, Performance and Ultra Performance, chosen in the launcher. The game renders at its native 1920x1080 like the PS4 and DLSS upscales to your window or screen. AMD and older NVIDIA cards can use FSR instead. |
| **Resolution and ultrawide** | The launcher offers output sizes from 1280x720 up to **3840x2160 (4K)**, plus 3440x1440 with 21:9 presentation and a *stretch to fill* option, so DLSS output covers the whole screen without black bars. |
| **Frame rate cap** | 60 / 90 / 120 / 144 FPS in the launcher (earlier versions forced 60). 120 FPS is the setting used for most of the testing. |
| **PS4 Pro mode** | A checkbox in the launcher that makes the game run its PS4 Pro path. Experimental and needs plenty of video memory. The game's Pro-mode flip buffers are now recognised, which fixed the black screen and flicker the mode used to show. |
| **Cutscenes** | Dynamic resolution is switched off by a game patch, which removes the black columns on the right side of cutscenes (game versions 1.00 and 1.60). |
| **Utah Mammoth + roster** | Arizona becomes the Utah Mammoth everywhere: names, team select and the home, away and alternate jerseys with the Mammoth crest. A full custom roster for all 32 teams with real line combinations, and an installer. See below. |
| **Image quality** | 16x anisotropic filtering, a texture crispness (mip bias) option, per-player portrait pictures for created players and players without a photo. |
| **Stability** | *Create a team* and *Be a Pro* no longer crash; a mid-match abort caused by a texture/buffer overlap is handled; the 1.60 first-faceoff freeze has a patch entry; Vulkan validation fixes (see below). |
| **Smoothness** | A search that froze the game for up to a second about every 3 s is gone (it held the memory lock), and the driver's compiled-pipeline cache is kept between runs, so start-up no longer recompiles every saved shader. |
| **Diagnostics** | A built-in diagnostics suite (frame time log with the cause of each stutter, draw log, captures) that is off by default, switched on with `NHLPORT_DIAG=1`. |

**Utah Mammoth.** Arizona becomes the Utah Mammoth everywhere in the game: name and team select, and the home, away and
alternate jerseys with the Mammoth crest. The leftover Coyotes chest crest and shoulder patches are hidden on the
Utah jerseys while every other team keeps its crests, and Utah's uniform records are forced to the right set. Install the textures
from the release (`user\texture_override`); notes in `docs/UTAH_CREST_NOTES.md`.

**Vulkan and shader fixes (v1.0.0):** colour views of depth images, stale stages in cached pipelines, the 16-bit shader feature,
wave64 scratch-store races and a missing transfer usage on the DLSS output image, all found with the Vulkan validation layer.
Pipelines preloaded at start-up now fill the driver's pipeline cache.

**Slow start-up and freezes (v1.0.1)**
- The driver's compiled-pipeline cache is now kept between runs (`user\cache\<serial>.vkcache`, about 50 MB). Before, every start made the
  graphics driver recompile all saved pipelines, which took about 105 s with about 3,000 cached shaders; the first frame now appears after about 15 s.
  The first start after updating still builds the cache once, and the file is safe to delete.
- The Utah uniform patch used to search all of the game's memory about every 3 s while holding the memory manager's lock, so every
  memory map/unmap the game made waited for it and the whole game froze for up to a second. The search now copies the memory in pieces
  without holding the lock; the patch itself is unchanged. Short hitches while a match loads can remain.

**Crash fixes**
- *Create a team* no longer crashes: the shader recompiler now handles register-indexed jump tables (`s_setpc_b64`) whose
  index register is overwritten before the jump, by copying the index to a spare register.
- *Be a Pro* crash fixed by the same jump-table expansion (it runs before the control-flow graph is built).
- Texture cache recovers from an equal-address image overlap, and from a texel buffer that starts inside an image, instead of aborting.
- The texture dumper no longer switches itself on (only with `NHLPORT_DUMP_TEXTURES`).

Releases contain the jersey textures and a ready roster with its installer. Everything is for personal use with your own copy of the game.

## Credits

- [HajDEO](https://github.com/HajDEO/nhl24pc) – the original NHL 24 port this fork is based on.
- The [shadPS4](https://github.com/shadps4-emu/shadPS4) team and contributors – the emulator.
- LEVASTAK's NHL24PC V3.2 (GPL-2.0) – the idea and byte signatures of the dynamic-resolution patch.
- NVIDIA – DLSS and the NGX runtime (`nvngx_dlss.dll` is NVIDIA's redistributable). DLSS is a trademark of NVIDIA.

## How it works

The PS4 is an x86-64 machine. NHL 24's executable (`eboot.bin`) and its modules are ordinary AMD64 code,
so this port **does not emulate a CPU**: the game code is loaded into the process and executed by your
CPU directly, at native speed. What the emulator provides is everything around it:

- **HLE of the PS4 operating system** – kernel calls, threads, memory, files, audio (AJM), pads, video
  output, trophies, saves… are implemented as native Windows code (shadPS4's "libraries"). NHL 24 needs
  about 91 % of its imports resolved by HLE; the rest comes from the game's own modules.
- **GPU** – the PS4's GCN command streams are interpreted by a command-processor thread, the GCN
  shaders are recompiled to SPIR-V and executed through Vulkan, with a texture/buffer cache keeping guest
  memory and GPU memory coherent.
- **Game-specific work in this fork** (the full diary is `docs/DEVLOG.sk.md`): fence writes through a
  backing view instead of page faults (no GPU drain per fence), readbacks that wait only for the
  submission that wrote the data, full mip-chain depth/colour copies, CMASK/HTILE metadata with reference
  counting and a UI-coverage emulation (the game builds its HUD mask from CMASK), libSceJson2 and AJM
  fixes, wave64 lowering fixes, a GPU-thread idle backoff and pending-ops polling that halve the idle CPU
  load, a diagnostics suite (frame trace, draw log, image/buffer dumps, hotkey capture) compiled in but
  off by default, and memory patches (`game_patches/nhl24.xml`, loaded automatically from
  `user\patches\`).
- **Launcher** (`launcher/`, C# WinForms, needs only the .NET Framework 4 that ships with Windows):
  extracts the base/update PKG with shadPS4's PKG/PFS code built into `nhl24.exe`
  (`nhl24.exe --extract-pkg`), shows the installed version, exposes the useful settings (resolution,
  fullscreen, vsync, sharpening, language, controls, the Windows red-zone protection) and starts the game.

## Support

If this fork is useful to you (DLSS, frame rate, Pro mode, crash fixes and roster tooling), you can support my work:

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/hishak)

The NHL 24 port itself is the work of the original author, [HajDEO](https://github.com/HajDEO/nhl24pc) - you can support him too: [ko-fi.com/E1E61VGHII](https://ko-fi.com/E1E61VGHII)

## Playing

1. Download `NHL24-PC-v…-ready-to-run.zip` from the [latest release](https://github.com/hilals/nhl24pc/releases/latest) and unpack it –
   everything you need is there (`launcher.exe`, `nhl24.exe`, the DLSS files, runtime DLLs, `user\`). Keep the
   unpacked folder somewhere you can write to (not Program Files). (The repository's `release\` folder has no DLSS files; use the zip for DLSS.)
2. Start `launcher.exe`. Click **Install PKG** and pick your NHL 24 **base game** package (v01.00).
   The base game works as it is. The 1.60 update also runs, with the patches in `game_patches/nhl24.xml`
   (they remove the first-faceoff freeze and the dynamic resolution that causes black bars in cutscenes).
   The launcher checks the title ID (CUSA37933) and the kind
   of package, shows size and version, and extracts into `game\` next to the launcher, with progress.
3. Press **PLAY**. Simple view = the settings most people touch; Advanced = everything, including the
   keyboard mapping and the game folder.

First run: saves, the profile and the shader cache are created under `user\`. The first minutes stutter
while the shaders compile; afterwards the game runs at 50–60 fps on a mid-range GPU (Ryzen 5 4500 +
RTX 5060: 51 fps in play, 30 fps locked in menus and cutscenes, like the console).

**Requirements**: Windows 10/11 x64; a Vulkan 1.3 GPU with current drivers (NVIDIA GTX 10xx/RTX,
AMD RDNA; integrated GPUs are too slow); 8 GB RAM (16 GB recommended); about 40 GB free for the
extracted game; a gamepad (Xbox, DualShock 4, DualSense) is detected automatically, the keyboard works
too.

## Repository layout

```
release/        ready-to-run package (binaries + clean user template), rebuilt by tools\make_release.ps1
launcher/       launcher sources (Launcher.cs), pictures, icon, build script (csc from .NET Framework)
src/            emulator sources (shadPS4 fork; NHL-specific changes are marked "nhlport")
externals/      third-party libraries as git submodules (clone with --recursive)
game_patches/   nhl24.xml – memory patches applied by the emulator at start
tools/          build, run, measurement and diagnostics scripts (PowerShell / Python)
docs/           DEVLOG.sk.md (development diary, Slovak), bbport_analysis.md, patches/ (early fixes)
documents/      shadPS4's own documentation and README
```

## Building

Only needed to change the emulator or the launcher; players use `release/`.

- Windows 10/11, [Visual Studio 2022 Build Tools](https://visualstudio.microsoft.com/visual-cpp-build-tools/)
  (MSVC v143 + Windows SDK), [LLVM](https://github.com/llvm/llvm-project/releases) (clang-cl, lld),
  [CMake](https://cmake.org/) 3.24+, [Ninja](https://ninja-build.org/), [Vulkan SDK](https://vulkan.lunarg.com/).
- `git clone --recursive https://github.com/HajDEO/nhl24pc.git`
- `powershell -ExecutionPolicy Bypass -File tools\build_nhlport.ps1 -DebugInfo` → `build\nhl24.exe` (+ `.pdb`)
- `powershell -ExecutionPolicy Bypass -File launcher\build_launcher.ps1` → `build\launcher.exe`
- `powershell -ExecutionPolicy Bypass -File tools\make_release.ps1` → refreshes `release\`

Development notes: `tools\run_nhl24.ps1` runs the build with the diagnostics log enabled
(`NHLPORT_DIAG=1`; F1 captures a frame: draw log, image dumps, screenshot), `tools\measure_nhl24.ps1`
scripts measurement runs, `tools\capture_report.py` summarises a capture. The version lives in
`src/common/port_version.h` (also used for the exe resource and the launcher title).

## Contributing

Issues and pull requests are welcome – performance, the open rendering issues above, DualSense
features, Linux. Keep changes focused; NHL-specific code is marked `nhlport` in comments so it can be
told apart from upstream shadPS4. Please do not upload game files, PKGs, saves or shader caches.

## Legal

This project does not contain any game data. You need your own copy of NHL 24 for PlayStation 4,
dumped as a PKG from a console you own. "NHL" and "EA SPORTS" are trademarks of their respective
owners; this project is not affiliated with EA, the NHL or Sony. The emulator is derived from
[shadPS4](https://github.com/shadps4-emu/shadPS4) (GPL-2.0-or-later); this repository is distributed
under the same license, see `LICENSE` and `LICENSES/`.

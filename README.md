<div align="center">

<img src="https://cdn2.steamgriddb.com/logo/e88a994b60847fd3f5bf29ddce531b13.png" alt="Need for Speed: Underground" width="520">

### Xbox static recompilation for Nintendo Switch and Linux

The original Xbox (NTSC-U) release of **Need for Speed: Underground**, lifted
instruction by instruction to C and running natively, with no emulator.

![Switch](https://img.shields.io/badge/Nintendo%20Switch-homebrew-E60012?logo=nintendoswitch&logoColor=white)
![Linux](https://img.shields.io/badge/Linux-x86__64-FCC624?logo=linux&logoColor=black)
![Vulkan](https://img.shields.io/badge/Vulkan-1.3-AC162C?logo=vulkan&logoColor=white)
![OpenGL](https://img.shields.io/badge/OpenGL-renderer-5586A4?logo=opengl&logoColor=white)
![Version](https://img.shields.io/badge/version-0.2.5-blue)

[Features](#-features) · [Playing on Switch](#-playing-on-switch) · [Building](#%EF%B8%8F-building) · [Configuration](#%EF%B8%8F-configuration) · [Status](#-status)

</div>

---

> [!IMPORTANT]
> **No game data is included.** You need your own copy of the Xbox disc,
> extracted (`default.xbe`, `FRONTEND/`, `CARS/`, ...).

## ✨ Features

- 🏁 **Native code**: the whole game runs as recompiled C, built with
  [xboxrecomp](https://github.com/sp00nznet/xboxrecomp)
- 🎮 **Two renderers**: Vulkan (NVK on Switch, recommended) and OpenGL, with
  render scaling up to 4x
- ⚡ **30 fps races on Switch** (the game's own cap) with the Vulkan build
  and raised clocks
- 🪞 **Car reflections** as cube maps (Vulkan)
- 🖼️ **Cleaner picture** (Vulkan): mipmaps with trilinear and 16x
  anisotropic filtering, FXAA, square-pixel 1280x720 for the 16:9 view
- 🎬 **Movies** played by the game's own EA `.mad` decoder
- 🔊 **Audio** through an emulated Xbox APU, with 5.1 downmixed to stereo
- 📳 **Rumble** on Switch HD rumble and SDL controllers
- ⏱️ **CPU/GPU/memory clocks** settable per mode (handheld and docked),
  kept across dock changes, HOME and sleep
- Shares its runtime with the
  [Underground 2](https://github.com/antoxa2584x/nfsu2-sw) port

## 🕹️ Playing on Switch

1. Copy the NRO to `sdmc:/switch/nfsu1x/` (`nfsu1x-vulkan.nro` for the Vulkan
   build, `nfsu1x.nro` for OpenGL).
2. Copy the **extracted** disc (not the ISO) to `sdmc:/switch/nfsu1x/game/`.
3. Start it with **title takeover**: hold **R** while launching any game.
   Applet mode leaves too little memory.

```
sdmc:/switch/nfsu1x/
├── nfsu1x-vulkan.nro
├── nfsu1x_env.txt      optional settings, KEY=VALUE per line
├── save/               created on first run
└── game/
    ├── default.xbe
    └── ...
```

Buttons map by label (Switch A = Xbox A). Settings go in `nfsu1x_env.txt`
(see [Configuration](#%EF%B8%8F-configuration)) and the log is written to the
same folder.

Recommended `nfsu1x_env.txt`:

```ini
RECOMP_GIL_EAGER=1
RECOMP_CULL=0
RECOMP_GL_SCALE=1.5
RECOMP_GL_SCALE_X=2
NFSU2_CPU_MHZ=1785
NFSU2_GPU_MHZ=768
```

## 🛠️ Building

<details open>
<summary><b>1. Extract the disc and lift the XBE to C</b></summary>

```sh
python3 -m tools.xiso unpack "Need for Speed - Underground (USA).iso" /path/to/game   # from xboxrecomp/
# Writes NFSU1_GEN_DIR (default /root/nfsu1x/gen)
NFSU1_XBE=/path/to/game/default.xbe NFSU1_GEN_DIR=/path/to/gen tools/regen.sh
```

</details>

<details open>
<summary><b>2a. Linux</b> (SDL2 + OpenGL or Vulkan)</summary>

```sh
cmake -S . -B build -G Ninja -DNFSU2_GEN_DIR=/path/to/gen   # add -DNFSU2_VULKAN=ON for Vulkan
cmake --build build
NFSU2_GAME_DIR=/path/to/game build/nfsu2_recomp
```

</details>

<details open>
<summary><b>2b. Nintendo Switch</b> (devkitA64, switch-sdl2, switch-mesa)</summary>

```sh
NFSU2_GEN_DIR=/path/to/gen NFSU2_GAME_SRC=/path/to/game switch/build.sh
# Vulkan build (needs mesa-switch NVK and glslang for Switch)
VULKAN=1 JOBS=6 NFSU2_GEN_DIR=/path/to/gen switch/build.sh
```

</details>

CMake options and most runtime switches keep their `NFSU2_*` names: the glue
code is shared with the Underground 2 port.

### Project layout

| Path | What |
|---|---|
| `src/main.c` | boot and runtime defaults |
| `src/recomp_manual.c` | hand-written overrides of lifted functions (D3D, DirectSound, CRT) |
| `src/switch_nx.c` | Switch log device, env file, clocks, profiler, loading screen |
| `config/seed_functions.json` | entry points the static pass cannot see |
| `xboxrecomp/` | the toolkit (MIT), vendored with this port's changes: NV2A renderers (Vulkan, OpenGL), SDL audio, Switch platform layer, translator fixes |
| `tools/regen.sh` | XBE → lifted C (`gen/`, never committed) |
| `tools/prof_report.py` | Switch profiler (`prof.bin`) → per-thread function tables |
| `switch/build.sh` | Switch NRO build + SD-card staging |

## ⚙️ Configuration

On Linux these are ordinary environment variables. On the Switch they go in
`sdmc:/switch/nfsu1x/nfsu1x_env.txt`, one `KEY=VALUE` per line (`#` starts a
comment). The [Underground 2 README](https://github.com/antoxa2584x/nfsu2-sw#%EF%B8%8F-configuration)
lists every runtime switch; the ones that matter here:

<details open>
<summary><b>Performance and clocks (Switch)</b></summary>

| Variable | Meaning |
|---|---|
| `NFSU2_CPU_MHZ`, `NFSU2_GPU_MHZ`, `NFSU2_MEM_MHZ` | Clocks (closest listed rate, capped at 1785 / 921.6 / 1600 MHz). The CPU is what limits the port. Old rates come back on exit. |
| `NFSU2_CPU_MHZ_DOCKED`, `NFSU2_GPU_MHZ_DOCKED`, `NFSU2_MEM_MHZ_DOCKED` | Different rates while docked (otherwise the plain keys apply in both modes). |
| `RECOMP_GIL_EAGER=1` | Hand the guest lock over by guest priority (the main thread pre-empts workers). |
| `RECOMP_CULL=0` | No off-screen batch culling (in NFSU1 it costs more than it saves). |
| `RECOMP_VK_INSTANCE=0` | Vulkan: don't merge repeated draws into instanced draws. |
| `RECOMP_GL_THREAD=1` | OpenGL: GL calls on their own thread. |
| `RECOMP_NX_PROFILE` | Sampling profiler into `prof.bin`: 1 = busy threads, 2 = every thread above 2%. |
| `NFSU2_NO_LOG=1` | No log file, no `[perf]` reports, no profiler. |

</details>

<details>
<summary><b>Graphics</b></summary>

| Variable | Meaning |
|---|---|
| `RECOMP_GL_SCALE` | Render resolution multiple, 0.5..4 (fractions allowed). |
| `RECOMP_GL_SCALE_X` | Vulkan: horizontal multiple, if different (4/3 of `RECOMP_GL_SCALE` = square pixels at 16:9). |
| `RECOMP_VK_MIPS=0` | Vulkan: no mipmaps (distant textures shimmer, the old look). |
| `RECOMP_VK_ANISO` | Vulkan: anisotropic filtering, 1..16 (default 16; 1 = off). |
| `RECOMP_VK_LOD_BIAS` | Vulkan: mip bias, negative = sharper (default -0.25). |
| `RECOMP_VK_FXAA=0` | Vulkan: no FXAA (plain scaled blit to the screen). |
| `RECOMP_VK_CUBE=0` | Vulkan: no cube maps (car reflections off). |
| `RECOMP_GL_DXT=0` | Decode DXT textures on the CPU instead of uploading them compressed. |
| `RECOMP_GL_TEX_MB` | Texture cache budget in MB. |
| `RECOMP_PROG_CACHE` | Shader/pipeline cache file (`progcache.bin`), `=0` off. |
| `RECOMP_GL_DUMP=<prefix>[,every]` | Write presented frames as BMP. |
| `RECOMP_FPS_LOG=1` | Presented frames per 10 s. |
| `RECOMP_DRAW_STATS=1` | Vulkan: draws → draw calls per frame. |

</details>

<details>
<summary><b>Game, audio and input</b></summary>

| Variable | Meaning |
|---|---|
| `NFSU2_GAME_DIR` | Extracted disc (Linux; default `./game`). The Switch always uses `sdmc:/switch/nfsu1x/game/`. |
| `NFSU2_GL=0` | Use the executor's CPU renderer instead of the GPU renderer. |
| `RECOMP_AUDIO_VOLUME` | Master volume 0..100 (default 50). |
| `RECOMP_AUDIO=0` | No host audio device. |
| `RECOMP_PAD_SCRIPT` | Timed presses, e.g. `4000:start:300,9000:a:200` (ms from the first pad read). |
| `RECOMP_PAD_LAYOUT=position` | Map buttons by position (Xbox layout) instead of by label. |
| `RECOMP_RUMBLE=0` | No rumble. |

</details>

## 🚦 Status

| Platform | State |
|---|---|
| 🐧 Linux | Movies, title, menus, Quick Race at 30 fps with HUD, AI and audio |
| 🎮 Switch hardware | Menus and Quick Race at 30 fps (Vulkan, CPU 1785 MHz) |

Still open: the main-menu car's rear window shows through its side windows;
cube maps on OpenGL; Career mode untested.

## 📚 More

- [`CLAUDE.md`](CLAUDE.md): detailed engineering notes (address map against
  Underground 2, fixes, performance measurements)

<div align="center">
<sub>Need for Speed and Underground are trademarks of Electronic Arts. This is
an unofficial fan project, not affiliated with or endorsed by EA or Microsoft.</sub>
</div>

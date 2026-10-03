# NFSU1 (Xbox) static recompilation — notes for Claude

Xbox NTSC-U Need for Speed: Underground (title 0x45410047, XDK 5558), lifted
to C with xboxrecomp and built for Linux and Nintendo Switch (libnx NRO).
Started 2026-10-02 from the NFSU2 port (`..\nfsu2-xbox`, read its CLAUDE.md
for everything the runtime does on Horizon). Repo:
https://github.com/antoxa2584x/nfsu1-sw (`main`, commits as
`Anton Artemov <antoxa2584@gmail.com>`).

## Rules

- Never commit game data (disc, `default.xbe`, `switch_sd/`) or generated C
  (`gen/`). Ask before committing or pushing.
- The Switch build reads the **unpacked** disc at `sdmc:/switch/nfsu1x/game/`,
  never the ISO. Logs, `nfsu1x_env.txt` and shader caches live in
  `sdmc:/switch/nfsu1x/` (separate from NFSU2's `nfsu2x/`).
- Same Linux test hygiene as NFSU2: one run at a time, kill by PID
  (the process renames itself; `rtk proxy ps`).
- Under gdb use `set disable-randomization off` -- with ASLR off the Xbox
  kernel VA 0x80010000 and the tiled aperture cannot be mapped.

## Layout and builds

| Where | What |
|---|---|
| `/root/nfsu1x/` (WSL) | `game/` extracted disc, `gen/` lifted C, `build-linux/`, `build-switch/`, `build-switch-vk/`, `run/` (test logs, frame dumps), the ISO |
| `xboxrecomp/` | vendored toolkit: copy of `/root/nfsu2x/xboxrecomp-pr128` + local changes (below), with its own `tools/*/output` |
| `src/main.c` | NFSU2's boot with entry 0x00164356; NFSU2's game-time cap patch disabled (`if (0 && ...)`, wrong address here) |
| `src/recomp_manual.c` | library overrides only (D3D, DirectSound, CRT), remapped from NFSU2 |
| `src/switch_nx.c` | as NFSU2 (log, env file, profiler), paths renamed |
| `config/seed_functions.json` | entry points the static pass misses |

- Disc: `Downloads\Need for Speed - Underground (USA).7z` is the Xbox image
  (the `.zip` of the same name is PS2). `python3 -m tools.xiso unpack`.
- Regenerate C: `tools/regen.sh` (`LIFT_ONLY=1` after manual-override edits).
- Linux: `cmake --build /root/nfsu1x/build-linux -j12`; run with
  `NFSU2_GAME_DIR=/root/nfsu1x/game SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy`.
  Quick Race pad script (start + A presses, race at ~140 s):
  `RECOMP_PAD_SCRIPT="5000:start:300,15000:start:300,25000:start:300,35000:a:200,45000:a:200,55000:start:300,65000:a:200,75000:a:200,85000:a:200,95000:a:200,105000:a:200,110000:start:300,115000:a:200,120000:a:200,125000:start:300,130000:a:200"`.
- Switch: `JOBS=6 bash switch/build.sh` (GL, `nfsu1x.nro`),
  `VULKAN=1 BUILD_DIR=/root/nfsu1x/build-switch-vk JOBS=6 bash switch/build.sh`
  (`nfsu1x-vulkan.nro`; NVK/glslang from `/root/nfsu2x/`). Staged in
  `switch_sd/switch/nfsu1x/`. No FFmpeg (movies are not VP6).
- CMake options and env switches keep their `NFSU2_*` names (shared glue).
- Eden: its `sdmc\switch` is NFSU2's `switch_sd\switch`; that folder holds a
  junction `nfsu1x` -> this repo's `switch_sd\switch\nfsu1x` (made with
  `mklink /J`). Eden can't run the Vulkan NRO (NVK submissions never complete
  there) -- use `nfsu1x.nro`. An Eden run overwrites `nfsu1x_log.txt`.

## Findings

- **XDK 5558 vs NFSU2's 5849:** D3D, DSOUND and the CRT are the same code
  apart from relocations; game code is not (different compiler output), so
  NFSU2's native game leaves, time cap, stream guard and text patch do not
  carry over. Address map (NFSU2 -> NFSU1): BlockOnTime 0x2E8F20 -> 0x1D0340
  (device ptr 0x2F7798 -> 0x1DE9E8), vblank 0x2F1D80 -> 0x1D9BB0, PGRAPH
  0x2F22F0 -> 0x1DA120, PersistDisplay 0x2EA9C0 -> 0x1D2CA0, BlockOnFence
  0x2E9530 -> 0x1D1850, KickOff 0x2E8D40 -> 0x1D0160, flip queue 0x2F2080 ->
  0x1D9EB0, D3D SSE mul 0x2EEA80 -> 0x1D4CE0, DSP ack 0x32EB65 -> 0x1E4CA1,
  AC97 reset 0x33518D -> 0x1EA5B2, DSOUND lock 0x32BFF0 -> 0x1E28B0,
  XGetVideoFlags 0x21AEA8 -> 0x1BBEF0, memmove -> 0x1B3640, _ftol2 -> 0x1B2344.
  First-24-instruction matcher: scratchpad `match.py` idea -- normalise
  addresses, compare against every function start.
- Without the D3D wrappers boot stalls in the vblank handler's `in al,dx`
  (0x1D9C2D) before SetDisplayMode.
- **Movies** are EA `.mad` (MOVIES\\*.mad); the lifted decoder plays them
  correctly. Speed on hardware unknown.
- **Missed functions** (unresolved ICALL / ABI breakage): 0x781F0 (chunk
  handler 0x80034020 -- without it the car load crashed), 0x1EBC9C,
  0x1EC4DA, 0x1F0717, 0x1EFCB2 (XPP/USB).
- **Race load crash (fixed in xboxrecomp/tools/disasm/functions.py):** D3D
  writes NV2A pushbuffer words as immediates (`mov [edi], 0x00100A20` = 4
  dwords to method 0xA20); NFSU1's .text covers those values, so the
  imm_ref_target pass made them function starts, split sub_001009C0, and its
  switch lost `cmp eax,4; ja default`. Out-of-range -> wild ITAIL -> esi/edi
  popped from the wrong slots -> null vtable crash in sub_000FC590. Now
  immediates from the D3D section are ignored (test in
  test_imm_ref_boundaries.py). Upstream candidate.
- `-DRECOMP_ABI_CHECK` (CMAKE_C_FLAGS) finds this class fast:
  `[ABI] sub_X: esi edi esp(epilogue never ran)`.
- Template bug: xboxrecomp `templates/new-game/src/recomp_manual.c` needs
  `#include <stddef.h>`.

## Status (2026-10-03)

- Linux: movies -> title -> menus -> Quick Race racing at 30 fps (vblank),
  HUD and AI fine.
- Switch: GL and Vulkan NROs build; not run on Eden or hardware yet.
- Loading screen: NFSU1 logo (`assets/nfsu2_logo.png` ->
  `tools/make_logo.py` -> `src/nfsu2_logo.h`); icon: SteamGridDB NFSU1 icon.

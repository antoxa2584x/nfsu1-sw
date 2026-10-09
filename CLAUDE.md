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

- **Car reflections = cube maps (ported from NFSU2 f0b3f1e, 2026-10-08,
  Vulkan only):** texture mode 3 as 6-layer cube images, dynamic cubes
  assembled from the six face surfaces (`cube_from_surfaces`), samplerCube in
  gl_psh.c for VK only. `RECOMP_VK_CUBE=0` turns it off. Builds; not run yet.


- **Switch perf (2026-10-08, Vulkan focus):** CPU-bound: game thread +
  executor (decode + VK on one thread) peg two cores, GPU idle. NFSU1's
  Present (0x1CF75B) already waits on the *previous* frame's fence (one frame
  in flight), so NFSU2's RECOMP_FRAME_LAG does not apply (its BlockOnFence
  0x1D1850 has one caller, 0x16EEB0, a flush-and-unbind routine); Linux race
  main thread ~15 ms/frame in that wait = renderer side is the limit. Ring
  space waits: 0x1D05A4. A game-drawn SZ_I8 (0x0B) 512x512 + 128x512 pair
  changes its indices every race frame (palette fixed): ~16 ms/frame of
  per-texel swizzle on the console; now two lookup tables (swizzle_tables,
  GL + VK, bit-exact). Ported from NFSU2: 66b876d (VK instancing: race
  2382 draws -> 1408 calls; off-screen batch cull, 0 mismatches, 1-3%
  skipped) and 7691775 (clock keeper 0x2C + applet hook), plus
  NFSU2_*_MHZ_DOCKED overrides. nfsu1x_env.txt sets RECOMP_GIL_EAGER=1.
  Upstream xboxrecomp v0.13.x: lifter/kernel correctness only, nothing
  for the GPU path.
- **Console run 2026-10-08 (VK, 1785/768/1600 MHz, scale 1.5):** race
  26-28 fps (was 13-17). nv2a_ack_thread (executor + VK) 95-100%, game
  thread ~50% (waits on it). Its profile: pb_scan + exec_method self 33%
  (one call per pushbuffer word; race words: VS constants 0x0B80-0x0BFC
  ~55%, ARRAY_ELEMENT16 ~28%), our VK glue ~20%, cull_bbox 4.3% (culls
  1-3% -> RECOMP_CULL=0 in the env file), malloc/free in Mesa's
  BindVertexBuffers2 3.6% (STACK_ARRAY > 8), NVK ~10%. Fixed:
  `nv2a_pb_exec_run` takes constant and index runs in one loop (skipped
  under RECOMP_PB_SCAN, which now alone enables the survey), vertex buffers
  bound 8 + 8, vertex input re-sent only on change, textures decoded
  straight into the ring. Linux race frames and draw counts unchanged.
- **Light line top/left at RECOMP_GL_SCALE > 1:** NFSU2 7d48eac ported
  (`nv2a_snap` offset u_surf.w = 0.5 - 0.5*w/pw, GL + VK). Linux VK race at
  2x: edge rows/columns flat. NFSU2's in-game Resolution Scale row
  (48b1b27) is NFSU2 menu code; NFSU1 uses RECOMP_GL_SCALE in the env file.
- **Console 2026-10-08 23:21 (all of the above):** race steady 30 fps (the
  game's cap), executor 62-90%. Audio: NFSU2's centre/LFE/I3DL2 downmix
  (7d48eac apu_dsp.c) ported -- engine and voice were left-only.
  Open: main-menu car's rear window seen through the side windows (not
  reproduced in Linux dumps; the menu camera hides the glass).

- **VK picture quality (2026-10-09, nv2a_vk.c):** textures had no mips
  (maxLod 0, 1 level): distant road was grainy. Now the guest's level
  count (format [19:16]) is honoured: DXT uploads the title's levels,
  decoded formats get GPU blits (tex_gen_mips); samplers with MIN 3..6 are
  trilinear + 8x anisotropic. Present is one FXAA 3.11 draw (surface
  texels, letterbox rect) instead of the blit. RECOMP_VK_MIPS / _ANISO /
  _FXAA; headless dumps add `_fxaa.bmp`. Linux race draw counts unchanged,
  validation clean (headless + windowed). Not run on the console yet.
  Then: RECOMP_GL_SCALE_X (per-axis surface scale; env file 2 x 1.5 =
  1280x720 square pixels under Hor+; VK snap offset now per axis,
  u_snap_y in gl_vsh.c's VK block), aniso 16x default, LOD bias -0.25,
  dynamic reflection cubes mipmapped (gen_mips after the face blits).
- **Hor+ 16:9 (2026-10-09, recomp_manual.c):** NFSU1 has no widescreen
  mode (only XGetVideoFlags caller 0x16EA50 tests PAL-60), so 4:3 was
  stretched. View projection sub_00018E60 (views at 0x262640 stride 0x360,
  proj +0x1F0; m00 = cot(fov/2), m11 = cot(fov*H/W/2)) -> sub_00018E40
  view*proj -> frustum planes from the product. Wrappers scale m00 by 3/4
  before the product for 4:3 targets (race id 1 640x480, id 4 320x240,
  id 8, FE id 0; env faces ids 10-15 128x128 untouched). Culling follows.
  `RECOMP_HORPLUS=0` (or RECOMP_WIDESCREEN=0) for the old picture. Front
  end (game flow state 0x283434 == 3; 4 loading, 6 racing) zooms instead
  (m11 x 4/3) so the garage keeps its 4:3 width -- the set ends (tunnel
  mouth) just past it on the right; its camera (views 1 and 4, same as the
  race) also turns left RECOMP_FE_YAW degrees (default 5). Main menu runs
  views 0,1,4,8,10-15 like a race; view ids alone can't tell FE from race.
  HUD and FE 2D are still stretched. EAGL::ViewPort (0x179E10/0x179FB0)
  is not used for 3D. Found via VS-constant dump (WVP at c96) + RAM search
  + gdb watch; FE state via .data diff menu/loading/race.
- **Options -> Camera rows (2026-10-09, recomp_manual.c `s_rows`):** NFSU2's
  Video rows, on Camera (Display has 7 rows, no room). All option screens
  are one class on MU_Options.fng; the Options menu (sub_000C3EA0) leaves
  the sub-screen in [0x2BBB54] (0 Audio, 1 Camera, 2 Car, 3 Controller,
  4 Display). Camera setup sub_000C2A70 is wrapped: rows 3..7 =
  sub_000C1D30(row, data, select button) + sub_000C1E10(row, 0) arrows,
  pad left/right go to this+0x3C+4*(row-1) (dispatch 0x000C281C, game's
  own: sub_000C0C90), ours at int3 bytes 0x000C0C83+ via
  recomp_lookup_manual. Texts: sub_000DD490(pkg, "OptionName_N" /
  "OptionData_N", text) with our own guest strings (language files are
  Huffman-packed). Car Reflections Off also clears the six EnvMap views'
  +8 (ids 10..15, view(id) = 0x2C5F20 + id*0x60) around the race render
  sub_00016050 (~25 fewer draws a race frame). Vulkan-only rows: scale,
  FXAA, aniso, square (nv2a_vk.c from NFSU2 77b9a83 + FXAA_SUBPIX). Saved
  in nfsu1x_options.txt, loaded in main.c. Pause menu's Camera is another
  screen (no rows). Linux path: Main Menu "No" -> up to Options -> a ->
  down -> a.
- **Loading screen shifted right (fixed 2026-10-09, recomp_manual.c):** not
  the renderer (viewport offset 0.53125 everywhere). Display -> Menu Size
  (float 0x295B00, 0.84..1.0, default 0.92, saved in the profile at .nfs
  +0x1D) scales the FE render (sub_00016460, view 0) toward the centre for
  TV overscan; Loading.fng's backdrop is ~680 wide and 13 px right of
  centre, so at 0.92 a strip of the last frame shows on the left (movies
  got borders too). Before each FE frame 0.92 -> 1.0 unless the slider
  (sub_000C1860) set it this session. RECOMP_POKE=0x295B00:0x3F800000
  was the probe. sub_00011200 (also scales by it) is a fatal-error screen
  ending in `jmp $`.
- **EA Games Trax crash (fixed in xboxrecomp/tools/disasm/functions.py):**
  the Options menu handler sub_000C3EA0 (vtable only, in a gap) parks a
  `push edi` block after `ret 0x10` that two `je` reach; the gap-prologue
  pass made it a function, so the handler ended there and its blocks at
  0xC3F4D/0xC3F92 became "not detected" stubs: messages above 0x911C0A4B
  (EA Trax's select) returned without the epilogue -> esi garbage in
  sub_000DA280. jcc targets are now never gap-prologue starts (test in
  test_gap_prologue.py); also healed 0x187A60 and 0x18A5B0. Upstream
  candidate. Found with -DRECOMP_ABI_CHECK.
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

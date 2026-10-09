/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* The generated register model (g_eax/g_esp are thread-local there) and the
 * XBOX_PTR/MEM32 accessors; the register names below are its macros. */
#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

extern ptrdiff_t g_xbox_mem_offset;

/* ── CRT memmove/memcpy ───────────────────────────────────────
 *
 * Two copies of the MSVC CRT memmove are linked (0x001B3640 and 0x002A9450;
 * memcpy is the same body). Their tail and backward-copy paths dispatch
 * through UnwindDown-style vectors indexed with a *negative* register -- the
 * table address in the instruction is the end of the table -- which the
 * lifter's forward table reader cannot follow, so those jumps were lowered
 * to indirect tail calls into the middle of the function and failed to
 * resolve: every copy whose length was not a multiple of 4, and every
 * overlapping backward copy, silently lost its tail.
 *
 * Replaced by the host memmove. cdecl (dst, src, count), returns dst, and
 * esi/edi are preserved, as the original restores them. Generated bodies are
 * skipped by tools.recomp --exclude-manual reading this file. */
static void crt_memmove(void)
{
    uint32_t dst = MEM32(esp + 4);
    uint32_t src = MEM32(esp + 8);
    uint32_t n   = MEM32(esp + 12);

    if (n)
        memmove((void *)XBOX_PTR(dst), (const void *)XBOX_PTR(src), n);
    eax = dst;
    esp += 4;   /* return address; cdecl, the caller pops the arguments */
}

void sub_001B3640(void) { crt_memmove(); }

/* ── D3D fence wait, D3D_BlockOnTime (0x001D0340) ────────────
 *
 * stdcall (fence, flags), ret 8. The stock XDK 5849 D3D keeps its device at
 * 0x002F77A0, reached through the global at 0x002F7798:
 *   +0x2C  write sequence, bumped as fences are inserted
 *   +0x30  pointer to the semaphore the GPU releases as it completes work
 * and this spins until *(+0x30) catches up with the fence it was asked for.
 *
 * Without the pushbuffer executor nothing ever releases the semaphore, so the
 * GPU is reported caught up. With it (RECOMP_PB_EXEC) that would let D3D
 * reuse ring space and vertex memory the executor has not read yet (see the
 * toolkit's d3d8ltcg-device-context.md), so instead the real semaphore is
 * handed to the executor once and the original wait runs against it. */
#define NFSU2_D3D_DEVICE_PTR 0x001DE9E8u
extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
extern void sub_001D0340_gen(void);

void sub_001D0340(void)
{
    static int exec = -1;
    uint32_t dev = MEM32(NFSU2_D3D_DEVICE_PTR);
    uint32_t sem = dev ? MEM32(dev + 0x30) : 0;

    if (exec < 0)
        exec = getenv("RECOMP_PB_EXEC") != NULL;
    if (exec) {
        static uint32_t registered;
        if (sem && sem != registered) {
            nv2a_pb_set_semaphore_target(sem);
            fprintf(stderr, "[D3D] GPU semaphore at 0x%08X\n", sem);
            registered = sem;
        }
        if (0 /* NFSU2 main-loop return addresses */) {
            /* The main loop (sub_000AEA90) calls BlockOnFence on the fence
             * of the frame it just built, right before Present: the whole
             * frame has to be through the GPU before the next one starts,
             * so the game and the pushbuffer executor take turns instead
             * of overlapping (Eden race: game waited ~50% of the time,
             * executor ~20% idle). RECOMP_FRAME_LAG=1 waits for the
             * previous frame's fence instead: one frame in flight, as on a
             * PC. Race frames checked on Linux, no corruption. */
            static int lag = -1;
            static uint32_t prev;
            if (lag < 0) {
                const char *e = getenv("RECOMP_FRAME_LAG");
                lag = e && *e == '1';
                fprintf(stderr, "[D3D] frame fence: %s\n",
                        lag ? "previous frame (RECOMP_FRAME_LAG=1)" : "this frame");
            }
            if (lag) {
                uint32_t mine = MEM32(esp + 4);
                if (prev)
                    MEM32(esp + 4) = prev;
                prev = mine;
            }
        }
        sub_001D0340_gen();
        return;
    }
    if (sem)
        MEM32(sem) = MEM32(dev + 0x2C);     /* "GPU" is always caught up */
    eax = 0;
    esp += 4 + 8;                           /* return address, 2 stdcall args */
}

/* ── D3D interrupt handlers at DISPATCH_LEVEL ────────────────
 *
 * D3D services the GPU from its DPC (0x002F2530) and, with the interrupt
 * masked, from its own busy-waits (BlockUntilIdle and friends) on the game
 * thread: both call the vblank handler (0x001D9BB0) and the PGRAPH handler
 * (0x001DA120), which queues flips. On the Xbox the two never interleave --
 * one is a DPC, the other runs with the interrupt off. Here the DPC runs on
 * the kernel timer thread, and lifted code hands over the guest lock at any
 * function entry, so the vblank count could move between D3D computing a
 * flip's target vblank and checking it. D3D retires a flip only when the
 * count *equals* its target, so that flip stayed pending forever and
 * PersistDisplay, which waits for pending flips, never returned: the hang
 * starting a Quick Race or Career. Run at DISPATCH_LEVEL (the dispatch lock,
 * kernel_hal.c), neither can be interrupted by the other.
 *
 * Raising blocks on the lock that the timer thread holds while it waits for
 * the guest lock, so the guest lock is let go for the raise, as a kernel
 * call would; already at DISPATCH (inside the DPC) there is nothing to take. */
extern uint8_t xbox_KfRaiseIrql(uint8_t irql);
extern void xbox_KfLowerIrql(uint8_t irql);
extern uint8_t xbox_CurrentIrql(void);
extern int xbox_gil_suspend(void);
extern void xbox_gil_resume(int depth);
extern void xbox_Nv2aVblankTaken(void);

static uint8_t d3d_isr_enter(void)
{
    uint8_t old = xbox_CurrentIrql();

    if (old < 2) {
        int d = xbox_gil_suspend();
        old = xbox_KfRaiseIrql(2);
        xbox_gil_resume(d);
    }
    return old;
}

static void d3d_isr_leave(uint8_t old)
{
    if (old < 2)
        xbox_KfLowerIrql(old);
}

/* Flips retired by a handler call: D3D's retire index, at +0x1BC of the
 * block both handlers take in ecx (their own reads index the pending-flip
 * slots from it). The pushbuffer executor's FLIP_STALL waits on these, as
 * PGRAPH waits on the retire's PGRAPH_INCREMENT write. */
extern void nv2a_pb_flip_retired(unsigned n);

/* Vblank handler, thiscall. It ends by writing PCRTC_INTR and spinning until
 * PMC_INTR bit 24 drops. Its callers (the DPC, D3D's busy-waits) have read
 * the bits before calling it, so this is the vblank being taken: clear them
 * now, and the spin ends at once instead of holding the guest lock until the
 * runtime's ack thread comes round. */
extern void sub_001D9BB0_gen(void);

void sub_001D9BB0(void)
{
    uint32_t blk = ecx;
    uint32_t head = MEM32(blk + 0x1BC);
    uint8_t old = d3d_isr_enter();

    xbox_Nv2aVblankTaken();
    sub_001D9BB0_gen();
    nv2a_pb_flip_retired(MEM32(blk + 0x1BC) - head);
    d3d_isr_leave(old);
}

/* PGRAPH handler, thiscall, ecx = the device's hardware block (+0 =
 * 0xFD000000). Reads a software-method trap from PGRAPH_INTR / NSOURCE /
 * TRAPPED_ADDR / TRAPPED_DATA, acknowledges it by writing PGRAPH_INTR back,
 * and acts on it: NOP(5) sets the event BlockOnTime sleeps on, a flip trap
 * queues the flip (sub_002F2080). The acknowledge is write-1-to-clear on
 * hardware and a no-op on RAM. Left pending, every turn of a busy-wait took
 * the same trap again (tens of thousands of flips a second, until the flip
 * targets were millions of vblanks ahead); guessed from the handler's
 * PGRAPH_FIFO writes, a trap posted during a call was sometimes dropped (the
 * fence event never set). So the executor posts traps under a lock held here
 * around the call, and a call that found one reports it taken, which clears
 * it (nv2a_pb_exec.c). */
extern void sub_001DA120_gen(void);
extern void nv2a_pb_trap_lock(void);
extern void nv2a_pb_trap_unlock(void);
extern void nv2a_pb_trap_taken(void);

void sub_001DA120(void)
{
    volatile uint32_t *nv = (volatile uint32_t *)XBOX_PTR(0xFD000000u);
    uint32_t blk = ecx;
    uint32_t head = MEM32(blk + 0x1BC);
    uint8_t old = d3d_isr_enter();
    uint32_t nsource;

    nv2a_pb_trap_lock();
    nsource = nv[0x400108 / 4];
    sub_001DA120_gen();
    nv2a_pb_flip_retired(MEM32(blk + 0x1BC) - head);   /* a flip queued on its vblank retires at once */
    if (nsource)
        nv2a_pb_trap_taken();
    nv2a_pb_trap_unlock();
    d3d_isr_leave(old);
}

/* ── DirectSound DSP command post (0x001E4CA1) ───────────────
 *
 * thiscall, ecx = the DSP-side object. Copies a command block into the GP
 * DSP's scratch area, stores the command code at scratch + 0x810, and spins
 * until the DSP zeroes it (this loop does re-read memory). The scratch block
 * is ***(this + 8 + 0x10), the same chain the runtime's RECOMP_DSP_ACK notes
 * describe. There is no DSP, so register the word with the runtime's
 * completion list before running the original body. */
extern int xbox_ApuDspAckWord(uint32_t va);
extern void sub_001E4CA1_gen(void);

void sub_001E4CA1(void)
{
    uint32_t obj = MEM32(ecx + 8);
    uint32_t blk = obj ? MEM32(obj + 0x10) : 0;
    uint32_t scratch = blk ? MEM32(blk) : 0;

    if (scratch)
        xbox_ApuDspAckWord(scratch + 0x810);
    sub_001E4CA1_gen();
}

/* ── DirectSound AC'97 channel reset (0x001EA5B2) ────────────
 *
 * thiscall, ecx = channel object. Sets RR (bit 1) in the channel's NABM
 * control byte, then waits for the controller to clear it -- except MSVC
 * hoisted the load out of the loop, so the original reads the byte once and
 * spins on the register copy forever unless the bit is already clear at that
 * one read:
 *
 *     mov  byte [eax+0xFEC0010B], 2
 *     mov  cl, [eax+0xFEC0010B]
 *     and  cl, 2
 *   L: test cl, cl
 *     jne  L
 *
 * On hardware the reset has completed by then. The Windows runtime answers it
 * with a write trap on the NABM page; there is no fault handling on POSIX or
 * Switch, so this is the same function with the reset completing at once.
 * Everything else -- the lock taken through sub_001E28B0/sub_001E28D2, the
 * buffer-descriptor base store, the 0xFEC0017C write for channel 1 and the
 * sub_001EA3BC re-arm -- is as the original does it. */
void sub_001EA5B2(void)
{
    uint32_t ebp_saved = g_ebp, frame, chan, nabm;

    PUSH32(esp, ebp_saved);
    frame = esp;
    g_ebp = frame;
    g_seh_ebp = frame;
    esp -= 8;
    MEM32(frame - 4) = 0;
    PUSH32(esp, esi);
    esi = ecx;
    chan = esi;

    ecx = frame - 8;
    PUSH32(esp, 0x001EA5C6u); RECOMP_ABI_CALL(0x001E28B0u, sub_001E28B0);

    nabm = MEM32(MEM32(chan) * 4 + 0x001EB200u);
    MEM8(nabm + 0xFEC0010Bu) = 2;
    MEM8(nabm + 0xFEC0010Bu) &= (uint8_t)~2u;   /* reset complete */
    eax = nabm;
    SET_LO8(ecx, 0);

    MEM32(nabm + 0xFEC00100u) = MEM32(chan + 0x1C);
    if (MEM32(chan) == 1)
        MEM32(0xFEC0017Cu) = MEM32(chan + 0x28);

    PUSH32(esp, 1);
    PUSH32(esp, 1);
    ecx = chan;
    PUSH32(esp, MEM8(chan + 0x24));
    PUSH32(esp, MEM8(chan + 0x25));
    eax = MEM8(chan + 0x25);
    g_ebp = frame;
    g_seh_ebp = frame;
    PUSH32(esp, 0x001EA619u); RECOMP_ABI_CALL(0x001EA3BCu, sub_001EA3BC);

    ecx = frame - 8;
    g_ebp = frame;
    g_seh_ebp = frame;
    PUSH32(esp, 0x001EA621u); RECOMP_ABI_CALL(0x001E28D2u, sub_001E28D2);

    POP32(esp, esi);
    esp = frame;
    POP32(esp, ebp_saved);
    g_ebp = ebp_saved;
    esp += 4;
}

/* Hor+ 16:9. NFSU1 has no widescreen mode (its only XGetVideoFlags caller,
 * 0x16EA50, reads the PAL-60 bit), so with the TV reported 16:9 the
 * presenter stretched the 4:3 picture. The view projection (sub_00018E60,
 * thiscall: ecx = the view's matrix block, arg = the view) builds
 *   m00 = cot(fov/2), m11 = cot(fov * H/W / 2)
 * from the render target's size, then sub_00018E40 forms view*proj, and the
 * frustum planes are taken from that product. Scaling m00 by 3/4 just
 * before the product widens the picture to 16:9 at the same vertical FOV,
 * culling included. Only 4:3 targets (race 640x480, its 320x240 copy, the
 * front end): the 128x128 environment-map faces stay square.
 * RECOMP_HORPLUS=0 restores the stretched 4:3 picture. */
extern int xbox_video_widescreen(void);
extern void sub_00018E60_gen(void);
extern void sub_00018E40_gen(void);
static uint32_t s_proj_view;

static int horplus_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_HORPLUS");
        on = xbox_video_widescreen() && !(e && e[0] == '0');
    }
    return on;
}

void sub_00018E60(void)
{
    s_proj_view = MEM32(esp + 4);
    sub_00018E60_gen();
    s_proj_view = 0;
}

/* Game flow state: 3 in the front end, 4 loading, 6 racing (the game
 * itself tests `cmp [0x283434], 3`). */
#define NFSU1_GAMEFLOW_STATE 0x00283434u

void sub_00018E40(void)
{
    uint32_t v = s_proj_view;
    int fe;

    if (!v || MEM32(esp) != 0x0001902Au) {
        sub_00018E40_gen();
        return;
    }
    fe = MEM32(NFSU1_GAMEFLOW_STATE) == 3;

    if (horplus_on()) {
        uint32_t rt = MEM32(v + 0x58);
        uint32_t w = rt ? MEM32(rt + 0x1C) : 0, h = rt ? MEM32(rt + 0x20) : 0;
        if (w && w * 3 == h * 4) {
            /* The front end zooms instead (m11 x 4/3): its garage set
             * ends just past the 4:3 frame -- the tunnel mouth on the
             * right -- so it keeps the original width, minus top and
             * bottom. */
            uint32_t at = ecx + (fe ? 0x54 : 0x40);   /* m11 : m00 */
            float m;
            memcpy(&m, (void *)XBOX_PTR(at), 4);
            m *= fe ? 4.0f / 3.0f : 0.75f;
            memcpy((void *)XBOX_PTR(at), &m, 4);
        }
    }
    /* Front-end garage camera (views 1 and 4 share it) turned left by
     * RECOMP_FE_YAW degrees (default 5): rows 0 and 2 of the view matrix
     * at ecx (row-major, column vectors, eye +z forward) rotated about
     * the eye's y axis. sub_00018E60 copies the camera in afresh. */
    if (fe && (MEM32(v + 4) == 1 || MEM32(v + 4) == 4)) {
        static float yaw = -1000.0f;
        if (yaw == -1000.0f) {
            const char *e = getenv("RECOMP_FE_YAW");
            yaw = (e ? (float)atof(e) : 5.0f) * 3.14159265f / 180.0f;
        }
        if (yaw != 0.0f) {
            float m[16], c = cosf(yaw), s = sinf(yaw);
            int j;
            memcpy(m, (void *)XBOX_PTR(ecx), sizeof m);
            for (j = 0; j < 4; j++) {
                float r0 = m[j], r2 = m[8 + j];
                m[j]     = c * r0 + s * r2;
                m[8 + j] = -s * r0 + c * r2;
            }
            memcpy((void *)XBOX_PTR(ecx), m, sizeof m);
        }
    }
    sub_00018E40_gen();
}

/* ── Options -> Camera: car reflections and picture rows ──────
 *
 * The port's own settings, as NFSU2 has them under Options -> Video, live
 * on the Camera screen: Display already has seven rows and no room for
 * more, Camera has two (Favorite Drive Camera, Jump Cameras).
 *
 * All option screens are one class on MU_Options.fng; the Options menu
 * leaves the chosen one in [0x2BBB54] (1 = Camera) and the screen calls
 * its setup: sub_000C2A70 for Camera. A row N (1..10) is the package's
 * OptionName_N / OptionData_N / LeftArrow_N / RightArrow_N objects:
 * sub_000C1D30(N, data, select button) shows it and makes it selectable,
 * sub_000C1E10(N, 0) adds the arrows. Pad left/right on a row call
 * this+0x3C+4*(N-1) (thiscall, the message, ret 4; dispatch at
 * 0x000C281C), Favorite Drive Camera's being sub_000C0C90: step a value,
 * rewrite the texts with sub_000DD490(package, object name, text) and
 * play the arrow sound (sub_000DDDE0 / sub_000DDD60). Rows 3..7 are added
 * the same way after the original setup; their handlers are our own, at
 * unused int3 bytes after sub_000C0C20 (ROW_HANDLER_VA, recomp_lookup_manual).
 * Texts are our own strings in guest memory -- the language files are
 * Huffman-packed, and dd490 takes any text.
 *
 * Car Reflections Off: the Vulkan renderer's cube stages sample black
 * (nv2a_vk_cube_maps; the GL renderer never drew them) and the race
 * renderer sub_00016050 skips the six "EnvMap %d" cube-face views (ids
 * 10..15, view(id) = 0x2C5F20 + id*0x60, drawn only while byte +8 is set):
 * the bytes are cleared for the call and put back after it.
 *
 * Vulkan build only: Resolution Scale 1x/1.5x/2x/2.5x (nv2a_vk_scale_pct,
 * applied at the next flip; without a saved value RECOMP_GL_SCALE rules
 * and the row shows the nearest step), Anti-Aliasing = FXAA
 * (nv2a_vk_fxaa), Anisotropic Off..16x (nv2a_vk_aniso), Square Pixels
 * (nv2a_vk_square: 4/3 the columns in 16:9). The renderer's env switches
 * still force each off.
 *
 * Kept in nfsu1x_options.txt (sdmc:/switch/nfsu1x/ on the Switch, the
 * working directory elsewhere), not in the profile; loaded at boot
 * (nfsu1_options_load, main.c). */
#ifdef __SWITCH__
#  define NFSU1_OPTIONS_FILE "sdmc:/switch/nfsu1x/nfsu1x_options.txt"
#else
#  define NFSU1_OPTIONS_FILE "nfsu1x_options.txt"
#endif
#define FE_PAD_LEFT     0x9120409Eu
#define FE_PAD_RIGHT    0xB5971BF1u
#define ROW_HANDLER_VA  0x000C0C83u     /* + row index: int3 padding */
#define ROW_FIRST       3               /* rows 1-2 are the game's */
#define ENVMAP_VIEW(id) (0x002C5F20u + (uint32_t)(id) * 0x60u)

#ifdef NFSU2_VULKAN
extern volatile int nv2a_vk_cube_maps;
extern volatile int nv2a_vk_scale_pct;
extern volatile int nv2a_vk_fxaa;
extern volatile int nv2a_vk_aniso;
extern volatile int nv2a_vk_square;
#endif
extern uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t alignment);
extern void sub_000C2A70_gen(void);
extern void sub_00016050_gen(void);

static const int s_scale_pct[] = { 100, 150, 200, 250 };
static const int s_aniso_lvl[] = { 1, 2, 4, 8, 16 };

static int s_refl = 1;                  /* 0 off, 1 on */
static int s_scale_idx = 1;             /* into s_scale_pct: 1.5x */
static int s_scale_saved;               /* nfsu1x_options.txt had scale= */
static int s_fxaa = 1;                  /* 0 off, 1 on */
static int s_aniso_idx = 4;             /* into s_aniso_lvl: 16x */
static int s_square = 1;                /* 0 off, 1 on */

typedef struct {
    const char *label;
    const char *const *texts;
    int n;
    int *value;
    void (*changed)(void);
} OptRow;

static void refl_changed(void)
{
#ifdef NFSU2_VULKAN
    nv2a_vk_cube_maps = s_refl;
#endif
}

#ifdef NFSU2_VULKAN
static void scale_changed(void)
{
    nv2a_vk_scale_pct = s_scale_pct[s_scale_idx];
    s_scale_saved = 1;
}

static void picture_changed(void)
{
    nv2a_vk_fxaa = s_fxaa;
    nv2a_vk_aniso = s_aniso_lvl[s_aniso_idx];
    nv2a_vk_square = s_square;
}
#endif

static const char *const s_onoff[] = { "Off", "On" };
#ifdef NFSU2_VULKAN
static const char *const s_scale_texts[] = { "1x", "1.5x", "2x", "2.5x" };
static const char *const s_aniso_texts[] = { "Off", "2x", "4x", "8x", "16x" };
#endif
static const OptRow s_rows[] = {
    { "Car Reflections:", s_onoff, 2, &s_refl, refl_changed },
#ifdef NFSU2_VULKAN
    { "Resolution Scale:", s_scale_texts, 4, &s_scale_idx, scale_changed },
    { "Anti-Aliasing:", s_onoff, 2, &s_fxaa, picture_changed },
    { "Anisotropic:", s_aniso_texts, 5, &s_aniso_idx, picture_changed },
    { "Square Pixels:", s_onoff, 2, &s_square, picture_changed },
#endif
};
#define N_ROWS ((int)(sizeof s_rows / sizeof s_rows[0]))

static void options_log(void)
{
#ifdef NFSU2_VULKAN
    fprintf(stderr, "[options] car reflections %s, resolution scale %d%%%s, anti-aliasing %s,"
            " anisotropic %dx, square pixels %s\n",
            s_refl ? "on" : "off", s_scale_pct[s_scale_idx],
            nv2a_vk_scale_pct ? "" : " (RECOMP_GL_SCALE)", s_fxaa ? "on" : "off",
            s_aniso_lvl[s_aniso_idx], s_square ? "on" : "off");
#else
    fprintf(stderr, "[options] car reflections %s\n", s_refl ? "on" : "off");
#endif
}

void nfsu1_options_load(void)
{
    char line[128];
    FILE *f = fopen(NFSU1_OPTIONS_FILE, "r");
    int i;

    if (f) {
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "reflections=", 12))
                s_refl = line[12] != '0';
            else if (!strncmp(line, "fxaa=", 5))
                s_fxaa = line[5] != '0';
            else if (!strncmp(line, "square=", 7))
                s_square = line[7] != '0';
            else if (!strncmp(line, "aniso=", 6)) {
                int a = atoi(line + 6);
                for (i = 0; i < 5; i++)
                    if (s_aniso_lvl[i] == a)
                        s_aniso_idx = i;
            }
            else if (!strncmp(line, "scale=", 6)) {
                int pct = atoi(line + 6);
                for (i = 0; i < 4; i++)
                    if (s_scale_pct[i] == pct) {
                        s_scale_idx = i;
                        s_scale_saved = 1;
                    }
            }
        }
        fclose(f);
    }
    refl_changed();
#ifdef NFSU2_VULKAN
    {
        const char *e = getenv("RECOMP_GL_SCALE");
        if (!s_scale_saved && e && *e) {
            /* RECOMP_GL_SCALE stays in charge until the option is changed;
             * the row shows the nearest step. */
            double k = strtod(e, NULL), best = 1e9;
            for (i = 0; i < 4; i++) {
                double d = fabs(k * 100.0 - s_scale_pct[i]);
                if (d < best) { best = d; s_scale_idx = i; }
            }
        } else {
            nv2a_vk_scale_pct = s_scale_pct[s_scale_idx];
        }
    }
    picture_changed();
#endif
    options_log();
}

static void options_save(void)
{
    FILE *f = fopen(NFSU1_OPTIONS_FILE, "w");
    if (!f) {
        fprintf(stderr, "[options] cannot write " NFSU1_OPTIONS_FILE "\n");
        return;
    }
    fprintf(f, "reflections=%d\n", s_refl);
    if (s_scale_saved)
        fprintf(f, "scale=%d\n", s_scale_pct[s_scale_idx]);
#ifdef NFSU2_VULKAN
    fprintf(f, "fxaa=%d\naniso=%d\nsquare=%d\n", s_fxaa, s_aniso_lvl[s_aniso_idx], s_square);
#endif
    fclose(f);
}

/* Our texts in guest memory, each copied once. */
static uint32_t guest_str(const char *s)
{
    static struct { const char *s; uint32_t va; } cache[32];
    static uint32_t pool, used;
    size_t n = strlen(s) + 1;
    int i;

    for (i = 0; i < 32 && cache[i].s; i++)
        if (cache[i].s == s)
            return cache[i].va;
    if (i == 32)
        return 0;
    if (!pool) {
        pool = xbox_ContiguousAlloc(0x400u, 16);
        if (!pool)
            return 0;
    }
    if (used + n > 0x400u)
        return 0;
    memcpy((void *)XBOX_PTR(pool + used), s, n);
    cache[i].s = s;
    cache[i].va = pool + used;
    used += (uint32_t)n;
    return cache[i].va;
}

/* sub_000DD490(package, object name, text), cdecl. */
static void fe_set_text(uint32_t screen, const char *object, const char *text)
{
    uint32_t name = guest_str(object), str = guest_str(text);

    if (!name || !str)
        return;
    PUSH32(esp, str);
    PUSH32(esp, name);
    PUSH32(esp, MEM32(screen + 0xCu));
    PUSH32(esp, ROW_HANDLER_VA);
    sub_000DD490();
    esp += 12;
}

static const char *const s_name_obj[] = {
    "OptionName_3", "OptionName_4", "OptionName_5", "OptionName_6", "OptionName_7",
};
static const char *const s_data_obj[] = {
    "OptionData_3", "OptionData_4", "OptionData_5", "OptionData_6", "OptionData_7",
};

static void opt_row_text(uint32_t screen, int i)
{
    fe_set_text(screen, s_name_obj[i], s_rows[i].label);
    fe_set_text(screen, s_data_obj[i], s_rows[i].texts[*s_rows[i].value]);
}

/* Camera setup (thiscall, ecx = options screen, ret): the game's two rows,
 * then ours. */
void sub_000C2A70(void)
{
    uint32_t screen = ecx;
    int i;

    sub_000C2A70_gen();
    for (i = 0; i < N_ROWS; i++) {
        int row = ROW_FIRST + i;
        PUSH32(esp, 0);                     /* no select button */
        PUSH32(esp, 1);                     /* value text */
        PUSH32(esp, (uint32_t)row);
        ecx = screen;
        PUSH32(esp, 0x000C2A99u);
        sub_000C1D30();                     /* thiscall, pops its 3 */
        PUSH32(esp, 0);                     /* arrows, not -/+ */
        PUSH32(esp, (uint32_t)row);
        ecx = screen;
        PUSH32(esp, 0x000C2AB3u);
        sub_000C1E10();                     /* thiscall, pops its 2 */
        MEM32(screen + 0x3Cu + 4u * (uint32_t)(row - 1)) = ROW_HANDLER_VA + (uint32_t)i;
        opt_row_text(screen, i);
    }
}

/* Row input (thiscall, the message, ret 4), as sub_000C0C90: left/right
 * step the value (wrapping), then the texts and the arrow sound. */
static void opt_row_input(int i)
{
    uint32_t screen = ecx, msg = MEM32(esp + 4);
    const OptRow *o = &s_rows[i];

    if (msg == FE_PAD_LEFT || msg == FE_PAD_RIGHT) {
        *o->value = (*o->value + (msg == FE_PAD_RIGHT ? 1 : o->n - 1)) % o->n;
        o->changed();
        options_save();
        options_log();
        opt_row_text(screen, i);
        PUSH32(esp, 0xBu);
        PUSH32(esp, 0xAu);
        PUSH32(esp, msg);
        PUSH32(esp, ROW_HANDLER_VA);
        sub_000DDDE0();
        esp += 12;
        PUSH32(esp, eax);
        PUSH32(esp, MEM32(screen + 0xCu));
        PUSH32(esp, ROW_HANDLER_VA);
        sub_000DDD60();
        esp += 8;
    }
    esp += 8;
}

static void opt_row_input_0(void) { opt_row_input(0); }
#ifdef NFSU2_VULKAN
static void opt_row_input_1(void) { opt_row_input(1); }
static void opt_row_input_2(void) { opt_row_input(2); }
static void opt_row_input_3(void) { opt_row_input(3); }
static void opt_row_input_4(void) { opt_row_input(4); }
#endif

/* Race render (cdecl, no arguments): cube-face views off for the call. */
void sub_00016050(void)
{
    uint8_t keep[6];
    int n;

    if (s_refl) {
        sub_00016050_gen();
        return;
    }
    for (n = 0; n < 6; n++) {
        keep[n] = MEM8(ENVMAP_VIEW(10 + n) + 8u);
        MEM8(ENVMAP_VIEW(10 + n) + 8u) = 0;
    }
    sub_00016050_gen();
    for (n = 0; n < 6; n++)
        MEM8(ENVMAP_VIEW(10 + n) + 8u) = keep[n];
}

/* ── Menu Size: full screen by default ───────────────────────
 *
 * Options -> Display -> Menu Size (0.84..1.0, the game's default 0.92,
 * float at 0x295B00, kept in the profile) shrinks the front end toward
 * the centre for TV overscan: the FE render sub_00016460 scales view 0
 * by it. The loading screen's backdrop is about 680 wide and sits 13
 * pixels right of centre, so at 0.92 its left edge comes on screen and a
 * strip of the previous frame (the garage car) shows there; movies get
 * borders. A handheld or a modern TV has no overscan, so the default is
 * taken as 1.0: before each FE frame 0.92 becomes 1.0 unless the slider
 * (sub_000C1860, Menu Size's row input) set it this session. */
#define NFSU1_MENU_SIZE 0x00295B00u

extern void sub_00016460_gen(void);
extern void sub_000C1860_gen(void);
static uint32_t s_menu_size_user;       /* bits the slider last stored */

void sub_00016460(void)
{
    uint32_t v = MEM32(NFSU1_MENU_SIZE);
    float f;

    memcpy(&f, &v, 4);
    if (v != s_menu_size_user && f > 0.919f && f < 0.921f)
        MEM32(NFSU1_MENU_SIZE) = 0x3F800000u;   /* 1.0f */
    sub_00016460_gen();
}

void sub_000C1860(void)
{
    sub_000C1860_gen();
    s_menu_size_user = MEM32(NFSU1_MENU_SIZE);
}

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    if (xbox_va - ROW_HANDLER_VA < (uint32_t)N_ROWS) {
        static const recomp_func_t row_input[] = {
            opt_row_input_0,
#ifdef NFSU2_VULKAN
            opt_row_input_1, opt_row_input_2, opt_row_input_3, opt_row_input_4,
#endif
        };
        return row_input[xbox_va - ROW_HANDLER_VA];
    }
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}

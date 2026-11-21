/* FLARE_FIX -- NFSU1 light trails at 30 Hz while the game renders at 60.
 *
 * NFSU1 draws its light halos (car lights, street lamps) through a trail
 * buffer, a 640x480 colour surface that is never cleared. Every rendered
 * frame (sub_00015370, BLENDCOLOR set at 0x153B4):
 *   1. decay:  full-screen quad, blend ZERO / CONSTANT_COLOR with blend
 *              colour 0x008296B4 -> buffer *= (0.51, 0.59, 0.71);
 *   2. flares: the light-flare quads, SRC_ALPHA / ONE, depth-tested against
 *              the scene's Z;
 *   3. bloom:  the blurred quarter-size scene, CONSTANT_ALPHA (0xAA) / ONE;
 *   4. the buffer is added onto the frame, ONE / ONE.
 * Steps 1-3 run per frame, so the trail's length and weight were tuned for
 * the Xbox's 30 fps. At 60 fps (uncapped flip interval) a moving light
 * leaves its decaying copies twice as close together: they overlap into a
 * bigger, brighter blob -- oncoming headlights most of all.
 *
 * RECOMP_FLARE_FIX=1 runs steps 1-3 once per 33.3 ms of real time (every
 * second frame at 60 fps, every frame at 30 fps or slower) and skips them
 * otherwise; step 4 runs every frame. The buffer then evolves exactly as on
 * a 30 fps Xbox. =2 updates on every second frame regardless of time (test
 * mode for slow hosts). Default 0: behaviour unchanged.
 *
 * Trail shape (with the fix on; only the decay colour of step 1 changes, no
 * extra pass, the newest flare keeps full intensity). A copy left n updates
 * ago weighs k^n (k = the decay colour per channel), so it falls below a
 * visibility threshold e after n = ln(e) / ln(k) updates.
 *   RECOMP_FLARE_TRAIL=t (default 0.10; 1 = the Xbox's, i.e. the first
 *     FLARE_FIX build): decay k^(1/t) gives n' = t * n, exactly t times
 *     the length.
 *   RECOMP_FLARE_TRAIL_OPACITY=o (default 0.7; 1 = none): every older copy
 *     a further factor o per update, so copy n weighs (o * k^(1/t))^n.
 * Decay colour k' = o * k^(1/t), instead of k = (0.510, 0.588, 0.706).
 * Defaults (t 0.10, o 0.7): k' = (0.0008, 0.0035, 0.021), blend colour
 * 0x00000105 -- the previous copy keeps 0.1 / 0.4 / 2 % (Xbox 51 / 59 /
 * 71 %): practically no trail. t 0.25, o 0.7: (0.047, 0.084, 0.174),
 * 0x000C152C. t 0.5, o 1: (0.260, 0.346, 0.498), 0x0042587F. Side effect:
 * a light that stays put builds up to F / (1 - k') instead of F / (1 - k):
 * with the defaults its halo is the newest flare alone, 0.49 / 0.41 / 0.30
 * of the Xbox's per channel (t 0.5, o 1: 0.66 / 0.63 / 0.59).
 *
 * The trail surface is recognised by its decay pass, not by address.
 * Header-only (no build list changes); revert = this file, flare_glare.h
 * and the FLARE_FIX lines in nv2a_vk.c. */
#ifndef FLARE_FIX_H
#define FLARE_FIX_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FLARE_FIX_PERIOD 0.0333333   /* the Xbox's 30 fps, seconds */
#define FLARE_FIX_DECAY  0x008296B4u /* the title's decay colour (0x153B4) */

static int      s_ff_mode = -1;      /* RECOMP_FLARE_FIX: 0 off, 1 33.3 ms, 2 every 2nd frame */
static int      s_ff_glare;          /* RECOMP_FLARE_GLARE: 0 off, 1 on, 2 even frames only (flare_glare.h) */
static uint32_t s_ff_trail_va;       /* the trail surface, once seen */
static uint32_t s_ff_trail_frame = ~0u;   /* frame of its last decay pass */
static uint32_t s_ff_decay = FLARE_FIX_DECAY;

static double flare_fix_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

static void flare_fix_init(void)
{
    const char *e = getenv("RECOMP_FLARE_FIX");
    s_ff_mode = e && (*e == '1' || *e == '2') ? *e - '0' : 0;
    e = getenv("RECOMP_FLARE_GLARE");
    s_ff_glare = e && (*e == '1' || *e == '2') ? *e - '0' : 0;
    if (s_ff_mode) {
        double t = 0.10, o = 0.7;
        int i;
        e = getenv("RECOMP_FLARE_TRAIL");
        if (e && *e)
            t = atof(e);
        if (!(t >= 0.05 && t <= 4.0))
            t = 0.10;
        e = getenv("RECOMP_FLARE_TRAIL_OPACITY");
        if (e && *e)
            o = atof(e);
        if (!(o >= 0.0 && o <= 1.0))
            o = 0.7;
        s_ff_decay = 0;
        for (i = 0; i < 24; i += 8) {
            double k = ((FLARE_FIX_DECAY >> i) & 0xFF) / 255.0;
            s_ff_decay |= (uint32_t)lround(255.0 * o * pow(k, 1.0 / t)) << i;
        }
        fprintf(stderr, "[flare] trail updates at 30 Hz (%s), length x%.2f, opacity %.2f (decay %08X -> %08X)\n",
                s_ff_mode == 1 ? "33.3 ms" : "every 2nd frame", t, o, FLARE_FIX_DECAY, s_ff_decay);
    }
}

/* 1: drop this draw (a trail-buffer update on a frame that is not a 30 Hz
 * tick). May point *pb / *pr at a copy with the trail length's decay. */
static int flare_fix_draw(const Nv2aRawBatch **pb, const uint32_t **pr, uint32_t frame)
{
    static int tick;
    static uint32_t n_frames, n_ticks;
    static double acc, last, log_t;
    static Nv2aRawBatch cb;
    static uint32_t cr[0x2000 / 4];
    const Nv2aRawBatch *b = *pb;
    const uint32_t *r = *pr;

    if (s_ff_mode < 0)
        flare_fix_init();
    if (!s_ff_mode && !s_ff_glare)
        return 0;
    /* The decay pass opens each frame's update of the trail buffer. */
    if (r[0x304 / 4] && r[0x344 / 4] == 0 && r[0x348 / 4] == 0x8001 && r[0x34C / 4] == FLARE_FIX_DECAY) {
        if (frame != s_ff_trail_frame) {
            double now = flare_fix_now();
            s_ff_trail_va = b->surface.color_va;
            s_ff_trail_frame = frame;
            if (s_ff_mode == 2) {
                tick = !tick;
            } else if (s_ff_mode == 1) {
                acc = last ? acc + (now - last) : FLARE_FIX_PERIOD;
                last = now;
                tick = acc >= FLARE_FIX_PERIOD;
                if (tick)
                    acc = acc >= 2 * FLARE_FIX_PERIOD ? 0 : acc - FLARE_FIX_PERIOD;
            } else {
                tick = 1;                       /* glare only: the title's own cadence */
            }
            n_frames++;
            n_ticks += tick;
            if (!log_t)
                log_t = now;
            if (s_ff_mode && now - log_t >= 10.0) {
                fprintf(stderr, "[flare] trail %08X updated on %u of %u frames\n", s_ff_trail_va, n_ticks, n_frames);
                n_frames = n_ticks = 0;
                log_t = now;
            }
        }
        if (tick && s_ff_decay != FLARE_FIX_DECAY) {
            memcpy(cr, r, sizeof cr);
            cr[0x34C / 4] = s_ff_decay;
            cb = *b;
            cb.regs = cr;
            *pb = &cb;
            *pr = cr;
        }
    }
    return s_ff_mode && !tick && frame == s_ff_trail_frame && b->surface.color_va == s_ff_trail_va;
}

#endif

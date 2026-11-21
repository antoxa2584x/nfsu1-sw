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
 * The trail surface is recognised by its decay pass, not by address.
 * Header-only (no build list changes); revert = this file plus the
 * FLARE_FIX lines in nv2a_vk.c. */
#ifndef FLARE_FIX_H
#define FLARE_FIX_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define FLARE_FIX_PERIOD 0.0333333   /* the Xbox's 30 fps, seconds */

static double flare_fix_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

/* 1: drop this draw (a trail-buffer update on a frame that is not a 30 Hz tick). */
static int flare_fix_skip(const Nv2aRawBatch *b, uint32_t frame)
{
    static int mode = -1, tick;
    static uint32_t trail_va, cur_frame = ~0u, n_frames, n_ticks;
    static double acc, last, log_t;
    const uint32_t *r = b->regs;

    if (mode < 0) {
        const char *e = getenv("RECOMP_FLARE_FIX");
        mode = e && (*e == '1' || *e == '2') ? *e - '0' : 0;
        if (mode)
            fprintf(stderr, "[flare] trail updates at 30 Hz (%s)\n", mode == 1 ? "33.3 ms" : "every 2nd frame");
    }
    if (!mode)
        return 0;
    /* The decay pass opens each frame's update of the trail buffer. */
    if (r[0x304 / 4] && r[0x344 / 4] == 0 && r[0x348 / 4] == 0x8001 && r[0x34C / 4] == 0x008296B4u
     && frame != cur_frame) {
        double now = flare_fix_now();
        trail_va = b->surface.color_va;
        cur_frame = frame;
        if (mode == 2) {
            tick = !tick;
        } else {
            acc = last ? acc + (now - last) : FLARE_FIX_PERIOD;
            last = now;
            tick = acc >= FLARE_FIX_PERIOD;
            if (tick)
                acc = acc >= 2 * FLARE_FIX_PERIOD ? 0 : acc - FLARE_FIX_PERIOD;
        }
        n_frames++;
        n_ticks += tick;
        if (!log_t)
            log_t = now;
        if (now - log_t >= 10.0) {
            fprintf(stderr, "[flare] trail %08X updated on %u of %u frames\n", trail_va, n_ticks, n_frames);
            n_frames = n_ticks = 0;
            log_t = now;
        }
    }
    return !tick && frame == cur_frame && b->surface.color_va == trail_va;
}

#endif

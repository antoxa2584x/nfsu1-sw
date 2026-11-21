/*
 * fast_time.h - RECOMP_FAST_TIME=1: the host clocks from the TSC (default off).
 *
 * Why: on the PS5 clock_gettime is a system call, 20.3 us against 11.8 ns for
 * a TSC read (PS5_Vulkan docs/HARDWARE_FINDINGS.md, 2026-09-23, R34 cost
 * probe), and a race reads the clock ~110 times a frame through the guest
 * rdtsc emulation alone (QueryPerformanceCounter <- xbox_ReadTimeStampCounter),
 * plus GetTickCount64 on every kernel call. In a PS5 sampling profile ~80% of
 * the samples in race hitch frames were inside clock_gettime.
 *
 * One clock: ns = anchor_ns + (rdtsc - anchor_tsc) * scale. The anchor is a
 * CLOCK_MONOTONIC reading paired with the TSC midpoint of that call; whichever
 * thread finds it older than FT_ANCHOR_NS refreshes it (a try-lock -- the
 * others keep extrapolating, nobody waits on the system call). Two anchor slots,
 * each under its own sequence number, so a reader never waits for a writer.
 * scale is re-measured from anchors >= 100 ms apart and smoothed (it tracks
 * CLOCK_MONOTONIC being slewed: under WSL it runs at 4.33 or 4.69 GHz of TSC,
 * switching every few seconds, and the error peaks at each switch). The value
 * handed out is the atomic max of everything handed out, so a re-anchor that
 * lands behind the extrapolation holds the clock still instead of stepping it
 * back. QueryPerformanceCounter, GetTickCount(64) and GetSystemTimeAsFileTime
 * (wall offset read once at init + this clock) all read it, so every guest
 * time source -- rdtsc emulation, KeQueryPerformanceCounter, KeQuerySystemTime,
 * KeQueryInterruptTime, KeTickCount, KeSystemTime/KeInterruptTime -- runs on
 * the same clock in the same units as before.
 *
 * Self-check: each accepted anchor compares the extrapolated value with the
 * fresh clock_gettime at the same TSC; every 10 s the re-anchoring thread
 * prints "[time] fast on: ..." (counters per 10 s window). "max |fast-real|"
 * takes anchors within 1.25 * FT_ANCHOR_NS of the previous one: readers are
 * served at most FT_ANCHOR_NS of extrapolation (the first read past it
 * re-anchors and gets the fresh value; one that loses the re-anchor try-lock
 * gets at most 2 * FT_ANCHOR_NS), so this is the error they could see.
 * After a longer gap nobody read the far end of the line; that error is
 * "gaps" (it grows with the gap when the rate is off).
 *
 * Discontinuities (rest mode: the TSC reset, or the TSC and CLOCK_MONOTONIC
 * advanced by different amounts while asleep): a TSC more than FT_BACK_NS
 * behind the anchor makes the anchor stale, and a re-anchor that finds the TSC
 * behind the anchor or |fast-real| > FT_JUMP_NS restarts from clock_gettime --
 * fresh anchor, fresh scale baseline, scale kept -- and counts a
 * "discontinuity". No reader is handed more than 2 * FT_ANCHOR_NS past the
 * anchor's real time, so the max above never runs ahead and holds the clock.
 *
 * The state is static: included by platform/win32_compat.c only (one copy for
 * the runtime); ps5/tests/fast_time_test.c includes it for a copy of its own.
 */
#ifndef FAST_TIME_H
#define FAST_TIME_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#if defined(__x86_64__) || defined(__i386__)
#  define FT_HAVE_TSC 1
#else
#  define FT_HAVE_TSC 0
#endif

#define FT_ANCHOR_NS    2000000ull    /* re-anchor once the anchor is 2 ms old */
#define FT_WINDOW_NS     100000ull    /* clock_gettime that took longer pins nothing */
#define FT_BASE_NS    100000000ull    /* scale baseline: re-measured every 100 ms */
#define FT_REPORT_NS 10000000000ull   /* [time] line cadence */
#define FT_BACK_NS      1000000ull    /* TSC this far behind the anchor: reset, not core skew */
#define FT_JUMP_NS     50000000ull    /* |fast-real| past this at a re-anchor: a discontinuity */

typedef struct {
    uint32_t seq;                     /* odd while being written */
    uint64_t tsc, ns;                 /* the pair */
    uint64_t scale;                   /* ns per tick, 32.32 fixed point */
} FtAnchor;

static int      ft_on = -1;           /* -1 until ft_init */
static FtAnchor ft_a[2];
static uint32_t ft_cur;               /* published slot */
static uint32_t ft_busy;              /* re-anchor try-lock */
static uint64_t ft_last;              /* largest value handed out */
static int64_t  ft_wall_off;          /* CLOCK_REALTIME - CLOCK_MONOTONIC at init, ns */
/* Writer-only (under ft_busy) */
static uint64_t ft_base_tsc, ft_base_ns, ft_report_ns;
static int      ft_scale_measured;
/* Per-window counters */
static uint64_t ft_n_anchor, ft_n_skip, ft_n_clamp, ft_max_err, ft_max_err_gap, ft_n_disc;

static inline uint64_t ft_rdtsc(void)
{
#if defined(FT_TEST_TSC)
    return FT_TEST_TSC();             /* ps5/tests/fast_time_test.c: a TSC that jumps */
#elif FT_HAVE_TSC
    return __builtin_ia32_rdtsc();
#else
    return 0;
#endif
}

static inline uint64_t ft_mono_ns(clockid_t id)
{
    struct timespec ts;
    clock_gettime(id, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline uint64_t ft_scaled(uint64_t ticks, uint64_t scale)
{
    return (uint64_t)(((unsigned __int128)ticks * scale) >> 32);
}

static inline int ft_active(void)
{
    return __atomic_load_n(&ft_on, __ATOMIC_ACQUIRE) == 1;
}

static inline void ft_publish(uint64_t tsc, uint64_t ns, uint64_t scale)
{
    FtAnchor *a = &ft_a[(__atomic_load_n(&ft_cur, __ATOMIC_RELAXED) + 1) & 1];
    uint32_t s = __atomic_load_n(&a->seq, __ATOMIC_RELAXED);
    __atomic_store_n(&a->seq, s + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&a->tsc, tsc, __ATOMIC_RELAXED);
    __atomic_store_n(&a->ns, ns, __ATOMIC_RELAXED);
    __atomic_store_n(&a->scale, scale, __ATOMIC_RELAXED);
    __atomic_store_n(&a->seq, s + 2, __ATOMIC_RELEASE);
    __atomic_store_n(&ft_cur, (uint32_t)(a - ft_a), __ATOMIC_RELEASE);
}

static inline void ft_load(uint64_t *tsc, uint64_t *ns, uint64_t *scale)
{
    for (;;) {
        const FtAnchor *a = &ft_a[__atomic_load_n(&ft_cur, __ATOMIC_ACQUIRE) & 1];
        uint32_t s = __atomic_load_n(&a->seq, __ATOMIC_ACQUIRE);
        *tsc = __atomic_load_n(&a->tsc, __ATOMIC_RELAXED);
        *ns = __atomic_load_n(&a->ns, __ATOMIC_RELAXED);
        *scale = __atomic_load_n(&a->scale, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        /* A slot is rewritten two anchors (>= 4 ms) later: retried only by a
         * reader pre-empted that long between the loads. */
        if (!(s & 1) && __atomic_load_n(&a->seq, __ATOMIC_RELAXED) == s)
            return;
    }
}

static inline void ft_report(FILE *out, uint64_t scale)
{
    fprintf(out, "[time] fast on: anchors %llu, max |fast-real| %.1f us, backsteps clamped %llu, "
                 "scale %.6f GHz, skipped %llu, gaps %.1f us, discontinuities %llu (per 10 s)\n",
            (unsigned long long)__atomic_load_n(&ft_n_anchor, __ATOMIC_RELAXED),
            __atomic_load_n(&ft_max_err, __ATOMIC_RELAXED) / 1000.0,
            (unsigned long long)__atomic_load_n(&ft_n_clamp, __ATOMIC_RELAXED),
            4294967296.0 / (double)scale,
            (unsigned long long)__atomic_load_n(&ft_n_skip, __ATOMIC_RELAXED),
            __atomic_load_n(&ft_max_err_gap, __ATOMIC_RELAXED) / 1000.0,
            (unsigned long long)__atomic_load_n(&ft_n_disc, __ATOMIC_RELAXED));
    fflush(out);
}

/* One CLOCK_MONOTONIC reading and the TSC midpoint of the call that took it
 * (it sampled the clock somewhere in between): the tightest of up to three
 * tries, fewer once one is within `enough` ticks. Returns the window, ticks. */
static inline uint64_t ft_pair(uint64_t *tsc, uint64_t *ns, uint64_t enough)
{
    uint64_t best = UINT64_MAX;
    int i;
    for (i = 0; i < 3 && best > enough; i++) {
        uint64_t t0 = ft_rdtsc(), n = ft_mono_ns(CLOCK_MONOTONIC), t1 = ft_rdtsc();
        if (t1 - t0 < best) {
            best = t1 - t0;
            *tsc = t0 + best / 2;
            *ns = n;
        }
    }
    return best;
}

/* Caller holds ft_busy. cur_*: the published anchor. */
static __attribute__((noinline)) void ft_reanchor(uint64_t cur_tsc, uint64_t cur_ns, uint64_t scale)
{
    uint64_t tsc, ns, pred, err, win = ft_pair(&tsc, &ns, scale ? (FT_WINDOW_NS << 32) / scale : 0);

    pred = cur_ns + ft_scaled(tsc > cur_tsc ? tsc - cur_tsc : 0, scale);
    err = pred > ns ? pred - ns : ns - pred;
    if (tsc < cur_tsc || err > FT_JUMP_NS + ft_scaled(win, scale)) {
        /* The TSC was reset or jumped against CLOCK_MONOTONIC: the line is
         * void and so is the scale baseline across the jump. Restart both
         * from the real time; keep scale (the rate itself did not change). */
        __atomic_add_fetch(&ft_n_disc, 1, __ATOMIC_RELAXED);
        ft_base_tsc = tsc;
        ft_base_ns = ns;
    } else if (ft_scaled(win, scale) > FT_WINDOW_NS) {
        /* Too coarse to correct anything: move the anchor along the current
         * line (the clock itself does not change) and try again next time. */
        __atomic_add_fetch(&ft_n_skip, 1, __ATOMIC_RELAXED);
        ft_publish(tsc, pred, scale);
        return;
    } else {
        uint64_t *m = pred - cur_ns <= FT_ANCHOR_NS + FT_ANCHOR_NS / 4 ? &ft_max_err : &ft_max_err_gap;
        if (err > __atomic_load_n(m, __ATOMIC_RELAXED))
            __atomic_store_n(m, err, __ATOMIC_RELAXED);
        __atomic_add_fetch(&ft_n_anchor, 1, __ATOMIC_RELAXED);

        if (ns - ft_base_ns >= FT_BASE_NS && tsc > ft_base_tsc) {
            uint64_t m = (uint64_t)(((unsigned __int128)(ns - ft_base_ns) << 32) / (tsc - ft_base_tsc));
            /* The first baseline replaces the 2 ms boot estimate; later ones are
             * smoothed (each end is +-FT_WINDOW_NS/2: <= 0.1% of 100 ms). */
            scale = ft_scale_measured ? (uint64_t)((int64_t)scale + ((int64_t)m - (int64_t)scale) / 4) : m;
            ft_scale_measured = 1;
            ft_base_tsc = tsc;
            ft_base_ns = ns;
        }
    }
    ft_publish(tsc, ns, scale);

    if (ns - ft_report_ns >= FT_REPORT_NS) {
        ft_report_ns = ns;
        ft_report(stderr, scale);
        __atomic_store_n(&ft_n_anchor, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&ft_n_skip, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&ft_n_clamp, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&ft_max_err, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&ft_max_err_gap, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&ft_n_disc, 0, __ATOMIC_RELAXED);
    }
}

/* Re-anchor due: the anchor is FT_ANCHOR_NS old, or the TSC is far behind it. */
static inline int ft_due(uint64_t t, uint64_t a_tsc, uint64_t scale)
{
    return t >= a_tsc ? ft_scaled(t - a_tsc, scale) >= FT_ANCHOR_NS : ft_scaled(a_tsc - t, scale) > FT_BACK_NS;
}

/* Monotonic nanoseconds (CLOCK_MONOTONIC's epoch). Only when ft_active(). */
static inline uint64_t ft_now_ns(void)
{
    uint64_t a_tsc, a_ns, scale, t, ns, last;

    for (;;) {
        ft_load(&a_tsc, &a_ns, &scale);
        t = ft_rdtsc();
        /* rdtsc is not ordered after the loads, and may come from a core a
         * few ticks behind the anchor's: never extrapolate backwards. Far
         * behind (FT_BACK_NS), the TSC was reset: re-anchor. */
        ns = a_ns + ft_scaled(t > a_tsc ? t - a_tsc : 0, scale);
        if (ns - a_ns < FT_ANCHOR_NS && (t >= a_tsc || ft_scaled(a_tsc - t, scale) <= FT_BACK_NS))
            break;
        {
            uint32_t idle = 0;
            if (!__atomic_compare_exchange_n(&ft_busy, &idle, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                /* Someone else re-anchors: extrapolate, but not past 2 anchor
                 * periods -- beyond that the TSC jumped and the line is void. */
                if (ns - a_ns > 2 * FT_ANCHOR_NS)
                    ns = a_ns + 2 * FT_ANCHOR_NS;
                break;
            }
            ft_load(&a_tsc, &a_ns, &scale);   /* it may have just finished */
            if (ft_due(ft_rdtsc(), a_tsc, scale))
                ft_reanchor(a_tsc, a_ns, scale);
            __atomic_store_n(&ft_busy, 0, __ATOMIC_RELEASE);
        }
    }

    last = __atomic_load_n(&ft_last, __ATOMIC_RELAXED);
    while (ns > last)
        if (__atomic_compare_exchange_n(&ft_last, &last, ns, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return ns;
    if (ns < last)
        __atomic_add_fetch(&ft_n_clamp, 1, __ATOMIC_RELAXED);
    return last;
}

/* Wall clock in ns since 1970 on the same clock (offset read once). */
static inline uint64_t ft_wall_ns(void)
{
    return ft_now_ns() + (uint64_t)ft_wall_off;
}

/* Reads RECOMP_FAST_TIME once, calibrates, prints "[time] fast on|off".
 * Until it has run, ft_active() is false and callers keep clock_gettime:
 * call it after the environment is final (kernel_bridge.c gil_enabled). */
static inline void ft_init(void)
{
    const char *e = getenv("RECOMP_FAST_TIME");
    int on = e && *e == '1' && FT_HAVE_TSC, unset = -1;

    /* Once: ft_on -1 -> 2 (calibrating, still inactive). A racing or later
     * caller returns and keeps clock_gettime until ft_on is published. */
    if (!__atomic_compare_exchange_n(&ft_on, &unset, 2, 0, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE))
        return;
    if (on) {
        /* Boot estimate of the rate over >= 2 ms, as perf_init does. */
        uint64_t t0, n0, n1, t1, r, m;
        ft_pair(&t0, &n0, 0);
        while (ft_mono_ns(CLOCK_MONOTONIC) - n0 < 2000000ull) { }
        ft_pair(&t1, &n1, 0);
        r = ft_mono_ns(CLOCK_REALTIME);
        m = ft_mono_ns(CLOCK_MONOTONIC);
        ft_wall_off = (int64_t)(r - m);
        ft_base_tsc = t1;
        ft_base_ns = n1;
        ft_report_ns = n1;
        __atomic_store_n(&ft_last, n1, __ATOMIC_RELAXED);
        ft_publish(t1, n1, (uint64_t)((((unsigned __int128)(n1 - n0)) << 32) / (t1 - t0 ? t1 - t0 : 1)));
    }
    fprintf(stderr, "[time] fast %s\n", on ? "on" : "off");
    __atomic_store_n(&ft_on, on, __ATOMIC_RELEASE);
}

#endif /* FAST_TIME_H */

/*
 * NFSU1-specific runtime helpers that watch or nudge the title's own state
 * from host threads (no guest code runs here):
 *
 *   - a watchdog for the title's multimedia timers (RECOMP_TIMER_WATCH)
 *   - R3 skips to the next EA Trax song in a race (NFSU1_R3_NEXT_SONG=1)
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern unsigned long long xbox_KeQueryInterruptTime(void);
extern int  xbox_kernel_timer_state(uint32_t timer_va, long long *due_in_ms, int *signaled);
extern void xbox_kernel_timer_fire(uint32_t timer_va);
extern void (*usb_gamepad_report_hook)(int dev, uint8_t *digital);

#define GMEM32(va) (*(volatile uint32_t *)(xbox_GetMemoryOffset() + (uintptr_t)(va)))
#define GMEM16(va) (*(volatile uint16_t *)(xbox_GetMemoryOffset() + (uintptr_t)(va)))
#define GMEM8(va)  (*(volatile uint8_t  *)(xbox_GetMemoryOffset() + (uintptr_t)(va)))

#define NFSU1_GAMEFLOW_STATE 0x00283434u    /* 3 front end, 4 loading, 6 racing */

/* ── Multimedia timer watchdog ────────────────────────────────────────
 *
 * The title's timeSetEvent (its thread sub_001BBF39) keeps 63 timers of
 * 0x44 bytes in the table at [0x2DFDE4]: a KTIMER at +0, the callback at
 * +0x28, active at +0x30, the id at +0x32, the due time at +0x34 (interrupt
 * time, 100 ns). Its service thread sleeps on the KTIMERs. One whose signal
 * was lost stops the clock the autosave delay and the music run from: the
 * save never finishes and the music stops (cherepavel/nfsu1-android's
 * timer-race notes). The race itself is fixed in kernel_bridge.c; this
 * catches whatever else loses a firing. Once a second: an active entry over
 * 2 s overdue whose KTIMER is neither armed nor signalled, seen so twice in
 * a row with the same id, gets its KTIMER fired and the table's wake flag
 * (+0x10C0) set. RECOMP_TIMER_WATCH=0 turns it off. */
#define MMT_TABLE_PTR 0x002DFDE4u
#define MMT_COUNT     63
#define MMT_STRIDE    0x44u

static void *timer_watch(void *arg)
{
    static uint32_t seen[MMT_COUNT];        /* id + 1 of an entry seen stuck */
    unsigned fired = 0;
    (void)arg;
    for (;;) {
        uint32_t table, k;
        unsigned long long now;

        sleep(1);
        table = GMEM32(MMT_TABLE_PTR);
        if (!table)
            continue;
        now = xbox_KeQueryInterruptTime();
        for (k = 0; k < MMT_COUNT; k++) {
            uint32_t e = table + k * MMT_STRIDE;
            unsigned long long due, late;
            long long due_in = 0;
            int signaled;
            uint32_t id;

            if (!GMEM8(e + 0x30)) {
                seen[k] = 0;
                continue;
            }
            id = GMEM16(e + 0x32);
            due = GMEM32(e + 0x34) | (unsigned long long)GMEM32(e + 0x38) << 32;
            if (due > now || (late = now - due) <= 20000000ull) {
                seen[k] = 0;
                continue;
            }
            if (xbox_kernel_timer_state(e, &due_in, &signaled) != 0 || signaled) {
                seen[k] = 0;                /* armed, or about to be serviced */
                continue;
            }
            if (seen[k] != id + 1) {
                seen[k] = id + 1;           /* once more before acting on it */
                continue;
            }
            seen[k] = 0;
            xbox_kernel_timer_fire(e);
            GMEM32(table + 0x10C0) = 1;
            if (fired++ < 32)
                fprintf(stderr, "[timer] timeSetEvent timer %u (id %04X, callback %08X) "
                        "lost its firing %llu ms ago; fired it\n",
                        k, id, GMEM32(e + 0x28), late / 10000);
        }
    }
    return NULL;
}

static void timer_watch_start(void)
{
    pthread_t t;
    const char *e = getenv("RECOMP_TIMER_WATCH");
    if (e && *e == '0')
        return;
    if (pthread_create(&t, NULL, timer_watch, NULL) == 0)
        pthread_detach(t);
}

/* ── R3: next song ────────────────────────────────────────────────────
 *
 * NFSU1 has no button for the next EA Trax song. In a race, a fresh R3 press
 * (pad 1) finds the music player under the audio manager at [0x2BEF9C]: its
 * list at +0x104 holds five channels (+0x14..+0x24); the music one (vtable
 * 0x203500 or 0x203770) has its current stream at +0x2C + 4 * [+0x20]. A
 * playing stream (+0x14 == 1) with no request pending (+0xC == 0) is asked
 * to end (+0xC = 2), and the player moves on to the next song. In a race R3
 * itself is kept from the game. Opt-in. */
static void r3_next_song(int dev, uint8_t *digital)
{
    static uint8_t held;
    uint8_t r3 = *digital & 0x80;
    uint32_t mgr, list, p;
    const char *what = "no music controller ready";

    if (dev)
        return;
    if (GMEM32(NFSU1_GAMEFLOW_STATE) != 6) {
        held = r3;
        return;
    }
    *digital &= 0x7F;
    if (!r3 || held) {
        held = r3;
        return;
    }
    held = r3;
    mgr = GMEM32(0x002BEF9Cu);
    list = mgr ? GMEM32(mgr + 0x104) : 0;
    for (p = list + 0x14; list && p < list + 0x28; p += 4) {
        uint32_t ch = GMEM32(p), vt, idx, st;
        if (!ch)
            continue;
        vt = GMEM32(ch);
        if (vt != 0x00203500u && vt != 0x00203770u)
            continue;
        idx = GMEM32(ch + 0x20);
        if (idx > 3 || !(st = GMEM32(ch + 0x2C + idx * 4)))
            continue;
        if (GMEM32(st + 0x14) != 1 || GMEM32(st + 0xC) != 0)
            continue;
        GMEM32(st + 0xC) = 2;
        what = "next song";
        break;
    }
    fprintf(stderr, "[trax] R3: %s\n", what);
}

static void r3_install(void)
{
    const char *e = getenv("NFSU1_R3_NEXT_SONG");
    if (e && *e == '1')
        usb_gamepad_report_hook = r3_next_song;
}

void nfsu1_extras_init(void)
{
    timer_watch_start();
    r3_install();
}

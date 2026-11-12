/*
 * genet.c -- deterministic simulation for netplay (and replay checks).
 *
 * GE_NET_DETERM=1 turns on fixed-step mode:
 *   - every game frame advances the simulation by exactly GE_NET_STEP ticks
 *     (default 1 = 60 Hz), paced on an absolute wall-clock schedule, instead
 *     of "however many 1/60 s ticks of real time passed" (D117);
 *   - controller input reaches the game through the RAMROM demo player's own
 *     seam (joySetPlaybackFunc + joySetContDataIndex(1)): exactly one sample
 *     for all four pads per frame, on the game thread;
 *   - GE_NET_HASHLOG=<file> writes one "tick hash" line per frame (state at
 *     the start of the frame), so two runs can be compared line by line.
 *
 * Test input: GE_NET_BOT=<mask> drives the pads in <mask> (bit 0 = pad 1)
 * with a scripted bot (a pure function of the tick and pad number).
 * GE_NET_FRAMES=<n> quits after n frames.
 *
 * Pin the boot seed with GE_RSEED=<hex> (port/src/random.c).
 */
#include <ultra64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "system.h"
#include "genet.h"
#include "joy.h"
#include "player.h"
#include "bondview.h"

extern u64 g_randomSeed;
extern u64 g_chrObjRandomSeed;
extern s32 g_GlobalTimer;
extern s32 currentFrameCounter;
extern s32 g_StageNum;

typedef struct ChrRecord ChrRecord;
extern ChrRecord *g_ChrSlots;
extern s32 g_NumChrSlots;

static int s_Init = 0;
static int s_Determ = 0;
static s32 s_Step = 1;
static u32 s_BotMask = 0;
static long s_QuitFrames = 0;
static FILE *s_HashLog = NULL;

static u32 s_Tick = 0;          /* frames simulated in fixed-step mode */
static uint64_t s_NextUs = 0;   /* absolute schedule */
static int s_PlaybackOn = 0;

#define GENET_TICK_US 16667ULL

static void genetInit(void)
{
    const char *e;

    if (s_Init) {
        return;
    }
    s_Init = 1;

    e = getenv("GE_NET_DETERM");
    s_Determ = (e && *e && atoi(e) != 0);

    e = getenv("GE_NET_STEP");
    if (e && *e) {
        s_Step = atoi(e);
        if (s_Step < 1) s_Step = 1;
        if (s_Step > 6) s_Step = 6;
    }

    e = getenv("GE_NET_BOT");
    if (e && *e) s_BotMask = (u32)strtol(e, NULL, 0) & 0xf;

    e = getenv("GE_NET_FRAMES");
    if (e && *e) s_QuitFrames = atoi(e);

    e = getenv("GE_NET_HASHLOG");
    if (e && *e) {
        s_HashLog = fopen(e, "w");
        if (!s_HashLog) {
            fprintf(stderr, "genet: cannot open hash log %s\n", e);
        }
    }

    if (s_Determ) {
        fprintf(stderr, "genet: fixed-step mode, %d tick(s) per frame, bot mask 0x%x\n", s_Step, s_BotMask);
    }
}

int genetFixedStep(void)
{
    genetInit();
    return s_Determ;
}

/* ---- state hash ------------------------------------------------------ */

static u64 s_H;

static void hashBytes(const void *p, size_t n)
{
    const u8 *b = (const u8 *)p;
    for (size_t i = 0; i < n; i++) {
        s_H ^= b[i];
        s_H *= 0x100000001b3ULL;
    }
}

#define HASHV(v) hashBytes(&(v), sizeof(v))

static u64 genetStateHash(void)
{
    s_H = 0xcbf29ce484222325ULL;

    HASHV(g_randomSeed);
    HASHV(g_chrObjRandomSeed);
    HASHV(g_GlobalTimer);
    HASHV(currentFrameCounter);
    HASHV(g_StageNum);

    for (int i = 0; i < 4; i++) {
        struct player *pl = g_playerPointers[i];
        if (!pl) continue;
        HASHV(pl->pos);
        HASHV(pl->bondhealth);
        HASHV(pl->bondarmour);
        HASHV(pl->vv_theta);
        HASHV(pl->vv_verta);
        if (pl->prop) HASHV(pl->prop->pos);
    }

    if (g_ChrSlots) {
        for (s32 i = 0; i < g_NumChrSlots; i++) {
            struct ChrRecord *c = &g_ChrSlots[i];
            if (c->chrnum < 0 || !c->prop) continue;
            HASHV(c->chrnum);
            u8 at = (u8)c->actiontype;
            HASHV(at);
            HASHV(c->damage);
            HASHV(c->prop->pos);
        }
    }

    return s_H;
}

/* ---- pacing ---------------------------------------------------------- */

static void sleepUntilUs(uint64_t t)
{
    for (;;) {
        uint64_t now = sysGetMicroseconds();
        if (now >= t) return;
        uint64_t d = t - now;
        if (d > 2000) d -= 1000; /* wake a little early, then spin briefly */
        else if (d > 200) d = 100;
        else continue;
        struct timespec ts = { (time_t)(d / 1000000), (long)(d % 1000000) * 1000 };
        nanosleep(&ts, NULL);
    }
}

static void genetPace(void)
{
    uint64_t now = sysGetMicroseconds();
    uint64_t period = GENET_TICK_US * (uint64_t)s_Step;

    if (s_NextUs == 0 || now > s_NextUs + 4 * period) {
        /* first frame, or far behind (stage load): restart the schedule */
        s_NextUs = now;
    } else {
        sleepUntilUs(s_NextUs);
    }
    s_NextUs += period;
}

/* ---- input ----------------------------------------------------------- */

static void botPad(u32 tick, int pad, OSContPad *out)
{
    /* walk forward, turn in slow arcs, strafe now and then, fire in bursts */
    u32 phase = (tick / 45 + (u32)pad * 5) % 10;
    s8 sx = 0, sy = 0;
    u16 b = 0;

    switch (phase) {
    case 0: case 1: sy = 70; sx = 0; break;
    case 2: sy = 40; sx = 70; break;            /* sharp turn while walking */
    case 3: sy = -50; sx = 0; break;            /* back off a wall */
    case 4: sy = 0; sx = -80; break;            /* spin */
    case 5: sy = 70; sx = -25; break;
    case 6: sy = 60; sx = 0; b |= L_CBUTTONS; break; /* strafe */
    case 7: sy = 0; sx = 80; break;             /* spin */
    case 8: sy = 70; sx = 30; break;
    default: sy = 30; sx = 0; b |= R_CBUTTONS; break;
    }
    if (((tick + (u32)pad * 13) % 90) < 30 && (tick % 6) < 3) b |= Z_TRIG;
    if (((tick + (u32)pad * 31) % 600) == 300) b |= R_TRIG;

    out->stick_x = sx;
    out->stick_y = sy;
    out->button = b;
    out->errno = 0;
}

static s32 genetPlayback(struct contsample *samples, s32 curlast)
{
    const u64 h = genetStateHash();

    if (s_HashLog) {
        fprintf(s_HashLog, "%u %016llx\n", s_Tick, (unsigned long long)h);
        if ((s_Tick & 63) == 0) fflush(s_HashLog);
    }

    s32 idx = (curlast + 1) % CONTSAMPLE_LEN;
    for (int p = 0; p < MAXCONTROLLERS; p++) {
        OSContPad *pad = &samples[idx].pads[p];
        memset(pad, 0, sizeof(*pad));
        if (s_BotMask & (1u << p)) {
            botPad(s_Tick, p, pad);
        }
    }

    s_Tick++;
    if (s_QuitFrames > 0 && (long)s_Tick >= s_QuitFrames) {
        if (s_HashLog) fflush(s_HashLog);
        fprintf(stderr, "genet: %u frames done, quitting\n", s_Tick);
        fflush(stderr);
        _Exit(0);
    }
    return idx;
}

/* Called from waitForNextFrame (game thread) in fixed-step mode: waits for
 * the next slot of the schedule and returns the tick count for this frame. */
s32 genetFrameTicks(void)
{
    genetInit();

    if (!s_PlaybackOn) {
        s_PlaybackOn = 1;
        joySetPlaybackFunc(genetPlayback, MAXCONTROLLERS);
        joySetContDataIndex(1);
    }

    genetPace();
    return s_Step;
}

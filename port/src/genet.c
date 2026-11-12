/*
 * genet.c -- deterministic simulation and online play (netplay).
 *
 * Fixed-step mode (GE_NET_DETERM=1, and always during an online session):
 *   - every game frame advances the simulation by exactly GE_NET_STEP ticks
 *     (default 1 = 60 Hz), paced on an absolute wall-clock schedule, instead
 *     of "however many 1/60 s ticks of real time passed" (D117);
 *   - controller input reaches the game through the RAMROM demo player's own
 *     seam (joySetPlaybackFunc + joySetContDataIndex(1)): exactly one sample
 *     for all four pads per frame, on the game thread;
 *   - GE_NET_HASHLOG=<file> writes one "tick hash" line per frame (state at
 *     the start of the frame), so two runs can be compared line by line.
 *
 * Online (lockstep through n64net_input; the whole game is shared from boot,
 * as if both controllers were plugged into one console):
 *   host:  GE_NET_HOST=1 [GE_NET_PORT=7501]
 *   guest: GE_NET_PEER=a.b.c.d[:7501] [GE_NET_PORT=7502]
 *   or both GE_NET_SERVER=host:port GE_NET_ROOM=ABCD [GE_NET_PASSWORD, GE_NET_RELAY=1]
 *   GE_NET_DELAY (input delay, ticks; default 2, raised automatically up to 6),
 *   GE_NET_WAIT_SECS (60), GE_NET_DROP_SECS (20: a silent peer = offline),
 *   GE_NET_SIM_LATENCY/_JITTER (ms) /_LOSS (%) (a simulated bad link),
 *   GE_NET_TEST_DESYNC=<tick> (perturbs the RNG there: tests desync detection).
 * The host is player 1, the guest player 2. The guest plays on the host's save
 * (kept in memory) and the host's random seed.
 *
 * Test input: GE_NET_BOT=<mask> drives the pads in <mask> (bit 0 = pad 1)
 * with a scripted bot (a pure function of the tick and pad number); online,
 * any nonzero mask makes the local player a bot. GE_NET_FRAMES=<n> quits
 * after n frames. Offline, pin the boot seed with GE_RSEED=<hex>.
 */
#include <ultra64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#include "system.h"
#include "genet.h"
#include "joy.h"
#include "player.h"
#include "bondview.h"
#include "n64net_input.h"
#include "genet_transport.h"
#include "genet_build_id.h"

extern u64 g_randomSeed;
extern u64 g_chrObjRandomSeed;
extern s32 g_GlobalTimer;
extern s32 currentFrameCounter;
extern s32 g_StageNum;

typedef struct ChrRecord ChrRecord;
extern ChrRecord *g_ChrSlots;
extern s32 g_NumChrSlots;

void joyPortLatestPad(s32 channel, OSContPad *out);  /* src/joy.c */
void portEepromCopy(u8 *out);                        /* port/src/libultra.c */
void portEepromAdopt(const u8 *in);
void inputNetPure(int on);                           /* port/src/input.c */

#define GENET_TICK_US   16667ULL
#define GENET_EEP_SIZE  2048
#define GENET_EEP_CHUNK 512
#define GENET_EEP_CHUNKS (GENET_EEP_SIZE / GENET_EEP_CHUNK)

#define HS_MAGIC   0x47454e48u /* 'GENH' handshake */
#define HS_HELLO    1
#define HS_WELCOME  2
#define HS_MISMATCH 3
#define HS_BYE      4
#define SAVE_MAGIC 0x47454e53u /* 'GENS' the host's save */
#define SAVE_REQ    1
#define SAVE_DATA   2

#define SYNC_EVERY  30
#define STATS_EVERY 1800

static int s_Init = 0;
static int s_Determ = 0;
static s32 s_Step = 1;
static u32 s_BotMask = 0;
static long s_QuitFrames = 0;
static FILE *s_HashLog = NULL;

static u32 s_Tick = 0;          /* frames simulated in fixed-step mode */
static uint64_t s_NextUs = 0;   /* absolute schedule */
static int s_PlaybackOn = 0;

/* online */
static int s_Online = 0;        /* a session was set up (connect succeeded) */
static int s_IsHost = 0;
static int s_Offline = 0;       /* the other player left or went silent: play on alone */
static int s_PeerLeft = 0;
static int s_Mismatch = 0;
static int s_NumPlayers = 2;
static int s_Local = 0;
static n64net_input *s_Net = NULL;
static u64 s_Seed = 0;
static int s_HaveSeed = 0;
static int s_WaitSecs = 60;
static int s_DropSecs = 20;
static long s_TestDesync = -1;
static int s_DesyncReported = 0;
static u32 s_Stalls = 0;
static unsigned long long s_StallUs = 0;
static u8 s_HostEep[GENET_EEP_SIZE];
static u32 s_EepChunks = 0;     /* guest: bit i = chunk i received */
static char s_Room[64];

/* ---- helpers ---------------------------------------------------------- */

static const char *envStr(const char *name)
{
    const char *v = getenv(name);
    return (v && v[0]) ? v : NULL;
}

static void netLog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "genet: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (fmt[0] && fmt[strlen(fmt) - 1] != '\n') fputc('\n', stderr);
    fflush(stderr);
}

static void put32(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static u32 get32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static void put64(u8 *p, u64 v) { put32(p, (u32)(v >> 32)); put32(p + 4, (u32)v); }
static u64 get64(const u8 *p) { return ((u64)get32(p) << 32) | get32(p + 4); }

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

int genetSessionSeed(u64 *out)
{
    if (!s_Online || !s_HaveSeed) return 0;
    *out = s_Seed;
    return 1;
}

int genetOnline(void)
{
    return s_Online && !s_Offline;
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
        gnt_sleep_us((unsigned)d);
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

/* ---- network ---------------------------------------------------------- */

static void sendHandshake(u8 type)
{
    u8 buf[32];
    memset(buf, 0, sizeof(buf));
    put32(buf, HS_MAGIC);
    buf[4] = type;
    memcpy(buf + 5, GENET_BUILD_ID, 12);
    put64(buf + 17, s_Seed);
    gnt_send(buf, 25);
}

static void sendSave(u8 kind, int chunk)
{
    u8 buf[8 + GENET_EEP_CHUNK];
    memset(buf, 0, 8);
    put32(buf, SAVE_MAGIC);
    buf[4] = kind;
    buf[5] = (u8)chunk;
    if (kind == SAVE_DATA) {
        memcpy(buf + 8, s_HostEep + chunk * GENET_EEP_CHUNK, GENET_EEP_CHUNK);
        gnt_send(buf, sizeof(buf));
    } else {
        gnt_send(buf, 8);
    }
}

/* Reads every waiting datagram. Returns 1 once the other machine is known to be in the session. */
static int netPoll(void)
{
    int started = 0;
    u8 buf[1500];

    for (;;) {
        int n = gnt_recv(buf, sizeof(buf));
        if (n <= 0) break;

        if (n >= 8 && get32(buf) == SAVE_MAGIC) {
            if (buf[4] == SAVE_REQ && s_IsHost) {
                for (int c = 0; c < GENET_EEP_CHUNKS; c++) sendSave(SAVE_DATA, c);
            } else if (buf[4] == SAVE_DATA && !s_IsHost && n >= 8 + GENET_EEP_CHUNK && buf[5] < GENET_EEP_CHUNKS) {
                memcpy(s_HostEep + buf[5] * GENET_EEP_CHUNK, buf + 8, GENET_EEP_CHUNK);
                s_EepChunks |= 1u << buf[5];
            }
            continue;
        }
        if (n >= 25 && get32(buf) == HS_MAGIC) {
            const int same = memcmp(buf + 5, GENET_BUILD_ID, 12) == 0;
            if (buf[4] == HS_BYE) {
                if (!s_PeerLeft && s_Online) netLog("the other player quit");
                s_PeerLeft = 1;
            } else if (s_IsHost && buf[4] == HS_HELLO) {
                gnt_accept_last();
                if (!same) {
                    sendHandshake(HS_MISMATCH);
                    netLog("the guest has a different build (%.12s, this is %s)", (const char *)(buf + 5), GENET_BUILD_ID);
                    continue;
                }
                sendHandshake(HS_WELCOME);
                started = 1;
            } else if (!s_IsHost && buf[4] == HS_WELCOME) {
                s_Seed = get64(buf + 17);
                s_HaveSeed = 1;
                started = 1;
            } else if (!s_IsHost && buf[4] == HS_MISMATCH) {
                s_Mismatch = 1;
            }
            continue;
        }
        if (s_Net && n64net_input_handle_packet(s_Net, buf, (uint32_t)n)) {
            started = 1;
        }
    }
    return started;
}

static void netSend(void *user, const uint8_t *data, uint32_t size)
{
    (void)user;
    gnt_send(data, size);
}

static void netLogCb(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    netLog("%s", buf);
}

static int peerGone(void)
{
    if (s_PeerLeft) return 1;
    if (n64net_input_peer_away(s_Net)) return 0; /* its app is in the background: wait as long as it takes */
    return n64net_input_peer_silence_usecs(s_Net) > (unsigned long long)s_DropSecs * 1000000ull;
}

static void goOffline(void)
{
    if (s_Offline) return;
    s_Offline = 1;
    netLog("%s Playing on offline.", s_PeerLeft ? "The other player left." : "Lost the connection to the other player.");
}

static int connectFail(const char *what)
{
    netLog("Netplay: %s. Playing offline.", what);
    gnt_close();
    s_Online = 0;
    return 0;
}

/* Called from main() before the game starts. Returns 1 when an online session is set up. */
int genetConnect(void)
{
    const char *server = envStr("GE_NET_SERVER");
    const char *peer = envStr("GE_NET_PEER");
    const char *s;
    int port;
    unsigned long long deadline, nextHello = 0;

    genetInit();
    s_IsHost = envStr("GE_NET_HOST") && atoi(envStr("GE_NET_HOST")) != 0;
    if (!s_IsHost && !peer && !(server && envStr("GE_NET_ROOM"))) {
        return 0;
    }
    if ((s = envStr("GE_NET_WAIT_SECS"))) s_WaitSecs = atoi(s);
    if ((s = envStr("GE_NET_DROP_SECS"))) s_DropSecs = atoi(s) > 0 ? atoi(s) : 20;
    if ((s = envStr("GE_NET_TEST_DESYNC"))) s_TestDesync = atoi(s);
    s_Local = s_IsHost ? 0 : 1;

    port = server ? 0 : (s_IsHost ? 7501 : 7502);
    if ((s = envStr("GE_NET_PORT"))) port = atoi(s);
    if (gnt_open(port) != 0) return connectFail(gnt_error());
    {
        const char *lat = envStr("GE_NET_SIM_LATENCY");
        const char *jit = envStr("GE_NET_SIM_JITTER");
        const char *loss = envStr("GE_NET_SIM_LOSS");
        gnt_sim(lat ? (unsigned)atoi(lat) : 0, jit ? (unsigned)atoi(jit) : 0, loss ? (unsigned)atoi(loss) : 0);
    }

    if (server) {
        char game[64];
        snprintf(game, sizeof(game), "ge007-us+%s", GENET_BUILD_ID);
        if (gnt_rendezvous(server, game, envStr("GE_NET_ROOM"), envStr("GE_NET_PASSWORD"), s_IsHost,
                           envStr("GE_NET_RELAY") && atoi(envStr("GE_NET_RELAY")), s_WaitSecs, netLogCb,
                           s_Room, sizeof(s_Room)) != 0) {
            return connectFail(gnt_error());
        }
        netLog("room %s", s_Room);
    } else if (!s_IsHost && gnt_set_peer(peer, 7501) != 0) {
        return connectFail(gnt_error());
    }

    if (s_IsHost) {
        /* the session's seed: different every session, the same on both machines */
        s_Seed = ((u64)gnt_now_us() * 0x9E3779B97F4A7C15ULL) ^ ((u64)sysGetMicroseconds() << 17);
        s_HaveSeed = 1;
        portEepromCopy(s_HostEep);
    }

    netLog("%s on UDP %d, build %s, waiting up to %d s", s_IsHost ? "hosting" : "joining", port, GENET_BUILD_ID, s_WaitSecs);
    deadline = gnt_now_us() + (unsigned long long)s_WaitSecs * 1000000ull;
    for (;;) {
        unsigned long long now;
        if (netPoll()) break;
        if (s_Mismatch) return connectFail("the other player has a different build of the game");
        now = gnt_now_us();
        if (now > deadline) return connectFail("the other player didn't connect in time");
        if (!s_IsHost && now >= nextHello) {
            sendHandshake(HS_HELLO);
            nextHello = now + 100000;
        }
        gnt_sleep_us(1000);
    }

    if (!s_IsHost) {
        /* the host's save, in chunks */
        unsigned long long next = 0;
        deadline = gnt_now_us() + 10000000ull;
        while (s_EepChunks != (1u << GENET_EEP_CHUNKS) - 1) {
            unsigned long long now = gnt_now_us();
            netPoll();
            if (now > deadline || s_PeerLeft) return connectFail("didn't receive the host's save data");
            if (now >= next) {
                sendSave(SAVE_REQ, 0);
                next = now + 100000;
            }
            gnt_sleep_us(1000);
        }
        portEepromAdopt(s_HostEep);
    } else {
        /* answer the guest's save requests until its first input arrives (netPoll in the game loop) */
    }

    {
        n64net_input_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.num_players = s_NumPlayers;
        cfg.local_player = s_Local;
        cfg.session_id = (u32)(s_Seed ^ (s_Seed >> 32));
        cfg.input_delay = (s = envStr("GE_NET_DELAY")) ? (u32)atoi(s) : 2;
        cfg.rollback_window = 0; /* lockstep */
        cfg.ticks_per_second = 60 / s_Step;
        cfg.send = netSend;
        cfg.log = netLogCb;
        s_Net = n64net_input_create(&cfg);
        n64net_input_set_auto_delay(s_Net, cfg.input_delay, 6);
    }

    s_Online = 1;
    s_Determ = 1; /* online play is always fixed-step */
    inputNetPure(1);
    netLog("connected as player %d (lockstep, input delay %u, seed %016llx)", s_Local + 1,
           n64net_input_get_delay(s_Net), (unsigned long long)s_Seed);
    return 1;
}

void genetShutdown(void)
{
    if (s_Online && !s_Offline) {
        for (int i = 0; i < 3; i++) sendHandshake(HS_BYE);
    }
    if (s_Online) inputNetPure(0); /* the player's own control settings go back into the ini */
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

static n64net_pad toNet(const OSContPad *p)
{
    n64net_pad n;
    memset(&n, 0, sizeof(n));
    n.buttons = p->button;
    n.stick_x = p->stick_x;
    n.stick_y = p->stick_y;
    return n;
}

/* This frame's pads for every player: lockstep (waits for the other player's input). */
static void exchangeTick(u32 tick, const OSContPad *local, OSContPad *out)
{
    if (s_Offline) {
        out[s_Local] = *local;
        return;
    }
    netPoll();
    n64net_input_set_local(s_Net, tick, toNet(local));
    if (!n64net_input_ready(s_Net, tick)) {
        unsigned long long start = gnt_now_us(), lastCheck = start;
        s_Stalls++;
        while (!n64net_input_ready(s_Net, tick)) {
            unsigned long long now;
            netPoll();
            n64net_input_resend(s_Net);
            now = gnt_now_us();
            if (s_PeerLeft || now - lastCheck > 1000000) {
                lastCheck = now;
                if (peerGone()) {
                    s_StallUs += now - start;
                    goOffline();
                    out[s_Local] = *local;
                    return;
                }
            }
            gnt_sleep_us(250);
        }
        s_StallUs += gnt_now_us() - start;
    }
    for (int p = 0; p < s_NumPlayers; p++) {
        n64net_pad n;
        n64net_input_get(s_Net, p, tick, &n);
        out[p].button = n.buttons;
        out[p].stick_x = n.stick_x;
        out[p].stick_y = n.stick_y;
        out[p].errno = 0;
    }
}

static void endTick(u32 tick, u64 hash)
{
    u32 dtick;
    u8 dkind;

    if (!s_Online || s_Offline) return;
    if (peerGone()) {
        goOffline();
        return;
    }
    if (tick % SYNC_EVERY == 0) {
        n64net_sync_record(s_Net, tick, 0, (u32)(hash ^ (hash >> 32)));
    }
    if (!s_DesyncReported && n64net_sync_desynced(s_Net, &dtick, &dkind)) {
        s_DesyncReported = 1;
        netLog("DESYNC detected at tick %u", dtick);
    }
    if (tick % STATS_EVERY == 0 && tick > 0) {
        n64net_input_stats st;
        n64net_input_get_stats(s_Net, &st);
        netLog("tick %u, sync checks matched %u%s, stalls %u (%.2f s), delay %u, rtt avg %u ms max %u ms",
               tick, n64net_sync_matched(s_Net), s_DesyncReported ? " (DESYNC)" : "", s_Stalls,
               (double)s_StallUs / 1e6, n64net_input_get_delay(s_Net), st.rtt_avg_usecs / 1000,
               st.rtt_max_usecs / 1000);
    }
}

static s32 genetPlayback(struct contsample *samples, s32 curlast)
{
    const u32 tick = s_Tick;
    u64 h;
    OSContPad pads[MAXCONTROLLERS];

    if (s_Online && (long)tick == s_TestDesync) {
        g_randomSeed ^= 0x5a5a5a5aULL;
        netLog("test desync injected at tick %u", tick);
    }

    h = genetStateHash();
    if (s_HashLog) {
        fprintf(s_HashLog, "%u %016llx\n", tick, (unsigned long long)h);
        if ((tick & 63) == 0) fflush(s_HashLog);
    }

    memset(pads, 0, sizeof(pads));
    if (s_Online) {
        OSContPad local;
        memset(&local, 0, sizeof(local));
        if (s_BotMask) {
            botPad(tick, s_Local, &local);
        } else {
            joyPortLatestPad(0, &local); /* this machine's own controller is always its controller 1 */
        }
        exchangeTick(tick, &local, pads);
        endTick(tick, h);
    } else {
        for (int p = 0; p < MAXCONTROLLERS; p++) {
            if (s_BotMask & (1u << p)) botPad(tick, p, &pads[p]);
        }
    }

    s32 idx = (curlast + 1) % CONTSAMPLE_LEN;
    for (int p = 0; p < MAXCONTROLLERS; p++) {
        samples[idx].pads[p] = pads[p];
    }

    s_Tick++;
    if (s_QuitFrames > 0 && (long)s_Tick >= s_QuitFrames) {
        if (s_HashLog) fflush(s_HashLog);
        if (s_Online) {
            netLog("%u frames done (sync checks matched %u%s, stalls %u, %.2f s), quitting", s_Tick,
                   n64net_sync_matched(s_Net), s_DesyncReported ? ", DESYNC" : "", s_Stalls, (double)s_StallUs / 1e6);
            genetShutdown();
        } else {
            netLog("%u frames done, quitting", s_Tick);
        }
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
        /* online: the game sees exactly the session's controllers */
        joySetPlaybackFunc(genetPlayback, s_Online ? s_NumPlayers : MAXCONTROLLERS);
        joySetContDataIndex(1);
    }

    if (s_Online && !s_Offline) {
        netPoll(); /* host: answer save requests; both: keep the ledger moving */
    }
    genetPace();
    return s_Step;
}

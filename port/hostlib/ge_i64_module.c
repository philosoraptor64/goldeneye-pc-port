/*
 * GoldenEye 007 as an i64 game module (port/i64/n64_game.h). Built only with
 * -DGE_I64_MODULE=ON, which makes the game a shared library exporting just the
 * n64_game_* functions (port/i64/exports.txt).
 *
 * i64 owns the app, SDL, the window and the audio output. The game keeps its
 * own threads (the N64 threads, the tick thread, the render worker): they run
 * and render by themselves, as in the stand-alone app, so n64_game_frame()
 * only pumps SDL events on i64's main thread (N64_GAME_INFO_OWN_THREAD). A run
 * ends with the R1 shutdown (every port thread stopped and joined, the
 * address window emptied) and the globals restored (reentry.c), so the module
 * can be started again in the same process.
 *
 * ROM import: the player's ROM (any byte order) is stored big-endian in the
 * game's data folder as ge007.<region>.z64 and converted there by the native
 * converter (port/convert), stamped with GE_CONVERT_STAMP.
 *
 * Online: genet's i64 path (genetSessionBlob / genetInitExternal /
 * genetExternalEvent): the host's save travels in the session blob, inputs on
 * i64's game channel; the game paces itself (its own threads).
 * Pause: offline the game thread waits at its next frame; online genet's away
 * handling holds and tells the other player.
 * Not yet: title art.
 */
#if defined(GE_I64_MODULE)

#define N64_GAME_BUILDING_MODULE 1
#include <stdbool.h>
#include <stdint.h>
#include "../i64/n64_game.h"

#include "ge_convert.h"
#include "reentry.h"
#include "genet_build_id.h"

#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* main.c / audio.c / system.c / gfx_sdl2.cpp (game side) */
extern int geModuleStart(void);
extern int (*g_portHostTakeReserved)(unsigned long long addr, unsigned long long size);   /* port_addr.c */
extern char g_portAddrError[512];
extern int geModuleFrame(void);
extern int geModuleStop(void);
extern void audioSetHostOutput(void (*write)(const short *, unsigned, unsigned), unsigned (*buffered)(unsigned));
extern char g_sysDataDir[1024];
extern void *g_gfxExternalWindow;
extern volatile int g_portHostPaused;   /* libultra.c */

/* genet.h (game side), same layout */
struct genetexternal {
    int32_t localplayer;
    uint64_t seed;
    const void *blob;
    uint32_t blobsize;
    int (*send)(const void *data, uint32_t size);
    int (*recv)(void *buf, uint32_t cap);
    int32_t hostpaced;
    int32_t numplayers;
    uint32_t localmask;
    int32_t padofplayer[4];
    int32_t machineofplayer[4];
    int32_t dropoutmenu;
    int32_t joining;
};
extern struct genetexternal *g_geNetExternal;   /* main.c */
extern uint32_t genetSessionBlob(void *out, uint32_t cap);
extern void genetExternalEvent(int32_t event);
extern void genetExternalEventFrom(int32_t event, int32_t machine);
extern int32_t genetDropout(int32_t player, int32_t action);
extern int32_t genetAdmit(int32_t machine);
extern int32_t genetCatchingUp(uint32_t *done, uint32_t *total);
extern void (*g_genetCatchupGate)(void);
extern volatile int g_genetCatchupHeld;
#ifndef N64_LAUNCH_JOINING
#define N64_LAUNCH_JOINING (1u << 2)
#endif
#ifndef N64_DROPOUT_CAN_JOIN
#define N64_DROPOUT_CAN_JOIN (1u << 3)
#endif
#define GE_FRAME_WAITING ((n64_frame_result)2)   /* N64_GAME_FRAME_WAITING (i64's catch-up driver) */
#ifndef N64_LAUNCH_DROPOUT_MENU
#define N64_LAUNCH_DROPOUT_MENU (1u << 1)   /* proposed: i64 shows the host a drop-out menu */
#endif
extern const char *genetEndReason(void);

/* Bump when the converter's output changes: older imports are redone. */
#define GE_CONVERT_STAMP "ge-convert-1"

static const n64_game_rom kRoms[] = {
    { "abe01e4aeb033b6c0836819f549c791b26cfde83", "USA" },
};

static const n64_game_info kInfo = {
    N64_GAME_ABI_VERSION,
    "ge007-us",
    "GoldenEye 007",
    kRoms,
    1,
    GENET_BUILD_ID,
    4,  /* multiplayer: up to 4 players over up to 4 devices */
    60,
    N64_GAME_INFO_OWN_THREAD | N64_GAME_INFO_CAN_UNLOCK_ALL,
    /* drop-outs: the player is removed (dies, stays down) or stays, with no input; a new device may take over */
    N64_DROPOUT_CAN_REMOVE | N64_DROPOUT_CAN_IDLE | N64_DROPOUT_CAN_JOIN,
    4,
};

/* survive the globals reset between runs (reentry.c) */
GE_KEEP static const n64_host *sHost = NULL;
GE_KEEP static int sRunning = 0;
GE_KEEP static int sBroken = 0;   /* a shutdown failed: can't start again */
GE_KEEP static void (*sAudioWrite)(const int16_t *, uint32_t, uint32_t);
GE_KEEP static uint32_t (*sAudioBuffered)(uint32_t);

N64_GAME_EXPORT const n64_game_info *n64_game_get_info(void)
{
    return &kInfo;
}

/* ---------------------------------------------------------------- import */

static int mkdirs(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    return (mkdir(tmp, 0755) != 0 && errno != EEXIST) ? -1 : 0;
}

static int stampOk(const char *dataDir)
{
    char path[1100], have[64] = "";
    snprintf(path, sizeof(path), "%s/ge007.convert", dataDir);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(have, sizeof(have), f)) have[0] = 0;
    fclose(f);
    have[strcspn(have, "\r\n")] = 0;
    return strcmp(have, GE_CONVERT_STAMP) == 0;
}

struct progressCtx { void (*fn)(float, void *); void *user; };
static void convProgress(float done, void *user)
{
    struct progressCtx *p = user;
    if (p->fn) p->fn(0.1f + 0.9f * done, p->user);
}

N64_GAME_EXPORT int n64_game_import_rom(const n64_host *host, const char *rom_path,
                                        void (*progress)(float done, void *user), void *user,
                                        char *err, size_t err_size)
{
    /* everything so far is static initialisation: the pristine state */
    reentrySavePristine();
    if (!host || !rom_path || !host->data_dir || !host->data_dir[0]) {
        snprintf(err, err_size, "missing folders");
        return 1;
    }
    if (sRunning) {
        snprintf(err, err_size, "the game is running");
        return 1;
    }
    if (progress) progress(0.0f, user);
    FILE *f = fopen(rom_path, "rb");
    if (!f) {
        snprintf(err, err_size, "can't read the ROM");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < (8L << 20) || n > (64L << 20) || (n % 4) != 0) {
        fclose(f);
        snprintf(err, err_size, "that file isn't an N64 ROM");
        return 1;
    }
    uint8_t *rom = malloc((size_t)n);
    if (!rom || fread(rom, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(rom);
        snprintf(err, err_size, "can't read the ROM");
        return 1;
    }
    fclose(f);
    /* byte order: big-endian dumps start 80 37 12 40 */
    if (!(rom[0] == 0x80 && rom[1] == 0x37 && rom[2] == 0x12 && rom[3] == 0x40)) {
        if (rom[0] == 0x37 && rom[1] == 0x80) {
            for (long i = 0; i + 1 < n; i += 2) { uint8_t t = rom[i]; rom[i] = rom[i + 1]; rom[i + 1] = t; }
        } else if (rom[0] == 0x40 && rom[1] == 0x12) {
            for (long i = 0; i + 3 < n; i += 4) {
                uint8_t a = rom[i], b = rom[i + 1];
                rom[i] = rom[i + 3]; rom[i + 1] = rom[i + 2]; rom[i + 2] = b; rom[i + 3] = a;
            }
        } else {
            free(rom);
            snprintf(err, err_size, "that file isn't an N64 ROM");
            return 1;
        }
    }
    if (mkdirs(host->data_dir) != 0) {
        free(rom);
        snprintf(err, err_size, "can't create the game's data folder");
        return 1;
    }
    char path[1100], stamp[1100];
    snprintf(stamp, sizeof(stamp), "%s/ge007.convert", host->data_dir);
    remove(stamp);
    snprintf(path, sizeof(path), "%s/ge007.%s.z64", host->data_dir, ge_convert_region());
    FILE *o = fopen(path, "wb");
    int ok = o && fwrite(rom, 1, (size_t)n, o) == (size_t)n;
    if (o) ok = (fclose(o) == 0) && ok;
    if (!ok) {
        free(rom);
        remove(path);
        snprintf(err, err_size, "not enough space to import the ROM");
        return 1;
    }
    if (progress) progress(0.1f, user);
    struct progressCtx pc = { progress, user };
    char why[512] = "";
    int rc = ge_convert_sidecars(rom, (size_t)n, host->data_dir, convProgress, &pc, why, sizeof(why));
    free(rom);
    if (rc != 0) {
        snprintf(err, err_size, "converting the game data failed: %s", why);
        return 1;
    }
    FILE *s = fopen(stamp, "w");
    if (s) {
        fprintf(s, "%s\n", GE_CONVERT_STAMP);
        fclose(s);
    }
    if (progress) progress(1.0f, user);
    return 0;
}

/* Online, on the session's host before a run: its save (unlocks, MP options).
 * Reading the save touches the game's globals, so this runs between a
 * pristine copy and a restore when no game is running. */
N64_GAME_EXPORT uint32_t n64_game_session_blob(const n64_host *host, void *out, uint32_t cap)
{
    if (!host || !host->data_dir || !host->data_dir[0])
        return 0;
    reentrySavePristine();
    if (sRunning) {
        return genetSessionBlob(out, cap);
    }
    snprintf(g_sysDataDir, sizeof(g_sysDataDir), "%s", host->data_dir);
    uint32_t n = genetSessionBlob(out, cap);
    reentryRestorePristine();
    return n;
}

/* ------------------------------------------------------------------ runs */

extern int (*g_portHostPadRead)(unsigned pad, unsigned short *buttons, signed char *sx, signed char *sy);   /* libultra.c */
extern void (*g_portHostPadRumble)(unsigned pad, float strength);
static int hostPadRead(unsigned pad, unsigned short *buttons, signed char *sx, signed char *sy)
{
    n64_pad_state st;
    if (!sHost || !sHost->pad_read || !sHost->pad_read(pad, &st)) return 0;
    *buttons = st.buttons;
    *sx = st.stick_x;
    *sy = st.stick_y;
    return 1;
}

static void hostAudioWrite(const short *stereo, unsigned frames, unsigned rate)
{
    if (sAudioWrite) sAudioWrite((const int16_t *)stereo, frames, rate);
}
static unsigned hostAudioBuffered(unsigned rate)
{
    return sAudioBuffered ? sAudioBuffered(rate) : 0;
}

/* i64's home screen: a picture of the game when i64 asks (fast3d/gfx_metal.mm captures it) */
extern volatile int gPortFrameCaptureRequest;
extern void (*gPortFrameCaptureCallback)(const uint8_t *rgba, uint32_t w, uint32_t h);
static unsigned sPreviewFrames;
/* i64 decides when it wants a home-screen picture (todo item 20: one rule in i64 for every game) */
static volatile int sPreviewWanted = 0;
N64_GAME_EXPORT void n64_game_capture_preview(void) { sPreviewWanted = 1; }
static int previewDue(unsigned f) {
    (void) f;
    if (!sPreviewWanted) return 0;
    sPreviewWanted = 0;
    return 1;
}

/* ---- joining mid-game ----------------------------------------------------
 * The host's module: n64_game_admit. The joiner (N64_LAUNCH_JOINING) plays the host's input log from tick 0 on the
 * game's own thread, unpaced, with nothing drawn or heard (genet.c catchingNow). i64's catch-up driver meters it:
 * each n64_game_catch_up_tick() lets the game thread play one tick (it waits at its frame boundary for the next
 * one) and waits a little for it. Without that driver (an older i64 calls n64_game_frame() instead), the game
 * thread catches up by itself as fast as it can. */
static pthread_mutex_t sCatchLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sCatchCond = PTHREAD_COND_INITIALIZER;
static uint64_t sCatchArrived;   /* times the game thread reached the gate (= ticks played + 1) */
static uint64_t sCatchTarget;    /* the tick being granted is done when sCatchArrived reaches this */
static int sCatchGranted;        /* a tick the game thread may play */
static int sCatchSelf;           /* no driver: free-running */
static int sCatchDriverTest;     /* GE_CATCHUP_TESTDRIVER=1 */
static unsigned sCatchDriverTicks, sCatchDriverFrames;

static void catchupGate(void)   /* game thread, at each frame boundary while catching up */
{
    extern volatile int g_portQuit;
    pthread_mutex_lock(&sCatchLock);
    sCatchArrived++;
    pthread_cond_broadcast(&sCatchCond);
    if (!sCatchSelf && !sCatchGranted) g_genetCatchupHeld = 1;   /* no retraces meanwhile (libultra.c) */
    while (!sCatchSelf && !sCatchGranted && !g_portQuit && genetCatchingUp(NULL, NULL)) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&sCatchCond, &sCatchLock, &ts);
    }
    sCatchGranted = 0;
    g_genetCatchupHeld = 0;
    pthread_mutex_unlock(&sCatchLock);
}

N64_GAME_EXPORT int n64_game_catching_up(uint32_t *done, uint32_t *total)
{
    if (!sRunning) return 0;
    return genetCatchingUp(done, total) ? 1 : 0;
}

N64_GAME_EXPORT n64_frame_result n64_game_catch_up_tick(void)
{
    if (!sRunning || genetEndReason()[0]) return N64_GAME_FRAME_EXIT;
    if (!genetCatchingUp(NULL, NULL)) return N64_GAME_FRAME_CONTINUE;
    pthread_mutex_lock(&sCatchLock);
    if (sCatchArrived >= sCatchTarget) {   /* the last tick is done (or none was granted): grant the next */
        sCatchTarget = sCatchArrived + 1;
        sCatchGranted = 1;
        pthread_cond_broadcast(&sCatchCond);
    }
    /* a short wait keeps i64's frame responsive: a long tick (a stage load) or inputs still on their way just
     * mean "try again next frame" */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 4 * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    while (sCatchArrived < sCatchTarget) {
        if (pthread_cond_timedwait(&sCatchCond, &sCatchLock, &ts) != 0) break;
    }
    const int done = sCatchArrived >= sCatchTarget;
    pthread_mutex_unlock(&sCatchLock);
    return done ? N64_GAME_FRAME_CONTINUE : GE_FRAME_WAITING;
}

/* the session's host: a new device takes over `machine`'s players (they are idle or removed since it left) */
N64_GAME_EXPORT int n64_game_admit(uint32_t machine)
{
    return genetAdmit((int32_t)machine);
}

N64_GAME_EXPORT int n64_game_start(const n64_host *host, const n64_launch *launch, char *err, size_t err_size)
{
    if (sRunning) {
        snprintf(err, err_size, "already running");
        return 1;
    }
    if (sBroken) {
        snprintf(err, err_size, "the last run didn't stop cleanly: restart i64");
        return 1;
    }
    if (!host || !launch || host->abi_version != N64_GAME_ABI_VERSION || launch->abi_version != N64_GAME_ABI_VERSION) {
        snprintf(err, err_size, "i64 and this game module disagree on the interface version");
        return 1;
    }
    if (launch->mode == N64_LAUNCH_ONLINE &&
        (!host->net_send || !host->net_recv || launch->player_count < 2 || launch->player_count > 4)) {
        snprintf(err, err_size, "online needs i64's game channel and 2-4 players");
        return 1;
    }
    if (!host->data_dir || !host->data_dir[0]) {
        snprintf(err, err_size, "missing folders");
        return 1;
    }
    {
        char p[1100];
        struct stat st;
        snprintf(p, sizeof(p), "%s/ge007.%s.z64", host->data_dir, ge_convert_region());
        if (stat(p, &st) != 0) {
            snprintf(err, err_size, "import your GoldenEye 007 ROM first");
            return 1;
        }
        if (!stampOk(host->data_dir)) {
            snprintf(err, err_size, "the game data is from another version: import the ROM again");
            return 1;
        }
    }
    /* first start: what the module has done so far is static initialisation */
    reentrySavePristine();
    sHost = host;
    sPreviewFrames = 0;
    gPortFrameCaptureCallback = host->offer_title_art;
    sAudioWrite = host->audio_write;
    sAudioBuffered = host->audio_buffered;
    /* game-side settings (reset with the globals after every run) */
    snprintf(g_sysDataDir, sizeof(g_sysDataDir), "%s", host->data_dir);
    {
        /* everything unlocked (i64's per-game choice; online the host's, the same launch flag on every machine):
         * All unlocked for this run and its own save file, ge007-unlocked.eep (port/src/libultra.c) */
        extern int32_t portForceAllUnlocked;
        portForceAllUnlocked = (launch->flags & N64_LAUNCH_UNLOCK_ALL) != 0;
        if (portForceAllUnlocked && host->log) host->log("ge: everything unlocked (save: ge007-unlocked.eep)");
    }
    g_gfxExternalWindow = host->window;
    if (host->audio_write) audioSetHostOutput(hostAudioWrite, hostAudioBuffered);
    static struct genetexternal ext;   /* reset with the globals after the run */
    if (launch->mode == N64_LAUNCH_ONLINE) {
        memset(&ext, 0, sizeof(ext));
        ext.localplayer = launch->local_machine == 0 ? 0 : 1;   /* genet: 0 = the session's host */
        ext.numplayers = (int32_t)launch->player_count;
        ext.dropoutmenu = (launch->flags & N64_LAUNCH_DROPOUT_MENU) != 0;
        ext.joining = (launch->flags & N64_LAUNCH_JOINING) != 0;
        for (uint32_t p = 0; p < launch->player_count && p < 4; p++) {
            ext.machineofplayer[p] = launch->players[p].machine;
            if (launch->players[p].local) {
                ext.localmask |= 1u << p;
                ext.padofplayer[p] = launch->players[p].pad;
            }
        }
        ext.seed = launch->seed;
        ext.blob = launch->session_blob;
        ext.blobsize = launch->session_blob_size;
        ext.send = host->net_send;
        ext.recv = host->net_recv;
        ext.hostpaced = 0;   /* the game's own threads keep its schedule */
        g_geNetExternal = &ext;
    }
    sCatchArrived = 0;
    sCatchTarget = 0;
    sCatchGranted = 0;
    sCatchSelf = getenv("GE_CATCHUP_SELF") != NULL;   /* test aid: no driver even with a new i64 */
    sCatchDriverTest = getenv("GE_CATCHUP_TESTDRIVER") != NULL;
    sCatchDriverTicks = sCatchDriverFrames = 0;
    g_genetCatchupGate = catchupGate;
    g_portHostTakeReserved = (int (*)(unsigned long long, unsigned long long))host->take_reserved;
    if (host->abi_version >= 2 && host->pad_read) {   /* i64's controllers for every game */
        g_portHostPadRead = hostPadRead;
        g_portHostPadRumble = host->pad_rumble;
    }
    g_portAddrError[0] = '\0';
    if (geModuleStart() != 0) {
        if (g_portAddrError[0]) {
            snprintf(err, err_size, "GoldenEye couldn't get the memory it needs at its fixed addresses. "
                     "Quit i64 completely and start GoldenEye again. (%s)", g_portAddrError);
        } else {
            snprintf(err, err_size, "the game couldn't start (see ge007.log in its data folder)");
        }
        /* partly started: undo what can be undone */
        if (geModuleStop() != 0) sBroken = 1;
        return 1;
    }
    sRunning = 1;
    return 0;
}


/* drop-outs (the session's host): the player stays idle (multiplayer has no CPU players) */
N64_GAME_EXPORT int n64_game_dropout(uint32_t player, n64_dropout_action action, const uint32_t *selection,
                                     uint32_t selection_count)
{
    (void)selection;
    (void)selection_count;
    return genetDropout((int32_t)player, (int32_t)action);
}

extern _Bool gfx_sdl_run_hidden;   /* a C++ bool (1 byte) in gfx_sdl2.cpp: i64 out of view (n64_game_set_hidden below) */

N64_GAME_EXPORT n64_frame_result n64_game_frame(void)
{
    if (!sRunning) return N64_GAME_FRAME_EXIT;
    if (sCatchDriverTest && genetCatchingUp(NULL, NULL)) {
        /* test aid GE_CATCHUP_TESTDRIVER=1: what i64's catch-up driver does (until it is in i64) */
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        unsigned n = 0;
        for (;;) {
            const n64_frame_result r = n64_game_catch_up_tick();
            if (r != N64_GAME_FRAME_CONTINUE) break;
            n++;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if ((t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec) > 14000000L || !genetCatchingUp(NULL, NULL)) break;
        }
        sCatchDriverTicks += n;
        sCatchDriverFrames++;
        if (!genetCatchingUp(NULL, NULL) && sHost && sHost->log) {
            char m[160];
            snprintf(m, sizeof(m), "ge: test driver: caught up, %u ticks over %u frames", sCatchDriverTicks, sCatchDriverFrames);
            sHost->log(m);
        }
        return N64_GAME_FRAME_CONTINUE;
    }
    if (genetCatchingUp(NULL, NULL) && !sCatchSelf) {
        /* i64 calls frames while this machine catches up: no catch-up driver, the game thread runs freely */
        pthread_mutex_lock(&sCatchLock);
        sCatchSelf = 1;
        pthread_cond_broadcast(&sCatchCond);
        pthread_mutex_unlock(&sCatchLock);
        if (sHost && sHost->log) sHost->log("ge: catching up on the game's own thread (no catch-up driver in i64)");
    }
    if (!gfx_sdl_run_hidden && !genetCatchingUp(NULL, NULL) && previewDue(++sPreviewFrames) && gPortFrameCaptureCallback) gPortFrameCaptureRequest = 1;
    if (sHost && sHost->abi_version >= 2 && sHost->net_poll_event_from) {
        uint32_t m = 0;
        int ev;
        while ((ev = sHost->net_poll_event_from(&m)) != 0)
            genetExternalEventFrom(ev, (int32_t)m);   /* same numbers: GENET_EXT_* = n64_net_event */
    } else if (sHost && sHost->net_poll_event) {
        int ev;
        while ((ev = sHost->net_poll_event()) != 0)
            genetExternalEvent(ev);
    }
    if (genetEndReason()[0]) return N64_GAME_FRAME_EXIT;   /* a desync or the friend gone: i64 shows why */
    return geModuleFrame() ? N64_GAME_FRAME_CONTINUE : N64_GAME_FRAME_EXIT;
}

N64_GAME_EXPORT void n64_game_exit_message(char *out, size_t cap)
{
    snprintf(out, cap, "%s", genetEndReason());
}

/* i64 out of view (iOS background) = 1, back = 0: the game keeps running (i64 goes on calling frame()); the
 * Metal backend draws nothing meanwhile, and netplay doesn't hold the others. */
N64_GAME_EXPORT void n64_game_set_hidden(int hidden)
{
    gfx_sdl_run_hidden = hidden != 0;
    if (sHost && sHost->log) sHost->log(hidden ? "ge: out of view, running without drawing" : "ge: back in view");
}

N64_GAME_EXPORT void n64_game_set_paused(int paused)
{
    if (sRunning) g_portHostPaused = paused;
}

N64_GAME_EXPORT void n64_game_stop(void)
{
    gfx_sdl_run_hidden = false;
    if (!sRunning) return;
    gPortFrameCaptureCallback = NULL;
    gPortFrameCaptureRequest = 0;
    pthread_mutex_lock(&sCatchLock);   /* a game thread waiting at the catch-up gate must reach its quit point */
    sCatchSelf = 1;
    pthread_cond_broadcast(&sCatchCond);
    pthread_mutex_unlock(&sCatchLock);
    if (geModuleStop() != 0) sBroken = 1;
    sRunning = 0;
    sHost = NULL;
    sAudioWrite = NULL;
    sAudioBuffered = NULL;
}

#endif /* GE_I64_MODULE */

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
#include "../i64/n64_game.h"

#include "ge_convert.h"
#include "reentry.h"
#include "genet_build_id.h"

#include <errno.h>
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
};
extern struct genetexternal *g_geNetExternal;   /* main.c */
extern uint32_t genetSessionBlob(void *out, uint32_t cap);
extern void genetExternalEvent(int32_t event);
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
    2,
    60,
    N64_GAME_INFO_OWN_THREAD,
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

static void hostAudioWrite(const short *stereo, unsigned frames, unsigned rate)
{
    if (sAudioWrite) sAudioWrite((const int16_t *)stereo, frames, rate);
}
static unsigned hostAudioBuffered(unsigned rate)
{
    return sAudioBuffered ? sAudioBuffered(rate) : 0;
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
        (!host->net_send || !host->net_recv || launch->local_player > 1)) {
        snprintf(err, err_size, "online needs i64's game channel and 2 players");
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
    sAudioWrite = host->audio_write;
    sAudioBuffered = host->audio_buffered;
    /* game-side settings (reset with the globals after every run) */
    snprintf(g_sysDataDir, sizeof(g_sysDataDir), "%s", host->data_dir);
    g_gfxExternalWindow = host->window;
    if (host->audio_write) audioSetHostOutput(hostAudioWrite, hostAudioBuffered);
    static struct genetexternal ext;   /* reset with the globals after the run */
    if (launch->mode == N64_LAUNCH_ONLINE) {
        memset(&ext, 0, sizeof(ext));
        ext.localplayer = (int32_t)launch->local_player;
        ext.seed = launch->seed;
        ext.blob = launch->session_blob;
        ext.blobsize = launch->session_blob_size;
        ext.send = host->net_send;
        ext.recv = host->net_recv;
        ext.hostpaced = 0;   /* the game's own threads keep its schedule */
        g_geNetExternal = &ext;
    }
    g_portHostTakeReserved = (int (*)(unsigned long long, unsigned long long))host->take_reserved;
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

N64_GAME_EXPORT n64_frame_result n64_game_frame(void)
{
    if (!sRunning) return N64_GAME_FRAME_EXIT;
    if (sHost && sHost->net_poll_event) {
        int ev;
        while ((ev = sHost->net_poll_event()) != 0)
            genetExternalEvent(ev);   /* same numbers: GENET_EXT_* = n64_net_event */
    }
    if (genetEndReason()[0]) return N64_GAME_FRAME_EXIT;   /* a desync or the friend gone: i64 shows why */
    return geModuleFrame() ? N64_GAME_FRAME_CONTINUE : N64_GAME_FRAME_EXIT;
}

N64_GAME_EXPORT void n64_game_exit_message(char *out, size_t cap)
{
    snprintf(out, cap, "%s", genetEndReason());
}

N64_GAME_EXPORT void n64_game_set_paused(int paused)
{
    if (sRunning) g_portHostPaused = paused;
}

N64_GAME_EXPORT void n64_game_stop(void)
{
    if (!sRunning) return;
    if (geModuleStop() != 0) sBroken = 1;
    sRunning = 0;
    sHost = NULL;
    sAudioWrite = NULL;
    sAudioBuffered = NULL;
}

#endif /* GE_I64_MODULE */

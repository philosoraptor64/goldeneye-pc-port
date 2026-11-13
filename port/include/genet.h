#ifndef PORT_GENET_H
#define PORT_GENET_H

#include <PR/ultratypes.h>

/* Deterministic fixed-step simulation and online play (port/src/genet.c). */
int genetFixedStep(void);
s32 genetFrameTicks(void);
int genetConnect(void);       /* main(), before the game starts; 1 = online session */
int genetOnline(void);
int genetSessionSeed(u64 *out);
void genetShutdown(void);

/* i64: the host app owns the connection (same shape as PD's pdnetexternal) */
struct genetexternal {
    s32 localplayer;            /* 0 = the session's host (player 1), 1 = guest */
    u64 seed;                   /* the same on every machine */
    const void *blob;           /* the host's genetSessionBlob, delivered reliably by the host app */
    u32 blobsize;
    int (*send)(const void *data, u32 size);  /* game channel: unreliable datagrams */
    int (*recv)(void *buf, u32 cap);          /* one datagram: size, 0 if none */
    s32 hostpaced;              /* the host app calls the frames at 60 Hz: no own pacing */
};
enum { GENET_EXT_PEER_AWAY = 1, GENET_EXT_PEER_BACK = 2, GENET_EXT_PEER_LEFT = 3 }; /* = n64_net_event */
u32 genetSessionBlob(void *out, u32 cap);   /* host, before every run; 0 = too small */
s32 genetInitExternal(const struct genetexternal *x);   /* at game start instead of genetConnect; 0 or -1 */
void genetExternalEvent(s32 event);         /* the host app's session events */
const char *genetEndReason(void);           /* in a host app: why the match ends (desync, friend gone); "" = it doesn't */
s32 genetHostPaced(void);

/* src/snd.c, game thread: 1 = this sound already started for the tick being
 * resimulated (*state/*sound: what it returned then); genetSfxPlayed records
 * a sound that was started */
int genetSfxSkip(s16 soundIndex, void **state, void **sound);
void genetSfxPlayed(s16 soundIndex, void *state, void *sound);

/* port/src/gesnap.c: rollback snapshots (Apple clang builds) */
int gesnapSupported(void);
int gesnapInit(int slots);
void gesnapSave(int slot);
int gesnapLoad(int slot);
void gesnapInvalidateAll(void);
size_t gesnapCompareLive(int slot, char *report, size_t cap);
void gesnapDumpDiff(int slot, const char *path);
double gesnapLastSaveMs(void);
double gesnapLastLoadMs(void);
size_t gesnapSize(void);

#endif

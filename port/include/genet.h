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

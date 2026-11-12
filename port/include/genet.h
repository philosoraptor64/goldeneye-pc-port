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

#endif

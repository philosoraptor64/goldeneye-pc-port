#ifndef PORT_GENET_H
#define PORT_GENET_H

#include <PR/ultratypes.h>

/* Deterministic fixed-step simulation for netplay (port/src/genet.c). */
int genetFixedStep(void);
s32 genetFrameTicks(void);

#endif

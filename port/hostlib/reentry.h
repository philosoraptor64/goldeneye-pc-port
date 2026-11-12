/*
 * Re-entry (i64): start and stop the game several times in one process. At
 * the very top of main (or the i64 module's first start), before any game
 * code runs, reentrySavePristine() copies this image's own writable data
 * (__DATA,__data / __bss / __common and the __GESNAP segment); after a full shutdown,
 * reentryRestorePristine() puts it back, so the next start sees every global
 * as a fresh process would. Same approach as Mario Kart's Reentry.cpp and
 * BattleShip's reentry.cpp: zero pages are reset by mapping fresh zero pages
 * over them (no copy), only pages with start-up data are copied.
 *
 * State that must survive a restore lives in __DATA,__gekeep (GE_KEEP).
 * Dynamically linked libraries (SDL on the Mac, i64's SDL for the module)
 * keep their state: the shutdown must leave them clean.
 */
#pragma once

#define GE_KEEP __attribute__((section("__DATA,__gekeep")))

#ifdef __cplusplus
extern "C" {
#endif

void reentrySavePristine(void);
void reentryRestorePristine(void);
/* copied / zero-mapped KB at the save; physical footprint KB and thread count now */
void reentryStats(unsigned long *copiedKb, unsigned long *zeroKb, unsigned long *footprintKb,
                  unsigned int *threads);

#ifdef __cplusplus
}
#endif

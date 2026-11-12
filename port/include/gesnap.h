/* Rollback snapshot section placement (port/src/gesnap.c).
 *
 * Force-included (CMake, Apple clang builds) into the game's C files
 * (src/game, most of src/: not audio, the scheduler or the thread stacks). It
 * puts their mutable globals, function-static locals included, into the
 * __GESNAP segment, so a snapshot of the simulation = that segment + the N64
 * DRAM arena (the game's heaps and memory pools, port/src/dram.c).
 * GE_SNAPSHOT_OUTSIDE moves one global of such a file back out (state another
 * thread writes, e.g. the polled controller buffers).
 *
 * Elsewhere (GCC, Windows, Linux) this is a no-op and rollback is unavailable:
 * those builds play online in lockstep. */
#ifndef GE_SNAPSHOT_SECTION_H
#define GE_SNAPSHOT_SECTION_H

#if defined(__APPLE__) && defined(__clang__)
#define GE_SNAPSHOT_SUPPORTED 1
#define GE_SNAPSHOT_SEGMENT "__GESNAP"
#define GE_SNAPSHOT_OUTSIDE __attribute__((section("__DATA,__bss")))
#if defined(GE_SNAPSHOT_FORCE) && !defined(__cplusplus) && !defined(__ASSEMBLER__)
#pragma clang section bss = "__GESNAP,__bss" data = "__GESNAP,__data"
#endif
#else
#define GE_SNAPSHOT_SUPPORTED 0
#define GE_SNAPSHOT_OUTSIDE
#endif

#endif

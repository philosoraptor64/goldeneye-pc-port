/*
 * gesnap.c -- whole-simulation snapshots for rollback netplay.
 *
 * A snapshot is: the game's globals (the __GESNAP segment, see
 * port/include/gesnap.h), the N64 DRAM arena (8 MB: the game's heaps, memory
 * pools, display lists and texture cache) and the playback controller buffer
 * (genet's input; the polled half stays outside, another thread writes it).
 * Saved at a frame boundary on the game thread.
 */
#include <ultra64.h>
#include <stdio.h>
#include <string.h>

#include "system.h"
#include "portaddr.h"
#include "gesnap.h"
#include "genet.h"

/* host libc (the decomp's include/stdlib.h stub shadows the host header) */
void *malloc(size_t n);
void free(void *p);

#if GE_SNAPSHOT_SUPPORTED
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
void *realloc(void *p, size_t n);
void *calloc(size_t n, size_t m);
char *getenv(const char *name);
int getpid(void);
#include "n64net_reloc.h"
/* i64's game memory arena (n64_game.h: I64_ARENA_BASE, I64_ARENA_SIZE) */
#define GE_I64_ARENA_BASE 0x110000000ULL
#define GE_I64_ARENA_SIZE (576ULL << 20)

void joyPortPlaybackData(void **p, size_t *n);   /* src/joy.c */

typedef struct {
    unsigned char *base;
    size_t size;
    const char *name;
} Range;

#define MAX_RANGES 4

typedef struct {
    unsigned char *buf;
    size_t size;
    int valid;
} Slot;

#undef MAX_RANGES
#define MAX_RANGES 8
static Range s_R[MAX_RANGES];
static int s_NumR = 0;
static size_t s_Total = 0;
static Slot *s_Slots = NULL;
static int s_NumSlots = 0;
static unsigned char *s_Scratch = NULL;
static double s_SaveMs = 0, s_LoadMs = 0;

static double nowMs(void)
{
    return (double)sysGetMicroseconds() / 1000.0;
}

int gesnapSupported(void)
{
    return 1;
}

/* A part of the DRAM arena another thread owns (the audio heap, src/music.c):
 * left out of snapshots. Called at boot, before gesnapInit. */
static unsigned char *s_ExclBase = NULL;
static size_t s_ExclSize = 0;

void gesnapExcludeRange(void *p, unsigned long size)
{
    s_ExclBase = (unsigned char *)p;
    s_ExclSize = (size_t)size;
}

/* this image (the app, or the i64 module dylib) */
extern const char __dso_handle;

static void setupRanges(void)
{
    unsigned long size = 0;
    void *p;
    size_t n;

    s_NumR = 0;
    p = getsectiondata((const struct mach_header_64 *)&__dso_handle, GE_SNAPSHOT_SEGMENT, "__data", &size);
    if (p && size) s_R[s_NumR++] = (Range){ p, size, "globals(data)" };
    size = 0;
    p = getsectiondata((const struct mach_header_64 *)&__dso_handle, GE_SNAPSHOT_SEGMENT, "__bss", &size);
    if (p && size) s_R[s_NumR++] = (Range){ p, size, "globals(bss)" };
    {
        unsigned char *d0 = (unsigned char *)(uintptr_t)PORT_DRAM_V1_BASE, *d1 = d0 + PORT_DRAM_SIZE;
        /* the audio heap may be reported through either DRAM view */
        unsigned char *x0 = s_ExclBase, *x1;
        if (x0 >= (unsigned char *)(uintptr_t)PORT_DRAM_K0_BASE &&
            x0 < (unsigned char *)(uintptr_t)PORT_DRAM_K0_BASE + PORT_DRAM_SIZE) {
            x0 = d0 + (x0 - (unsigned char *)(uintptr_t)PORT_DRAM_K0_BASE);
        }
        x1 = x0 + s_ExclSize;
        if (x0 && x0 >= d0 && x1 <= d1) {
            s_R[s_NumR++] = (Range){ d0, (size_t)(x0 - d0), "dram" };
            s_R[s_NumR++] = (Range){ x1, (size_t)(d1 - x1), "dram(after audio heap)" };
            sysLogPrintf(LOG_NOTE, "snapshot: audio heap %p +%zu left out", (void *)x0, s_ExclSize);
        } else {
            s_R[s_NumR++] = (Range){ d0, PORT_DRAM_SIZE, "dram" };
        }
    }
    joyPortPlaybackData(&p, &n);
    s_R[s_NumR++] = (Range){ p, n, "contdata(playback)" };

    s_Total = 0;
    for (int i = 0; i < s_NumR; i++) s_Total += s_R[i].size;
}

int gesnapInit(int slots)
{
    setupRanges();
    s_Slots = (Slot *)malloc(sizeof(Slot) * (size_t)slots);
    if (!s_Slots) return 0;
    for (int i = 0; i < slots; i++) {
        s_Slots[i].buf = (unsigned char *)malloc(s_Total);
        s_Slots[i].size = s_Total;
        s_Slots[i].valid = 0;
        if (!s_Slots[i].buf) return 0;
    }
    s_NumSlots = slots;
    s_Scratch = (unsigned char *)malloc(s_Total);
    sysLogPrintf(LOG_NOTE, "snapshot: %d slots of %zu KB (globals %zu + %zu, dram %zu)", slots, s_Total / 1024,
                 s_NumR > 0 ? s_R[0].size : 0, s_NumR > 1 ? s_R[1].size : 0, (size_t)PORT_DRAM_SIZE);
    return 1;
}

static void saveInto(unsigned char *dst)
{
    for (int i = 0; i < s_NumR; i++) {
        memcpy(dst, s_R[i].base, s_R[i].size);
        dst += s_R[i].size;
    }
}

void gesnapSave(int slot)
{
    double t0 = nowMs();
    if (!s_NumSlots) return;
    Slot *s = &s_Slots[slot % s_NumSlots];
    saveInto(s->buf);
    s->valid = 1;
    s_SaveMs = nowMs() - t0;
}

int gesnapLoad(int slot)
{
    double t0 = nowMs();
    const unsigned char *src;
    if (!s_NumSlots) return 0;
    Slot *s = &s_Slots[slot % s_NumSlots];
    if (!s->valid) return 0;
    src = s->buf;
    for (int i = 0; i < s_NumR; i++) {
        memcpy(s_R[i].base, src, s_R[i].size);
        src += s_R[i].size;
    }
    s_LoadMs = nowMs() - t0;
    return 1;
}

void gesnapInvalidateAll(void)
{
    for (int i = 0; i < s_NumSlots; i++) s_Slots[i].valid = 0;
}

/* ---- resync (getting back in step after a desync): the same ranges in a form another device can load ----
 * (n64net_reloc.h). DRAM, the stacks and the cart sit at the same fixed addresses on every device (i64's game
 * memory arena), so only pointers into this image need translating. */
static n64net_reloc s_Reloc;

static int resyncSpans(n64net_reloc_span *sp)
{
    if (!s_NumR) setupRanges();
    n64net_reloc_free(&s_Reloc);
    if (!n64net_reloc_init(&s_Reloc, &__dso_handle)) return 0;
    /* i64's game memory arena (DRAM, stacks, the cart): at one fixed address on every device, so pointers into
     * it go as offsets that mean the same thing there (not as "elsewhere in memory", which the guest would keep
     * its own of) */
    n64net_reloc_add(&s_Reloc, (const void *)(uintptr_t)GE_I64_ARENA_BASE, (size_t)GE_I64_ARENA_SIZE);
    s_Reloc.no_foreign = 1;   /* no heap pointers in the state (a resync of states in step: 0 own, 0 matched) */
    for (int i = 0; i < s_NumR && i < N64NET_RELOC_MAX_SPANS; i++) {
        sp[i].base = s_R[i].base;
        sp[i].used = sp[i].cap = s_R[i].size;
    }
    return s_NumR;
}

size_t gesnapResyncSize(void)
{
    unsigned long size = 0, total = sizeof(n64net_reloc_header) + 8 * MAX_RANGES;
    void *p;
    size_t n;
    /* the most it can be (the audio heap is left out later): globals, all of DRAM, the playback buffer */
    if (getsectiondata((const struct mach_header_64 *)&__dso_handle, GE_SNAPSHOT_SEGMENT, "__data", &size)) total += size;
    size = 0;
    if (getsectiondata((const struct mach_header_64 *)&__dso_handle, GE_SNAPSHOT_SEGMENT, "__bss", &size)) total += size;
    joyPortPlaybackData(&p, &n);
    return total + PORT_DRAM_SIZE + n;
}

void gesnapResyncSave(unsigned char *out)
{
    n64net_reloc_span sp[MAX_RANGES];
    n64net_reloc_stats st;
    const int n = resyncSpans(sp);
    if (!n) return;
    n64net_reloc_scan(&s_Reloc);
    n64net_reloc_save(&s_Reloc, sp, n, out, &st);
    sysLogPrintf(LOG_NOTE, "resync state: %d ranges, %llu words in the game's image, %llu elsewhere", n,
                 (unsigned long long)st.region_ptrs, (unsigned long long)st.foreign);
    if (getenv("GE_NET_RELOC_DUMP")) {   /* test: <dir>/ge-resync-<pid>.bin, to compare two devices */
        char path[1024];
        FILE *f;
        snprintf(path, sizeof(path), "%s/ge-resync-%d.bin", getenv("GE_NET_RELOC_DUMP"), (int)getpid());
        if ((f = fopen(path, "wb"))) {
            fwrite(out, 1, n64net_reloc_state_size(sp, n), f);
            fclose(f);
        }
    }
}

int gesnapResyncLoad(const unsigned char *in)
{
    n64net_reloc_span sp[MAX_RANGES];
    n64net_reloc_stats st;
    const int n = resyncSpans(sp);
    int ok;
    if (!n) return 0;
    n64net_reloc_scan(&s_Reloc);
    ok = n64net_reloc_load(&s_Reloc, sp, n, in, NULL, &st);
    sysLogPrintf(LOG_NOTE, "resync load %s: %llu words kept, %llu translated, %llu own pointers, %llu matched, "
                 "%llu guessed", ok ? "done" : "REFUSED", (unsigned long long)st.same,
                 (unsigned long long)st.translated, (unsigned long long)st.own_pointer,
                 (unsigned long long)st.matched, (unsigned long long)st.guessed);
    if (ok) gesnapInvalidateAll();   /* the ring is from the old timeline */
    return ok;
}

/* Compares the live state with a slot; writes a short report of the first
 * differing bytes per range (addresses: atos -o <binary> -l <image base>).
 * Returns the number of differing bytes. */
size_t gesnapCompareLive(int slot, char *report, size_t cap)
{
    size_t diff = 0, off = 0, used = 0;
    const unsigned char *a, *b;
    if (!s_NumSlots || !s_Scratch) return 0;
    saveInto(s_Scratch);
    a = s_Slots[slot % s_NumSlots].buf;
    b = s_Scratch;
    if (report && cap) report[0] = 0;
    for (int r = 0; r < s_NumR; r++) {
        int shown = 0;
        for (size_t i = 0; i < s_R[r].size; i++) {
            if (a[off + i] != b[off + i]) {
                diff++;
                if (report && shown < 4 && used + 96 < cap) {
                    used += (size_t)snprintf(report + used, cap - used, " %s+0x%zx(%p):%02x->%02x", s_R[r].name, i,
                                             (void *)(s_R[r].base + i), a[off + i], b[off + i]);
                    shown++;
                    /* skip the rest of this run */
                    while (i + 1 < s_R[r].size && a[off + i + 1] != b[off + i + 1]) { i++; diff++; }
                }
            }
        }
        off += s_R[r].size;
    }
    return diff;
}

/* Debug: writes every differing run between a slot and the live state as
 * "address length range" lines (GE_SYNCTEST_DUMP, symbolize with atos). */
void gesnapDumpDiff(int slot, const char *path)
{
    FILE *f;
    size_t off = 0;
    const unsigned char *a, *b;
    if (!s_NumSlots || !s_Scratch || !path) return;
    f = fopen(path, "w");
    if (!f) return;
    saveInto(s_Scratch);
    a = s_Slots[slot % s_NumSlots].buf;
    b = s_Scratch;
    for (int r = 0; r < s_NumR; r++) {
        for (size_t i = 0; i < s_R[r].size; i++) {
            if (a[off + i] != b[off + i]) {
                size_t j = i;
                while (j < s_R[r].size && a[off + j] != b[off + j]) j++;
                fprintf(f, "%p %zu %s\n", (void *)(s_R[r].base + i), j - i, s_R[r].name);
                i = j;
            }
        }
        off += s_R[r].size;
    }
    fclose(f);
}

double gesnapLastSaveMs(void) { return s_SaveMs; }
double gesnapLastLoadMs(void) { return s_LoadMs; }
size_t gesnapSize(void) { return s_Total; }

#else

int gesnapSupported(void) { return 0; }
size_t gesnapResyncSize(void) { return 0; }
void gesnapResyncSave(unsigned char *out) { (void)out; }
int gesnapResyncLoad(const unsigned char *in) { (void)in; return 0; }
void gesnapExcludeRange(void *p, unsigned long size) { (void)p; (void)size; }
int gesnapInit(int slots) { (void)slots; return 0; }
void gesnapSave(int slot) { (void)slot; }
int gesnapLoad(int slot) { (void)slot; return 0; }
void gesnapInvalidateAll(void) { }
size_t gesnapCompareLive(int slot, char *report, size_t cap) { (void)slot; (void)report; (void)cap; return 0; }
double gesnapLastSaveMs(void) { return 0; }
double gesnapLastLoadMs(void) { return 0; }
size_t gesnapSize(void) { return 0; }
void gesnapDumpDiff(int slot, const char *path) { (void)slot; (void)path; }

#endif

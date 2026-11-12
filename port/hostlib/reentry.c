/* See reentry.h. C version of SpaghettiKart's src/port/Reentry.cpp. */
#include "reentry.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <sys/mman.h>
#include <unistd.h>
extern const struct mach_header_64 __dso_handle; /* this image (the app, or the i64 module) */
#endif

typedef struct {
    uint8_t *base;
    size_t size;
    uint8_t *copy; /* pristine bytes, or NULL = all zero (whole pages) */
} Piece;

#define MAX_PIECES (1 << 16)
GE_KEEP static Piece *sPieces = NULL;
GE_KEEP static int sPieceCount = 0;
GE_KEEP static size_t sCopied = 0, sZero = 0;
GE_KEEP static int sSaved = 0;

static void addPiece(uint8_t *base, size_t size, int zero)
{
    if (size == 0 || sPieceCount >= MAX_PIECES)
        return;
    Piece *p = &sPieces[sPieceCount++];
    p->base = base;
    p->size = size;
    p->copy = NULL;
    if (zero) {
        sZero += size;
        return;
    }
    p->copy = malloc(size);
    memcpy(p->copy, base, size);
    sCopied += size;
}

static int allZero(const uint8_t *p, size_t n)
{
    const uint64_t *w = (const uint64_t *)p;
    for (size_t i = 0; i < n / 8; i++)
        if (w[i])
            return 0;
    return 1;
}

static void saveSection(const char *seg, const char *sect)
{
#if defined(__APPLE__)
    unsigned long size = 0;
    uint8_t *base = getsectiondata(&__dso_handle, seg, sect, &size);
    if (!base || !size)
        return;
    const uintptr_t page = (uintptr_t)getpagesize();
    uintptr_t start = (uintptr_t)base, end = start + size;
    uintptr_t first = (start + page - 1) & ~(page - 1), last = end & ~(page - 1);
    if (first >= last) {
        addPiece(base, size, 0);
        return;
    }
    addPiece(base, first - start, 0);
    uintptr_t runStart = first;
    int runZero = allZero((const uint8_t *)first, page);
    for (uintptr_t p = first + page; p < last; p += page) {
        int z = allZero((const uint8_t *)p, page);
        if (z != runZero) {
            addPiece((uint8_t *)runStart, p - runStart, runZero);
            runStart = p;
            runZero = z;
        }
    }
    addPiece((uint8_t *)runStart, last - runStart, runZero);
    addPiece((uint8_t *)last, end - last, 0);
#else
    (void)seg;
    (void)sect;
#endif
}

void reentrySavePristine(void)
{
    if (sSaved)
        return;
    sSaved = 1;
    sPieces = calloc(MAX_PIECES, sizeof(Piece));
    saveSection("__DATA", "__data");
    saveSection("__DATA", "__bss");
    saveSection("__DATA", "__common");
    /* the decomp's globals (rollback snapshot segment, port/include/gesnap) */
    saveSection("__GESNAP", "__data");
    saveSection("__GESNAP", "__bss");
}

void reentryRestorePristine(void)
{
#if defined(__APPLE__)
    for (int i = 0; i < sPieceCount; i++) {
        const Piece *p = &sPieces[i];
        if (p->copy) {
            memcpy(p->base, p->copy, p->size);
        } else {
            void *r = mmap(p->base, p->size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
            if (r == MAP_FAILED)
                memset(p->base, 0, p->size);
        }
    }
#endif
}

void reentryStats(unsigned long *copiedKb, unsigned long *zeroKb, unsigned long *footprintKb,
                  unsigned int *threads)
{
    *copiedKb = (unsigned long)(sCopied / 1024);
    *zeroKb = (unsigned long)(sZero / 1024);
    *footprintKb = 0;
    *threads = 0;
#if defined(__APPLE__)
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
        *footprintKb = (unsigned long)(info.phys_footprint / 1024);
    thread_act_array_t list;
    mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &list, &n) == KERN_SUCCESS) {
        *threads = n;
        for (mach_msg_type_number_t i = 0; i < n; i++)
            mach_port_deallocate(mach_task_self(), list[i]);
        vm_deallocate(mach_task_self(), (vm_address_t)list, n * sizeof(thread_act_t));
    }
#endif
}

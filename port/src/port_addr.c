/*
 * port_addr.c — the N64 address-space window (see port/include/portaddr.h,
 * D607's one-header model; the conversion inlines live in that header, this
 * file owns the window reservation, the image-base state, and the self-test).
 *
 * On macOS/arm64 the whole window is reserved PROT_NONE up front so that the
 * fixed-address carves (cart, DRAM V1/V2, game-thread stacks, in dram.c /
 * romdata.c / libultra.c) land inside a range the process already owns and
 * cannot collide with libmalloc or the dyld shared cache. Reserving is cheap:
 * PROT_NONE pages are not committed.
 *
 * On Windows/Linux PORT_ADDR_BASE is 0 and this is a no-op beyond recording
 * the image base.
 */

#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>

#include "platform.h"
#include "system.h"
#include "portaddr.h"
#ifdef PORT_ADDR_STRICT
#if defined(PLATFORM_MACOS) || defined(__APPLE__) || defined(__linux__)
/* backtrace support is POSIX-only; the block below is guarded the same way
 * (mingw-w64 has no execinfo.h without -static-libgcc; strict checks still
 * log the bad address there, just without a native backtrace). */
#include <execinfo.h>
#endif
#endif

#if defined(PLATFORM_MACOS) && defined(PLATFORM_ARM)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#elif defined(GE_IOS)
/* The iOS SDK blocks <mach/mach_vm.h>; vm_map is the same call with 64-bit
 * vm_address_t on arm64. */
#include <mach/mach.h>
#define mach_vm_address_t vm_address_t
#define mach_vm_size_t vm_size_t
#define mach_vm_map vm_map
#define mach_vm_deallocate vm_deallocate
#define mach_vm_region vm_region_64
#endif

uintptr_t g_portImageBase = 0;
int g_portUseImageRel = 0;

#include "reentry.h"
/* R1: the regions stay reserved across re-entry cycles (portAddrResetWindow
 * swaps their contents for fresh reservations); survives the globals reset. */
GE_KEEP static int s_windowReserved = 0;

/* i64 (n64_host.take_reserved, set by the module before each start): i64 reserves the
 * window before main(), so nothing the app allocates can land in it; we take it over. */
int (*g_portHostTakeReserved)(unsigned long long addr, unsigned long long size) = NULL;
char g_portAddrError[512] = "";

#if PORT_ADDR_BASE != 0
/* what occupies [start, end) (failure reports: which mapping is in the way) */
static void portAddrDescribe(unsigned long long start, unsigned long long end, char *out, size_t cap)
{
    size_t at = 0;
    out[0] = '\0';
    mach_vm_address_t addr = (mach_vm_address_t)start;
    for (int n = 0; n < 8 && addr < end && at + 96 < cap; n++) {
        mach_vm_size_t size = 0;
        vm_region_extended_info_data_t info;
        mach_msg_type_number_t count = VM_REGION_EXTENDED_INFO_COUNT;
        mach_port_t object = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &addr, &size, VM_REGION_EXTENDED_INFO, (vm_region_info_t)&info, &count,
                           &object) != KERN_SUCCESS || addr >= end)
            break;
        at += (size_t)snprintf(out + at, cap - at, "%s0x%llx-0x%llx (%llu MB, prot %d, tag %u)", at ? ", " : "",
                               (unsigned long long)addr, (unsigned long long)(addr + size),
                               (unsigned long long)(size >> 20), info.protection, info.user_tag);
        addr += size;
    }
    if (!at) snprintf(out, cap, "nothing mapped");
}
#endif

#if PORT_ADDR_BASE != 0
/* Only the regions the port maps (portaddr.h), not the whole 4 GiB window:
 * the rest is never touched, so the OS keeps it. */
static const struct { unsigned long long off, size; const char *what; } kPortRegions[] = {
    { PORT_CART_OFF,          PORT_CART_RESERVE,      "cart (ROM + sidecars)" },
    { 0x70000000ULL,          PORT_DRAM_SIZE,         "DRAM" },
    { 0x80000000ULL,          PORT_DRAM_SIZE,         "DRAM KSEG0 mirror" },
    { PORT_STACK_REGION_OFF,  PORT_STACK_REGION_SIZE, "thread stacks" },
};
#define PORT_NREGIONS ((int)(sizeof(kPortRegions) / sizeof(kPortRegions[0])))
#endif

/* Defined by port/src/dram_syms*.s; must equal PORT_ADDR_BASE + 0x70000000. */
extern unsigned char cfb_16[];

/* ROM-free verification of the address model: window round-trips for each
 * region, the absolute-symbol base, and the image-relative encoding. Logs
 * PASS/FAIL for each so a run without a ROM still exercises the core. */
static int portAddrCheck(const char *what, int ok)
{
    sysLogPrintf(ok ? LOG_INFO : LOG_ERROR, "portAddr: %-26s %s",
                 what, ok ? "PASS" : "FAIL");
    return ok;
}

static void portAddrSelfTest(void)
{
    const uintptr_t B = (uintptr_t)PORT_ADDR_BASE;
    int ok = 1;
    ok &= portAddrCheck("host->n64 DRAM V1",
                        portHostToN64((void *)(B + 0x70001234u)) == 0x70001234u);
    ok &= portAddrCheck("n64->host DRAM V1",
                        portN64ToHost(0x70001234u) == (void *)(B + 0x70001234u));
    ok &= portAddrCheck("host->n64 DRAM V2",
                        portHostToN64((void *)(B + 0x80005678u)) == 0x80005678u);
    ok &= portAddrCheck("host->n64 cart",
                        portHostToN64((void *)(B + 0x10456789u)) == 0x10456789u);
    ok &= portAddrCheck("n64->host cart",
                        portN64ToHost(0x10456789u) == (void *)(B + 0x10456789u));
    ok &= portAddrCheck("cfb_16 at base+DRAM_V1",
                        (uintptr_t)cfb_16 == B + 0x70000000u);
    ok &= portAddrCheck("in-window: NULL rejected",
                        !portAddrIsMapped(0));
    ok &= portAddrCheck("in-window: low guard",
                        !portAddrIsMapped((void *)(B + PORT_ADDR_LOW_GUARD - 1u)));
    ok &= portAddrCheck("in-window: DRAM V1",
                        portAddrIsMapped((void *)(B + 0x70001234u)));
    ok &= portAddrCheck("in-window: above window",
                        !portAddrIsMapped((void *)(B + (uintptr_t)PORT_ADDR_WINDOW)));
    if (g_portUseImageRel) {
        ok &= portAddrCheck("host->n64 image",
                            portHostToN64((void *)(g_portImageBase + 0x1234u)) == 0x40001234u);
        ok &= portAddrCheck("n64->host image",
                            portN64ToHost(0x40001234u) == (void *)(g_portImageBase + 0x1234u));
    }
    sysLogPrintf(ok ? LOG_INFO : LOG_ERROR, "portAddr: selftest %s",
                 ok ? "ALL PASS" : "FAILURES");
}

#ifdef PORT_ADDR_STRICT
/*
 * Strict address validation (-DPORT_ADDR_STRICT=1, off by default).
 *
 * Every bug in the MACOS-SFX-BOUND through MACOS-TEXALIGN family reaches portN64ToHost() holding a value
 * that is not a real N64 address -- either a host pointer truncated to 32 bits
 * or an address that was never re-based. The fault then happens later, in
 * unrelated code, which is what made these expensive to find. This checks the
 * value against the regions the port actually maps and reports the bad ones
 * at the point of conversion, naming the caller.
 *
 * Regions (see port/include/portaddr.h and port/src/dram.c):
 *   0x10000000..0x1fffffff  cart image + pcmodels/pccg sidecars
 *   0x40000000..0x6fffffff  image-relative encoding (exe globals)
 *   0x70000000..0x707fffff  DRAM V1
 *   0x80000000..0x807fffff  DRAM V2 (KSEG0 mirror of V1)
 *   0xa0000000..0xbfffffff  game-thread stacks
 * Segmented DL addresses (0x00000000..0x0fffffff) never name host memory and
 * are resolved by fast3d's seg_addr(), not here -- so they are reported too.
 */
static int portAddrRegionOk(uint32_t a)
{
    if (a >= 0x10000000u && a <= 0x1fffffffu) return 1;   /* cart + sidecars */
    if (a >= 0x40000000u && a <= 0x6fffffffu) return 1;   /* image-relative  */
    if (a >= 0x70000000u && a <= 0x707fffffu) return 1;   /* DRAM V1         */
    if (a >= 0x80000000u && a <= 0x807fffffu) return 1;   /* DRAM V2         */
    if (a >= 0xa0000000u && a <= 0xbfffffffu) return 1;   /* thread stacks   */
    return 0;
}

/* Negative self-test: GE_ADDRSTRICT_SELFTEST=1 feeds the validator a value
 * that is deliberately not a mapped N64 address, so a run can prove the
 * detector is actually live rather than merely silent. */
void portAddrStrictSelfTest(void)
{
    if (getenv("GE_ADDRSTRICT_SELFTEST")) {
        sysLogPrintf(LOG_INFO, "PORT_ADDR_STRICT: self-test, expect one report below");
        portAddrStrictCheck(0x30000000u);  /* in the gap between cart and image-rel */
    }
}

void portAddrStrictCheck(uint32_t a)
{
    /* Report each distinct bad value once -- a bad pointer in a per-frame path
     * would otherwise emit thousands of identical lines and bury the first. */
    enum { SEEN_MAX = 64 };
    static uint32_t seen[SEEN_MAX];
    static int seenCount;
    int i;

    if (portAddrRegionOk(a)) {
        return;
    }

    for (i = 0; i < seenCount; i++) {
        if (seen[i] == a) {
            return;
        }
    }
    if (seenCount < SEEN_MAX) {
        seen[seenCount++] = a;
    }

    sysLogPrintf(LOG_ERROR,
                 "PORT_ADDR_STRICT: 0x%08x is not a mapped N64 address "
                 "(-> %p). Truncated host pointer, or an address that was "
                 "never re-based (MACOS-SFX-BOUND through MACOS-TEXALIGN class).",
                 (unsigned)a, (void *)((uintptr_t)PORT_ADDR_BASE + (uintptr_t)a));
#if defined(PLATFORM_MACOS) || defined(__APPLE__) || defined(__linux__)
    {
        void *bt[24];
        int n = backtrace(bt, (int)(sizeof(bt) / sizeof(bt[0])));
        /* Skip frame 0 (this function); the caller is what matters. */
        if (n > 1) {
            backtrace_symbols_fd(bt + 1, n - 1, 2);
        }
    }
#endif
}
#endif /* PORT_ADDR_STRICT */

int portAddrInit(void)
{
    g_portImageBase = sysImageBase();
    /* The image needs the 0x40000000 encoding iff it lies outside the window,
     * i.e. its address does not survive a u32 truncation. False on Linux
     * (image at 0x20000000, inside the window), true on Windows (0x140000000)
     * and macOS (0x1_00xxxxxx + slide). */
    g_portUseImageRel = (g_portImageBase >= (uintptr_t)PORT_ADDR_WINDOW) ? 1 : 0;

#if PORT_ADDR_BASE != 0
    if (!s_windowReserved) {
        /* Non-clobbering: VM_FLAGS_FIXED without VM_FLAGS_OVERWRITE fails with
         * KERN_NO_SPACE if the range is already in use, rather than silently
         * replacing a live mapping (mmap MAP_FIXED on macOS does the latter). */
#define regions kPortRegions
        unsigned long long total = 0;
        /* inside i64: map our regions over i64's reservation (atomic replace) */
        int fromHost = g_portHostTakeReserved != NULL;
        for (size_t i = 0; fromHost && i < sizeof(regions) / sizeof(regions[0]); i++)
            fromHost = g_portHostTakeReserved(PORT_ADDR_BASE + regions[i].off, regions[i].size);
        for (size_t i = 0; fromHost && i < sizeof(regions) / sizeof(regions[0]); i++) {
            mach_vm_address_t addr = (mach_vm_address_t)(PORT_ADDR_BASE + regions[i].off);
            kern_return_t kr = mach_vm_map(mach_task_self(), &addr, (mach_vm_size_t)regions[i].size, 0,
                                           VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, MEMORY_OBJECT_NULL, 0, FALSE,
                                           VM_PROT_NONE, VM_PROT_ALL, VM_INHERIT_DEFAULT);
            if (kr != KERN_SUCCESS || addr != (mach_vm_address_t)(PORT_ADDR_BASE + regions[i].off)) {
                snprintf(g_portAddrError, sizeof(g_portAddrError), "portAddr: couldn't map the %s region inside i64's "
                         "window at 0x%llx (%s)", regions[i].what,
                         (unsigned long long)(PORT_ADDR_BASE + regions[i].off), mach_error_string(kr));
                sysLogPrintf(LOG_ERROR, "%s", g_portAddrError);
                return -1;
            }
        }
        for (size_t i = 0; !fromHost && i < sizeof(regions) / sizeof(regions[0]); i++) {
            mach_vm_address_t addr = (mach_vm_address_t)(PORT_ADDR_BASE + regions[i].off);
            kern_return_t kr = mach_vm_map(mach_task_self(), &addr,
                                           (mach_vm_size_t)regions[i].size, 0,
                                           VM_FLAGS_FIXED, MEMORY_OBJECT_NULL, 0, FALSE,
                                           VM_PROT_NONE, VM_PROT_ALL, VM_INHERIT_DEFAULT);
            if (kr != KERN_SUCCESS || addr != (mach_vm_address_t)(PORT_ADDR_BASE + regions[i].off)) {
                char occ[400];
                portAddrDescribe(PORT_ADDR_BASE + regions[i].off, PORT_ADDR_BASE + regions[i].off + regions[i].size,
                                 occ, sizeof(occ));
                for (size_t j = 0; j < i; j++)   /* nothing half-reserved stays behind */
                    mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)(PORT_ADDR_BASE + regions[j].off),
                                       (mach_vm_size_t)regions[j].size);
                snprintf(g_portAddrError, sizeof(g_portAddrError),
                         "portAddr: cannot reserve the %s region (%llu MB) at 0x%llx (%s); there: %s",
                         regions[i].what, regions[i].size >> 20,
                         (unsigned long long)(PORT_ADDR_BASE + regions[i].off), mach_error_string(kr), occ);
#if defined(GE_I64_MODULE)
                sysLogPrintf(LOG_ERROR, "%s", g_portAddrError);   /* inside i64: report it, don't take i64 down */
                return -1;
#else
                sysFatalError("%s. See docs/dev/MACOS-ARM64-PLAN.md sec 2.", g_portAddrError);
#endif
            }
            total += regions[i].size;
        }
        if (fromHost)
            for (size_t i = 0; i < sizeof(regions) / sizeof(regions[0]); i++) total += regions[i].size;
        sysLogPrintf(LOG_INFO, "portAddr: reserved %llu MB of address space in %d regions%s",
                     total >> 20, PORT_NREGIONS, fromHost ? " (inside i64's window)" : "");
#undef regions
        s_windowReserved = 1;
    } else {
        sysLogPrintf(LOG_INFO, "portAddr: window still reserved (re-entry)");
    }
#endif

    /* The committed .darwin.s absolute symbols must have been generated with
     * the same --base; cfb_16 is a region-independent sentinel for that. */
    if ((uintptr_t)cfb_16 != (uintptr_t)PORT_ADDR_BASE + 0x70000000ULL) {
        sysFatalError("portAddr: dram_syms base mismatch (cfb_16=%p, expected 0x%llx). "
                      "Regenerate with: scripts/gen_romassets.py <region> --darwin "
                      "--base 0x%llx --rom-size <size> --out ... --dram-out ...",
                      (void *)cfb_16,
                      (unsigned long long)((uintptr_t)PORT_ADDR_BASE + 0x70000000ULL),
                      (unsigned long long)PORT_ADDR_BASE);
    }

    sysLogPrintf(LOG_INFO, "portAddr: window base=0x%llx image=0x%llx image_rel=%d",
                 (unsigned long long)PORT_ADDR_BASE,
                 (unsigned long long)g_portImageBase, g_portUseImageRel);

    portAddrSelfTest();

#ifdef PORT_ADDR_STRICT
    portAddrStrictSelfTest();
#endif
    return 0;
}


/* R1 (i64 re-entry): after every port thread has stopped, replace everything
 * mapped in the window (DRAM views, the ROM image, thread stacks) with fresh
 * PROT_NONE reservations in one step each (VM_FLAGS_OVERWRITE: no moment in
 * which the range is free for someone else to take). The next start maps
 * DRAM, the ROM and stacks again as on first boot. */
void portAddrResetWindow(void)
{
#if PORT_ADDR_BASE != 0
#define regions kPortRegions
    for (size_t i = 0; i < sizeof(regions) / sizeof(regions[0]); i++) {
        mach_vm_address_t addr = (mach_vm_address_t)(PORT_ADDR_BASE + regions[i].off);
        kern_return_t kr = mach_vm_map(mach_task_self(), &addr, (mach_vm_size_t)regions[i].size, 0,
                                       VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, MEMORY_OBJECT_NULL, 0, FALSE,
                                       VM_PROT_NONE, VM_PROT_ALL, VM_INHERIT_DEFAULT);
        if (kr != KERN_SUCCESS)
            sysFatalError("portAddr: cannot reset region %d (%s)", (int)i, mach_error_string(kr));
    }
#undef regions
#endif
}

#if defined(GE_I64_MODULE) && PORT_ADDR_BASE != 0
/* i64 module: reserve the window when i64 loads the module (it does that when
 * it lists its games at launch), before the app's own heap can grow into it
 * (on an iPhone the only base that fits sits right above the heap). On
 * failure nothing stays reserved and portAddrInit reports it at start. */
__attribute__((constructor)) static void portAddrReserveAtLoad(void)
{
    for (int i = 0; i < PORT_NREGIONS; i++) {
        mach_vm_address_t addr = (mach_vm_address_t)(PORT_ADDR_BASE + kPortRegions[i].off);
        kern_return_t kr = mach_vm_map(mach_task_self(), &addr, (mach_vm_size_t)kPortRegions[i].size, 0,
                                       VM_FLAGS_FIXED, MEMORY_OBJECT_NULL, 0, FALSE,
                                       VM_PROT_NONE, VM_PROT_ALL, VM_INHERIT_DEFAULT);
        if (kr != KERN_SUCCESS || addr != (mach_vm_address_t)(PORT_ADDR_BASE + kPortRegions[i].off)) {
            for (int j = 0; j < i; j++)
                mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)(PORT_ADDR_BASE + kPortRegions[j].off),
                                   (mach_vm_size_t)kPortRegions[j].size);
            return;
        }
    }
    s_windowReserved = 1;
}
#endif

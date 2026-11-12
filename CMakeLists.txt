# ============================================================================
# GoldenEye 007 — PC port build
#
# This is a PARALLEL build system to the N64 `Makefile`. It compiles the same
# game C sources (src/game, src) for a modern host (Windows/Linux/macOS)
# against the PC shims in `port/`. The N64 build is untouched.
#
# See docs/internals.md for the architecture and plan.
# Modelled on the Perfect Dark PC port (perfect-dark-pc-port/perfect_dark).
#
# Usage:
#   cmake -S . -B build-pc -DROMID=ntsc-final
#   cmake --build build-pc -j
# ============================================================================

cmake_minimum_required(VERSION 3.16)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

# The decomp uses the `#define inherits struct` pattern (47x in
# src/bondtypes.h): a nested `struct Base;` inside a derived struct. GCC
# treats this as embedding the full base struct (correct layout, matches the
# N64/IDO behaviour the decomp relies on); Clang treats it as a bare forward
# declaration (no members) and rejects base-member access. So prefer GCC.
# (GCC does hard-error on the one struct that declares a member with the same
# name as an inherited one — CCTVRecord's `pad` — see docs/internals.md.)
# An explicit -DCMAKE_C_COMPILER=... always wins.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  # iPhone / iOS Simulator: Apple clang only (see the clang flags below).
  if(NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang)
    set(CMAKE_CXX_COMPILER clang++)
  endif()
elseif(NOT DEFINED CMAKE_C_COMPILER)
  if(CMAKE_HOST_APPLE)
    # Homebrew installs real GNU GCC with a version suffix; /usr/bin/gcc is
    # Apple Clang and does not support the Plan 9 struct extensions required
    # by the decomp's `inherits` pattern.
    find_program(_GE007_GCC
      NAMES gcc-16 gcc-15 gcc-14 gcc-13 gcc-12 gcc-11 gcc)
  else()
    find_program(_GE007_GCC NAMES gcc)
  endif()
  if(_GE007_GCC)
    set(CMAKE_C_COMPILER "${_GE007_GCC}" CACHE FILEPATH "C compiler" FORCE)
    get_filename_component(_GE007_GCC_DIR "${_GE007_GCC}" DIRECTORY)
    if(CMAKE_HOST_APPLE)
      find_program(_GE007_GXX
        NAMES g++-16 g++-15 g++-14 g++-13 g++-12 g++-11 g++
        PATHS "${_GE007_GCC_DIR}" NO_DEFAULT_PATH)
    else()
      find_program(_GE007_GXX NAMES g++
        PATHS "${_GE007_GCC_DIR}" NO_DEFAULT_PATH)
    endif()
    if(_GE007_GXX)
      set(CMAKE_CXX_COMPILER "${_GE007_GXX}" CACHE FILEPATH "CXX compiler" FORCE)
    endif()
  endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "17.0" CACHE STRING "" FORCE)
elseif(APPLE)
  # CMAKE_OSX_ARCHITECTURES is normally EMPTY here (the build targets the host
  # arch), so an arm64 Mac used to fall into the 10.13 (Intel) floor and ld
  # warned that the brew-gcc aarch64 runtime libs and the generated .s objects
  # carry a newer floor than the linked one. D609 (macos branch): also consult
  # the host machine when no explicit -arch list was requested. CMake's own
  # CMAKE_HOST_SYSTEM_PROCESSOR is EMPTY on macOS (its `uname -p` probe
  # returns "unknown" there - same class as D515's stripped-env arch probe),
  # so ask `uname -m` directly. 11.0 is Big Sur, the first arm64 release.
  if(CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    set(_GE007_OSX_FLOOR "11.0")
  elseif(CMAKE_OSX_ARCHITECTURES STREQUAL "")
    # No /bin/uname on macOS, and CMake's execute_process looks the name up in
    # PATH - so pass the bare name.
    execute_process(COMMAND uname -m OUTPUT_VARIABLE _GE007_UNAME_M
                    RESULT_VARIABLE _GE007_UMRC)
    if(_GE007_UMRC EQUAL 0)
      string(STRIP "${_GE007_UNAME_M}" _GE007_UNAME_M)
      if(_GE007_UNAME_M MATCHES "^(aarch64|arm64)")
        set(_GE007_OSX_FLOOR "11.0")
      else()
        set(_GE007_OSX_FLOOR "10.13")
      endif()
    endif()
  endif()
  if(NOT DEFINED _GE007_OSX_FLOOR)
    set(_GE007_OSX_FLOOR "10.13")
  endif()
  set(CMAKE_OSX_DEPLOYMENT_TARGET "${_GE007_OSX_FLOOR}" CACHE STRING "" FORCE)
  unset(_GE007_OSX_FLOOR)
  message(STATUS "macOS deployment target floor: ${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()

# ASM is needed for the generated absolute ROM asset symbols
# (port/src/romassets_<region>.s, see scripts/gen_romassets.py).
project(ge007 C CXX ASM)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  # iPhone build (docs/netplay-notes.md "iOS"). GE_IOS selects the iOS paths
  # in port/ (Documents for data/saves, netplay.cfg, OpenGL ES context).
  set(GE_IOS ON)
  add_compile_definitions(GE_IOS=1)
  set(GE_IOS_BUNDLE_ID "io.github.philosoraptor.goldeneye" CACHE STRING "iOS bundle identifier")
endif()

# Debug gate: validate every u32 -> host-pointer conversion against the regions
# the port actually maps, and report offenders at the conversion site with a
# backtrace. Off by default (zero cost); enable with -DPORT_ADDR_STRICT=ON.
# Catches the MACOS-SFX-BOUND through MACOS-TEXALIGN truncated/never-rebased-pointer family early.
option(PORT_ADDR_STRICT "Validate N64->host address conversions at runtime" OFF)

include(TargetArch)
include(FindPkgConfig)

set(CMAKE_C_STANDARD 11)
# GNU extensions ON: strict -std=c11/-std=c++20 define __STRICT_ANSI__, which
# disables glibc's _DEFAULT_SOURCE and hides POSIX symbols the port needs
# (clock_gettime/CLOCK_REALTIME in port/src/libultra.c, mmap flags, etc.).
# -std=gnu11/gnu++20 is GCC's own default and what MinGW was already using.
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Opt-in ccache launcher. The game sources are frozen (1:1 with the decomp),
# so ccache makes branch-switch / worktree rebuilds nearly free. No-op when
# ccache is not installed.
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
  set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
  message(STATUS "ccache enabled: ${CCACHE_PROGRAM}")
endif()

# Default build type
if(NOT CMAKE_BUILD_TYPE AND NOT EXISTS ${CMAKE_BINARY_DIR}/CMakeCache.txt)
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "" FORCE)
endif()

# ----------------------------------------------------------------------------
# Options
# ----------------------------------------------------------------------------
# Which ROM the executable expects in the data/ dir.
set(ROMID "ntsc-final" CACHE STRING "ROM id: ntsc-final (US), pal-final (EU), jpn-final (JP)")

# ----------------------------------------------------------------------------
# Target arch / platform
# ----------------------------------------------------------------------------
target_architecture(TARGET_ARCH)
if(TARGET_ARCH MATCHES "64")
  set(TARGET_IS_64BIT TRUE)
else()
  set(TARGET_IS_64BIT FALSE)
endif()
message(STATUS "Target arch: ${TARGET_ARCH} (64bit=${TARGET_IS_64BIT})")

# Host base of the N64 address window (port/include/portaddr.h, D607). Must be
# set BEFORE add_definitions below, and AFTER target_architecture(): D609 showed
# CMAKE_SYSTEM_PROCESSOR/CMAKE_HOST_SYSTEM_PROCESSOR are EMPTY on macOS, so the
# gate uses TARGET_ARCH (the D609 pagezero gate uses the same test).
# 0 on Windows/Linux; 16 TiB on arm64 macOS (overridable with
# -DPORT_ADDR_BASE=... for tooling, e.g. an ASan build whose shadow region
# overlaps 16 TiB). Fed to the compiler (-DPORT_ADDR_BASE) and to
# scripts/gen_macho_syms.py --base for the romassets absolute symbols, so the
# cart pairing (romdata.c CART_HOST vs romassets_*.s values) agrees.
if(GE_IOS AND NOT CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
  # Real iPhone: apps get only ~17 GB of address space (0x4_0000_0000 and up is KERN_INVALID_ADDRESS on an
  # iPhone 14 Pro, iOS 27), and 0x1/0x2_xxxx_xxxx hold the app's own heap. Probed with perfect_dark's PD_VMPROBE
  # (tools/vmprobe-iphone14pro.txt there): 0x3_0000_0000 is the only 4 GB-aligned base where all four regions
  # (+0x10000000 64 MB, +0x70000000 8 MB, +0x80000000 8 MB, +0xA0000000 144 MB) can be reserved. Reserve them as
  # early as possible at startup, before other allocations can land there. (The Simulator runs in the Mac's
  # address space and keeps the macOS base below.)
  set(PORT_ADDR_BASE 0x300000000 CACHE STRING "Host base of the N64 address window (iPhone device)")
elseif(APPLE AND TARGET_ARCH MATCHES "^aarch64$")
  set(PORT_ADDR_BASE 0x100000000000 CACHE STRING "Host base of the N64 address window (arm64 macOS)")
else()
  set(PORT_ADDR_BASE 0x0 CACHE STRING "Host base of the N64 address window")
endif()

if(WIN32)
  set(TARGET_OS "windows")
elseif(APPLE)
  set(TARGET_OS "osx")
elseif(UNIX)
  set(TARGET_OS "linux")
else()
  message(FATAL_ERROR "Unknown target OS")
endif()
set(TARGET_PLATFORM "${TARGET_ARCH}-${TARGET_OS}")
message(STATUS "Target platform: ${TARGET_PLATFORM}")

# Region -> compile definitions.
#
# The game code #ifdefs a full per-region macro set (see the N64 Makefile,
# the VERSION blocks). e.g. VERSION_EU is used in 60+ files, LEFTOVERDEBUG in
# 26, BUGFIX_R1 in 21. We MUST emit the same set the N64 build uses for the
# matching region, or the PC build takes different conditional branches and
# silently diverges (and fails to link). These mirror Makefile LCDEFS exactly.
if(ROMID STREQUAL "ntsc-final")
  set(VERSION_IS_PAL 0)
  set(BIN_REGION "")
  set(ASSET_REGION "u")
  set(REGION_DEFS
    -DVERSION_US -DLANG_US -DREFRESH_NTSC
    -DLEFTOVERDEBUG -DLEFTOVERSPECTRUM -DBUGFIX_R0 -DBYTEMATCH)
elseif(ROMID STREQUAL "pal-final")
  set(VERSION_IS_PAL 1)
  set(BIN_REGION ".pal")
  set(ASSET_REGION "e")
  set(REGION_DEFS
    -DVERSION_EU -DLANG_EU -DREFRESH_PAL
    -DBUGFIX_R1 -DBUGFIX_R2 -DBYTEMATCH)
elseif(ROMID STREQUAL "jpn-final")
  set(VERSION_IS_PAL 0)
  set(BIN_REGION ".jpn")
  set(ASSET_REGION "j")
  set(REGION_DEFS
    -DVERSION_JP -DLANG_JP -DREFRESH_NTSC
    -DBUGFIX_R1 -DLEFTOVERDEBUG -DLEFTOVERSPECTRUM -DBYTEMATCH)
else()
  message(FATAL_ERROR "Unknown ROMID: ${ROMID}")
endif()

# G3: native ROM sidecar converter (port/convert, C port of tools_pc/d43/d69/
# d88). Added here, before the game's global include paths and compile options
# (they shadow <stdio.h>/<zlib.h> and force-include versioninfo.h).
add_subdirectory(port/convert)

# git hash for version info
execute_process(COMMAND git rev-parse --short HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE VERSION_HASH OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT VERSION_HASH)
  set(VERSION_HASH "unknown")
endif()

# Provenance, printed by --version and at startup (port/src/main.c): a build
# keeps pointing at this repository no matter who compiles or renames it.
set(VERSION_CODENAME "larkspur")
set(GE007_VERSION "0.6.0")   # bump on every release
set(ORIGIN_URL "https://github.com/jkdansereau/goldeneye-pc-port")

configure_file(
  "${CMAKE_SOURCE_DIR}/port/include/versioninfo.h.in"
  "${CMAKE_BINARY_DIR}/port/include/versioninfo.h" @ONLY)

# Host <string.h> path for port/shim/string.h. The N64 include/string.h
# declares strlen/strchr with `const unsigned char *`, which conflicts with
# the host declarations that SDL (and other system headers) pull in, and it
# sits earlier on the include path than the toolchain's own string.h — so the
# shim cannot use #include_next (it would find the N64 header first). Derive
# the toolchain include dir from the compiler location (MinGW/MSYS2 layout:
# bin/../include) and generate an absolute-path include.
get_filename_component(_PORT_GCC_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
if(APPLE)
  # The SDK being built against (an iOS build passes its own sysroot), else
  # the default macOS SDK.
  if(CMAKE_OSX_SYSROOT AND IS_ABSOLUTE "${CMAKE_OSX_SYSROOT}" AND EXISTS "${CMAKE_OSX_SYSROOT}/usr/include/string.h")
    set(_PORT_MACOS_SDK "${CMAKE_OSX_SYSROOT}")
  elseif(CMAKE_OSX_SYSROOT)
    execute_process(
      COMMAND xcrun --sdk ${CMAKE_OSX_SYSROOT} --show-sdk-path
      OUTPUT_VARIABLE _PORT_MACOS_SDK OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
  endif()
  if(NOT _PORT_MACOS_SDK)
    execute_process(
      COMMAND xcrun --show-sdk-path
      OUTPUT_VARIABLE _PORT_MACOS_SDK OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
  endif()
  if(NOT _PORT_MACOS_SDK)
    message(FATAL_ERROR "Cannot locate the macOS SDK; install the Xcode Command Line Tools")
  endif()
  set(PORT_HOST_STRING_H "${_PORT_MACOS_SDK}/usr/include/string.h")
else()
  get_filename_component(PORT_HOST_STRING_H "${_PORT_GCC_DIR}/../include/string.h" ABSOLUTE)
endif()
configure_file(
  "${CMAKE_SOURCE_DIR}/port/include/hoststring.h.in"
  "${CMAKE_BINARY_DIR}/port/include/hoststring.h" @ONLY)

# Host <sched.h> path for port/shim/sched.h. The decomp's src/sched.h is on
# the include path and shadows the system header; libstdc++'s <thread>/gthr
# headers in the fast3d C++ TUs need the real <sched.h> (cpu_set_t,
# sched_yield). Both MinGW and Linux keep <sched.h> at bin/../include.
if(APPLE)
  set(PORT_HOST_SCHED_H "${_PORT_MACOS_SDK}/usr/include/sched.h")
else()
  get_filename_component(PORT_HOST_SCHED_H "${_PORT_GCC_DIR}/../include/sched.h" ABSOLUTE)
endif()
configure_file(
  "${CMAKE_SOURCE_DIR}/port/include/hostsched.h.in"
  "${CMAKE_BINARY_DIR}/port/include/hostsched.h" @ONLY)

# Host <stdlib.h> path for port/shim/stdlib.h. The N64 include/stdlib.h
# typedefs lldiv_t/ldiv_t, which conflict with the host's when both land in
# one C++ TU (fast3d): MinGW's own sec_api/stdlib_s.h does `#include
# <stdlib.h>`, which resolves to the N64 header via -I. The shim routes C++
# TUs to the host header by absolute path (same trick as hoststring.h).
if(APPLE)
  set(PORT_HOST_STDLIB_H "${_PORT_MACOS_SDK}/usr/include/stdlib.h")
else()
  get_filename_component(PORT_HOST_STDLIB_H "${_PORT_GCC_DIR}/../include/stdlib.h" ABSOLUTE)
endif()
configure_file(
  "${CMAKE_SOURCE_DIR}/port/include/hoststdlib.h.in"
  "${CMAKE_BINARY_DIR}/port/include/hoststdlib.h" @ONLY)

# Host <stddef.h> path for port/shim/stddef.h (same reason as stdlib: the N64
# stub shadows the host header in C++ TUs, hiding max_align_t from cstddef).
# stddef.h is compiler-internal, not in bin/../include: on a native Linux GCC
# it lives under lib/gcc/<triple>/<ver>/include. Ask the compiler where it is;
# fall back to the MinGW bin/../include layout.
execute_process(
  COMMAND "${CMAKE_C_COMPILER}" -print-file-name=include/stddef.h
  OUTPUT_VARIABLE _PORT_GCC_STDDEF OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(_PORT_GCC_STDDEF AND EXISTS "${_PORT_GCC_STDDEF}")
  get_filename_component(PORT_HOST_STDDDEF_H "${_PORT_GCC_STDDEF}" ABSOLUTE)
else()
  get_filename_component(PORT_HOST_STDDDEF_H "${_PORT_GCC_DIR}/../include/stddef.h" ABSOLUTE)
endif()
configure_file(
  "${CMAKE_SOURCE_DIR}/port/include/hoststddef.h.in"
  "${CMAKE_BINARY_DIR}/port/include/hoststddef.h" @ONLY)

# libc++ (Apple clang) wraps the C headers in <SDK>/usr/include/c++/v1, and its
# <cmath>/<cstring>/<cstdlib>/<cstddef> include the C header and then require
# the wrapper's _LIBCPP_*_H guard. The decomp's include/ stubs sit ahead of the
# system path, so the generated host*.h shims also pull the wrappers in by
# absolute path for C++ TUs (danturn's macos-arm64 branch, findings D295).
# libstdc++ (GCC) has no such wrappers.
set(PORT_HAVE_LIBCPP 0)
foreach(_h MATH STRING STDLIB STDDEF)
  set(PORT_LIBCPP_${_h}_H "")
endforeach()
if(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND EXISTS "${_PORT_MACOS_SDK}/usr/include/c++/v1/math.h")
  set(PORT_HAVE_LIBCPP 1)
  set(PORT_LIBCPP_MATH_H   "${_PORT_MACOS_SDK}/usr/include/c++/v1/math.h")
  set(PORT_LIBCPP_STRING_H "${_PORT_MACOS_SDK}/usr/include/c++/v1/string.h")
  set(PORT_LIBCPP_STDLIB_H "${_PORT_MACOS_SDK}/usr/include/c++/v1/stdlib.h")
  set(PORT_LIBCPP_STDDEF_H "${_PORT_MACOS_SDK}/usr/include/c++/v1/stddef.h")
endif()
if(APPLE)
  set(PORT_HOST_MATH_H "${_PORT_MACOS_SDK}/usr/include/math.h")
else()
  get_filename_component(PORT_HOST_MATH_H "${_PORT_GCC_DIR}/../include/math.h" ABSOLUTE)
endif()
foreach(_h string stdlib stddef math)
  configure_file(
    "${CMAKE_SOURCE_DIR}/port/include/host${_h}.h.in"
    "${CMAKE_BINARY_DIR}/port/include/host${_h}.h" @ONLY)
endforeach()

# ----------------------------------------------------------------------------
# Compiler definitions & flags
# ----------------------------------------------------------------------------
add_definitions(
  -DPORT=1
  -D_LANGUAGE_C=1
  -DAVOID_UB=1
  -DPAL=${VERSION_IS_PAL}
  -DPORT_ADDR_BASE=${PORT_ADDR_BASE}
  ${REGION_DEFS}
)
if(PORT_ADDR_STRICT)
  add_definitions(-DPORT_ADDR_STRICT=1)
  message(STATUS "PORT_ADDR_STRICT enabled: N64->host conversions are validated at runtime")
endif()
if(TARGET_IS_64BIT)
  add_definitions(-DPLATFORM_64BIT=1)
endif()

# The decomp is big-endian N64 code with unaligned accesses; these flags are
# required to compile it cleanly on a little-endian host (same as the PD port).
# Netplay determinism: no fused multiply-add. GCC fuses by default
# (-ffp-contract=fast) and clang within expressions; machines that build with
# different compilers or for different CPUs (Mac, iPhone, x86) must round the
# same way, so the simulation runs bit-identically everywhere.
add_compile_options(-ffp-contract=off)

add_compile_options(
  -fno-strict-aliasing
  -funsigned-char
  -fwrapv
  -include "versioninfo.h"
)
# The decompiled game code has a class of latent fixed-size stack-buffer
# over/under-runs (e.g. stan.c sub_GAME_7F0B1DDC's tileStack, bondview2.c
# bondviewCalcIntroSwirlCamera's pointbuf[10] indexed [-3..11]) that write a
# few words into adjacent stack scratch. On the N64, and on the MinGW/Windows
# build (no default stack protector), this is harmless in practice — verified
# stable over thousands of frames. Only Ubuntu's gcc default
# (-fstack-protector-strong) turns these into a fatal __stack_chk_fail at
# stage load / first frames. Match the N64 and MinGW builds so the same code
# runs the same way; the genuinely understood cases are still fixed properly
# under #ifdef PORT (D189). A hardened Linux build would need the whole class
# swept first — tracked in docs/dev/findings.md.
if(NOT GE_STACK_PROTECTOR_ALL)
  add_compile_options(-fno-stack-protector)
endif()
# Keep frame pointers: dbghelp's StackWalk64 (crash handler + thread dumps)
# cannot read DWARF unwind info on MinGW, so the EBP chain is the only way to
# get a real backtrace of a hung/crashed game thread.
add_compile_options(-fno-omit-frame-pointer)

# Optimisation: the software RSP + software rasteriser in port/fast3d are the
# hot path and are very sensitive to -O level (the difference between ~1 fps
# and playable). Non-Debug builds keep the CMAKE_BUILD_TYPE optimisation
# (RelWithDebInfo => -O2); only an explicit Debug build drops to -Og.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_options(-Og)
endif()

add_compile_options(
  -Wall
  -Wno-address
  -Wno-misleading-indentation
  -Wno-missing-braces
  -Wno-multichar
  -Wno-tautological-compare
  -Wno-unused-but-set-variable
  -Wno-unused-value
  -Wno-unused-variable
)
# The decomp was written for IDO's leniency. These downgrade hard errors to
# warnings so the unmodified game code compiles; each class is a tracked
# decomp finding (see docs/internals.md):
#   int-conversion: raw 32-bit ROM address constants assigned to pointers
#     (WIP decomp; genuine 64-bit hazards to fix upstream as phases progress)
#   incompatible-pointer-types: Mtxf union vs float[4][4], Gfx array idioms
#   implicit-function-declaration: missing header declarations (e.g.
#     get_ptr_long_watch_text_for_item, defined in gunfire.c)
add_compile_options(
  -Wno-error=int-conversion
  -Wno-error=incompatible-pointer-types
  -Wno-error=implicit-function-declaration
)
add_compile_options($<$<COMPILE_LANGUAGE:C>:-Wno-pointer-sign>)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wno-unused-function>)

# The decomp's `#define inherits struct` idiom (src/bondtypes.h) makes the base
# struct an unnamed tagged member and accesses its fields directly
# (obj->type where type lives in the embedded PropRecord). GCC targeting
# *-w64-mingw32 enables this via -fms-extensions implicitly; a native Linux/
# macOS GCC does not, so `'ObjectRecord' has no member named 'type'`. Enable
# the Plan 9 extensions explicitly there (superset of -fms-extensions: also the
# automatic base-pointer conversion the idiom relies on). C only — the decomp
# is C; fast3d C++ does not use `inherits`.
if(NOT MINGW)
  # GCC-only flags: the macOS default compiler is Apple clang (via the
  # /usr/bin/gcc c99 shim), which rejects both of these.
  if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    add_compile_options($<$<COMPILE_LANGUAGE:C>:-fplan9-extensions>)
    # GCC 14 promoted several long-standing C leniencies to hard errors
    # (int<->pointer for a known prototype, incompatible-pointer arg types,
    # implicit int/function). The WIP decomp still trips these (e.g.
    # append_text_picked_up's vestigial u8* params in propobj.c). -fpermissive
    # downgrades them to warnings — the C counterpart of the -Wno-error lines
    # above, and the documented path for building older C against GCC 14+.
    add_compile_options($<$<COMPILE_LANGUAGE:C>:-fpermissive>)
  elseif(CMAKE_C_COMPILER_ID MATCHES "Clang")
    # Clang (needed for the iOS build and the Metal renderer): -fms-extensions
    # gives the anonymous tagged base member of the `inherits` idiom (field
    # access); the base-pointer conversion GCC's Plan 9 mode adds is only a
    # warning here, and the base is the first member, so the address is the
    # same. Clang's newer hard errors for the decomp's IDO-era leniencies go
    # back to warnings, like GCC's -fpermissive.
    add_compile_options(
      $<$<COMPILE_LANGUAGE:C>:-fms-extensions>
      $<$<COMPILE_LANGUAGE:C>:-Wno-microsoft-anon-tag>
      $<$<COMPILE_LANGUAGE:C>:-Wno-error=incompatible-function-pointer-types>
      $<$<COMPILE_LANGUAGE:C>:-Wno-error=implicit-int>
      $<$<COMPILE_LANGUAGE:C>:-Wno-error=return-type>
      $<$<COMPILE_LANGUAGE:C>:-Wno-error=int-conversion>
      $<$<COMPILE_LANGUAGE:C>:-Wno-error=incompatible-pointer-types>
      $<$<COMPILE_LANGUAGE:C>:-Wno-error=implicit-function-declaration>)
    add_compile_definitions(_FORTIFY_SOURCE=0)
    # Apple clang still defaults to -fcommon; tentative definitions (`s32 x;`)
    # would land in __DATA,__common, which a section pragma can't move (the
    # rollback snapshot segment, gesnap.h). GCC's default, and what links.
    add_compile_options($<$<COMPILE_LANGUAGE:C>:-fno-common>)
  endif()
endif()

# ASLR must be OFF: dram_syms.s defines absolute symbols (cfb_16 @ 0x70000000,
# _bssSegmentEnd @ 0x70050000) that the game references through pointer-typed
# externs (src/bondgame.h: `extern u32 *_bssSegmentEnd`). On x86-64/PE, taking
# their address emits a .refptr slot holding the absolute value PLUS a base
# relocation; with ASLR the loader rebases it to runtime_base+0x70050000 =
# garbage. With DYNAMICBASE disabled the image loads at its preferred base
# (0x140000000) and the relocations are no-ops. main.c sanity-checks the load
# base at startup.
if(MINGW)
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--disable-dynamicbase")
elseif(UNIX AND NOT APPLE)
  # Linux equivalent of the ASLR-off requirement above: dram_syms.s defines
  # absolute symbols at 0x70000000 referenced through pointer-typed externs. A
  # PIE build (GCC's default on modern distros) emits GOT/relative relocations
  # for them — R_X86_64_32S link errors, or a runtime rebase to garbage. Build
  # and link no-PIE so the image keeps its preferred load address.
  add_compile_options(-fno-pie)
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -no-pie")
  # Move the image base to 0x20000000. The default no-PIE base 0x400000 puts
  # .data/.bss symbols around 0x00500000-0x00600000, which COLLIDES with the
  # N64 address ranges fast3d's seg_addr() decodes: a game-built display list
  # that embeds `&symbol` (via the osVirtualToPhysical identity shim) hands
  # seg_addr a value < 0x800000, which it treats as a K0-physical DRAM offset
  # and remaps to 0x80xxxxxx (zeroed DRAM mirror) -> "unknown GBI opcode 0x00"
  # -> black screen. 0x20000000 sits in the gap between the segmented-address
  # space (< 0x10000000), the cart map (0x10000000), the D131 module range
  # ([0x40000000,0x70000000)) and DRAM (>= 0x70000000); seg_addr passes such
  # addresses through untouched. Still < 2GiB so -mcmodel=small / R_X86_64_32
  # relocations are fine (unlike a 0x140000000 base, which needs mcmodel=large).
  # This is the ELF analogue of the Windows build's fixed 0x140000000 base.
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-Ttext-segment=0x20000000")
elseif(APPLE)
  # Mach-O executables normally reserve [0, 4 GiB) as __PAGEZERO. The x86_64
  # (Intel) port needs fixed mappings at 0x10000000 (ROM), 0x70000000 (s32-safe
  # DRAM) and 0x80000000 (KSEG0 mirror), so shrink the guard to 64 KiB and keep
  # null/low-pointer protection only that far (#88).
  #
  # D609: on arm64 macOS that flag is FATAL AT EXEC, not at link time. The
  # Apple-Silicon kernel enforces the 4 GiB guard region (SVE): an image whose
  # __PAGEZERO is smaller than 0x100000000 is SIGKILL'd by the kernel during
  # exec, before dyld runs - no output, no crash report, no unified-log entry
  # at unprivileged levels, and lldb refuses the image ("Malformed Mach-o file").
  # Reproduced with a 6-line C program: `gcc probe.c -Wl,-pagezero_size,0x10000`
  # exits 137 (128+9); the same program with the default pagezero exits 7. The
  # arm64 port does not need the shrink at all - portaddr.h already shifts the
  # whole N64 window up by PORT_ADDR_BASE = 16 TiB, so every required mapping
  # (ROM, DRAM V1/K0) lives far above the guard; verified with the same minimal
  # probe (shm_open + the two MAP_FIXED mmaps at 0x100070000000 / 0x100080000000
  # alias correctly and exit 7). So: shrink on x86_64 only, default on arm64.
  if(TARGET_ARCH MATCHES "^aarch64$")
    message(STATUS "arm64 macOS: keeping the default 4 GiB __PAGEZERO (arena is at PORT_ADDR_BASE)")
  else()
    set(CMAKE_EXE_LINKER_FLAGS
      "${CMAKE_EXE_LINKER_FLAGS} -Wl,-pagezero_size,0x10000")
  endif()
endif()

# port/shim first: PC-only shadow headers (bondtypes.h, bondconstants.h,
# PR/gbi.h) that fix N64-compiler assumptions without touching src/ or
# include/. Inert in the N64 build (its Makefile never sees port/shim).
include_directories(
  "${CMAKE_SOURCE_DIR}/port/shim"
  "${CMAKE_SOURCE_DIR}"
  "${CMAKE_SOURCE_DIR}/include"
  "${CMAKE_SOURCE_DIR}/include/PR"
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/src/game"
  "${CMAKE_SOURCE_DIR}/src/libultra"
  "${CMAKE_SOURCE_DIR}/src/libultra/audio"
  "${CMAKE_SOURCE_DIR}/src/libultra/gu"
  "${CMAKE_SOURCE_DIR}/port/include"
  "${CMAKE_BINARY_DIR}/port/include"
)

# ----------------------------------------------------------------------------
# Dependencies: SDL2, zlib, OpenGL
# ----------------------------------------------------------------------------
set(LIBS "")

if(GE_IOS)
  # No find module for an iOS SDL: pass a static SDL2 built with the UIScene
  # patch (e.g. ~/Development/N64/deps-iossim/sdl2 for the simulator):
  # -DSDL2_INCLUDE_DIR=<prefix>/include/SDL2 -DSDL2_LIBRARY=<prefix>/lib/libSDL2.a
  # -DSDL2MAIN_LIBRARY=<prefix>/lib/libSDL2main.a
  if(NOT SDL2_LIBRARY OR NOT SDL2_INCLUDE_DIR OR NOT SDL2MAIN_LIBRARY)
    message(FATAL_ERROR "iOS: set SDL2_LIBRARY, SDL2MAIN_LIBRARY and SDL2_INCLUDE_DIR")
  endif()
else()
  find_package(SDL2 REQUIRED)
endif()
include_directories(SYSTEM "${SDL2_INCLUDE_DIR}")
list(APPEND LIBS "${SDL2_LIBRARY}")

if(GE_IOS)
  set(ZLIB_LIBRARY "-lz")
  set(ZLIB_INCLUDE_DIR "${_PORT_MACOS_SDK}/usr/include")
else()
  find_package(ZLIB REQUIRED)
endif()
if(ZLIB_INCLUDE_DIR)
  include_directories(SYSTEM "${ZLIB_INCLUDE_DIR}")
endif()
list(APPEND LIBS "${ZLIB_LIBRARY}")

# Real <zlib.h> for port/src/rzdecomp.c (which backs decompressdata() with real
# zlib). The game's own src/game/zlib.h sits on the -I path and shadows a plain
# `#include <zlib.h>` in a port TU, so generate an absolute-path include (same
# trick as hoststring.h). Prefer find_package(ZLIB)'s dir; fall back to the
# toolchain include dir (MinGW layout: bin/../include), where
# mingw-w64-x86_64-zlib installs zlib.h.
set(PORT_REAL_ZLIB_H "${ZLIB_INCLUDE_DIR}/zlib.h")
if(NOT EXISTS "${PORT_REAL_ZLIB_H}")
  set(PORT_REAL_ZLIB_H "${_PORT_GCC_DIR}/../include/zlib.h")
endif()
if(NOT EXISTS "${PORT_REAL_ZLIB_H}")
  message(FATAL_ERROR "Cannot locate the host zlib.h needed by port/src/rzdecomp.c")
endif()
configure_file(
  "${CMAKE_SOURCE_DIR}/port/include/realzlib.h.in"
  "${CMAKE_BINARY_DIR}/port/include/realzlib.h" @ONLY)

if(NOT GL_LIBRARY)
  if(WIN32)
    set(GL_LIBRARY opengl32)
  elseif(GE_IOS)
    set(GL_LIBRARY "-framework OpenGLES")
  elseif(APPLE)
    set(GL_LIBRARY "-framework OpenGL")
  else()
    set(GL_LIBRARY GL)
  endif()
endif()
list(APPEND LIBS "${GL_LIBRARY}")

if(NOT EXTRA_LIBRARIES)
  if(MINGW)
    # MinGW: pthreads for the real-OS-thread kernel (port/src/libultra.c).
    set(EXTRA_LIBRARIES dbghelp -static-libstdc++ pthread)
  elseif(WIN32)
    set(EXTRA_LIBRARIES dbghelp -static-libstdc++)
  elseif(APPLE)
    set(EXTRA_LIBRARIES stdc++ m pthread)
  else()
    set(EXTRA_LIBRARIES stdc++ m dl pthread)
  endif()
endif()
list(APPEND LIBS "${EXTRA_LIBRARIES}")

# ----------------------------------------------------------------------------
# Sources
# ----------------------------------------------------------------------------
# Port layer (shims + software RSP).
file(GLOB_RECURSE SRC_PORT      "${CMAKE_SOURCE_DIR}/port/src/*.c")
file(GLOB_RECURSE SRC_PORT_CXX  "${CMAKE_SOURCE_DIR}/port/src/*.cpp")
# Metal renderer (port/fast3d/gfx_metal.mm): Apple clang builds only (GCC has no
# Objective-C++ for the Apple frameworks). The iOS build is always clang.
if(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(GE_HAVE_METAL ON)
  enable_language(OBJCXX)
  add_compile_definitions(GE_HAVE_METAL=1)
endif()
file(GLOB_RECURSE SRC_FAST3D    "${CMAKE_SOURCE_DIR}/port/fast3d/*.cpp"
                               "${CMAKE_SOURCE_DIR}/port/fast3d/glad/*.c")
if(GE_HAVE_METAL)
  list(APPEND SRC_FAST3D "${CMAKE_SOURCE_DIR}/port/fast3d/gfx_metal.mm")
  set_source_files_properties("${CMAKE_SOURCE_DIR}/port/fast3d/gfx_metal.mm" PROPERTIES COMPILE_FLAGS "-fobjc-arc")
else()
  list(REMOVE_ITEM SRC_FAST3D "${CMAKE_SOURCE_DIR}/port/fast3d/gfx_metal_shader.cpp")
endif()

# Generated absolute ROM asset symbols for this region (see
# scripts/gen_romassets.py). Defines every obseg/ramrom/music label and the
# ge007.ld segment markers as 0x10000000 + rom_offset, so `&symbol` / romCopy()
# work once romdata.c maps the .z64 at 0x10000000.
set(SRC_ROMASSETS_INPUT "${CMAKE_SOURCE_DIR}/port/src/romassets_${ASSET_REGION}.s")
if(NOT EXISTS ${SRC_ROMASSETS_INPUT})
  message(FATAL_ERROR "Missing ${SRC_ROMASSETS_INPUT} — run: python3 scripts/gen_romassets.py ${ASSET_REGION}")
endif()

# Absolute symbols inside the reserved N64-DRAM region (port/src/dram.c):
# cfb_16 (replaces src/cfb.c) and _bssSegmentEnd (mempool start marker).
set(SRC_DRAM_SYMS_INPUT "${CMAKE_SOURCE_DIR}/port/src/dram_syms.S")

# D441 high-arena test flavour: relocate the DRAM arena for test builds
# (tier 1 = 0x90000000). Build option only — the default build is unchanged.
#   GE_HIGHARENA_BASE=0x90000000 ./build-pc.sh ntsc-final
# or -DGE_HIGHARENA_BASE=0x90000000 at configure time. The value must be a
# C/asm integer literal; it overrides PORT_DRAM_V1_BASE in portaddr.h for
# C, CXX and ASM (dram.c's mapping and the dram_syms.S absolute symbols both
# follow it).
set(GE_HIGHARENA_BASE "" CACHE STRING "Override PORT_DRAM_V1_BASE for test builds (high-arena flavour)")
if(NOT GE_HIGHARENA_BASE)
  set(GE_HIGHARENA_BASE "$ENV{GE_HIGHARENA_BASE}")
endif()
if(GE_HIGHARENA_BASE)
  message(STATUS "GE_HIGHARENA_BASE = ${GE_HIGHARENA_BASE}")
  set(_d441_arena_def "-DPORT_DRAM_V1_BASE=${GE_HIGHARENA_BASE}")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${_d441_arena_def}")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${_d441_arena_def}")
  set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} ${_d441_arena_def}")
endif()

# Apple: the two absolute-symbol files are converted to Mach-O spelling
# (scripts/gen_macho_syms.py: .global -> .globl with a leading underscore,
# ELF-style directives dropped). dram_syms.S is C-preprocessed first: it
# pulls the arena bases from portaddr.h (PORT_ADDR_BASE + offset; the header
# self-detects the 16 TiB arm64 base from its own __ASSEMBLER__ branch, no
# -D needed), and the Mach-O assembler does not run the C preprocessor on
# .s input. Only the high-arena test flavour passes a -D override.
if(APPLE)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  # The generated .s files land in a build-dir subdir; CMake creates dirs for
  # rules with C/C++ outputs only, so make it explicitly (the cc -E -o below
  # fails with "No such file or directory" otherwise).
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/port/src")
  set(SRC_ROMASSETS "${CMAKE_BINARY_DIR}/port/src/romassets_${ASSET_REGION}_macho.s")
  set(SRC_DRAM_SYMS_EXPANDED "${CMAKE_BINARY_DIR}/port/src/dram_syms_expanded.s")
  set(SRC_DRAM_SYMS "${CMAKE_BINARY_DIR}/port/src/dram_syms_macho.s")
  add_custom_command(
    OUTPUT "${SRC_ROMASSETS}"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/scripts/gen_macho_syms.py"
            --base "${PORT_ADDR_BASE}"
            "${SRC_ROMASSETS_INPUT}" "${SRC_ROMASSETS}"
    DEPENDS "${SRC_ROMASSETS_INPUT}"
            "${CMAKE_SOURCE_DIR}/scripts/gen_macho_syms.py"
    VERBATIM)
  set(_dram_syms_pp_def "")
  if(GE_HIGHARENA_BASE)
    set(_dram_syms_pp_def "-DPORT_DRAM_V1_BASE=${GE_HIGHARENA_BASE}")
  endif()
  # D607: the COMMAND list must stay one CMake argument per argv element:
  # quoting the whole "cc -E -I..." as ONE string works on Windows (cmd
  # parses the command line) but /bin/sh on macOS/Linux treats the entire
  # string as a single filename ("No such file or directory").
  add_custom_command(
    OUTPUT "${SRC_DRAM_SYMS_EXPANDED}"
    COMMAND "${CMAKE_C_COMPILER}" "-E"
            "-I${CMAKE_SOURCE_DIR}/port/include"
            "-DPORT_ADDR_BASE=${PORT_ADDR_BASE}"
            ${_dram_syms_pp_def}
            "${SRC_DRAM_SYMS_INPUT}"
            "-o" "${SRC_DRAM_SYMS_EXPANDED}"
    DEPENDS "${SRC_DRAM_SYMS_INPUT}"
            "${CMAKE_SOURCE_DIR}/port/include/portaddr.h"
    VERBATIM)
  add_custom_command(
    OUTPUT "${SRC_DRAM_SYMS}"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/scripts/gen_macho_syms.py"
            "${SRC_DRAM_SYMS_EXPANDED}" "${SRC_DRAM_SYMS}"
    DEPENDS "${SRC_DRAM_SYMS_EXPANDED}"
            "${CMAKE_SOURCE_DIR}/scripts/gen_macho_syms.py"
    VERBATIM)
  set_source_files_properties(${SRC_ROMASSETS} ${SRC_DRAM_SYMS_EXPANDED} ${SRC_DRAM_SYMS}
    PROPERTIES GENERATED TRUE)
else()
  set(SRC_ROMASSETS "${SRC_ROMASSETS_INPUT}")
  set(SRC_DRAM_SYMS "${SRC_DRAM_SYMS_INPUT}")
endif()

# Game code — compiled unmodified.
file(GLOB_RECURSE SRC_GAME "${CMAKE_SOURCE_DIR}/src/game/*.c")

# EXCLUDED from src/game (replaced by port/src/rzdecomp.c, which backs
# decompressdata()/rzipGetSomething() with real zlib):
#   decompress.c + zlib.c — GE's custom gzip-1.2.4 inflate builds its Huffman
#   tables contiguously into a caller-supplied fixed buffer of `struct huft`.
#   That struct is 8 bytes on MIPS (union holds a 32-bit pointer) but 16 bytes
#   on x86-64 (union holds a 64-bit pointer), so on a 64-bit host the tables
#   overflow load_resource's `u8 buffer[0x2100]` and clobber the caller's return
#   address -> SIGSEGV in langInit()'s first file load (finding D32). Real zlib
#   manages its own table memory, removing the fixed-buffer assumption. Mirrors
#   the PD port, which swaps its assembly rzip for a real-zlib-backed rzip_c.c.
#   The only externally-referenced symbols from these two files are
#   decompressdata() and rzipGetSomething(); both are provided by
#   port/src/rzdecomp.c (see the symbol sweep in docs/internals.md §F).
list(REMOVE_ITEM SRC_GAME
  "${CMAKE_SOURCE_DIR}/src/game/decompress.c"
  "${CMAKE_SOURCE_DIR}/src/game/zlib.c")

# libaudio (AL) — runs on the CPU, platform independent.
file(GLOB SRC_LIBAUDIO "${CMAKE_SOURCE_DIR}/src/libultra/audio/*.c")
# Rare's audio "New" driver (drvrNew/env/reverb) — synthesizer.c calls
# alSaveNew/alAuxBusNew/alMainBusNew/alLoadNew/alResampleNew/alEnvmixerNew/
# alEnvmixerParam/alFxParam, all defined here. Runs on the CPU; must be linked.
file(GLOB SRC_LIBULTRARE_AUDIO "${CMAKE_SOURCE_DIR}/src/libultrare/audio/*.c")
# gu matrix helpers.
file(GLOB SRC_GU "${CMAKE_SOURCE_DIR}/src/libultra/gu/*.c")

# GCC for Darwin crashes while lowering the decomp's ELF-style
# `#pragma weak alias = target` directives. Compile generated copies with
# those directives removed, then provide their aliases in Mach-O assembly.
set(SRC_MACHO_ALIASES "")
if(APPLE)
  set(_MACHO_GENERATED_SOURCES "")
  set(_MACHO_WEAK_INPUTS
    "${CMAKE_SOURCE_DIR}/src/game/lv.c"
    "${CMAKE_SOURCE_DIR}/src/game/objective_status.c"
    "${CMAKE_SOURCE_DIR}/src/game/spectrum.c"
    "${CMAKE_SOURCE_DIR}/src/libultra/gu/sinf.c"
    "${CMAKE_SOURCE_DIR}/src/libultra/gu/cosf.c")
  foreach(_INPUT IN LISTS _MACHO_WEAK_INPUTS)
    get_filename_component(_NAME "${_INPUT}" NAME)
    set(_OUTPUT "${CMAKE_BINARY_DIR}/port/src/macho_${_NAME}")
    add_custom_command(
      OUTPUT "${_OUTPUT}"
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_SOURCE_DIR}/scripts/strip_weak_pragmas.py"
              "${_INPUT}" "${_OUTPUT}"
      DEPENDS "${_INPUT}" "${CMAKE_SOURCE_DIR}/scripts/strip_weak_pragmas.py"
      VERBATIM)
    list(APPEND SRC_MACHO_ALIASES "${_OUTPUT}")
    list(APPEND _MACHO_GENERATED_SOURCES "${_OUTPUT}")
    list(REMOVE_ITEM SRC_GAME "${_INPUT}")
    list(REMOVE_ITEM SRC_GU "${_INPUT}")
  endforeach()
  list(APPEND SRC_MACHO_ALIASES
    "${CMAKE_SOURCE_DIR}/port/src/macho_weak_aliases.s")
  set_source_files_properties(${_MACHO_GENERATED_SOURCES}
    PROPERTIES GENERATED TRUE)
endif()

# VI mode table (DATA, not a driver): fr.c references osViModeTable ~15x.
# This is the only libultra/io file we compile — the rest are hardware
# drivers that get shimmed in port/src/libultra.c.
set(SRC_LIBULTRARE_DATA
  src/libultrare/io/vitbl.c
)
list(PREPEND SRC_LIBULTRARE_DATA "${CMAKE_SOURCE_DIR}")

# IDO printf engine: src/sprintf.c's sprintf() calls _Printf(prout, dst, fmt,
# va_list), the internal formatter from libultrare/libc (pure C, no hardware).
# xlitob/xldtob are its integer-conversion helpers (_Litob/_Ldtob). The
# _Printf prototype clash between xstdio.h and xprintf.c is resolved by the
# port/shim/libc/xstdio.h shadow header.
set(SRC_LIBULTRARE_LIBC
  src/libultrare/libc/xprintf.c
  src/libultra/libc/xlitob.c
  src/libultra/libc/xldtob.c
)
list(PREPEND SRC_LIBULTRARE_LIBC "${CMAKE_SOURCE_DIR}")

# ----------------------------------------------------------------------------
# Asset data — decompiled ROM arrays (pure data, no logic).
#
# Single-file assets compile unmodified. The per-model / per-level .c files
# under assets/obseg/{bg,brief,setup,stan} are NOT compiled: each one defines
# file-local globals with generic names (`header`, `room_data_table`,
# `intro`, ...) that the N64 build keeps apart by linking every file into its
# own ELF (see assets/obseg/Makefile.*). In a single host link they collide.
# The game loads those blobs from ROM at runtime anyway; their top-level
# symbols (C*Z, G*Z, P*Z, Tbg_*Z, U*Z, L*E/L*J, bg_*_all_p_seg, ...) are now
# absolute cart addresses in the generated romassets_<region>.s (see
# scripts/gen_romassets.py), backed by the ROM image that romdata.c maps at
# 0x10000000. The per-model data itself (chr/*/Model.c etc.) is gitignored
# ROM-derived and absent from this checkout. See docs/internals.md §11.
# D34 (docs/internals.md): animationtable_{data,entries}.c are NOT
# compiled on PC. Their arrays exist only so that &ANIM_DATA_x evaluates to
# the record's offset within the animation_data segment (N64 links it at VMA
# 0); on x86-64 assets/animationtable_data.h redefines them as lvalues at
# g_pc_animdata_base + offset instead. The data itself comes from ROM via
# romCopy, exactly like every other asset.
set(SRC_ASSETS
  assets/font_chardatae.c
  assets/font_chardataj.c
  assets/font_dl.c
  assets/oddtextures.c
  assets/rarewarelogo.c
)
list(PREPEND SRC_ASSETS "${CMAKE_SOURCE_DIR}")

# Engine files in src/ that are platform independent.
# NOTE: this list is a starting point and must be curated during Phase 1.
#
# INCLUDED here (platform-independent or CPU-side):
#   init.c    — provides mainproc() (the game's real main entry) + the thread
#               setup helpers. Its N64-only init() is compiled but NOT called;
#               the N64 symbols init() references are stubbed in
#               port/src/n64stubs.c (see A3 in docs/internals.md).
#   cfb.c     — defines cfb_16 (the framebuffer), referenced by fr.c and game
#               code (bondview2.c, front.c). Pure data.
#   stacks.c  — thread-stack arrays (sp_boot/sp_rmon/sp_idle/sp_shed/sp_main/
#               sp_audi + sp_debug under LEFTOVERDEBUG). Pure data; referenced
#               by init.c (thread setup) and audi.c (sp_audi). The SP_*_SZ
#               sizes in bondconstants.h match these exactly, so sizeof() is
#               correct. No duplicate-definition risk (all other refs are
#               extern decls in bondgame.h / audi.c).
#
# EXCLUDED (hardware drivers, replaced by port/ shims):
#   crash.c rmon.c usb.c tlb_*.c pi.c vi.c motor.c
#   (there is no src/spectrum.c in this decomp)
#   All of src/libultra/io/*.c and src/libultrare/io/*.c EXCEPT vitbl.c.
#   motor.c / pfsinit.c / pfsisplug.c are NOT compiled: joy.c calls
#   osMotorInit/Start/Stop and osPfsInit/osPfsIsPlug only for accessory
#   (Rumble Pak / Memory Pak) detection, so they are shimmed as no-ops in
#   port/src/libultra.c (see B2 — saves are EEPROM, not PFS).
#
# sched.c IS compiled (Phase 2): the game's real scheduler runs on the
# cooperative thread kernel; its hardware leaf calls (osSpTaskLoad/StartGo,
# osDpSetNextBuffer, osVi*) are shimmed in port/src/libultra.c, which runs
# the software RSP inline and posts the RSP_DONE/RDP_DONE messages.
set(SRC_ENGINE
  src/init.c
  src/sched.c
  src/audi.c
  src/snd.c
  src/music.c
  src/joy.c
  src/fr.c
  # src/cfb.c — cfb_16 now lives at 0x80000000 in the reserved DRAM region
  # (port/src/dram_syms.s) so OS_K0_TO_PHYSICAL(cfb_16[0]) is a small offset
  # that fast3d can map back; a host-BSS array would truncate to garbage.
  src/stacks.c
  src/boss.c
  src/mema.c
  src/memp.c
  src/deb.c
  src/debugmenu.c
  src/speed_graph.c
  src/token.c
  src/str.c
  src/sprintf.c
  src/ramrom.c
)
list(PREPEND SRC_ENGINE "${CMAKE_SOURCE_DIR}")

set(SRC
  ${SRC_GAME}
  ${SRC_PORT}
  ${SRC_ROMASSETS}
  ${SRC_DRAM_SYMS}
  ${SRC_PORT_CXX}
  ${SRC_FAST3D}
  ${SRC_LIBAUDIO}
  ${SRC_LIBULTRARE_AUDIO}
  ${SRC_LIBULTRARE_DATA}
  ${SRC_LIBULTRARE_LIBC}
  ${SRC_ASSETS}
  ${SRC_GU}
  ${SRC_MACHO_ALIASES}
  ${SRC_ENGINE}
)

# Rollback snapshots (port/include/gesnap.h): the game's own C files put their
# globals into the __GESNAP segment (Apple clang only). Not audio (its own
# thread), the scheduler, the thread stacks or the port layer.
if(GE_HAVE_METAL)
  set(_GE_SNAP_SRC ${SRC_GAME} ${SRC_ENGINE} ${_MACHO_GENERATED_SOURCES}
      "${CMAKE_SOURCE_DIR}/port/src/drawdistgameplay.c"    # D466 room sets: gameplay state
      "${CMAKE_SOURCE_DIR}/port/src/random.c")             # the game's RNG seeds
  # (SRC_ENGINE holds paths relative to the source dir)
  # joy.c: the scheduler thread polls into it; genet saves the playback half itself
  foreach(_x src/audi.c src/snd.c src/music.c src/sched.c src/stacks.c src/init.c src/speed_graph.c src/joy.c)
    list(REMOVE_ITEM _GE_SNAP_SRC "${CMAKE_SOURCE_DIR}/${_x}" "${_x}")
  endforeach()
  list(REMOVE_ITEM _GE_SNAP_SRC "${CMAKE_SOURCE_DIR}"
       "${CMAKE_SOURCE_DIR}/src/game/speedgraphrenderer.c")   # performance graph (display only)
  set_source_files_properties(${_GE_SNAP_SRC} PROPERTIES COMPILE_OPTIONS
    "-include;${CMAKE_SOURCE_DIR}/port/include/gesnap.h;-DGE_SNAPSHOT_FORCE")
endif()

# ----------------------------------------------------------------------------
# Target
# ----------------------------------------------------------------------------
set(BIN_NAME "ge007${BIN_REGION}.${TARGET_ARCH}")
add_executable(ge007 ${SRC})
target_link_libraries(ge007 ${LIBS})

# Netplay (port/src/genet.c): n64net's input ledger + rendezvous, and a build
# fingerprint refreshed at every build (cmake/NetBuildId.cmake) so only
# identical builds connect.
add_subdirectory(cmake/n64net "${CMAKE_BINARY_DIR}/n64net")
if(GE_HAVE_METAL AND NOT GE_IOS)
  target_link_libraries(ge007 "-framework Metal" "-framework QuartzCore" "-framework Foundation")
endif()
if(GE_IOS)
  target_link_libraries(ge007 "${SDL2MAIN_LIBRARY}"
    "-framework UIKit" "-framework CoreGraphics" "-framework CoreMotion" "-framework GameController"
    "-framework CoreHaptics" "-framework AVFoundation" "-framework AudioToolbox" "-framework CoreAudio"
    "-framework CoreBluetooth" "-framework ImageIO" "-framework UniformTypeIdentifiers"
    "-framework QuartzCore" "-framework Metal" "-framework Foundation" "-liconv")
  set_target_properties(ge007 PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/ios/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${GE_IOS_BUNDLE_ID}"
    MACOSX_BUNDLE_BUNDLE_NAME "GoldenEye"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1"
    MACOSX_BUNDLE_BUNDLE_VERSION "1"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${GE_IOS_BUNDLE_ID}")
endif()
target_link_libraries(ge007 n64net)
target_link_libraries(ge007 ge007_convert)
add_custom_target(ge_net_build_id ALL
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_BINARY_DIR}/generated/genet_build_id.h
          -P ${CMAKE_SOURCE_DIR}/cmake/NetBuildId.cmake
  BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/genet_build_id.h
  COMMENT "Netplay build fingerprint")
add_dependencies(ge007 ge_net_build_id)
target_include_directories(ge007 PRIVATE ${CMAKE_BINARY_DIR}/generated)
set_target_properties(ge007 PROPERTIES OUTPUT_NAME "${BIN_NAME}")
if(WIN32)
  set_target_properties(ge007 PROPERTIES WIN32_EXECUTABLE TRUE)
  # Explorer/taskbar icon: the original crosshair mark (cmake/icon/ge007.rc,
  # a static resource script; the ge007.ico it references is generated by
  # tools_pc/make_icon.py). Compiled by the resource compiler; ge007.ico is
  # resolved relative to this .rc file.
  target_sources(ge007 PRIVATE cmake/icon/ge007.rc)
endif()

message(STATUS "Output binary: ${BIN_NAME}")

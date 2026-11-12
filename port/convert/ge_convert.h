/*
 * ge_convert: native (C) version of the ROM sidecar converters
 * (tools_pc/d43_emit.py -> pcmodels-<region>/, d69_emit.py + d88_emit.py
 * --regen -> pccg-<region>/). Output is byte-identical to the Python tools
 * (checked by tools_pc/check_native_convert.sh). Used where the bundled
 * Python converter can't run (iOS: no fork/exec) and by the i64 module's ROM
 * import. Built as its own library with plain system headers (the game's
 * include path shadows <stdio.h>/<zlib.h>).
 *
 * The file tables (which ROM ranges to convert, model header counts) are
 * generated at build time from the repo by tools_pc/gen_convert_tables.py
 * for one region (the build's ASSET_REGION).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The region these tables were generated for ("ntsc-final", ...). */
const char *ge_convert_region(void);

/* Converts `rom` (big-endian .z64 image) and writes
 *   <out_root>/pcmodels-<region>/{pcmodels.bin,manifest.csv}
 *   <out_root>/pccg-<region>/{pccg.bin,manifest.csv}
 * Each .bin is written last (via a temporary name), so a present .bin means
 * a complete conversion. progress (may be NULL) gets 0..1.
 * Returns 0 on success, else -1 with a reason in err. */
int ge_convert_sidecars(const uint8_t *rom, size_t rom_size, const char *out_root,
                        void (*progress)(float done, void *user), void *user,
                        char *err, size_t err_size);

#ifdef __cplusplus
}
#endif

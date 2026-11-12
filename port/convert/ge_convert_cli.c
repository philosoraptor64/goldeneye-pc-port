/* ge007-convert-native <rom.z64> <out_root>: runs the native converter (for
 * tools_pc/check_native_convert.sh, which compares it with the Python tools). */
#include "ge_convert.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <rom.z64> <out_root>\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *rom = malloc((size_t)n);
    if (!rom || fread(rom, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "read failed\n"); return 1; }
    fclose(f);
    char err[512];
    if (ge_convert_sidecars(rom, (size_t)n, argv[2], NULL, NULL, err, sizeof(err)) != 0) {
        fprintf(stderr, "convert failed: %s\n", err);
        return 1;
    }
    printf("converted (%s) into %s\n", ge_convert_region(), argv[2]);
    free(rom);
    return 0;
}

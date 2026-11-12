/*
 * PC port shim for <math.h> / <cmath>.
 *
 * The decomp's include/math.h is a constants-only stub (M_PI & friends, plus
 * sinf/cosf/sqrtf) that sits ahead of the system header on the -I path.
 *
 *  - C TUs want it, and so does C++ everywhere except macOS: port/include/
 *    port_math.h reaches the real declarations with #include_next, the game
 *    relies on the N64 constants, and libstdc++/MSVC tolerate the stub.
 *  - macOS C++ TUs (fast3d) must not have it. libc++ does not use <math.h>
 *    directly: it includes the C header, then requires its own wrapper
 *    ($SDK/usr/include/c++/v1/math.h) to have defined _LIBCPP_MATH_H. With
 *    the stub shadowing the header, <cmath> hard-errors and acosf/fmodf/
 *    fabsf stay undeclared.
 *
 * So macOS C++ TUs are routed to the HOST header + libc++ wrapper by absolute
 * path through the generated hostmath.h; everything else is unchanged.
 *
 * Inert in the N64 build (port/shim is not on its include path).
 */
#if defined(__cplusplus) && defined(__APPLE__)
#include "hostmath.h"
#else
#include "include/math.h"
#endif

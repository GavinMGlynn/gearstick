// gs_common.h - the vocabulary every part of the simulation shares.
//
// This header, and everything under src/core/, is written to link nothing:
// no SDL, no libm, no allocator of ours. See cmake/Layering.cmake.
#ifndef GS_COMMON_H
#define GS_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// C23 gives us `nullptr`; a toolchain that took -std=c23 without implementing
// it gets the shim. Which one you have is probed at configure time rather than
// inferred from a version number - see cmake/Platform.cmake.
#ifdef GS_NO_NULLPTR
#define nullptr ((void *)0)
#endif

#define GS_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define GS_MIN(a, b) ((a) < (b) ? (a) : (b))
#define GS_MAX(a, b) ((a) > (b) ? (a) : (b))
#define GS_CLAMP(v, lo, hi) GS_MIN(GS_MAX((v), (lo)), (hi))

#endif // GS_COMMON_H

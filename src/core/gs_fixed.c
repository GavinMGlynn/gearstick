// gs_fixed.c - the arithmetic the simulation is made of. See gs_fixed.h for
// why none of it is floating point.

#include "core/gs_fixed.h"
#include "core/gs_tables.h"

// Integer square root, digit by digit in base 4. Exact, branch-predictable and
// identical on every platform - which is the only property that matters here.
static uint64_t gs_isqrt64(uint64_t n) {
    uint64_t res = 0;
    uint64_t bit = (uint64_t)1 << 62;

    while (bit > n) bit >>= 2;

    while (bit != 0) {
        if (n >= res + bit) {
            n -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

gs_fix gs_fix_sqrt(gs_fix a) {
    if (a <= 0) return 0;
    // sqrt of a Q16.16 value is sqrt(a * 2^16), which lands back in Q16.16.
    return (gs_fix)gs_isqrt64((uint64_t)(uint32_t)a << GS_FIX_SHIFT);
}

gs_fix gs_fix_len2(gs_fix x, gs_fix y) {
    // Squared in Q32.32, where the root of the sum is already Q16.16 - so no
    // shifting, and no chance of the square overflowing before the root is
    // taken. Safe for any coordinate up to about 46341 tiles, which is three
    // orders of magnitude past the largest track.
    int64_t sum = (int64_t)x * (int64_t)x + (int64_t)y * (int64_t)y;
    return (gs_fix)gs_isqrt64((uint64_t)sum);
}

// Sine over the first quarter turn, `q` in [0, GS_QUARTER]. The table is
// coarser than the angle type, so the low bits interpolate: at 1024 steps per
// quarter the residual is under 1e-5 of full scale, well below anything the
// physics can notice and - more to the point - identical everywhere.
static gs_fix gs_quarter_sin(uint32_t q) {
    const uint32_t sub = 14u - GS_SINTAB_BITS;   // bits below one table step
    uint32_t idx = q >> sub;

    if (idx >= (uint32_t)(GS_SINTAB_LEN - 1)) return GS_ONE;

    int32_t a = gs_sintab[idx];
    int32_t b = gs_sintab[idx + 1];
    int32_t frac = (int32_t)(q & ((1u << sub) - 1u));

    return (gs_fix)(a + (int32_t)(((int64_t)(b - a) * frac) >> sub));
}

gs_fix gs_sin(gs_angle a) {
    uint32_t quad = (uint32_t)(a >> 14) & 3u;
    uint32_t q = (uint32_t)a & (uint32_t)(GS_QUARTER - 1);

    // Odd quadrants run the quarter backwards: sin(pi - x) == sin(x). When q is
    // zero that reflects to GS_QUARTER itself, which is why gs_quarter_sin
    // accepts the closed interval.
    if ((quad & 1u) != 0u) q = (uint32_t)GS_QUARTER - q;

    gs_fix s = gs_quarter_sin(q);
    return (quad >= 2u) ? -s : s;
}

gs_fix gs_cos(gs_angle a) {
    return gs_sin((gs_angle)(a + GS_QUARTER));
}

gs_angle gs_atan2(gs_fix y, gs_fix x) {
    if (x == 0 && y == 0) return 0;

    int32_t ix = x;
    int32_t iy = y;
    int32_t base = 0;

    // Fold into the right half-plane: atan2(-y, -x) + half a turn.
    if (ix < 0) {
        // INT32_MIN has no positive counterpart; nudging it costs nothing at
        // this magnitude and keeps the negation total.
        if (ix == INT32_MIN) ix = -INT32_MAX;
        if (iy == INT32_MIN) iy = -INT32_MAX;
        ix = -ix;
        iy = -iy;
        base = GS_TURN / 2;
    }

    // CORDIC's vector magnitude grows by about 1.647 over the whole loop, so
    // shed the top bits first. Only the ratio matters, so this is free.
    while (ix > (1 << 29) || iy > (1 << 29) || iy < -(1 << 29)) {
        ix >>= 1;
        iy >>= 1;
    }

    int32_t angle = 0;
    for (uint32_t i = 0; i < (uint32_t)GS_ATANTAB_LEN; i++) {
        int32_t dx = iy >> i;
        int32_t dy = ix >> i;
        if (iy > 0) {
            ix += dx;
            iy -= dy;
            angle += gs_atantab[i];
        } else {
            ix -= dx;
            iy += dy;
            angle -= gs_atantab[i];
        }
    }

    return (gs_angle)(uint16_t)(uint32_t)(base + angle);
}

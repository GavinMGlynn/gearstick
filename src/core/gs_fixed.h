// gs_fixed.h - Q16.16 fixed point, and the trigonometry the simulation runs on.
//
// **Why not float.** Determinism is the feature the rest of the design hangs
// off: input-log replays, ghosts that mean the same thing on someone else's
// machine, tracks identified by content hash so their times aggregate, and
// rollback netcode that resimulates without drifting. Floating point defeats
// all four quietly - x87 excess precision, FMA contraction, a different libm's
// last bit in sinf - and the symptom is not a crash but two cars in different
// places after ninety seconds. So the whole simulation is integers, and
// cmake/Layering.cmake refuses to configure if a float appears in src/core/.
//
// Q16.16 in an int32: 16 bits of whole units, 16 of fraction. One unit is one
// track tile, so a 64-tile track spans 64.0 and the largest coordinate the type
// can hold is 32767 tiles - three orders of magnitude of headroom. The fraction
// resolves 1/65536 of a tile, which at a 64 px tile is a 1024th of a pixel.
//
// Every multiply goes through int64 and comes back. That is the one rule; the
// rest of this file is consequences of it.
#ifndef GS_FIXED_H
#define GS_FIXED_H

#include "core/gs_common.h"

typedef int32_t gs_fix;

#define GS_FIX_SHIFT 16
#define GS_ONE       ((gs_fix)(1 << GS_FIX_SHIFT))
#define GS_HALF      (GS_ONE / 2)

// A whole number, and a ratio, as literals: GS_INT(3), GS_RATIO(1, 4).
#define GS_INT(n)         ((gs_fix)((n) * GS_ONE))
#define GS_RATIO(num, den) ((gs_fix)(((int64_t)(num) * GS_ONE) / (den)))

// Angles are a full turn in 65536 steps, held in a uint16_t so that wrapping is
// the type's job and not a `while (a > TAU)` loop that a compiler is free to
// reassociate. Adding two headings is plain integer addition; there is no
// modulus anywhere in the physics.
typedef uint16_t gs_angle;

#define GS_TURN      65536
#define GS_QUARTER   ((gs_angle)16384)
#define GS_DEG(d)    ((gs_angle)(((int32_t)(d) * GS_TURN / 360) & 0xffff))

static inline gs_fix gs_fix_mul(gs_fix a, gs_fix b) {
    return (gs_fix)(((int64_t)a * (int64_t)b) >> GS_FIX_SHIFT);
}

static inline gs_fix gs_fix_div(gs_fix a, gs_fix b) {
    if (b == 0) return a < 0 ? INT32_MIN : INT32_MAX;
    // Multiplied rather than shifted. Shifting a negative value left is not
    // something every standard version defines, the sanitizer says so, and a
    // compiler emits the identical instruction either way - so there is nothing
    // to buy by writing the version that needs a footnote.
    return (gs_fix)(((int64_t)a * (int64_t)GS_ONE) / (int64_t)b);
}

static inline gs_fix gs_fix_abs(gs_fix a) { return a < 0 ? -a : a; }

static inline int32_t gs_fix_floor(gs_fix a) { return a >> GS_FIX_SHIFT; }

static inline gs_fix gs_fix_frac(gs_fix a) { return a & (GS_ONE - 1); }

// Linear interpolation, t in [0, GS_ONE]. Used for the bilinear ground-height
// sample, so it is on the hot path of every grounded tick.
static inline gs_fix gs_lerp(gs_fix a, gs_fix b, gs_fix t) {
    return a + gs_fix_mul(b - a, t);
}

// Move `v` towards zero by `d`, without overshooting into the other sign. This
// is what friction and braking are: a subtraction that stops at rest rather
// than one that reverses the car.
static inline gs_fix gs_toward_zero(gs_fix v, gs_fix d) {
    if (v > d) return v - d;
    if (v < -d) return v + d;
    return 0;
}

gs_fix gs_fix_sqrt(gs_fix a);

// Length of a 2D vector, computed in Q32.32 so that a component near the track
// edge cannot overflow the square before the root is taken.
gs_fix gs_fix_len2(gs_fix x, gs_fix y);

gs_fix gs_sin(gs_angle a);
gs_fix gs_cos(gs_angle a);

// The angle whose tangent is y/x, in the same 65536-step units - the inverse of
// the pair above, and the only way heading is ever recovered from a velocity.
gs_angle gs_atan2(gs_fix y, gs_fix x);

// The signed shortest way round from `from` to `to`, in [-32768, 32767]. Steering
// and the AI both need "which way, and how far" rather than a bare difference.
static inline int32_t gs_angle_delta(gs_angle from, gs_angle to) {
    return (int16_t)((uint16_t)(to - from));
}

#endif // GS_FIXED_H

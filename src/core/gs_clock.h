// gs_clock.h - the bridge between a machine's frame rate and the simulation's.
//
// **The simulation runs at a fixed 120 Hz and frames do not.** This is the
// piece that keeps those two facts from contaminating each other: it takes
// however long the last frame happened to take and answers one question - how
// many fixed steps are owed - leaving whatever is left over as the fraction the
// renderer interpolates across.
//
// It lives in `src/core/` rather than beside the window, for one reason: it is
// the only part of the frame loop whose correctness is a *simulation* property.
// If this is wrong, the same input log produces different races on a fast
// machine and a slow one, and every replay and every ghost is worth nothing.
// That is a fact worth a test, and a test needs it somewhere a test can reach -
// which is why it takes nanoseconds as an argument rather than asking SDL for
// the time itself.
#ifndef GS_CLOCK_H
#define GS_CLOCK_H

#include "core/gs_fixed.h"
#include "core/gs_sim.h"

// One tick, in nanoseconds. The division is not exact - 120 Hz is 8333333.33 ns
// and this truncates - so the clock runs about 40 ns fast per second, which is
// four microseconds an hour and below anything that could matter. It is *not*
// a source of divergence: every machine truncates identically, because this is
// integer arithmetic and not a rounded float.
#define GS_STEP_NS (1000000000ULL / GS_TICK_HZ)

// The most catching-up one frame may ask for, in nanoseconds.
//
// A machine that stalls - a breakpoint, a window drag, a laptop lid closing -
// comes back with an enormous delta, and without a cap the next frame runs
// thousands of ticks and the game briefly goes mad. Capping makes it briefly go
// slow instead, which is the better of the two failures by a distance.
#define GS_CLOCK_MAX_CATCHUP_NS 250000000ULL

typedef struct gs_clock {
    uint64_t accum_ns;     // time owed to the simulation, always < GS_STEP_NS after advance
    uint64_t steps_run;    // total steps this clock has asked for, for the record
} gs_clock;

void gs_clock_init(gs_clock *c);

// Hand it the length of the frame that just passed; it returns how many fixed
// steps to run before drawing the next one. The caller runs exactly that many.
uint32_t gs_clock_advance(gs_clock *c, uint64_t delta_ns);

// How far between the last completed step and the next one, as Q16.16 in
// [0, GS_ONE). This is what the renderer interpolates across, and it is why
// motion is smooth at frame rates that have nothing to do with 120.
gs_fix gs_clock_alpha(const gs_clock *c);

#endif // GS_CLOCK_H

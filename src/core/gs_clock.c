// gs_clock.c - see gs_clock.h.

#include "core/gs_clock.h"

void gs_clock_init(gs_clock *c) {
    c->accum_ns = 0;
    c->steps_run = 0;
}

uint32_t gs_clock_advance(gs_clock *c, uint64_t delta_ns) {
    if (delta_ns > GS_CLOCK_MAX_CATCHUP_NS) delta_ns = GS_CLOCK_MAX_CATCHUP_NS;

    c->accum_ns += delta_ns;

    uint32_t steps = (uint32_t)(c->accum_ns / GS_STEP_NS);
    c->accum_ns -= (uint64_t)steps * GS_STEP_NS;
    c->steps_run += steps;

    return steps;
}

gs_fix gs_clock_alpha(const gs_clock *c) {
    // accum_ns is below one step by construction, so this cannot reach GS_ONE
    // and the renderer never interpolates past the state it has.
    return (gs_fix)((c->accum_ns << GS_FIX_SHIFT) / GS_STEP_NS);
}

// gs_ai.h - somebody to race against.
//
// **It plans, it does not follow.** There is no baked line and no recorded lap:
// every tick it looks at where the next gate is, works out how fast it could go
// round the corner that implies given the grip it actually has *here and now* -
// this surface, this gravity, these tyres, this much wear - and drives
// accordingly.
//
// That matters because gravity is a race parameter and a paintable one. A
// speed profile computed when the track was authored would be wrong the moment
// somebody moved the dial, and wrong in the most dangerous possible way: it
// would look right.
//
// Integer throughout and a pure function of the world, so an AI car is
// deterministic exactly as a human one is, and a replay of a race against it
// re-races to the bit.
#ifndef GS_AI_H
#define GS_AI_H

#include "core/gs_sim.h"
#include "core/gs_track.h"

// What this car would press, this tick.
gs_input gs_ai_drive(const gs_world *w, const gs_track *t, uint8_t car);

#endif // GS_AI_H

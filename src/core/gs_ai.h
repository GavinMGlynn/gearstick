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

// How near the limit a driver is willing to run, as a fraction of the speed the
// grip would actually bear. Under one for everybody: the estimate is a chord
// approximation, and a driver exactly at the limit is a driver about to be over
// it.
//
// This is the whole of "difficulty". Not rubber-banding, not extra power, not
// cheating on grip - just how much of the available road-holding the opponent
// is prepared to use, which is the same thing that separates two human drivers.
#define GS_AI_CAUTIOUS GS_RATIO(62, 100)
#define GS_AI_NORMAL   GS_RATIO(82, 100)
#define GS_AI_QUICK    GS_RATIO(96, 100)

// What this car would press, this tick, driving at the default pace.
gs_input gs_ai_drive(const gs_world *w, const gs_track *t, uint8_t car);

// The same, at a stated pace.
gs_input gs_ai_drive_at(const gs_world *w, const gs_track *t, uint8_t car,
                        gs_fix margin);

#endif // GS_AI_H

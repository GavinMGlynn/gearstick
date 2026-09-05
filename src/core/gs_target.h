#ifndef GS_TARGET_H
#define GS_TARGET_H

#include "core/gs_sim.h"
#include "core/gs_track.h"

// **A time to beat on every track**, computed rather than authored. The
// computer drives the track once with every machine, headless, at the race's
// own gravity and skill, and the best lap it manages is the target - a
// number the game works out in a few milliseconds rather than one somebody
// has to type in, and the same number every time it is asked, because the
// simulation and the driver are deterministic. A target, never a medal:
// nothing unlocks and nothing is withheld; it is only the difference between
// a solo lap with a point and one without.
//
// Returns the lap in ticks, or 0 when no machine got round within the
// analyser's budget. `vehicle`, if given, is set to the machine that set it.
uint32_t gs_target_lap(const gs_track *t, gs_fix gravity, int skill,
                       uint8_t *vehicle);

#endif // GS_TARGET_H

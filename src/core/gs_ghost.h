// gs_ghost.h - a recorded run, re-raced alongside a live one.
//
// A ghost is not an animation and holds no positions. It is a replay - the
// conditions, the vehicles and one byte per car per tick - stepped through the
// same simulation as the race it appears in, one tick for one tick. Everything
// that makes it possible to store a race as its inputs makes it possible to
// carry a ghost around: half a kilobyte a second, and the same car on every
// machine that runs it.
//
// That last part is the whole point of a ghost and the reason this is worth
// building on the replay rather than on a track of recorded coordinates. A
// coordinate track is a video of somebody else's race. This is somebody else's
// race, happening again.
#ifndef GS_GHOST_H
#define GS_GHOST_H

#include "core/gs_replay.h"

// Around 300 KB, nearly all of it the replay. A static or a heap object, never
// a local - the same rule the replay itself carries.
// **When the ghost reached each gate.** One tick per crossing, indexed by
// lap and gate - see gs_split_index - taken from one headless playback of
// the recording when the ghost is armed, so that the moment a live car
// crosses the same gate the answer to "am I ahead" is a subtraction. Twenty
// laps of the most gates a track can have; zero means the ghost never got
// there, and a tick is stored one high so that zero can mean that.
#define GS_GHOST_SPLITS (GS_TRACK_MAX_GATES * 20)

typedef struct gs_ghost {
    gs_replay replay;

    // The ghost's own world, stepped in lockstep with the live one. `prev` is
    // last tick, so the renderer can interpolate a ghost as smoothly as it
    // interpolates a car anybody is driving.
    gs_world  world;
    gs_world  prev;
    uint32_t  split[GS_GHOST_SPLITS];

    bool     loaded;      // there is a replay in here
    bool     ready;       // and it matches the track we are racing on
    bool     finished;    // it has run out of recorded input
    uint8_t  car;         // which car of the recording to follow
    uint32_t tick;
} gs_ghost;

// Take a recording as the ghost. `t` is the track it will be raced against:
// false, and `ready` stays clear, when the recording was made on another one.
// The bytes are still kept, so loading the right track afterwards works.
bool gs_ghost_load(gs_ghost *g, const gs_track *t, const uint8_t *buf, size_t len);

// Take a recording already in memory - the run that just finished, most often,
// which is how a ghost appears without anything being written to disk.
bool gs_ghost_take(gs_ghost *g, const gs_replay *r, const gs_track *t);

// Empty it. A ghost that is not there draws nothing rather than drawing a car
// parked at the start line.
void gs_ghost_clear(gs_ghost *g);

// Back to the start line, ready to run again beside a restarted race.
void gs_ghost_reset(gs_ghost *g, const gs_track *t);

// One tick, or nothing once the recording has run out. A ghost that has
// finished stays where it finished rather than vanishing: the last thing you
// want to know is where it beat you.
void gs_ghost_step(gs_ghost *g, const gs_track *t);

// The ghost's car, or null when there is nothing to draw. `prev` is the same
// car as it was last tick, for interpolation.
const gs_car *gs_ghost_car(const gs_ghost *g);
const gs_car *gs_ghost_prev_car(const gs_ghost *g);

// How many ticks of recording there are, and how far in we are.
uint32_t gs_ghost_length(const gs_ghost *g);

// The index of the crossing a car has just made: its lap times the gate
// count plus the gate it crossed. The same number for the ghost and for the
// live car, which is what makes them comparable. -1 if it has crossed none.
int32_t gs_split_index(const gs_track *t, const gs_car *c);

// The tick at which the ghost made crossing `index`, if it did.
bool gs_ghost_split(const gs_ghost *g, int32_t index, uint32_t *tick);

#endif // GS_GHOST_H

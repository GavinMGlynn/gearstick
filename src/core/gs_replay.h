// gs_replay.h - a race as the inputs that produced it.
//
// A replay stores no positions. It stores the conditions, the vehicles and one
// byte per car per tick, and re-races them - which is only possible because the
// simulation is deterministic, and is the reason it is. At 120 Hz and four
// cars that is under half a kilobyte a second, so a whole race fits in a URL's
// worth of compressed bytes and a ghost costs nothing to carry around.
//
// The track is referenced by content hash rather than by name. A replay against
// a track you have is playable; against one you do not, it says so instead of
// quietly re-racing a different track under the same name.
#ifndef GS_REPLAY_H
#define GS_REPLAY_H

#include "core/gs_sim.h"

#define GS_REPLAY_MAGIC     0x50525347u   // "GSRP"
#define GS_REPLAY_VERSION   2u
#define GS_REPLAY_MAX_TICKS (GS_TICK_HZ * 60 * 10)   // ten minutes

typedef struct gs_replay_meta {
    uint64_t track_hash;
    gs_fix   gravity;
    gs_fix   drag_scale;
    gs_fix   friction_scale;
    gs_fix   damage_scale;
    uint8_t  car_count;
    uint8_t  vehicle[GS_MAX_CARS];

    // **The grid, so a recording is self-contained.** Version 1 stored the
    // conditions and the inputs and left the starting positions to the caller,
    // which is fine while the only thing replaying a race is the program that
    // recorded it and useless the moment somebody sends you a ghost: the same
    // inputs from a different square are a different race. A replay that does
    // not know where it started is not a replay of anything.
    gs_fix   start_x[GS_MAX_CARS];
    gs_fix   start_y[GS_MAX_CARS];
    gs_angle start_heading[GS_MAX_CARS];

    uint32_t tick_count;
} gs_replay_meta;

// Fixed capacity and no allocation, because src/core/ owns no allocator. Nearly
// 300 KB, so this is a static or a heap object, never a local.
typedef struct gs_replay {
    gs_replay_meta meta;
    gs_input       input[GS_REPLAY_MAX_TICKS][GS_MAX_CARS];
} gs_replay;

// Start recording the conditions `w` is currently under, against `t`.
void gs_replay_begin(gs_replay *r, const gs_world *w, const gs_track *t);

// Append one tick of input. Returns false when full, which is the only failure.
bool gs_replay_record(gs_replay *r, const gs_input *in);

// The input for a tick, or all-zero past the end.
const gs_input *gs_replay_at(const gs_replay *r, uint32_t tick);

// Set `w` up as the replay describes - the conditions, the vehicles and the
// grid they started from - ready to be stepped against a track whose hash
// matches. Returns false if it does not.
bool gs_replay_restore(const gs_replay *r, gs_world *w, const gs_track *t);

// Re-race the whole thing and hand back the final world. Same track, same
// bytes, same answer, on any machine gearstick builds on. The world is set up
// from the recording, so this needs nothing from the caller but somewhere to
// put the answer.
bool gs_replay_playback(const gs_replay *r, const gs_track *t, gs_world *out);

// On disk, little-endian and explicit, so a replay recorded on one machine
// plays on another regardless of what the compiler chose to pad.
size_t gs_replay_size(const gs_replay *r);
size_t gs_replay_serialize(const gs_replay *r, uint8_t *buf, size_t cap);
bool   gs_replay_deserialize(gs_replay *r, const uint8_t *buf, size_t len);

#endif // GS_REPLAY_H

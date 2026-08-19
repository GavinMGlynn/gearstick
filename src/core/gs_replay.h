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

// As long as a driver's name can be. The same sixteen the records table uses,
// stated here rather than included from there because a replay is core and
// knows nothing about tables.
#define GS_REPLAY_NAME      16
// **Four, because a recording now says who was driving.**
//
// Version three is still read, with the names blank. See docs/THREATS.md: a
// replay that does not name its driver is a bearer token - anyone who obtains
// one can hand it in as their own, and the verifier will correctly agree that
// the time was driven, because it was. It was just not driven by them.
// **Five, because a recording now carries the ending everybody agreed on.**
//
// Versions four and three are still read, with the agreed hash zero - which
// means "this recording does not say" and not "this recording agrees with
// anything", so a claim is checked against it only when it is there.
#define GS_REPLAY_VERSION   5u
#define GS_REPLAY_OLDEST    3u
#define GS_REPLAY_MAX_TICKS (GS_TICK_HZ * 60 * 10)   // ten minutes

typedef struct gs_replay_meta {
    uint64_t track_hash;
    gs_fix   gravity;
    gs_fix   drag_scale;
    gs_fix   friction_scale;
    gs_fix   damage_scale;

    // What race this was. A recording that did not carry these would re-race
    // the same driving under different rules and finish somewhere else.
    uint8_t  mode;
    uint16_t laps_to_win;

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

    // Who was in each car. Blank for a car nobody claimed, and blank throughout
    // a recording made before version four - which means "not recorded" and not
    // "nobody", and is why a claim against a blank name is refused rather than
    // waved through.
    char     driver[GS_MAX_CARS][GS_REPLAY_NAME];

    // **The state every peer agreed the race ended in.** Re-racing the log has
    // to arrive here, which is a much stronger statement than "the claimed lap
    // is not better than the driven one": it says this log is the race that
    // actually happened, all of it, for everybody in it. One flipped bit
    // anywhere in the inputs lands somewhere else and says so.
    //
    // Zero means the recording does not carry one - every version four and
    // three recording, and every race run on one machine, where there is nobody
    // to have agreed with.
    uint64_t agreed_hash;
} gs_replay_meta;

// Fixed capacity and no allocation, because src/core/ owns no allocator. Nearly
// 300 KB, so this is a static or a heap object, never a local.
typedef struct gs_replay {
    gs_replay_meta meta;
    gs_input       input[GS_REPLAY_MAX_TICKS][GS_MAX_CARS];
} gs_replay;

// Start recording the conditions `w` is currently under, against `t`.
void gs_replay_begin(gs_replay *r, const gs_world *w, const gs_track *t);

// Say who is in a car. Called after gs_replay_begin and before anybody submits
// the result; a name longer than fits is truncated rather than refused, because
// the alternative is a recording that silently has no driver.
void gs_replay_set_driver(gs_replay *r, uint8_t car, const char *name);

// Who a recording says was in a car. Never null; an empty string means the
// recording does not say, which every version three recording does.
const char *gs_replay_driver(const gs_replay *r, uint8_t car);

// The state everybody agreed the race ended in, to be checked against later.
// Only a networked race has one; a race on one machine has nobody to agree with
// and leaves it zero.
void gs_replay_set_agreed(gs_replay *r, uint64_t hash);

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

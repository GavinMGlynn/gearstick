// fuzz_replay.c - the replay deserialiser, and the re-race behind it.
//
// This is the parser with the most attacker-controlled bytes going through it:
// a proof is hundreds of kilobytes arriving in chunks, and the server takes one
// from anybody who claims a time. It is also the one whose output is
// immediately *executed* - the inputs it holds are driven through the physics -
// so parsing it and then re-racing it is the honest shape for this target.
#include "core/gs_records.h"
#include "core/gs_replay.h"
#include "core/gs_track.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

// A real track to re-race against, built once. Deserialising a replay says
// nothing until something plays it, and playing it needs somewhere to drive.
static const gs_track *gs_fuzz_track(void) {
    static gs_track t;
    static bool built = false;
    if (built) return &t;

    gs_track_init(&t, 40, 16, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= t.h; y++) {
        for (uint8_t x = 0; x <= t.w; x++) {
            gs_track_set_corner(&t, x, y, x > 18 && x < 24 ? GS_INT(1) : 0);
        }
    }
    gs_track_add_gate(&t, GS_INT(6), GS_INT(8), 0, GS_INT(6));
    gs_track_add_gate(&t, GS_INT(30), GS_INT(8), 0, GS_INT(6));
    built = true;
    return &t;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static gs_replay r;
    if (!gs_replay_deserialize(&r, data, size)) return 0;

    // **A replay that parses is a replay that gets driven.** Every field the
    // parser accepted - the car count, the vehicle ids, the grid, the tick
    // count - is an index or a loop bound in here, so stopping at "it parsed"
    // would leave the interesting half untested.
    //
    // Long recordings are re-raced only in part: the parser has already been
    // exercised by then, and ten minutes of physics per input is a fuzzer that
    // explores nothing. The cap is on work, not on what is accepted.
    if (r.meta.tick_count > 4000) r.meta.tick_count = 4000;

    static gs_world w;
    if (gs_replay_playback(&r, gs_fuzz_track(), &w)) {
        (void)gs_world_hash(&w);
        (void)gs_conditions_hash(&w);
    }

    for (uint8_t c = 0; c < GS_MAX_CARS; c++) (void)gs_replay_driver(&r, c);
    (void)gs_replay_at(&r, r.meta.tick_count);
    (void)gs_replay_at(&r, 0);
    return 0;
}

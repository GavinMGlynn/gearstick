// fuzz_track.c - the track deserialiser, which reads bytes that came off a
// socket from somebody who may be hostile.
//
// A track arrives in pieces over UDP and is rebuilt before anything looks at
// it, so this parser runs on attacker-controlled bytes on the server and on
// every client in the race. It is also the one with the most structure in it -
// dimensions, per-corner heights, a gate list - which is to say the most
// arithmetic, which is to say the most places to be wrong.
#include "core/gs_track.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static gs_track t;

    if (!gs_track_deserialize(&t, data, size)) return 0;

    // **Deserialising is not the end of it.** A track that parses is then
    // sampled, hashed and driven on, and a parser that lets an out-of-range
    // dimension through does its damage in the code that trusts the result. So
    // the accepted track is used the way the game uses it.
    (void)gs_track_hash(&t);

    for (uint8_t y = 0; y <= t.h && y < GS_TRACK_MAX; y++) {
        for (uint8_t x = 0; x <= t.w && x < GS_TRACK_MAX; x++) {
            (void)gs_track_corner_at(&t, x, y);
        }
    }

    // Off the edges as well as inside them, because a track that parses is
    // still driven off by somebody.
    for (int i = -2; i < 4; i++) {
        (void)gs_track_height(&t, GS_INT(i), GS_INT(i));
        (void)gs_track_surface(&t, GS_INT(i), GS_INT(i));
    }
    return 0;
}

// gs_verify.h - is that time real?
//
// **The strongest thing this project's determinism buys.** A claimed time
// arrives with the inputs that produced it, and the server re-races them
// through the same simulation the player used. If the replay does not produce
// the claim, the claim is thrown away.
//
// That reduces cheating to "produce an input log that genuinely drives that
// fast", which is not cheating - it is being good at the game. It is possible
// only because a race is exactly reproducible from its inputs, which is the
// property everything in `src/core/` is arranged around, and it is the best
// argument for having built it that way.
//
// No sockets and no SDL: this takes bytes and a track and answers a question,
// so it can be tested without a network and run anywhere.
#ifndef GS_VERIFY_H
#define GS_VERIFY_H

#include "core/gs_replay.h"
#include "core/gs_track.h"

typedef enum gs_verdict {
    GS_VERDICT_OK = 0,        // the replay produces the claim
    GS_VERDICT_NOT_A_REPLAY,  // the bytes are not a recording
    GS_VERDICT_WRONG_TRACK,   // recorded somewhere else
    GS_VERDICT_WRONG_RULES,   // a different race: mode, laps, or the dials
    GS_VERDICT_NO_SUCH_CAR,   // the claim is about a car that was not in it
    GS_VERDICT_UNFINISHED,    // that car never finished
    GS_VERDICT_LAP_TOO_GOOD,  // the claimed lap is not the one that was driven
    GS_VERDICT_RACE_TOO_GOOD,

    // **The claim is about somebody else's driving.** The recording says who
    // was in that car and it is not who is handing it in - which is the whole
    // reason a recording says so, because an honest replay that named nobody
    // was a bearer token anyone who obtained one could spend.
    GS_VERDICT_WRONG_DRIVER,

    GS_VERDICT_COUNT
} gs_verdict;

const char *gs_verdict_text(gs_verdict v);

// What a client says it did.
typedef struct gs_claim {
    uint64_t track;
    uint64_t conditions;
    uint16_t laps;
    uint8_t  car;             // which car of the recording is being claimed
    uint32_t lap_ticks;
    uint32_t race_ticks;

    // Who is claiming it. Checked against the name the recording carries for
    // that car; an empty one here means the caller is not asserting an identity
    // and the check is skipped, which is what a local ghost or an offline
    // analysis wants.
    //
    // **This is only as good as knowing who sent it**, which today is nothing:
    // the name on a datagram is whatever the sender typed. It stops a replay
    // somebody found being spent as their own, and it becomes a real defence
    // when there are accounts. See docs/THREATS.md.
    char     who[GS_REPLAY_NAME];
} gs_claim;

// Re-race `replay` against `t` and decide. `out` may be null; when given it is
// the world the recording actually produced, which is what the server should
// believe rather than what it was told.
gs_verdict gs_verify(const gs_replay *r, const gs_track *t, const gs_claim *c,
                     gs_world *out);

// The same, from the bytes a client sent.
gs_verdict gs_verify_bytes(const uint8_t *bytes, size_t len, const gs_track *t,
                           const gs_claim *c, gs_world *out);

#endif // GS_VERIFY_H

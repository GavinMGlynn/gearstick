#include "net/gs_verify.h"

#include "core/gs_records.h"

#include <string.h>

static const char *const gs_verdict_names[GS_VERDICT_COUNT] = {
    [GS_VERDICT_OK]            = "verified",
    [GS_VERDICT_NOT_A_REPLAY]  = "not a recording",
    [GS_VERDICT_WRONG_TRACK]   = "recorded on a different track",
    [GS_VERDICT_WRONG_RULES]   = "a different race - mode, laps or dials",
    [GS_VERDICT_NO_SUCH_CAR]   = "no such car in that race",
    [GS_VERDICT_UNFINISHED]    = "that car never finished",
    [GS_VERDICT_LAP_TOO_GOOD]  = "the lap claimed is not the lap driven",
    [GS_VERDICT_RACE_TOO_GOOD] = "the time claimed is not the time driven",
    [GS_VERDICT_WRONG_DRIVER]  = "somebody else drove that",
    [GS_VERDICT_NOT_THAT_RACE]  = "those inputs do not produce that race",
};

const char *gs_verdict_text(gs_verdict v) {
    return v < GS_VERDICT_COUNT ? gs_verdict_names[v] : "?";
}

gs_verdict gs_verify(const gs_replay *r, const gs_track *t, const gs_claim *c,
                     gs_world *out) {
    // The recording has to be of this track. It says so itself, and the track
    // says what it is, so this is a comparison rather than a matter of trust.
    if (r->meta.track_hash != gs_track_hash(t)) return GS_VERDICT_WRONG_TRACK;
    if (c->car >= r->meta.car_count) return GS_VERDICT_NO_SUCH_CAR;

    // And of this race. A time set over two laps is not a three-lap record, and
    // a time set at a sixth of gravity is not a time - both are in the
    // recording, so neither has to be taken on anybody's word.
    if (r->meta.laps_to_win != c->laps) return GS_VERDICT_WRONG_RULES;

    // **And driven by whoever is claiming it.**
    //
    // A recording that named nobody was a bearer token: it proves a time was
    // driven, which is exactly what it should prove, and says nothing about
    // *whose* it is - so anyone who obtained one could hand it in and be
    // correctly told it was genuine.
    //
    // An empty name in the claim means the caller is not asserting an identity
    // at all, which is what a local ghost or an offline analysis wants, and the
    // check is skipped. An empty name in the *recording* is a version three
    // replay that does not know, and a claim of identity against it is refused
    // rather than waved through - "it does not say" is not "it says you".
    if (c->who[0] != '\0') {
        const char *drove = gs_replay_driver(r, c->car);

        // Written out rather than left to the comparison below, which would
        // catch it anyway - an empty name matches nothing. It is here because
        // this is the case the whole feature turns on, and a future change to
        // how names are compared should have to walk past it.
        if (drove[0] == '\0') return GS_VERDICT_WRONG_DRIVER;

        if (strncmp(drove, c->who, GS_REPLAY_NAME) != 0) return GS_VERDICT_WRONG_DRIVER;
    }

    gs_world w;
    if (!gs_replay_playback(r, t, &w)) return GS_VERDICT_NOT_A_REPLAY;

    // **The conditions the race was actually run under.** Hashed the same way
    // the client hashes them, so a claim about Earth gravity cannot be paid for
    // with a lap driven on the Moon.
    uint64_t was = gs_conditions_hash(&w);
    if (was != c->conditions) return GS_VERDICT_WRONG_RULES;

    // **The whole race, when the recording says what the whole race was.**
    //
    // Everything above this line is about one car and one lap, and a log
    // altered anywhere else passes all of it. A networked recording carries the
    // state every peer agreed the race ended in, and re-racing the log has to
    // arrive there - which is a statement about every tick and every car, and
    // is nearly free because the simulation is exactly reproducible. That is
    // the argument for having built it that way, collected.
    //
    // A recording with no agreed ending - one machine, or a version before
    // five - is not failed for it. "It does not say" is not "it disagrees".
    if (r->meta.agreed_hash != 0 && gs_world_hash(&w) != r->meta.agreed_hash) {
        return GS_VERDICT_NOT_THAT_RACE;
    }

    const gs_car *car = &w.car[c->car];
    if (out != nullptr) *out = w;

    if (c->race_ticks > 0) {
        if (car->finish_tick == 0) return GS_VERDICT_UNFINISHED;

        // **Not equality.** A claim is rejected for being *better* than what
        // the inputs produced; a slower claim is somebody's own honest mistake
        // and costs them nothing but the record they did not take.
        if (c->race_ticks < car->finish_tick) return GS_VERDICT_RACE_TOO_GOOD;
    }

    if (c->lap_ticks > 0) {
        if (car->best_lap == 0) return GS_VERDICT_UNFINISHED;
        if (c->lap_ticks < car->best_lap) return GS_VERDICT_LAP_TOO_GOOD;
    }

    return GS_VERDICT_OK;
}

gs_verdict gs_verify_bytes(const uint8_t *bytes, size_t len, const gs_track *t,
                           const gs_claim *c, gs_world *out) {
    // Nearly 300 KB, so static rather than a local - the same rule the replay
    // itself carries.
    static gs_replay r;
    if (!gs_replay_deserialize(&r, bytes, len)) return GS_VERDICT_NOT_A_REPLAY;
    return gs_verify(&r, t, c, out);
}

#include "net/gs_verify.h"

#include "core/gs_records.h"

static const char *const gs_verdict_names[GS_VERDICT_COUNT] = {
    [GS_VERDICT_OK]            = "verified",
    [GS_VERDICT_NOT_A_REPLAY]  = "not a recording",
    [GS_VERDICT_WRONG_TRACK]   = "recorded on a different track",
    [GS_VERDICT_WRONG_RULES]   = "a different race - mode, laps or dials",
    [GS_VERDICT_NO_SUCH_CAR]   = "no such car in that race",
    [GS_VERDICT_UNFINISHED]    = "that car never finished",
    [GS_VERDICT_LAP_TOO_GOOD]  = "the lap claimed is not the lap driven",
    [GS_VERDICT_RACE_TOO_GOOD] = "the time claimed is not the time driven",
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

    gs_world w;
    if (!gs_replay_playback(r, t, &w)) return GS_VERDICT_NOT_A_REPLAY;

    // **The conditions the race was actually run under.** Hashed the same way
    // the client hashes them, so a claim about Earth gravity cannot be paid for
    // with a lap driven on the Moon.
    uint64_t was = gs_conditions_hash(&w);
    if (was != c->conditions) return GS_VERDICT_WRONG_RULES;

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

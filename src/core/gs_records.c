#include "core/gs_records.h"

#include <string.h>

// What one row takes on disk, per version. Version two appended eight bytes for
// the date; everything before it is byte for byte where it was, which is what
// lets one reader handle both.
#define GS_RECORD_BYTES_V1 (8 + 8 + 4 + 4 + 2 + 1 + 1 + GS_NAME_MAX)
#define GS_RECORD_BYTES    (GS_RECORD_BYTES_V1 + 8)
#define GS_RECORDS_HEADER (4 + 4 + 4)

void gs_records_clear(gs_records *r) {
    memset(r, 0, sizeof *r);
}

uint64_t gs_conditions_hash(const gs_world *w) {
    // FNV-1a over the dials, the same way a track hashes itself. Not the mode
    // or the lap count: those are stored beside a record rather than folded in,
    // because a results screen wants to say "the lap record here is X" without
    // caring how long the race that set it was.
    uint64_t h = 1469598103934665603ULL;
    const uint32_t v[4] = {
        (uint32_t)w->gravity, (uint32_t)w->drag_scale,
        (uint32_t)w->friction_scale, (uint32_t)w->damage_scale,
    };
    for (int i = 0; i < 4; i++) {
        for (int b = 0; b < 4; b++) {
            h ^= (uint64_t)((v[i] >> (8 * b)) & 0xffu);
            h *= 1099511628211ULL;
        }
    }
    return h;
}

static bool gs_same_place(const gs_record *e, uint64_t track, uint64_t conditions) {
    return e->track == track && e->conditions == conditions;
}

const gs_record *gs_records_best_lap(const gs_records *r, uint64_t track,
                                     uint64_t conditions) {
    const gs_record *best = nullptr;
    for (uint16_t i = 0; i < r->count; i++) {
        const gs_record *e = &r->entry[i];
        if (!gs_same_place(e, track, conditions) || e->lap == 0) continue;
        if (best == nullptr || e->lap < best->lap) best = e;
    }
    return best;
}

const gs_record *gs_records_best_race(const gs_records *r, uint64_t track,
                                      uint64_t conditions, uint16_t laps) {
    const gs_record *best = nullptr;
    for (uint16_t i = 0; i < r->count; i++) {
        const gs_record *e = &r->entry[i];
        if (!gs_same_place(e, track, conditions) || e->race == 0) continue;
        // A race time only means anything against a race of the same length.
        if (e->laps != laps) continue;
        if (best == nullptr || e->race < best->race) best = e;
    }
    return best;
}

uint16_t gs_records_for(const gs_records *r, uint64_t track, uint64_t conditions,
                        const gs_record **out, uint16_t cap) {
    uint16_t n = 0;
    for (uint16_t i = 0; i < r->count && n < cap; i++) {
        const gs_record *e = &r->entry[i];
        if (gs_same_place(e, track, conditions)) out[n++] = e;
    }

    // Quickest lap first. A short insertion sort, because `cap` is a screenful
    // and this runs when a race ends rather than in a frame.
    for (uint16_t i = 1; i < n; i++) {
        const gs_record *key = out[i];
        uint16_t j = i;
        while (j > 0 && out[j - 1]->lap > key->lap) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = key;
    }
    return n;
}

// One row per driver per track per conditions **per distance**. A table that
// grew a row per race would be unreadable after an evening, and the interesting
// question is somebody's best rather than their fourth attempt - but the
// distance has to be part of the key, because a three-lap time and a five-lap
// time are not two attempts at the same thing. Without it, driving a longer
// race quietly deleted the shorter record, which is exactly what happened.
static gs_record *gs_find_or_add(gs_records *r, uint64_t track, uint64_t conditions,
                                 uint16_t laps, const char *who) {
    for (uint16_t i = 0; i < r->count; i++) {
        gs_record *e = &r->entry[i];
        if (gs_same_place(e, track, conditions) && e->laps == laps &&
            strncmp(e->who, who, GS_NAME_MAX) == 0) {
            return e;
        }
    }

    if (r->count < GS_RECORDS_MAX) {
        gs_record *e = &r->entry[r->count++];
        memset(e, 0, sizeof *e);
        e->track = track;
        e->conditions = conditions;
        e->laps = laps;
        strncpy(e->who, who, GS_NAME_MAX - 1);
        return e;
    }

    // Full. The slowest lap anywhere makes way, so a full table is still the
    // best times rather than the oldest ones.
    gs_record *worst = &r->entry[0];
    for (uint16_t i = 1; i < r->count; i++) {
        if (r->entry[i].lap > worst->lap) worst = &r->entry[i];
    }
    memset(worst, 0, sizeof *worst);
    worst->track = track;
    worst->conditions = conditions;
    worst->laps = laps;
    strncpy(worst->who, who, GS_NAME_MAX - 1);
    return worst;
}

gs_record_beat gs_records_submit(gs_records *r, uint64_t track, uint64_t conditions,
                                 uint8_t vehicle, uint8_t mode, uint16_t laps,
                                 uint32_t lap_ticks, uint32_t race_ticks,
                                 const char *who, uint64_t when) {
    gs_record_beat beat = { false, false };
    if (who == nullptr) who = "";

    // What the table said before this result, so "you beat the record" means
    // the record as it stood rather than as it stands after being updated.
    const gs_record *was_lap = gs_records_best_lap(r, track, conditions);
    const gs_record *was_race = gs_records_best_race(r, track, conditions, laps);

    if (lap_ticks > 0 && (was_lap == nullptr || lap_ticks < was_lap->lap)) {
        beat.lap = true;
    }
    if (race_ticks > 0 && (was_race == nullptr || race_ticks < was_race->race)) {
        beat.race = true;
    }

    gs_record *e = gs_find_or_add(r, track, conditions, laps, who);
    e->vehicle = vehicle;
    e->mode = mode;

    // Dated only when something was actually beaten, so the date belongs to the
    // time in the row rather than to the last occasion somebody drove past.
    bool improved = false;
    if (lap_ticks > 0 && (e->lap == 0 || lap_ticks < e->lap)) {
        e->lap = lap_ticks;
        improved = true;
    }
    if (race_ticks > 0 && (e->race == 0 || race_ticks < e->race)) {
        e->race = race_ticks;
        improved = true;
    }
    if (improved) e->when = when;
    return beat;
}

// --- the wire format -------------------------------------------------------

static void gs_put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xffu);
}

static uint32_t gs_get32(const uint8_t *p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}

static void gs_put64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xffu);
}

static uint64_t gs_get64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static void gs_put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static uint16_t gs_get16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

size_t gs_records_size(const gs_records *r) {
    return GS_RECORDS_HEADER + (size_t)r->count * GS_RECORD_BYTES;
}

size_t gs_records_serialize(const gs_records *r, uint8_t *buf, size_t cap) {
    size_t need = gs_records_size(r);
    if (cap < need) return 0;

    uint8_t *p = buf;
    gs_put32(p, GS_RECORDS_MAGIC);   p += 4;
    gs_put32(p, GS_RECORDS_VERSION); p += 4;
    gs_put32(p, r->count);           p += 4;

    for (uint16_t i = 0; i < r->count; i++) {
        const gs_record *e = &r->entry[i];
        gs_put64(p, e->track);      p += 8;
        gs_put64(p, e->conditions); p += 8;
        gs_put32(p, e->lap);        p += 4;
        gs_put32(p, e->race);       p += 4;
        gs_put16(p, e->laps);       p += 2;
        *p++ = e->vehicle;
        *p++ = e->mode;
        memcpy(p, e->who, GS_NAME_MAX); p += GS_NAME_MAX;
        gs_put64(p, e->when);       p += 8;      // version 2
    }
    return need;
}

bool gs_records_deserialize(gs_records *r, const uint8_t *buf, size_t len) {
    if (len < GS_RECORDS_HEADER) return false;

    const uint8_t *p = buf;
    if (gs_get32(p) != GS_RECORDS_MAGIC) return false;
    p += 4;

    // **Anything from the oldest layout to this one.** Refusing everything but
    // the current version is not safety, it is somebody's history disappearing
    // the first time a field is added - and the field always gets added.
    uint32_t version = gs_get32(p); p += 4;
    if (version < GS_RECORDS_OLDEST || version > GS_RECORDS_VERSION) return false;

    size_t row = version >= 2 ? GS_RECORD_BYTES : GS_RECORD_BYTES_V1;

    uint32_t count = gs_get32(p); p += 4;
    if (count > GS_RECORDS_MAX) return false;
    if (len < GS_RECORDS_HEADER + (size_t)count * row) return false;

    gs_records_clear(r);
    r->count = (uint16_t)count;

    for (uint16_t i = 0; i < r->count; i++) {
        gs_record *e = &r->entry[i];
        e->track = gs_get64(p);      p += 8;
        e->conditions = gs_get64(p); p += 8;
        e->lap = gs_get32(p);        p += 4;
        e->race = gs_get32(p);       p += 4;
        e->laps = gs_get16(p);       p += 2;
        e->vehicle = *p++;
        e->mode = *p++;
        memcpy(e->who, p, GS_NAME_MAX); p += GS_NAME_MAX;
        e->who[GS_NAME_MAX - 1] = '\0';

        // Older rows end here. A record from before dates existed says it does
        // not know when it was set, which is the truth and is why zero means
        // that rather than the epoch.
        if (version >= 2) { e->when = gs_get64(p); p += 8; }
        else              { e->when = 0; }
    }
    return true;
}

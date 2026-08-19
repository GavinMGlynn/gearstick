// gs_replay.c - see gs_replay.h. Everything here is explicit little-endian on
// the wire; nothing depends on the shape the compiler gave a struct.

#include "core/gs_replay.h"

void gs_replay_begin(gs_replay *r, const gs_world *w, const gs_track *t) {
    r->meta = (gs_replay_meta){ 0 };
    r->meta.track_hash     = gs_track_hash(t);
    r->meta.gravity        = w->gravity;
    r->meta.drag_scale     = w->drag_scale;
    r->meta.friction_scale = w->friction_scale;
    r->meta.damage_scale   = w->damage_scale;
    r->meta.car_count      = w->car_count;
    for (uint8_t i = 0; i < w->car_count; i++) r->meta.vehicle[i] = w->car[i].vehicle;
    r->meta.tick_count     = 0;
}

bool gs_replay_record(gs_replay *r, const gs_input *in) {
    if (r->meta.tick_count >= GS_REPLAY_MAX_TICKS) return false;

    gs_input *row = r->input[r->meta.tick_count];
    for (uint8_t i = 0; i < GS_MAX_CARS; i++) {
        row[i] = (in != nullptr && i < r->meta.car_count) ? in[i] : (gs_input)0;
    }
    r->meta.tick_count++;
    return true;
}

const gs_input *gs_replay_at(const gs_replay *r, uint32_t tick) {
    static const gs_input none[GS_MAX_CARS] = { 0 };
    return tick < r->meta.tick_count ? r->input[tick] : none;
}

bool gs_replay_restore(const gs_replay *r, gs_world *w, const gs_track *t) {
    if (r->meta.track_hash != gs_track_hash(t)) return false;

    // The dials are set from the replay rather than scaled from Earth, because
    // a replay carries its own conditions - that is the point of storing them.
    gs_world_init(w, GS_ONE);
    w->gravity        = r->meta.gravity;
    w->drag_scale     = r->meta.drag_scale;
    w->friction_scale = r->meta.friction_scale;
    w->damage_scale   = r->meta.damage_scale;
    return true;
}

bool gs_replay_playback(const gs_replay *r, const gs_track *t, gs_world *out) {
    // Caller places the cars: a replay pins the conditions and the inputs, and
    // the starting grid belongs to the track. Playback only needs the world it
    // is handed to be the world the recording started from.
    for (uint32_t i = 0; i < r->meta.tick_count; i++) {
        gs_world_step(out, t, gs_replay_at(r, i));
    }
    return true;
}

// --- the wire format ------------------------------------------------------

static void gs_put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static uint32_t gs_get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void gs_put_u64(uint8_t *p, uint64_t v) {
    gs_put_u32(p, (uint32_t)(v & 0xffffffffu));
    gs_put_u32(p + 4, (uint32_t)(v >> 32));
}

static uint64_t gs_get_u64(const uint8_t *p) {
    return (uint64_t)gs_get_u32(p) | ((uint64_t)gs_get_u32(p + 4) << 32);
}

// magic, version, track hash, four dials, car count, vehicles, tick count.
#define GS_REPLAY_HEADER_BYTES (4 + 4 + 8 + 16 + 4 + GS_MAX_CARS + 4)

size_t gs_replay_size(const gs_replay *r) {
    return GS_REPLAY_HEADER_BYTES + (size_t)r->meta.tick_count * GS_MAX_CARS;
}

size_t gs_replay_serialize(const gs_replay *r, uint8_t *buf, size_t cap) {
    size_t need = gs_replay_size(r);
    if (cap < need) return 0;

    uint8_t *p = buf;
    gs_put_u32(p, GS_REPLAY_MAGIC);              p += 4;
    gs_put_u32(p, GS_REPLAY_VERSION);            p += 4;
    gs_put_u64(p, r->meta.track_hash);           p += 8;
    gs_put_u32(p, (uint32_t)r->meta.gravity);        p += 4;
    gs_put_u32(p, (uint32_t)r->meta.drag_scale);     p += 4;
    gs_put_u32(p, (uint32_t)r->meta.friction_scale); p += 4;
    gs_put_u32(p, (uint32_t)r->meta.damage_scale);   p += 4;
    gs_put_u32(p, (uint32_t)r->meta.car_count);  p += 4;
    for (uint8_t i = 0; i < GS_MAX_CARS; i++) *p++ = r->meta.vehicle[i];
    gs_put_u32(p, r->meta.tick_count);           p += 4;

    for (uint32_t i = 0; i < r->meta.tick_count; i++) {
        for (uint8_t c = 0; c < GS_MAX_CARS; c++) *p++ = r->input[i][c];
    }
    return need;
}

bool gs_replay_deserialize(gs_replay *r, const uint8_t *buf, size_t len) {
    if (len < GS_REPLAY_HEADER_BYTES) return false;

    const uint8_t *p = buf;
    if (gs_get_u32(p) != GS_REPLAY_MAGIC) return false;
    p += 4;
    if (gs_get_u32(p) != GS_REPLAY_VERSION) return false;
    p += 4;

    *r = (gs_replay){ 0 };
    r->meta.track_hash     = gs_get_u64(p);                    p += 8;
    r->meta.gravity        = (gs_fix)gs_get_u32(p);            p += 4;
    r->meta.drag_scale     = (gs_fix)gs_get_u32(p);            p += 4;
    r->meta.friction_scale = (gs_fix)gs_get_u32(p);            p += 4;
    r->meta.damage_scale   = (gs_fix)gs_get_u32(p);            p += 4;
    r->meta.car_count      = (uint8_t)gs_get_u32(p);           p += 4;
    for (uint8_t i = 0; i < GS_MAX_CARS; i++) r->meta.vehicle[i] = *p++;
    uint32_t ticks         = gs_get_u32(p);                    p += 4;

    if (r->meta.car_count > GS_MAX_CARS) return false;
    if (ticks > GS_REPLAY_MAX_TICKS) return false;
    if (len < GS_REPLAY_HEADER_BYTES + (size_t)ticks * GS_MAX_CARS) return false;

    for (uint32_t i = 0; i < ticks; i++) {
        for (uint8_t c = 0; c < GS_MAX_CARS; c++) r->input[i][c] = *p++;
    }
    r->meta.tick_count = ticks;
    return true;
}

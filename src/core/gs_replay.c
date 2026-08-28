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
    r->meta.mode           = w->mode;
    r->meta.laps_to_win    = w->laps_to_win;
    r->meta.car_count      = w->car_count;
    for (int k = 0; k < GS_HAZ_COUNT; k++) r->meta.loadout[k] = w->loadout[k];
    for (uint8_t i = 0; i < w->car_count; i++) {
        r->meta.vehicle[i]       = w->car[i].vehicle;
        r->meta.start_x[i]       = w->car[i].x;
        r->meta.start_y[i]       = w->car[i].y;
        r->meta.start_heading[i] = w->car[i].heading;
    }
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
    w->mode           = r->meta.mode;
    w->laps_to_win    = r->meta.laps_to_win;

    // **Armed before anybody is placed**, because that is what puts the ammo on
    // a car: gs_world_add_car takes the race's loadout as it goes on the grid.
    for (int k = GS_HAZ_NONE + 1; k < GS_HAZ_COUNT; k++) {
        gs_world_arm(w, (gs_hazard_kind)k, r->meta.loadout[k]);
    }

    for (uint8_t i = 0; i < r->meta.car_count; i++) {
        gs_world_add_car(w, t, r->meta.vehicle[i], r->meta.start_x[i],
                         r->meta.start_y[i], r->meta.start_heading[i]);
    }
    return true;
}

bool gs_replay_playback(const gs_replay *r, const gs_track *t, gs_world *out) {
    if (!gs_replay_restore(r, out, t)) return false;

    for (uint32_t i = 0; i < r->meta.tick_count; i++) {
        gs_world_step(out, t, gs_replay_at(r, i));
    }
    return true;
}

// --- the wire format ------------------------------------------------------

static void gs_put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static uint16_t gs_get_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

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

// magic, version, track hash, four dials, car count, vehicles, grid, tick count.
// Version four appends the drivers; everything before them is where it was,
// which is what lets one reader handle both.
#define GS_REPLAY_HEADER_V3 \
    (4 + 4 + 8 + 16 + 1 + 2 + 4 + GS_MAX_CARS + GS_MAX_CARS * (4 + 4 + 2) + 4)
#define GS_REPLAY_HEADER_V4 \
    (GS_REPLAY_HEADER_V3 + GS_MAX_CARS * GS_REPLAY_NAME)
// Version five appends the agreed ending. Again everything before it is where
// it was, which is what lets one reader handle all three.
#define GS_REPLAY_HEADER_V5 (GS_REPLAY_HEADER_V4 + 8)
// And version six appends what the race armed everybody with. Same rule, and
// the reason it keeps being possible to add one: nothing here is a struct laid
// out by a compiler, it is bytes in an order somebody wrote down.
#define GS_REPLAY_HEADER_BYTES (GS_REPLAY_HEADER_V5 + GS_HAZ_COUNT)

void gs_replay_set_driver(gs_replay *r, uint8_t car, const char *name) {
    if (car >= GS_MAX_CARS) return;

    int i = 0;
    if (name != nullptr) {
        for (; name[i] != '\0' && i < GS_REPLAY_NAME - 1; i++) {
            r->meta.driver[car][i] = name[i];
        }
    }
    for (; i < GS_REPLAY_NAME; i++) r->meta.driver[car][i] = '\0';
}

const char *gs_replay_driver(const gs_replay *r, uint8_t car) {
    return car < GS_MAX_CARS ? r->meta.driver[car] : "";
}

void gs_replay_set_agreed(gs_replay *r, uint64_t hash) {
    r->meta.agreed_hash = hash;
}

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
    *p++ = r->meta.mode;
    gs_put_u16(p, r->meta.laps_to_win);              p += 2;
    gs_put_u32(p, (uint32_t)r->meta.car_count);  p += 4;
    for (uint8_t i = 0; i < GS_MAX_CARS; i++) *p++ = r->meta.vehicle[i];
    for (uint8_t i = 0; i < GS_MAX_CARS; i++) {
        gs_put_u32(p, (uint32_t)r->meta.start_x[i]);       p += 4;
        gs_put_u32(p, (uint32_t)r->meta.start_y[i]);       p += 4;
        gs_put_u16(p, (uint16_t)r->meta.start_heading[i]); p += 2;
    }
    gs_put_u32(p, r->meta.tick_count);           p += 4;

    // Version four: who was in each car.
    for (uint8_t i = 0; i < GS_MAX_CARS; i++) {
        for (int k = 0; k < GS_REPLAY_NAME; k++) *p++ = (uint8_t)r->meta.driver[i][k];
    }

    // Version five: the ending everybody agreed on.
    gs_put_u64(p, r->meta.agreed_hash);          p += 8;
    for (int k = 0; k < GS_HAZ_COUNT; k++) *p++ = r->meta.loadout[k];

    for (uint32_t i = 0; i < r->meta.tick_count; i++) {
        for (uint8_t c = 0; c < GS_MAX_CARS; c++) *p++ = r->input[i][c];
    }
    return need;
}

bool gs_replay_deserialize(gs_replay *r, const uint8_t *buf, size_t len) {
    if (len < GS_REPLAY_HEADER_V3) return false;

    const uint8_t *p = buf;
    if (gs_get_u32(p) != GS_REPLAY_MAGIC) return false;
    p += 4;

    // Tolerant of the past and not of the future, like every other format here.
    uint32_t version = gs_get_u32(p); p += 4;
    if (version < GS_REPLAY_OLDEST || version > GS_REPLAY_VERSION) return false;

    size_t header = version >= 6 ? GS_REPLAY_HEADER_BYTES
                  : version >= 5 ? GS_REPLAY_HEADER_V5
                  : version >= 4 ? GS_REPLAY_HEADER_V4
                                 : GS_REPLAY_HEADER_V3;
    if (len < header) return false;

    *r = (gs_replay){ 0 };
    r->meta.track_hash     = gs_get_u64(p);                    p += 8;
    r->meta.gravity        = (gs_fix)gs_get_u32(p);            p += 4;
    r->meta.drag_scale     = (gs_fix)gs_get_u32(p);            p += 4;
    r->meta.friction_scale = (gs_fix)gs_get_u32(p);            p += 4;
    r->meta.damage_scale   = (gs_fix)gs_get_u32(p);            p += 4;
    r->meta.mode           = *p++;
    r->meta.laps_to_win    = gs_get_u16(p);                    p += 2;
    r->meta.car_count      = (uint8_t)gs_get_u32(p);           p += 4;
    for (uint8_t i = 0; i < GS_MAX_CARS; i++) r->meta.vehicle[i] = *p++;
    for (uint8_t i = 0; i < GS_MAX_CARS; i++) {
        r->meta.start_x[i]       = (gs_fix)gs_get_u32(p);   p += 4;
        r->meta.start_y[i]       = (gs_fix)gs_get_u32(p);   p += 4;
        r->meta.start_heading[i] = (gs_angle)gs_get_u16(p); p += 2;
    }
    uint32_t ticks         = gs_get_u32(p);                    p += 4;

    // Version four says who was driving. Version three does not, and blank is
    // the honest answer for it rather than a name invented here.
    if (version >= 4) {
        for (uint8_t i = 0; i < GS_MAX_CARS; i++) {
            for (int k = 0; k < GS_REPLAY_NAME; k++) r->meta.driver[i][k] = (char)*p++;
            r->meta.driver[i][GS_REPLAY_NAME - 1] = '\0';
        }
    }

    // Version five carries the agreed ending. Older ones leave it zero, which
    // means "does not say" - so they are checked for the lap they claim and not
    // for a race nobody wrote down.
    if (version >= 5) { r->meta.agreed_hash = gs_get_u64(p); p += 8; }

    // A recording made before weapons existed is a race with none, which is
    // what all-zero means everywhere else too.
    for (int k = 0; k < GS_HAZ_COUNT; k++) r->meta.loadout[k] = 0;
    if (version >= 6) {
        for (int k = 0; k < GS_HAZ_COUNT; k++) r->meta.loadout[k] = *p++;
    }

    if (r->meta.car_count > GS_MAX_CARS) return false;
    if (ticks > GS_REPLAY_MAX_TICKS) return false;
    if (len < header + (size_t)ticks * GS_MAX_CARS) return false;

    for (uint32_t i = 0; i < ticks; i++) {
        for (uint8_t c = 0; c < GS_MAX_CARS; c++) r->input[i][c] = *p++;
    }
    r->meta.tick_count = ticks;
    return true;
}

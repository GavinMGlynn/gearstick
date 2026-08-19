// gs_track.c - see gs_track.h for why the shape lives on the corners.

#include "core/gs_track.h"

// Grip is the sideways acceleration a surface will bear before the tyres let
// go, expressed as a multiple of gravity - so a low-gravity pocket makes every
// surface slippery, which is exactly the interaction the gravity brush exists
// to create. Drive is how much of the engine reaches the ground.
//
// Three surfaces, far apart. The point is that a corner takeable on pavement is
// not takeable on ice, and that a player can hold the difference in their head.
const gs_surface_def gs_surfaces[GS_SURF_COUNT] = {
    [GS_SURF_PAVEMENT] = { "pavement", GS_RATIO(110, 100), GS_RATIO(3, 100), GS_ONE },
    [GS_SURF_DIRT]     = { "dirt",     GS_RATIO( 65, 100), GS_RATIO(9, 100), GS_RATIO(80, 100) },
    [GS_SURF_ICE]      = { "ice",      GS_RATIO( 18, 100), GS_RATIO(1, 100), GS_RATIO(45, 100) },
};

void gs_track_init(gs_track *t, uint8_t w, uint8_t h, gs_surface surface) {
    if (w == 0) w = 1;
    if (h == 0) h = 1;
    if (w > GS_TRACK_MAX) w = GS_TRACK_MAX;
    if (h > GS_TRACK_MAX) h = GS_TRACK_MAX;

    for (size_t i = 0; i < GS_TRACK_CORNERS; i++) t->corner[i] = 0;
    for (size_t i = 0; i < GS_TRACK_TILES; i++) {
        t->surface[i] = (uint8_t)surface;
        t->gravity[i] = GS_GRAVITY_UNIT;
    }

    t->w = w;
    t->h = h;
    t->gate_count = 0;
    for (size_t i = 0; i < GS_TRACK_MAX_GATES; i++) t->gate[i] = (gs_gate){ 0 };
}

int gs_track_add_gate(gs_track *t, gs_fix x, gs_fix y, gs_angle heading, gs_fix half_width) {
    if (t->gate_count >= GS_TRACK_MAX_GATES) return -1;

    uint8_t i = t->gate_count++;
    t->gate[i] = (gs_gate){ .x = x, .y = y, .half_width = half_width,
                            .heading = heading, .pad = 0 };
    return (int)i;
}

bool gs_track_remove_gate(gs_track *t, uint8_t index) {
    if (index >= t->gate_count) return false;

    for (uint8_t i = index; i + 1 < t->gate_count; i++) t->gate[i] = t->gate[i + 1];
    t->gate_count--;
    t->gate[t->gate_count] = (gs_gate){ 0 };
    return true;
}

bool gs_gate_crossed(const gs_gate *g, gs_fix px, gs_fix py, gs_fix nx, gs_fix ny) {
    gs_fix fx = gs_cos(g->heading);
    gs_fix fy = gs_sin(g->heading);

    // How far in front of the gate each end of the step is. Crossing means
    // starting behind it and finishing in front - the other way round is a car
    // reversing over the line, which does not count.
    gs_fix before = gs_fix_mul(px - g->x, fx) + gs_fix_mul(py - g->y, fy);
    gs_fix after = gs_fix_mul(nx - g->x, fx) + gs_fix_mul(ny - g->y, fy);
    if (before >= 0 || after < 0) return false;

    // Where along the step the plane was met, and how far off centre that was.
    // A step that passes the plane outside the gate's width misses it, which is
    // what makes this a gate and not a tripwire across the whole world.
    gs_fix span = after - before;
    gs_fix at = span == 0 ? 0 : gs_fix_div(-before, span);

    gs_fix cx = px + gs_fix_mul(nx - px, at);
    gs_fix cy = py + gs_fix_mul(ny - py, at);

    gs_fix lateral = gs_fix_mul(cx - g->x, -fy) + gs_fix_mul(cy - g->y, fx);
    return gs_fix_abs(lateral) <= g->half_width;
}

// The tile a point falls in, clamped to the track. Off-track sampling returns
// the edge tile rather than refusing, which is what makes the surrounding plain
// a continuation of the track's edge instead of a void - there is no wall at
// the boundary and nothing falls off the world.
static void gs_tile_of(const gs_track *t, gs_fix x, gs_fix y, int32_t *tx, int32_t *ty) {
    int32_t ix = gs_fix_floor(x);
    int32_t iy = gs_fix_floor(y);
    *tx = GS_CLAMP(ix, 0, (int32_t)t->w - 1);
    *ty = GS_CLAMP(iy, 0, (int32_t)t->h - 1);
}

bool gs_track_contains(const gs_track *t, gs_fix x, gs_fix y) {
    return x >= 0 && y >= 0 &&
           x < GS_INT((int32_t)t->w) && y < GS_INT((int32_t)t->h);
}

static gs_fix gs_corner_height(const gs_track *t, int32_t x, int32_t y) {
    x = GS_CLAMP(x, 0, (int32_t)t->w);
    y = GS_CLAMP(y, 0, (int32_t)t->h);
    // Multiplied, not shifted: heights below the datum are negative, and
    // shifting a negative value left is undefined in C17 and flagged by the
    // sanitizer. Same instruction, no footnote.
    return (gs_fix)t->corner[(size_t)y * GS_CORNER_STRIDE + (size_t)x] *
           (gs_fix)(1 << GS_HEIGHT_SHIFT);
}

gs_fix gs_track_height(const gs_track *t, gs_fix x, gs_fix y) {
    int32_t tx, ty;
    gs_tile_of(t, x, y, &tx, &ty);

    // Position within the tile. Clamped for the off-track case, where the
    // fractional part is meaningless.
    gs_fix fx = GS_CLAMP(x - GS_INT(tx), 0, GS_ONE);
    gs_fix fy = GS_CLAMP(y - GS_INT(ty), 0, GS_ONE);

    gs_fix h00 = gs_corner_height(t, tx,     ty);
    gs_fix h10 = gs_corner_height(t, tx + 1, ty);
    gs_fix h01 = gs_corner_height(t, tx,     ty + 1);
    gs_fix h11 = gs_corner_height(t, tx + 1, ty + 1);

    return gs_lerp(gs_lerp(h00, h10, fx), gs_lerp(h01, h11, fx), fy);
}

void gs_track_slope(const gs_track *t, gs_fix x, gs_fix y, gs_fix *dzdx, gs_fix *dzdy) {
    int32_t tx, ty;
    gs_tile_of(t, x, y, &tx, &ty);

    gs_fix h00 = gs_corner_height(t, tx,     ty);
    gs_fix h10 = gs_corner_height(t, tx + 1, ty);
    gs_fix h01 = gs_corner_height(t, tx,     ty + 1);
    gs_fix h11 = gs_corner_height(t, tx + 1, ty + 1);

    // The average of the tile's two edges along each axis: the plane of best
    // fit through four corners that need not be coplanar. Taking one edge alone
    // makes a twisted tile read as flat from one side and steep from the other.
    if (dzdx != nullptr) *dzdx = ((h10 - h00) + (h11 - h01)) / 2;
    if (dzdy != nullptr) *dzdy = ((h01 - h00) + (h11 - h10)) / 2;
}

gs_surface gs_track_surface(const gs_track *t, gs_fix x, gs_fix y) {
    int32_t tx, ty;
    gs_tile_of(t, x, y, &tx, &ty);
    uint8_t s = t->surface[GS_TILE_INDEX(tx, ty)];
    return (s < GS_SURF_COUNT) ? (gs_surface)s : GS_SURF_PAVEMENT;
}

gs_fix gs_track_gravity(const gs_track *t, gs_fix x, gs_fix y) {
    int32_t tx, ty;
    gs_tile_of(t, x, y, &tx, &ty);
    return (gs_fix)((int32_t)t->gravity[GS_TILE_INDEX(tx, ty)] * GS_ONE / GS_GRAVITY_UNIT);
}

void gs_track_set_corner(gs_track *t, uint8_t x, uint8_t y, gs_fix height) {
    if (x > t->w || y > t->h) return;
    int32_t stored = height >> GS_HEIGHT_SHIFT;
    t->corner[(size_t)y * GS_CORNER_STRIDE + (size_t)x] =
        (int16_t)GS_CLAMP(stored, INT16_MIN, INT16_MAX);
}

void gs_track_set_surface(gs_track *t, uint8_t x, uint8_t y, gs_surface s) {
    if (x >= t->w || y >= t->h || s >= GS_SURF_COUNT) return;
    t->surface[GS_TILE_INDEX(x, y)] = (uint8_t)s;
}

void gs_track_set_gravity(gs_track *t, uint8_t x, uint8_t y, gs_fix multiplier) {
    if (x >= t->w || y >= t->h) return;
    int32_t units = (int32_t)(((int64_t)multiplier * GS_GRAVITY_UNIT) >> GS_FIX_SHIFT);
    t->gravity[GS_TILE_INDEX(x, y)] = (uint8_t)GS_CLAMP(units, 0, 255);
}

// FNV-1a, 64-bit. Not a cryptographic choice and does not need to be: this
// identifies a track, it does not defend one.
static void gs_hash_bytes(uint64_t *h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) {
        *h ^= b[i];
        *h *= 0x00000100000001b3ULL;
    }
}

static void gs_hash_i32(uint64_t *h, int32_t v) {
    uint8_t le[4] = { (uint8_t)((uint32_t)v & 0xffu),
                      (uint8_t)(((uint32_t)v >> 8) & 0xffu),
                      (uint8_t)(((uint32_t)v >> 16) & 0xffu),
                      (uint8_t)(((uint32_t)v >> 24) & 0xffu) };
    gs_hash_bytes(h, le, sizeof le);
}

uint64_t gs_track_hash(const gs_track *t) {
    uint64_t h = 0xcbf29ce484222325ULL;

    gs_hash_bytes(&h, &t->w, sizeof t->w);
    gs_hash_bytes(&h, &t->h, sizeof t->h);

    // Corner by corner and tile by tile over the used region only, and each
    // int16 written out a byte at a time - so the identity of a track is the
    // same on a big-endian machine, and does not change when GS_TRACK_MAX does.
    for (uint32_t y = 0; y <= t->h; y++) {
        for (uint32_t x = 0; x <= t->w; x++) {
            int16_t v = t->corner[(size_t)y * GS_CORNER_STRIDE + (size_t)x];
            uint8_t le[2] = { (uint8_t)((uint16_t)v & 0xffu),
                              (uint8_t)(((uint16_t)v >> 8) & 0xffu) };
            gs_hash_bytes(&h, le, sizeof le);
        }
    }
    for (uint32_t y = 0; y < t->h; y++) {
        for (uint32_t x = 0; x < t->w; x++) {
            gs_hash_bytes(&h, &t->surface[GS_TILE_INDEX(x, y)], 1);
            gs_hash_bytes(&h, &t->gravity[GS_TILE_INDEX(x, y)], 1);
        }
    }

    // The route is part of the track's identity: the same ground driven the
    // other way round is a different track, and its times are not comparable.
    gs_hash_bytes(&h, &t->gate_count, sizeof t->gate_count);
    for (uint8_t i = 0; i < t->gate_count; i++) {
        const gs_gate *g = &t->gate[i];
        gs_hash_i32(&h, g->x);
        gs_hash_i32(&h, g->y);
        gs_hash_i32(&h, g->half_width);
        gs_hash_i32(&h, (int32_t)g->heading);
    }
    return h;
}

// --- the file format ------------------------------------------------------
//
// See gs_track.h. Written a byte at a time on purpose: this is the format a
// shared track travels in, and it must not depend on the endianness or the
// struct padding of the machine that wrote it.

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

// magic, version, width, height, gate count.
#define GS_TRACK_HEADER_BYTES (4 + 4 + 1 + 1 + 1)

// x, y, half width, heading.
#define GS_GATE_BYTES (4 + 4 + 4 + 2)

static size_t gs_track_payload(const gs_track *t) {
    size_t corners = ((size_t)t->w + 1) * ((size_t)t->h + 1) * 2;  // int16 each
    size_t tiles = (size_t)t->w * (size_t)t->h * 2;                // surface, gravity
    size_t gates = (size_t)t->gate_count * GS_GATE_BYTES;
    return corners + tiles + gates;
}

size_t gs_track_size(const gs_track *t) {
    return GS_TRACK_HEADER_BYTES + gs_track_payload(t);
}

size_t gs_track_serialize(const gs_track *t, uint8_t *buf, size_t cap) {
    size_t need = gs_track_size(t);
    if (cap < need) return 0;

    uint8_t *p = buf;
    gs_put_u32(p, GS_TRACK_MAGIC);   p += 4;
    gs_put_u32(p, GS_TRACK_VERSION); p += 4;
    *p++ = t->w;
    *p++ = t->h;
    *p++ = t->gate_count;

    for (uint32_t y = 0; y <= t->h; y++) {
        for (uint32_t x = 0; x <= t->w; x++) {
            uint16_t v = (uint16_t)t->corner[(size_t)y * GS_CORNER_STRIDE + (size_t)x];
            *p++ = (uint8_t)(v & 0xffu);
            *p++ = (uint8_t)((v >> 8) & 0xffu);
        }
    }
    for (uint32_t y = 0; y < t->h; y++) {
        for (uint32_t x = 0; x < t->w; x++) {
            *p++ = t->surface[GS_TILE_INDEX(x, y)];
            *p++ = t->gravity[GS_TILE_INDEX(x, y)];
        }
    }
    for (uint8_t i = 0; i < t->gate_count; i++) {
        const gs_gate *g = &t->gate[i];
        gs_put_u32(p, (uint32_t)g->x);          p += 4;
        gs_put_u32(p, (uint32_t)g->y);          p += 4;
        gs_put_u32(p, (uint32_t)g->half_width); p += 4;
        p[0] = (uint8_t)(g->heading & 0xffu);
        p[1] = (uint8_t)((g->heading >> 8) & 0xffu);
        p += 2;
    }
    return need;
}

bool gs_track_deserialize(gs_track *t, const uint8_t *buf, size_t len) {
    if (len < GS_TRACK_HEADER_BYTES) return false;

    const uint8_t *p = buf;
    if (gs_get_u32(p) != GS_TRACK_MAGIC) return false;
    p += 4;
    if (gs_get_u32(p) != GS_TRACK_VERSION) return false;
    p += 4;

    uint8_t w = *p++;
    uint8_t h = *p++;
    uint8_t gates = *p++;
    if (w == 0 || h == 0 || w > GS_TRACK_MAX || h > GS_TRACK_MAX) return false;
    if (gates > GS_TRACK_MAX_GATES) return false;

    // Everything is checked before anything is written, so a refused file
    // leaves the caller's track exactly as it was. Half-loading is the failure
    // that matters here: it races, and it is not the track anybody built.
    size_t corners = ((size_t)w + 1) * ((size_t)h + 1) * 2;
    size_t tiles = (size_t)w * (size_t)h * 2;
    size_t route = (size_t)gates * GS_GATE_BYTES;
    if (len < GS_TRACK_HEADER_BYTES + corners + tiles + route) return false;

    gs_track_init(t, w, h, GS_SURF_PAVEMENT);

    for (uint32_t y = 0; y <= h; y++) {
        for (uint32_t x = 0; x <= w; x++) {
            uint16_t v = (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
            p += 2;
            t->corner[(size_t)y * GS_CORNER_STRIDE + (size_t)x] = (int16_t)v;
        }
    }
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint8_t s = *p++;
            uint8_t g = *p++;
            // A surface out of range would index the surface table off its end
            // every tick of every race. Clamped on the way in, once.
            t->surface[GS_TILE_INDEX(x, y)] = s < GS_SURF_COUNT ? s : (uint8_t)GS_SURF_PAVEMENT;
            t->gravity[GS_TILE_INDEX(x, y)] = g;
        }
    }
    for (uint8_t i = 0; i < gates; i++) {
        gs_gate *g = &t->gate[i];
        g->x = (gs_fix)gs_get_u32(p);          p += 4;
        g->y = (gs_fix)gs_get_u32(p);          p += 4;
        g->half_width = (gs_fix)gs_get_u32(p); p += 4;
        g->heading = (gs_angle)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
        g->pad = 0;
        p += 2;
    }
    t->gate_count = gates;
    return true;
}

// --- validation -----------------------------------------------------------

const char *gs_track_problem_text(gs_track_problem p) {
    switch (p) {
    case GS_TRACK_OK:               return "the route is sound";
    case GS_TRACK_NO_START:         return "the track has no start line";
    case GS_TRACK_TOO_FEW_GATES:    return "a route needs a second gate to go to";
    case GS_TRACK_GATE_OFF_TRACK:   return "a gate hangs off the edge of the track";
    case GS_TRACK_GATE_TOO_NARROW:  return "a gate is too narrow to drive through";
    case GS_TRACK_GATES_COINCIDE:   return "two gates are in the same place";
    }
    return "something is wrong with the route";
}

// Half a tile. Narrower than this and the car - which is two thirds of a tile
// long - cannot be aimed at it, so it is a mistake rather than a challenge.
#define GS_GATE_MIN_WIDTH (GS_ONE / 4)

// Gates closer together than this are treated as the same place: the order
// between them is then ambiguous, and an ambiguous order is a route that means
// different things to the game and to the person who built it.
#define GS_GATE_MIN_APART GS_ONE

gs_track_issue gs_track_validate(const gs_track *t) {
    gs_track_issue ok = { GS_TRACK_OK, -1, -1 };

    if (t->gate_count == 0) return (gs_track_issue){ GS_TRACK_NO_START, -1, -1 };
    if (t->gate_count < 2) return (gs_track_issue){ GS_TRACK_TOO_FEW_GATES, -1, -1 };

    for (uint8_t i = 0; i < t->gate_count; i++) {
        const gs_gate *g = &t->gate[i];

        if (g->half_width < GS_GATE_MIN_WIDTH) {
            return (gs_track_issue){ GS_TRACK_GATE_TOO_NARROW, (int)i, -1 };
        }

        // Both ends of the gate, not just its centre. A gate whose far end is
        // off the world is one a car can drive round, which is worse than one
        // that is obviously wrong.
        gs_fix fx = gs_cos(g->heading);
        gs_fix fy = gs_sin(g->heading);
        gs_fix ax = g->x + gs_fix_mul(fy, g->half_width);
        gs_fix ay = g->y - gs_fix_mul(fx, g->half_width);
        gs_fix bx = g->x - gs_fix_mul(fy, g->half_width);
        gs_fix by = g->y + gs_fix_mul(fx, g->half_width);

        if (!gs_track_contains(t, g->x, g->y) ||
            !gs_track_contains(t, ax, ay) ||
            !gs_track_contains(t, bx, by)) {
            return (gs_track_issue){ GS_TRACK_GATE_OFF_TRACK, (int)i, -1 };
        }

        for (uint8_t j = (uint8_t)(i + 1); j < t->gate_count; j++) {
            gs_fix dx = t->gate[j].x - g->x;
            gs_fix dy = t->gate[j].y - g->y;
            if (gs_fix_len2(dx, dy) < GS_GATE_MIN_APART) {
                return (gs_track_issue){ GS_TRACK_GATES_COINCIDE, (int)i, (int)j };
            }
        }
    }

    return ok;
}

// gs_stunts.c - see gs_stunts.h.

#include "core/gs_stunts.h"

// --- what the codes mean ----------------------------------------------------
//
// From the published format. Only the parts that survive the crossing are
// listed: this project has no loops, no pipes and no jumps-as-objects, so a
// piece that is one of those becomes road and the shape it implied is lost.
// That is honest importing rather than bad importing - the alternative is to
// invent geometry the donor never described.

// Road pieces, by surface. Each family is a straight pair then two runs of
// corners, laid out contiguously, which is what makes this a range test rather
// than a table of eighty entries.
#define GS_ST_PAVED_FIRST  0x04
#define GS_ST_PAVED_LAST   0x0D
#define GS_ST_DIRT_FIRST   0x0E
#define GS_ST_DIRT_LAST    0x17
#define GS_ST_ICE_FIRST    0x18
#define GS_ST_ICE_LAST     0x21

#define GS_ST_START_LINE   0x01
#define GS_ST_RAISED_FIRST 0x22
#define GS_ST_RAISED_LAST  0x23

#define GS_ST_CROSS_PAVED  0x4au
#define GS_ST_CROSS_DIRT   0x7du
#define GS_ST_CROSS_ICE    0x8au

// Fillers: the squares a multi-tile object stands on besides its own. They are
// road as far as we are concerned, because something is there.
#define GS_ST_FILLER_A 0xfdu
#define GS_ST_FILLER_B 0xfeu
#define GS_ST_FILLER_C 0xffu

// Terrain. Flat, water, coast, and then a raised plateau with slopes up to it.
#define GS_ST_FLAT        0x00
#define GS_ST_WATER       0x01
#define GS_ST_COAST_FIRST 0x02
#define GS_ST_COAST_LAST  0x05
#define GS_ST_HIGH        0x06
#define GS_ST_SLOPE_FIRST 0x07
#define GS_ST_SLOPE_LAST  0x12

// How high Stunts' one raised level is, in our tiles. The donor has exactly two
// elevations - sea level and "up" - so this is the whole of its vertical range,
// and it is chosen to be a climb rather than a wall: well inside what a car can
// get up, because a converted track that cannot be driven teaches nothing.
#define GS_ST_RISE GS_RATIO(150, 100)

// And how far below the rest the water sits. Shallow on purpose: this is a
// hazard to be avoided rather than a hole to disappear into, and the surround
// already does holes.
#define GS_ST_DEPTH GS_RATIO(60, 100)

// Is this square road, and of what?
static bool gs_stunts_road(uint8_t piece, gs_surface *surface) {
    if (piece >= GS_ST_PAVED_FIRST && piece <= GS_ST_PAVED_LAST) {
        *surface = GS_SURF_PAVEMENT;
        return true;
    }
    if (piece >= GS_ST_DIRT_FIRST && piece <= GS_ST_DIRT_LAST) {
        *surface = GS_SURF_DIRT;
        return true;
    }
    if (piece >= GS_ST_ICE_FIRST && piece <= GS_ST_ICE_LAST) {
        *surface = GS_SURF_ICE;
        return true;
    }

    switch (piece) {
    case GS_ST_START_LINE:
    case GS_ST_RAISED_FIRST:
    case GS_ST_RAISED_LAST:
    case GS_ST_CROSS_PAVED:
    case GS_ST_FILLER_A:
    case GS_ST_FILLER_B:
    case GS_ST_FILLER_C:
        *surface = GS_SURF_PAVEMENT;
        return true;
    case GS_ST_CROSS_DIRT:
        *surface = GS_SURF_DIRT;
        return true;
    case GS_ST_CROSS_ICE:
        *surface = GS_SURF_ICE;
        return true;
    default:
        break;
    }
    return false;
}

// How high the ground is on this square, as a corner height.
//
// Stunts describes a square's terrain as one code rather than four corners, so
// a slope is "this square is the slope from low to high" and which way it faces
// is in the code. **We take the plateau and the water and let the slopes fall
// out of the bilinear sampling between them**, which is what our terrain does
// anyway: a corner between a high square and a low one is already a ramp, and
// building the ramp explicitly as well would give it twice the height.
static gs_fix gs_stunts_height(uint8_t terrain) {
    if (terrain == GS_ST_WATER) return -GS_ST_DEPTH;
    if (terrain >= GS_ST_COAST_FIRST && terrain <= GS_ST_COAST_LAST) return 0;
    if (terrain == GS_ST_HIGH) return GS_ST_RISE;

    // A slope square is half way up, so the two joins either side of it are each
    // half the climb rather than one square doing all of it.
    if (terrain >= GS_ST_SLOPE_FIRST && terrain <= GS_ST_SLOPE_LAST) {
        return GS_ST_RISE / 2;
    }
    return 0;
}

static uint8_t gs_stunts_surface_at(const uint8_t *track, int x, int y) {
    // The road plane is stored bottom to top, so a row index has to be flipped
    // to match the terrain plane and our own top-to-bottom order. Getting this
    // wrong mirrors the track, which looks like a track and is not the one in
    // the file.
    int row = GS_STUNTS_SIDE - 1 - y;
    return track[(size_t)row * GS_STUNTS_SIDE + (size_t)x];
}

bool gs_stunts_read(gs_track *out, const uint8_t *bytes, size_t len,
                    gs_stunts_report *report) {
    gs_stunts_report r = { 0 };

    if (bytes == nullptr || len != GS_STUNTS_BYTES) {
        if (report != nullptr) *report = r;
        return false;
    }

    const uint8_t *road = bytes + GS_STUNTS_TRACK_AT;
    const uint8_t *ground = bytes + GS_STUNTS_TERRAIN_AT;

    r.horizon = bytes[GS_STUNTS_HORIZON_AT];

    // Everything that is not road is grass. Stunts draws scenery on it; we have
    // no scenery, and grass is the ground that says "you may drive here and you
    // will not enjoy it", which is what the space beside a Stunts road is for.
    gs_track_init(out, GS_STUNTS_SIDE, GS_STUNTS_SIDE, GS_SURF_GRASS);

    for (int y = 0; y < GS_STUNTS_SIDE; y++) {
        for (int x = 0; x < GS_STUNTS_SIDE; x++) {
            uint8_t piece = gs_stunts_surface_at(road, x, y);

            gs_surface s = GS_SURF_GRASS;
            if (gs_stunts_road(piece, &s)) {
                gs_track_set_surface(out, (uint8_t)x, (uint8_t)y, s);
                r.road_tiles++;
            } else if (piece != 0) {
                // **Something is there and we do not know what.** Stunts has
                // loops, pipes, corkscrews, bridges and jumps, and this project
                // has none of them - and the published element table this reader
                // was written from does not list them all anyway. Laying road
                // and counting it says both true things at once: a car can
                // drive where the donor put a road, and this reader did not
                // understand the shape it was in. Leaving it as grass would
                // silently cut the track in half.
                gs_track_set_surface(out, (uint8_t)x, (uint8_t)y, GS_SURF_PAVEMENT);
                r.road_tiles++;
                r.unknown_pieces++;
            }

            uint8_t code = ground[(size_t)y * GS_STUNTS_SIDE + (size_t)x];
            if (code != GS_ST_FLAT) r.raised_tiles++;
        }
    }

    // The corners. Each is the average of the squares that meet at it, which is
    // what turns a grid of per-square heights into a surface that stitches.
    for (int y = 0; y <= GS_STUNTS_SIDE; y++) {
        for (int x = 0; x <= GS_STUNTS_SIDE; x++) {
            int64_t total = 0;
            int n = 0;
            for (int dy = -1; dy <= 0; dy++) {
                for (int dx = -1; dx <= 0; dx++) {
                    int sx = x + dx, sy = y + dy;
                    if (sx < 0 || sy < 0 ||
                        sx >= GS_STUNTS_SIDE || sy >= GS_STUNTS_SIDE) {
                        continue;
                    }
                    total += gs_stunts_height(
                        ground[(size_t)sy * GS_STUNTS_SIDE + (size_t)sx]);
                    n++;
                }
            }
            gs_fix h = n > 0 ? (gs_fix)(total / n) : 0;
            gs_track_set_corner(out, (uint8_t)x, (uint8_t)y, h);
        }
    }

    r.ok = true;
    if (report != nullptr) *report = r;
    return true;
}

size_t gs_stunts_write(const gs_track *t, uint8_t *buf, size_t cap,
                       uint8_t horizon) {
    if (cap < GS_STUNTS_BYTES) return 0;

    for (size_t i = 0; i < GS_STUNTS_BYTES; i++) buf[i] = 0;
    buf[GS_STUNTS_HORIZON_AT] = horizon;

    uint8_t *road = buf + GS_STUNTS_TRACK_AT;
    uint8_t *ground = buf + GS_STUNTS_TERRAIN_AT;

    for (int y = 0; y < GS_STUNTS_SIDE; y++) {
        for (int x = 0; x < GS_STUNTS_SIDE; x++) {
            gs_fix cx = GS_INT(x) + GS_HALF;
            gs_fix cy = GS_INT(y) + GS_HALF;

            // The road plane runs bottom to top; the same flip as reading.
            int row = GS_STUNTS_SIDE - 1 - y;
            size_t at = (size_t)row * GS_STUNTS_SIDE + (size_t)x;

            switch (x < t->w && y < t->h ? gs_track_surface(t, cx, cy)
                                         : GS_SURF_GRASS) {
            case GS_SURF_PAVEMENT: road[at] = GS_ST_PAVED_FIRST; break;
            case GS_SURF_DIRT:     road[at] = GS_ST_DIRT_FIRST;  break;
            case GS_SURF_ICE:      road[at] = GS_ST_ICE_FIRST;   break;
            default:               road[at] = 0;                 break;
            }

            gs_fix h = gs_track_height(t, cx, cy);
            uint8_t code = GS_ST_FLAT;
            if (h < -GS_ST_DEPTH / 2)        code = GS_ST_WATER;
            else if (h > GS_ST_RISE * 3 / 4) code = GS_ST_HIGH;
            else if (h > GS_ST_RISE / 4)     code = GS_ST_SLOPE_FIRST;

            ground[(size_t)y * GS_STUNTS_SIDE + (size_t)x] = code;
        }
    }
    return GS_STUNTS_BYTES;
}

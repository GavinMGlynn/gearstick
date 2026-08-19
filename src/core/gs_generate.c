#include "core/gs_generate.h"

#include "core/gs_sim.h"

#include <string.h>

static const char *const gs_shape_names[GS_SHAPE_COUNT] = {
    "sprint", "circuit", "jumps", "mixed",
};

const char *gs_shape_name(gs_track_shape s) {
    return s < GS_SHAPE_COUNT ? gs_shape_names[s] : "?";
}

// xorshift, because it is four lines and the same four lines everywhere. The
// quality of the randomness is not what makes a track good; the shapes below
// are.
typedef struct gs_rng { uint32_t s; } gs_rng;

// **Every draw gets its own statement.** Two draws in one argument list are two
// draws in an order C does not define - gcc took them right to left and clang
// left to right, and the same seed then produced a different track depending on
// who built the binary. A generated track is identified by its seed, so that is
// the same class of break as a physics desync: two people typing the same number
// are no longer on the same ground.


static uint32_t gs_next(gs_rng *r) {
    r->s ^= r->s << 13;
    r->s ^= r->s >> 17;
    r->s ^= r->s << 5;
    return r->s;
}

static uint32_t gs_pick(gs_rng *r, uint32_t n) {
    return n == 0 ? 0 : gs_next(r) % n;
}

// A height in whole quarter-tiles, so generated terrain lands on the same
// values a person would paint with the editor's default step. **Never zero**: a
// ridge of no height is not a shallower ridge, it is a missing one, and a track
// that rolled zero here comes out as a flat field the sweep has to catch.
static gs_fix gs_height(gs_rng *r, int quarters) {
    uint32_t q = 1 + gs_pick(r, (uint32_t)quarters);
    return (gs_fix)((int64_t)GS_ONE * (int64_t)q / 4);
}

// --- the shapes -------------------------------------------------------------
//
// Each one writes corner heights and surfaces, and leaves the route to the
// caller. They are written to be read: a ridge is a ridge, a bowl is a bowl.

static void gs_lay_flat(gs_track *t, uint8_t w, uint8_t h, gs_surface s) {
    gs_track_init(t, w, h, s);
}

// **The steepest a generated slope is allowed to be.** A car cannot climb past
// GS_MAX_CLIMB - steeper ground stops it rather than launching it - so a
// generator that picks a height and a ramp independently eventually picks a pair
// nobody can drive up. Five eighths of the limit rather than all of it: a ridge
// built right at the limit becomes a wall the moment it is added to ground that
// was already sloping, and generated ridges are added to whatever is under them.
#define GS_GEN_GRADIENT (GS_MAX_CLIMB * 5 / 8)

// **How much clear ground a car gets before the first thing in its way.** A car
// does not start at speed, and a hill needs to be arrived at with some: six
// tiles of run-up puts a stock car at two tiles a second, which is not enough to
// crest two tiles of ridge, and the track is then reported undriveable when it
// is only unreachable. Everything a shape builds starts at or after this.
#define GS_GEN_RUNUP 14

// A ridge across the track. **The ramp is derived from the height** rather than
// given: that is what makes every generated ridge driveable by construction
// instead of by a number somebody tuned until the sweep went quiet.
//
// Added to what is there rather than maximised with it, so a ridge on a bowl is
// a ridge on a bowl - taking the greater of the two leaves a step where the
// ramp meets the dip, and a step is a small wall.
static uint8_t gs_lay_ridge(gs_track *t, uint8_t from, uint8_t width,
                            gs_fix height) {
    if (height < 0) height = 0;

    // Ceiling division: the shallowest ramp that stays inside the limit.
    int32_t ramp = (int32_t)((gs_fix_div(height, GS_GEN_GRADIENT) + GS_ONE - 1) /
                             GS_ONE);
    if (ramp < 2) ramp = 2;

    // A ridge needs a flat top, so it has to be wider than its two ramps.
    if (width < ramp * 2 + 1) width = (uint8_t)(ramp * 2 + 1);
    uint8_t to = (uint8_t)(from + width);

    for (uint8_t y = 0; y <= t->h; y++) {
        for (uint8_t x = from; x <= to && x <= t->w; x++) {
            gs_fix at;
            if (x < from + ramp) {
                at = (gs_fix)((int64_t)height * (x - from) / ramp);
            } else if (x + ramp >= to) {
                at = (gs_fix)((int64_t)height * (to - x) / ramp);
            } else {
                at = height;
            }
            gs_track_set_corner(t, x, y, gs_track_corner_at(t, x, y) + at);
        }
    }
    return to;
}

static void gs_lay_band(gs_track *t, uint8_t from, uint8_t width, gs_surface s) {
    for (uint8_t x = from; x < from + width && x < t->w; x++) {
        for (uint8_t y = 0; y < t->h; y++) gs_track_set_surface(t, x, y, s);
    }
}

// A shallow bowl across the middle, so there is something to be thrown out of
// sideways rather than a flat plane with hills on it.
static void gs_lay_bowl(gs_track *t, gs_fix depth) {
    int32_t mid = t->h / 2;
    for (uint8_t y = 0; y <= t->h; y++) {
        int32_t dy = (int32_t)y - mid;
        gs_fix drop = (gs_fix)((int64_t)depth * (mid * mid - dy * dy) /
                               (mid * mid + 1));
        for (uint8_t x = 0; x <= t->w; x++) {
            gs_track_set_corner(t, x, y, gs_track_corner_at(t, x, y) - drop);
        }
    }
}

void gs_generate_shape(gs_track *t, uint32_t seed, gs_track_shape shape) {
    gs_rng r = { seed != 0 ? seed : 0x9e3779b9u };
    for (int i = 0; i < 4; i++) gs_next(&r);      // shake off a poor first value

    uint8_t w = (uint8_t)(36 + gs_pick(&r, 5) * 4);   // 36 to 52
    uint8_t h = (uint8_t)(18 + gs_pick(&r, 4) * 2);   // 18 to 24

    // **Every ground, not the three there used to be.** A generator that only
    // knew about pavement, dirt and ice would keep producing 1985 while the
    // editor offered nine worlds - and the surfaces nobody generates are the
    // surfaces nobody drives on.
    gs_surface base = (gs_surface)gs_pick(&r, GS_SURF_COUNT);

    switch (shape) {
    case GS_SHAPE_CIRCUIT: {
        // Raised edges and a dip through the middle: a loop you are pushed back
        // into rather than one drawn with walls, because there are no walls.
        gs_lay_flat(t, w, h, base);
        gs_lay_bowl(t, GS_RATIO(75, 100));
        for (uint8_t k = 0; k < 3; k++) {
            uint8_t at = (uint8_t)(GS_GEN_RUNUP + k * (w / 5));
            uint8_t width = (uint8_t)(5 + gs_pick(&r, 4));
            gs_fix height = gs_height(&r, 5);
            gs_lay_ridge(t, at, width, height);
        }
        uint8_t edge = (uint8_t)(4 + gs_pick(&r, 4));
        gs_surface edge_surface = (gs_surface)gs_pick(&r, GS_SURF_COUNT);
        gs_lay_band(t, 0, edge, edge_surface);
        break;
    }

    case GS_SHAPE_JUMPS: {
        // A run of ridges with room to land between them. The gap is what makes
        // it a jump rather than a rumble strip.
        gs_lay_flat(t, w, h, base);

        // Spaced by where the last ridge actually ended rather than by a fixed
        // stride: a ramp grows with its ridge, so a stride chosen up front puts
        // the next ramp on the back of the one before and leaves nothing flat to
        // gather speed on between two jumps.
        uint8_t at = GS_GEN_RUNUP;
        while (at + 12 < w - 6) {
            uint8_t width = (uint8_t)(4 + gs_pick(&r, 3));
            gs_fix height = (gs_fix)(GS_ONE + (int64_t)gs_height(&r, 4));
            uint8_t end = gs_lay_ridge(t, at, width, height);
            at = (uint8_t)(end + 8 + gs_pick(&r, 4));
        }
        break;
    }

    case GS_SHAPE_MIXED: {
        // Surfaces that change under you, which is where the machines sort
        // themselves out.
        gs_lay_flat(t, w, h, base);
        gs_lay_ridge(t, GS_GEN_RUNUP, 8, gs_height(&r, 6));
        for (uint8_t k = 0; k < 3; k++) {
            uint8_t width = (uint8_t)(6 + gs_pick(&r, 6));
            gs_surface s = (gs_surface)gs_pick(&r, GS_SURF_COUNT);
            gs_lay_band(t, (uint8_t)(4 + k * (w / 3)), width, s);
        }
        break;
    }

    default: {
        // Sprint: out and back with one thing in the way. The shape a first
        // track should be, and the one the analyser will always pass.
        gs_lay_flat(t, w, h, base);

        uint8_t width = (uint8_t)(8 + gs_pick(&r, 6));
        gs_fix height = gs_height(&r, 7);
        gs_lay_ridge(t, GS_GEN_RUNUP, width, height);

        uint8_t near_edge = (uint8_t)(4 + gs_pick(&r, 4));
        gs_surface near_surface = (gs_surface)gs_pick(&r, GS_SURF_COUNT);
        gs_lay_band(t, 0, near_edge, near_surface);

        gs_surface far_surface = (gs_surface)gs_pick(&r, GS_SURF_COUNT);
        gs_lay_band(t, (uint8_t)(w - 8), 8, far_surface);
        break;
    }
    }

    // Painted gravity, sometimes. A pocket over the middle where the jumping
    // happens, because that is where it is worth feeling.
    uint32_t painted = gs_pick(&r, 3);
    if (painted != 0) {
        uint8_t from = (uint8_t)(w / 3 + gs_pick(&r, 6));
        uint8_t wide = (uint8_t)(5 + gs_pick(&r, 6));
        gs_fix mul = gs_pick(&r, 2) == 0 ? GS_RATIO(35, 100) : GS_RATIO(175, 100);
        for (uint8_t x = from; x < from + wide && x < t->w; x++) {
            for (uint8_t y = 0; y < t->h; y++) gs_track_set_gravity(t, x, y, mul);
        }
    }

    // The route: a start line near the left edge and a finish near the right,
    // both wide enough to be crossed by somebody who is not aiming.
    gs_fix mid = GS_INT(t->h) / 2;
    gs_fix wide = GS_INT(5) + (gs_fix)((int64_t)GS_ONE * gs_pick(&r, 3));
    gs_track_add_gate(t, GS_INT(5), mid, 0, wide);
    gs_track_add_gate(t, GS_INT(t->w - 5), mid, 0, wide);
}

gs_track_shape gs_generate_shape_for(uint32_t seed) {
    gs_rng r = { seed != 0 ? seed : 0x9e3779b9u };
    gs_next(&r);
    return (gs_track_shape)gs_pick(&r, GS_SHAPE_COUNT);
}

void gs_generate(gs_track *t, uint32_t seed) {
    gs_generate_shape(t, seed, gs_generate_shape_for(seed));
}

// --- naming ----------------------------------------------------------------
//
// Two words from the seed. "Seed 2864434397" is not something anybody repeats
// out loud, and a name that comes back the same every time is worth more than a
// name that is merely pretty.

static const char *const gs_first[16] = {
    "long", "broken", "high", "quiet", "cold", "far", "old", "bright",
    "low", "narrow", "steep", "open", "grey", "last", "first", "wide",
};

static const char *const gs_second[16] = {
    "run", "ridge", "bowl", "drop", "loop", "flats", "climb", "reach",
    "bend", "gap", "crossing", "descent", "mile", "shelf", "pass", "circuit",
};

void gs_generate_name(char *out, size_t cap, uint32_t seed) {
    gs_rng r = { seed != 0 ? seed : 0x9e3779b9u };
    for (int i = 0; i < 6; i++) gs_next(&r);

    const char *a = gs_first[gs_pick(&r, 16)];
    const char *b = gs_second[gs_pick(&r, 16)];

    size_t n = 0;
    for (const char *p = a; *p != '\0' && n + 1 < cap; p++) out[n++] = *p;
    if (n + 1 < cap) out[n++] = ' ';
    for (const char *p = b; *p != '\0' && n + 1 < cap; p++) out[n++] = *p;
    if (cap > 0) out[n < cap ? n : cap - 1] = '\0';
}

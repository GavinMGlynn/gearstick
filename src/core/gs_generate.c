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

// **The route was two gates and that is why none of these were raceable.** A
// start line near the left edge and a finish near the right, on every shape,
// however the terrain had been laid: the ground varied and the route never did.
// Worse, the simulation counted a lap when the *last* gate was crossed, so a
// "lap" was a one-way trip, and lap two meant driving all the way back across
// an open field with nothing marking the way, to a line that was where you had
// started. Every piece worked as written and the whole was not a race.
//
// There are two routes now and a track says which it has - see gs_route_kind -
// and both are *carved into the ground* rather than dropped on top of it.

// How far in from the edge a route runs, so a car that goes wide has ground to
// go wide onto rather than a kerb and then the drop.
// **Wider than a gate is half wide**, or a gate laid on the outermost leg of
// the route has an end past the edge of the world and the track is refused. It
// was seven, and a gate's half width is six or seven.
#define GS_GEN_INSET 10

// How finely the centreline is sampled. Fine enough that the stamped discs
// overlap into a continuous road at the tightest corner an oval has.
// **Scaled to the route, not to the picture.** A hundred and ninety-two discs
// along a fifty-tile route is one every quarter tile and they overlap into a
// road; along twelve hundred tiles it is one every six, and what gets carved is
// a dotted line. The route is now as long as the field allows, so the sampling
// has to be too.
#define GS_GEN_SAMPLES 2048

// **Where the route goes: a serpentine, because length is the whole point.**
//
// This used to be a straight run with half a sine of swing on it, or an ellipse
// inset from the edges. Both are as long as the field is wide - fifty tiles,
// twenty-seven seconds of driving - and no amount of shaping the terrain makes
// a fifty-tile route into a race. Length comes from *turns*, and a turn a car
// can take has a radius, so the passes are laid a fixed pitch apart and joined
// by half circles.
//
// **Straights and arcs, not a spline through the corners.** The first attempt
// fitted a Catmull-Rom through the serpentine's corners, which overshoots by
// about a sixth of the segment either side of a right angle - and with passes a
// hundred tiles long that is sixteen tiles of overshoot, so every pass bulged
// into its neighbours and the carve turned the whole field into one car park
// with islands in it. Exact geometry has no overshoot to reason about: a
// straight is a straight and a turn is a circle of a radius a car can hold.

// Tiles between passes. Two things set this and the larger wins: a turn from
// one pass to the next has half of it as its radius, and the road is four tiles
// wide with four of verge either side, so passes closer than sixteen apart
// merge into each other. Twenty leaves four tiles of ground between them.
#define GS_GEN_PITCH   30

#define GS_GEN_MAX_SEG 64

typedef struct gs_seg {
    bool     arc;
    gs_fix   ax, ay, bx, by;     // a line's ends
    gs_fix   cx, cy, radius;     // an arc's centre
    gs_angle from;               // and where it starts
    int32_t  sweep;              // signed, in angle units; 32768 is half a turn
    gs_fix   len;                // in tiles, for walking the route by distance
} gs_seg;

typedef struct gs_route_plan {
    gs_seg  seg[GS_GEN_MAX_SEG];
    uint8_t count;
    uint8_t passes;
    gs_fix  total;               // the whole route, in tiles
    bool    loop;
} gs_route_plan;

static void gs_seg_line(gs_route_plan *p, gs_fix ax, gs_fix ay, gs_fix bx, gs_fix by) {
    if (p->count >= GS_GEN_MAX_SEG) return;
    gs_seg *g = &p->seg[p->count++];
    *g = (gs_seg){ .arc = false, .ax = ax, .ay = ay, .bx = bx, .by = by };
    g->len = gs_fix_len2(bx - ax, by - ay);
    p->total += g->len;
}

// An arc of `sweep` starting where `from` points, around a centre. Its length is
// the radius times the angle in radians, and a turn of 65536 units is two pi.
static void gs_seg_arc(gs_route_plan *p, gs_fix cx, gs_fix cy, gs_fix radius,
                       gs_angle from, int32_t sweep) {
    if (p->count >= GS_GEN_MAX_SEG) return;
    gs_seg *g = &p->seg[p->count++];
    *g = (gs_seg){ .arc = true, .cx = cx, .cy = cy, .radius = radius,
                   .from = from, .sweep = sweep };

    int32_t turn = sweep < 0 ? -sweep : sweep;
    // radius * (turn / 65536) * 2pi, with 2pi as 411775 / 65536.
    g->len = (gs_fix)(((int64_t)radius * turn / 65536) * 411775 / 65536);
    p->total += g->len;
}

static void gs_plan_route(const gs_track *t, bool loop, gs_route_plan *p) {
    p->count = 0;
    p->total = 0;
    p->loop = loop;

    const gs_fix x0 = GS_INT(GS_GEN_INSET);
    const gs_fix x1 = GS_INT(t->w - GS_GEN_INSET);
    const gs_fix y0 = GS_INT(GS_GEN_INSET);
    const gs_fix y1 = GS_INT(t->h - GS_GEN_INSET);
    const gs_fix pitch = GS_INT(GS_GEN_PITCH);
    const gs_fix r = pitch / 2;

    // A loop gives up a corridor down the right-hand side and a row across the
    // top to come home along; a path uses the whole field.
    const gs_fix xm = loop ? x1 - pitch : x1;
    const gs_fix ys = loop ? y0 + pitch : y0;

    int rows = (int)((y1 - ys) / pitch) + 1;
    if (rows < 2) rows = 2;
    if (rows > (GS_GEN_MAX_SEG - 8) / 2) rows = (GS_GEN_MAX_SEG - 8) / 2;

    // **A loop needs an odd number of passes**, or its last one ends on the far
    // side from the corridor that takes it home, and the return leg would have
    // to cross every pass to reach it.
    if (loop && (rows & 1) == 0) rows--;
    p->passes = (uint8_t)rows;

    // **The passes stop a turn's radius short of the field.** A half circle
    // joining one pass to the next bulges out by its radius past the end of
    // both, so passes that ran the full width put their turns off the edge of
    // the world - and a gate laid at the apex went with them.
    const gs_fix px0 = x0 + r, pxm = xm - r;

    for (int i = 0; i < rows; i++) {
        gs_fix y = ys + (gs_fix)((int64_t)pitch * i);
        bool rightward = (i & 1) == 0;
        gs_fix a = rightward ? px0 : pxm;
        gs_fix b = rightward ? pxm : px0;

        gs_seg_line(p, a, y, b, y);

        // The half circle onto the next pass, bulging the way the pass was
        // going so the car turns rather than reverses.
        if (i + 1 < rows) {
            // **Both start where the pass ended**, which is due south of the
            // turn's centre; what changes with the direction is which side the
            // arc bulges out. Starting a leftward turn at the *other* end of
            // its own arc walks it backwards, and the route then doubles back
            // on itself for half a turn - which laid gate nine of a serpentine
            // behind gate eight and validation caught it.
            gs_seg_arc(p, b, y + r, r, GS_DEG(270), rightward ? 32768 : -32768);
        }
    }

    if (!loop) return;

    // Home along the corridor: out to it, up the right-hand side, back across
    // the top, and down onto the first pass. Every corner a quarter circle.
    gs_fix y_last = ys + (gs_fix)((int64_t)pitch * (rows - 1));
    gs_seg_line(p, pxm, y_last, x1 - r, y_last);
    gs_seg_arc(p, x1 - r, y_last - r, r, GS_DEG(90), -16384);
    gs_seg_line(p, x1, y_last - r, x1, y0 + r);
    gs_seg_arc(p, x1 - r, y0 + r, r, 0, -16384);
    gs_seg_line(p, x1 - r, y0, px0, y0);
    gs_seg_arc(p, px0, y0 + r, r, GS_DEG(270), -16384);
    gs_seg_line(p, px0 - r, y0 + r, px0 - r, ys);
    gs_seg_arc(p, px0, ys, r, GS_DEG(180), 16384);
}

// A point on the plan, `along` running from zero to GS_ONE over the whole
// route - by distance, so the samples that carve the road are evenly spaced
// whatever the route is doing.
static void gs_route_at(const gs_route_plan *p, gs_fix along, gs_fix *x, gs_fix *y) {
    *x = 0; *y = 0;
    if (p->count == 0 || p->total <= 0) return;

    if (along < 0) along = 0;
    if (along > GS_ONE) along = GS_ONE;

    gs_fix want = gs_fix_mul(p->total, along);
    for (uint8_t i = 0; i < p->count; i++) {
        const gs_seg *g = &p->seg[i];
        if (want > g->len && i + 1 < p->count) { want -= g->len; continue; }

        gs_fix s = g->len > 0 ? gs_fix_div(want, g->len) : 0;
        if (s > GS_ONE) s = GS_ONE;

        if (!g->arc) {
            *x = g->ax + gs_fix_mul(g->bx - g->ax, s);
            *y = g->ay + gs_fix_mul(g->by - g->ay, s);
        } else {
            gs_angle a = (gs_angle)((int32_t)g->from +
                                    (int32_t)(((int64_t)g->sweep * s) >> 16));
            *x = g->cx + gs_fix_mul(g->radius, gs_cos(a));
            *y = g->cy + gs_fix_mul(g->radius, gs_sin(a));
        }
        return;
    }
}

// How wide the verge is: the tiles either side of the road where its level
// blends back into whatever the ground was doing.
#define GS_GEN_VERGE 4


// Flatten the road across its width without flattening it along its length, and
// let it back down into the terrain either side.
//
// **A road is level across and not along.** The terrain is laid first and the
// route cut through it, so a ridge in the way becomes a ramp up one side and
// down the other - which is a jump, and jumps are the point - while the car is
// never tipped sideways by ground that happens to fall away under one set of
// wheels. Before this the route was dropped onto whatever the shape generator
// had made, and on the jumps shape that put gates on the faces of ridges: seven
// of every two hundred seeds produced a track nobody could get round.
//
// **The verge is not decoration.** Stamping the road's level straight into the
// ground leaves a cliff at its edge wherever the road and the hillside disagree
// - a step no car can climb, and `no_generated_slope_is_steeper_than_a_car_can
// _climb` caught exactly that on the first attempt at this. The level is blended
// out over the verge instead, so the road meets the hillside on a slope
// something can drive up.
//
// Two passes, because one is order-dependent: with each sample stamping as it
// goes, a later sample's verge lands on an earlier sample's road and the route
// gets bitten into wherever it passes near itself. The first pass records, for
// every corner, the nearest point of the route and what height the road has
// there; the second applies it. What comes out does not depend on which end the
// route was walked from.
static void gs_carve(gs_track *t, const gs_route_plan *plan, gs_surface road) {
    const int32_t reach = GS_GEN_ROAD + GS_GEN_VERGE;

    // Nearest route sample per corner: how far, and what the road is doing
    // there. Squared distances in tiles, so no roots and no floats.
    static int32_t near2[GS_TRACK_CORNERS];
    static gs_fix  level[GS_TRACK_CORNERS];
    for (size_t i = 0; i < GS_TRACK_CORNERS; i++) near2[i] = INT32_MAX;

    for (uint16_t i = 0; i < GS_GEN_SAMPLES; i++) {
        gs_fix along = plan->loop
            ? (gs_fix)(((int64_t)i * 65536) / GS_GEN_SAMPLES)
            : (gs_fix)(((int64_t)i * GS_ONE) / (GS_GEN_SAMPLES - 1));

        gs_fix px = 0, py = 0;
        gs_route_at(plan, along, &px, &py);
        gs_fix here = gs_track_height(t, px, py);

        int32_t cx = gs_fix_floor(px), cy = gs_fix_floor(py);
        for (int32_t dy = -reach; dy <= reach + 1; dy++) {
            for (int32_t dx = -reach; dx <= reach + 1; dx++) {
                int32_t x = cx + dx, y = cy + dy;
                if (x < 0 || y < 0 || x > (int32_t)t->w || y > (int32_t)t->h) continue;

                int32_t d2 = dx * dx + dy * dy;
                if (d2 > reach * reach) continue;

                size_t at = (size_t)y * GS_CORNER_STRIDE + (size_t)x;
                if (d2 >= near2[at]) continue;
                near2[at] = d2;
                level[at] = here;
            }
        }
    }

    for (int32_t y = 0; y <= (int32_t)t->h; y++) {
        for (int32_t x = 0; x <= (int32_t)t->w; x++) {
            size_t at = (size_t)y * GS_CORNER_STRIDE + (size_t)x;
            if (near2[at] == INT32_MAX) continue;

            // Integer distance, rounded down, which is all the resolution a
            // tile lattice has anyway.
            int32_t d = 0;
            while ((d + 1) * (d + 1) <= near2[at]) d++;

            gs_fix was = gs_track_corner_at(t, (uint8_t)x, (uint8_t)y);
            gs_fix want;
            if (d <= GS_GEN_ROAD) {
                want = level[at];
            } else {
                // Out across the verge, back to whatever the ground was.
                gs_fix k = (gs_fix)(((int64_t)(d - GS_GEN_ROAD) * GS_ONE) /
                                    GS_GEN_VERGE);
                want = level[at] + gs_fix_mul(was - level[at], k);
            }
            gs_track_set_corner(t, (uint8_t)x, (uint8_t)y, want);

            // **And the road is a surface, not only a height.** Flattening
            // alone leaves a route you cannot see: the ground either side of it
            // is the same colour, so a track with a perfectly good loop cut into
            // it still reads as an open field with markers on it - which is what
            // a player said about these. The road's own surface down the
            // corridor is what makes the route a thing on the screen. Only the
            // road itself, not the verge, or the edge of it goes soft too.
            if (d <= GS_GEN_ROAD && x < (int32_t)t->w && y < (int32_t)t->h) {
                gs_track_set_surface(t, (uint8_t)x, (uint8_t)y, road);
            }
        }
    }
}

// **Nothing anywhere may be steeper than a car can climb.**
//
// Cutting a road through a ridge leaves the road at one height and the hillside
// beside it at another, and the verge only has so many tiles to give that
// difference back in. A four-tile verge across a five-tile ridge is a slope no
// car can climb, which is a track that cannot be finished for a reason that has
// nothing to do with how it was shaped -
// `no_generated_slope_is_steeper_than_a_car_can_climb` caught it.
//
// So the whole lattice is relaxed afterwards rather than the verge being made
// wide enough to guess at: wherever two neighbouring corners differ by more
// than the limit, both are moved half the excess toward each other, repeatedly,
// until nothing moves. The excess spreads outward until there is room for it,
// which is what a hillside does anyway.
//
// The limit is the car's, less an eighth. Sitting exactly on it means a car
// that can *just* climb every slope on the track, and "just" is not a margin.
static void gs_relax(gs_track *t) {
    const gs_fix limit = GS_MAX_CLIMB - GS_MAX_CLIMB / 8;

    for (int pass = 0; pass < 96; pass++) {
        bool moved = false;

        for (int32_t y = 0; y <= (int32_t)t->h; y++) {
            for (int32_t x = 1; x <= (int32_t)t->w; x++) {
                gs_fix a = gs_track_corner_at(t, (uint8_t)(x - 1), (uint8_t)y);
                gs_fix b = gs_track_corner_at(t, (uint8_t)x, (uint8_t)y);
                gs_fix d = b - a;
                gs_fix over = (d > limit) ? d - limit : (d < -limit ? d + limit : 0);
                if (over == 0) continue;
                gs_fix half = over / 2;
                gs_track_set_corner(t, (uint8_t)(x - 1), (uint8_t)y, a + half);
                gs_track_set_corner(t, (uint8_t)x, (uint8_t)y, b - half);
                moved = true;
            }
        }

        for (int32_t x = 0; x <= (int32_t)t->w; x++) {
            for (int32_t y = 1; y <= (int32_t)t->h; y++) {
                gs_fix a = gs_track_corner_at(t, (uint8_t)x, (uint8_t)(y - 1));
                gs_fix b = gs_track_corner_at(t, (uint8_t)x, (uint8_t)y);
                gs_fix d = b - a;
                gs_fix over = (d > limit) ? d - limit : (d < -limit ? d + limit : 0);
                if (over == 0) continue;
                gs_fix half = over / 2;
                gs_track_set_corner(t, (uint8_t)x, (uint8_t)(y - 1), a + half);
                gs_track_set_corner(t, (uint8_t)x, (uint8_t)y, b - half);
                moved = true;
            }
        }

        if (!moved) break;
    }
}

// The gates, laid on the same centreline the road was cut along, each facing
// the way a car is travelling when it arrives.
static void gs_lay_gates(gs_track *t, gs_rng *r, const gs_route_plan *plan) {
    // Enough that the way round is never in doubt from any one of them, few
    // enough that they are not a fence.
    // **Scaled to the route rather than fixed.** Eight gates over fifty tiles is
    // one every six; eight over eight hundred is one every hundred, which is a
    // route with nothing on it between checkpoints - and too coarse for the
    // validator, which has only the gates to work out which way the route goes
    // and reads a serpentine's neighbouring passes as a route doubling back.
    // One every dozen tiles or so, which is also close enough together that
    // the gates describe the route rather than merely marking it.
    bool loop = plan->loop;
    int want = (int)(plan->total / GS_INT(12)) + (int)gs_pick(r, 3);
    if (want > GS_TRACK_MAX_GATES - 4) want = GS_TRACK_MAX_GATES - 4;
    if (want < 6) want = 6;
    uint8_t gates = (uint8_t)want;

    // **Wider than the road, always.** A gate is finite across its line - that
    // is what makes it a gate rather than a tripwire across the world - and
    // these were three or four tiles either side of a road that is four. So a
    // car keeping to the outside of its own road passed *beside* a checkpoint
    // without crossing it, `next_gate` never advanced, and the finish line then
    // did nothing when it was reached, because gates count in order. A player
    // drove over the finish and the game did not notice.
    //
    // Two tiles of margin on the road's own half width, so leaving the tarmac
    // slightly is still a crossing and only genuinely going around one is not.
    gs_fix wide = GS_INT(GS_GEN_ROAD + 2) +
                  (gs_fix)((int64_t)GS_ONE * gs_pick(r, 2));

    for (uint8_t i = 0; i < gates; i++) {
        gs_fix along, ahead;
        if (loop) {
            along = (gs_fix)(((int64_t)i * 65536) / gates);
            ahead = along + (gs_fix)(65536 / (gates * 8));
        } else {
            along = (gs_fix)(((int64_t)i * GS_ONE) / (gates - 1));
            ahead = along + GS_ONE / (gates * 8);
            if (ahead > GS_ONE) {
                // The last gate faces the way the road was going as it arrived,
                // so a finish line is square to the road and not to the world.
                ahead = along;
                along = along - GS_ONE / (gates * 8);
            }
        }

        gs_fix x = 0, y = 0, nx = 0, ny = 0;
        gs_route_at(plan, along, &x, &y);
        gs_route_at(plan, ahead, &nx, &ny);

        gs_angle heading = gs_atan2(ny - y, nx - x);
        if (loop || i + 1 < gates) {
            gs_track_add_gate(t, x, y, heading, wide);
        } else {
            gs_track_add_gate(t, nx, ny, heading, wide);
        }
    }

    t->route = loop ? (uint8_t)GS_ROUTE_CIRCUIT : (uint8_t)GS_ROUTE_SPRINT;
}

static void gs_lay_route(gs_track *t, gs_rng *r, bool loop, gs_surface base) {
    // What the road is made of. Never the same as the ground it crosses, or
    // there is no road to see - and drawn from the surfaces a road is plausibly
    // made of rather than from all nine, because a route surfaced in slush is a
    // joke the first time and an unraceable track every time after.
    static const gs_surface made[] = {
        GS_SURF_PAVEMENT, GS_SURF_DIRT, GS_SURF_GRAVEL, GS_SURF_ROCK,
    };
    gs_surface road = made[gs_pick(r, 4)];
    if (road == base) road = (base == GS_SURF_PAVEMENT) ? GS_SURF_DIRT
                                                        : GS_SURF_PAVEMENT;

    static gs_route_plan plan;
    gs_plan_route(t, loop, &plan);

    gs_carve(t, &plan, road);
    gs_relax(t);
    gs_lay_gates(t, r, &plan);
}

void gs_generate_route(gs_track *t, uint32_t seed, bool loop) {
    gs_rng r = { seed != 0 ? seed : 0x9e3779b9u };
    for (int i = 0; i < 4; i++) gs_next(&r);

    // The road is whatever the ground under the start is not, so there is a
    // road to see - the same rule gs_lay_route follows.
    gs_surface base = gs_track_surface(t, GS_INT(t->w) / 2, GS_INT(t->h) / 2);
    gs_lay_route(t, &r, loop, base);
}

void gs_generate_shape(gs_track *t, uint32_t seed, gs_track_shape shape) {
    gs_rng r = { seed != 0 ? seed : 0x9e3779b9u };
    for (int i = 0; i < 4; i++) gs_next(&r);      // shake off a poor first value

    // **Big enough for the route to be a drive.** These were 52-64 by 26-52,
    // and a serpentine folded into that is still under two hundred tiles of
    // route - a minute at most. The field is what decides how long a route can
    // be: every pass costs twelve tiles of height, so the height *is* the pass
    // count and the width is what each pass is worth. Both kinds get most of
    // the board now, because both kinds need the room to be long.
    uint8_t w, h;
    if (shape == GS_SHAPE_CIRCUIT) {
        w = (uint8_t)(174 + gs_pick(&r, 4) * 5);      // 174 to 189
        h = (uint8_t)(174 + gs_pick(&r, 4) * 5);      // 174 to 189
    } else {
        w = (uint8_t)(178 + gs_pick(&r, 3) * 5);      // 178 to 188
        h = (uint8_t)(170 + gs_pick(&r, 4) * 5);      // 170 to 185
    }

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

        // **No bowl.** A dish across the field was how a loop kept cars in it
        // when the loop was an ellipse in the middle of open ground. A
        // serpentine already goes everywhere, so all a bowl adds is a long
        // adverse gradient on half the passes - and the one seed in six that
        // came back "nobody can get round it" was the one with the deepest one.
        gs_lay_bowl(t, GS_RATIO(25, 100));
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

    // The route.
    gs_lay_route(t, &r, shape == GS_SHAPE_CIRCUIT, base);
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

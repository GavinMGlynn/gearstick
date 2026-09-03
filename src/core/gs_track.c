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
    // Pavement does not care. Whatever you do to it, it is the same next lap,
    // which is what makes it the surface you can plan around.
    [GS_SURF_PAVEMENT] = { "pavement", GS_RATIO(110, 100), GS_RATIO(3, 100), GS_ONE,
                           0, GS_ONE, GS_ONE },

    // Dirt churns into ruts: the line everyone takes loses a third of its grip,
    // and the way round stops being the way round.
    [GS_SURF_DIRT]     = { "dirt",     GS_RATIO( 65, 100), GS_RATIO(9, 100), GS_RATIO(80, 100),
                           GS_RATIO(9, 100), GS_RATIO(62, 100), GS_ONE },

    // Ice polishes. It gets *faster* and looser at once, which is the nastiest
    // of the three: the line you have been using rewards you with more speed
    // and less ability to do anything with it.
    [GS_SURF_ICE]      = { "ice",      GS_RATIO( 18, 100), GS_RATIO(1, 100), GS_RATIO(45, 100),
                           GS_RATIO(7, 100), GS_RATIO(70, 100), GS_RATIO(40, 100) },

    // --- and one for every other world on the dial ---------------------------
    //
    // Each of these has to be a different thing to drive on, or it is a colour.
    // Read down the columns rather than across the rows: what makes a surface
    // itself is which of grip, rolling resistance and drive it is bad at, and no
    // two of these are bad at the same pair.

    // Sand robs you of *drive* above all: the wheels turn and the car does not
    // go. Grip is poor but not catastrophic, so it is the one surface where
    // lifting off is faster than flooring it. Churns into deep ruts.
    [GS_SURF_SAND]     = { "sand",     GS_RATIO( 52, 100), GS_RATIO(21, 100), GS_RATIO(55, 100),
                           GS_RATIO(11, 100), GS_RATIO(78, 100), GS_RATIO(125, 100) },

    // Gravel gives you grip only while it is still there. It rolls under the
    // tyre, so the first car through has more of it than the fifth - the only
    // surface here that gets *better* on the line for the person behind.
    [GS_SURF_GRAVEL]   = { "gravel",   GS_RATIO( 60, 100), GS_RATIO(10, 100), GS_RATIO(64, 100),
                           GS_RATIO(8, 100), GS_RATIO(125, 100), GS_RATIO(78, 100) },

    // Basalt: more grip than pavement and it costs you. Rolling resistance is
    // high and it does not wear at all, so a fast line on rock is fast on the
    // last lap too - and every landing on it is taken at full violence.
    [GS_SURF_ROCK]     = { "rock",     GS_RATIO(155, 100), GS_RATIO(20, 100), GS_ONE,
                           0, GS_ONE, GS_ONE },

    // Regolith, never weathered, never swept. Almost no rolling resistance and
    // almost no grip either - the closest thing to ice that is not slippery
    // because it is smooth. It has never been driven on, so it churns faster
    // than anything else here.
    [GS_SURF_DUST]     = { "dust",     GS_RATIO( 34, 100), GS_RATIO(2, 100), GS_RATIO(62, 100),
                           GS_RATIO(16, 100), GS_RATIO(72, 100), GS_RATIO(160, 100) },

    // Slush drags. Middling grip, the worst rolling resistance of anything here,
    // and it packs down under use into something quicker and looser - so the
    // line is worth taking and gets harder to hold each time you take it.
    [GS_SURF_SLUSH]    = { "slush",    GS_RATIO( 58, 100), GS_RATIO(26, 100), GS_RATIO(70, 100),
                           GS_RATIO(10, 100), GS_RATIO(76, 100), GS_RATIO(58, 100) },

    // Grass is fine until it is not. Nearly dirt's grip while it is whole, and
    // it tears into mud faster than dirt ruts - the surface that punishes the
    // car in front for having found the good line.
    [GS_SURF_GRASS]    = { "grass",    GS_RATIO( 90, 100), GS_RATIO(19, 100), GS_RATIO(94, 100),
                           GS_RATIO(14, 100), GS_RATIO(55, 100), GS_RATIO(150, 100) },
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

    // A fresh track is a path until something says otherwise, which is the
    // safer of the two: a sprint's finish is its last gate, and a track with
    // one gate then finishes where it starts rather than never at all.
    t->route = (uint8_t)GS_ROUTE_SPRINT;
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

// **Which way the route goes through gate `i`.** The chord from the gate before
// it to the gate after it, which is the tangent to within a degree on anything
// anybody would call a track. A loop wraps; a path takes its first and last
// gates from the ends, where there is only one direction to be had.
//
// A loop with two gates has no chord through either of them - the gate before
// and the gate after are the same gate - so it is read as a path, which is the
// only answer the geometry allows.
// The angle between two headings, the short way round, which is the only way a
// wrapping type can be compared without a modulus appearing in the physics.
static gs_angle gs_angle_apart(gs_angle a, gs_angle b) {
    uint16_t d = (uint16_t)((uint16_t)a - (uint16_t)b);
    if (d > 32768u) d = (uint16_t)(65536 - (int32_t)d);
    return (gs_angle)d;
}

// **Which way traffic goes through gate `i`, as three chords rather than one.**
//
// A gate's heading is compared against the route to catch one turned across it.
// The route between checkpoints is not recorded anywhere, so the chords through
// the neighbouring gates are all there is to go on - and no single chord is the
// tangent everywhere. Through a gate on a straight all three agree. On a curve
// the chord from the gate before to the gate after cuts the corner, and the two
// half chords lean the other way by about half the turn. A gate turned ninety
// degrees disagrees with all three, which is what this has to catch; a gate on a
// hairpin agrees with one of them, which is what it must not report.
static bool gs_faces_route(const gs_track *t, uint8_t i) {
    uint8_t n = t->gate_count;
    bool loop = t->route == (uint8_t)GS_ROUTE_CIRCUIT && n >= 3;
    gs_angle facing = t->gate[i].heading;

    // One cast around the whole conditional, not one per arm: a ternary's arms
    // promote to int before the result is converted back, so casting the arms
    // leaves an int-to-uint8_t narrowing that -Wconversion only forgives when
    // the optimiser happens to have proved the range - green at -O2 and red at
    // -O0, which is how this reached a commit.
    uint8_t before = (uint8_t)(loop ? (i + n - 1) % n : (i == 0 ? 0 : i - 1));
    uint8_t after = (uint8_t)(loop ? (i + 1) % n : (i + 1 < n ? i + 1 : i));

    const gs_gate *a = &t->gate[before];
    const gs_gate *b = &t->gate[i];
    const gs_gate *c = &t->gate[after];

    const gs_gate *pairs[3][2] = { { a, c }, { a, b }, { b, c } };
    for (int k = 0; k < 3; k++) {
        gs_fix dx = pairs[k][1]->x - pairs[k][0]->x;
        gs_fix dy = pairs[k][1]->y - pairs[k][0]->y;
        if (dx == 0 && dy == 0) continue;
        if (gs_angle_apart(facing, gs_atan2(dy, dx)) <= GS_GATE_FACING_MAX) {
            return true;
        }
    }
    return false;
}


uint8_t gs_track_route_legs(const gs_track *t) {
    if (t == nullptr || t->gate_count < 2) return 0;
    bool loop = t->route == (uint8_t)GS_ROUTE_CIRCUIT && t->gate_count >= 3;
    return loop ? t->gate_count : (uint8_t)(t->gate_count - 1);
}

gs_fix gs_track_route_length(const gs_track *t) {
    if (t == nullptr || t->gate_count < 2) return 0;

    // Chords rather than the Catmull-Rom the road is carved along: a chord is
    // shorter than the curve through the same gates, so this under-reports and
    // a track that clears the floor by this measure clears it by any other.
    gs_fix len = 0;
    for (uint8_t i = 0; i + 1 < t->gate_count; i++) {
        len += gs_fix_len2(t->gate[i + 1].x - t->gate[i].x,
                           t->gate[i + 1].y - t->gate[i].y);
    }
    if (t->route == (uint8_t)GS_ROUTE_CIRCUIT) {
        const gs_gate *last = &t->gate[t->gate_count - 1];
        len += gs_fix_len2(t->gate[0].x - last->x, t->gate[0].y - last->y);
    }
    return len;
}

gs_fix gs_track_race_length(const gs_track *t) {
    const gs_fix route = gs_track_route_length(t);
    if (!gs_track_is_circuit(t)) return route;
    return (gs_fix)((int64_t)route * GS_STOCK_LAPS);
}

void gs_track_route_point(const gs_track *t, uint8_t leg, gs_fix s,
                          gs_fix *out_x, gs_fix *out_y) {
    if (out_x != nullptr) *out_x = 0;
    if (out_y != nullptr) *out_y = 0;
    if (t == nullptr || t->gate_count < 2) return;

    uint8_t n = t->gate_count;
    bool loop = t->route == (uint8_t)GS_ROUTE_CIRCUIT && n >= 3;
    if (leg >= gs_track_route_legs(t)) return;

    // The four gates the curve is fitted through. A loop wraps; a path repeats
    // its end gates, which pins the curve to the ends rather than letting it
    // overshoot past the finish.
    uint8_t i1 = leg, i2 = (uint8_t)((leg + 1) % n), i0, i3;
    if (loop) {
        i0 = (uint8_t)((leg + n - 1) % n);
        i3 = (uint8_t)((leg + 2) % n);
    } else {
        i0 = leg == 0 ? leg : (uint8_t)(leg - 1);
        i3 = (uint8_t)(i2 + 1 < n ? i2 + 1 : i2);
    }

    if (s < 0) s = 0;
    if (s > GS_ONE) s = GS_ONE;
    gs_fix s2 = gs_fix_mul(s, s), s3 = gs_fix_mul(s2, s);

    // The Catmull-Rom basis, halved at the end rather than in each term, so the
    // rounding of the fixed point divide happens once.
    for (int axis = 0; axis < 2; axis++) {
        gs_fix p0 = axis ? t->gate[i0].y : t->gate[i0].x;
        gs_fix p1 = axis ? t->gate[i1].y : t->gate[i1].x;
        gs_fix p2 = axis ? t->gate[i2].y : t->gate[i2].x;
        gs_fix p3 = axis ? t->gate[i3].y : t->gate[i3].x;

        int64_t v = (int64_t)2 * p1;
        v += (int64_t)gs_fix_mul(p2 - p0, s);
        v += (int64_t)gs_fix_mul((gs_fix)(2 * p0 - 5 * p1 + 4 * p2 - p3), s2);
        v += (int64_t)gs_fix_mul((gs_fix)(-p0 + 3 * p1 - 3 * p2 + p3), s3);
        v /= 2;

        if (axis == 0) { if (out_x != nullptr) *out_x = (gs_fix)v; }
        else           { if (out_y != nullptr) *out_y = (gs_fix)v; }
    }
}

void gs_track_face_along_route(gs_track *t) {
    if (t == nullptr || t->gate_count < 2) return;

    uint8_t n = t->gate_count;
    bool loop = t->route == (uint8_t)GS_ROUTE_CIRCUIT && n >= 3;

    // The chord from the gate before to the gate after, which is the tangent to
    // within a degree on anything anybody would call a track. Positions are read
    // and only headings are written, so this needs no scratch copy.
    for (uint8_t i = 0; i < n; i++) {
        const gs_gate *a, *c;
        if (loop) {
            a = &t->gate[(uint8_t)((i + n - 1) % n)];
            c = &t->gate[(uint8_t)((i + 1) % n)];
        } else if (i == 0) {
            a = &t->gate[0]; c = &t->gate[1];
        } else if (i + 1 == n) {
            a = &t->gate[n - 2]; c = &t->gate[n - 1];
        } else {
            a = &t->gate[i - 1]; c = &t->gate[i + 1];
        }
        t->gate[i].heading = gs_atan2(c->y - a->y, c->x - a->x);
    }
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

bool gs_gate_missed(const gs_gate *g, gs_fix px, gs_fix py, gs_fix nx, gs_fix ny) {
    gs_fix fx = gs_cos(g->heading);
    gs_fix fy = gs_sin(g->heading);

    // Same rule as crossing for whether the plane was reached at all, and in
    // the direction a car driving the route reaches it.
    gs_fix before = gs_fix_mul(px - g->x, fx) + gs_fix_mul(py - g->y, fy);
    gs_fix after = gs_fix_mul(nx - g->x, fx) + gs_fix_mul(ny - g->y, fy);
    if (before >= 0 || after < 0) return false;

    gs_fix span = after - before;
    gs_fix at = span == 0 ? 0 : gs_fix_div(-before, span);

    gs_fix cx = px + gs_fix_mul(nx - px, at);
    gs_fix cy = py + gs_fix_mul(ny - py, at);

    // ...and then the opposite answer: outside the width rather than inside it.
    gs_fix lateral = gs_fix_mul(cx - g->x, -fy) + gs_fix_mul(cy - g->y, fx);
    return gs_fix_abs(lateral) > g->half_width;
}

// How far back the grid sits from the line. Three tiles: enough that a car has
// crossed properly rather than been placed astride the plane, and little enough
// that the run-up to the first corner is the track's business and not this
// function's.

void gs_track_grid(const gs_track *t, uint8_t slot,
                   gs_fix *x, gs_fix *y, gs_angle *heading) {
    if (t->gate_count == 0) {
        *x = GS_INT(t->w) / 2;
        *y = GS_INT(t->h) / 2;
        *heading = 0;
        return;
    }

    const gs_gate *g = &t->gate[0];
    if (slot >= GS_TRACK_GRID) slot = GS_TRACK_GRID - 1;

    gs_fix fx = gs_cos(g->heading);
    gs_fix fy = gs_sin(g->heading);

    // Evenly across the gate rather than from one edge, so the grid is centred
    // on the line whatever the field size.
    gs_fix across = gs_fix_mul(g->half_width,
                               (gs_fix)(((int64_t)(2 * slot + 1) - GS_TRACK_GRID) *
                                        GS_ONE / GS_TRACK_GRID));

    // Alternate slots a stagger further back, so no car is level with the car
    // beside it - see GS_GRID_STAGGER for the start that kept eliminating one,
    // and for why the grid does not simply step back all the way.
    gs_fix back = GS_GRID_BACK + gs_fix_mul(GS_GRID_STAGGER, GS_INT(slot));

    *x = g->x - gs_fix_mul(fx, back) - gs_fix_mul(fy, across);
    *y = g->y - gs_fix_mul(fy, back) + gs_fix_mul(fx, across);
    *heading = g->heading;

    // A gate near an edge would put the grid outside the track. Half a tile in
    // from the boundary keeps every car on the surface its gate belongs to.
    *x = GS_CLAMP(*x, GS_HALF, GS_INT(t->w) - GS_HALF);
    *y = GS_CLAMP(*y, GS_HALF, GS_INT(t->h) - GS_HALF);
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

// How far a point is outside the track, in tiles - zero anywhere on it. Measured
// along whichever axis is further out rather than as a diagonal distance, so the
// run-off is a border of even width and the corners are not pinched.
gs_fix gs_track_outside(const gs_track *t, gs_fix x, gs_fix y) {
    // Saturating, because a car flung a long way at low gravity can be further
    // out than the fixed-point type can express the doubling of, and a distance
    // that wrapped would put the ground above a car that had left the world.
    gs_fix out_x = 0, out_y = 0;
    if (x < 0) out_x = (x == INT32_MIN) ? INT32_MAX : -x;
    else if (x > GS_INT(t->w)) out_x = x - GS_INT(t->w);
    if (y < 0) out_y = (y == INT32_MIN) ? INT32_MAX : -y;
    else if (y > GS_INT(t->h)) out_y = y - GS_INT(t->h);
    return out_x > out_y ? out_x : out_y;
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

    gs_fix z = gs_lerp(gs_lerp(h00, h10, fx), gs_lerp(h01, h11, fx), fy);

    // **The shoulder, and then the drop.** Level for GS_RUNOFF_TILES past the
    // edge - at the height of the edge it left, so the join is seamless and a
    // car running wide is not launched by a step - and falling away after that.
    gs_fix out = gs_track_outside(t, x, y);
    if (out > GS_INT(GS_RUNOFF_TILES)) {
        gs_fix drop = gs_fix_mul(out - GS_INT(GS_RUNOFF_TILES), GS_RUNOFF_FALL);
        if (drop > GS_RUNOFF_FLOOR) drop = GS_RUNOFF_FLOOR;
        z -= drop;
    }
    return z;
}

void gs_track_slope(const gs_track *t, gs_fix x, gs_fix y, gs_fix *dzdx, gs_fix *dzdy) {
    int32_t tx, ty;
    gs_tile_of(t, x, y, &tx, &ty);

    // Past the shoulder the ground is the drop, and the drop is what the car is
    // on. Taken from the same rule the height uses rather than from the corners,
    // which stopped meaning anything at the boundary.
    gs_fix past = gs_track_outside(t, x, y);
    if (past > GS_INT(GS_RUNOFF_TILES)) {
        // Level again once the drop has bottomed out, so the slope agrees with
        // the height rather than promising a fall that is no longer there.
        if (gs_fix_mul(past - GS_INT(GS_RUNOFF_TILES), GS_RUNOFF_FALL) >=
            GS_RUNOFF_FLOOR) {
            if (dzdx != nullptr) *dzdx = 0;
            if (dzdy != nullptr) *dzdy = 0;
            return;
        }
        if (dzdx != nullptr) {
            *dzdx = x < 0 ? GS_RUNOFF_FALL
                          : (x > GS_INT(t->w) ? -GS_RUNOFF_FALL : 0);
        }
        if (dzdy != nullptr) {
            *dzdy = y < 0 ? GS_RUNOFF_FALL
                          : (y > GS_INT(t->h) ? -GS_RUNOFF_FALL : 0);
        }
        return;
    }

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
    // Off the track is run-off, whatever the edge tile happens to be made of.
    // Inheriting the edge's surface would mean a track that ends in ice has ice
    // for a shoulder, and the shoulder is supposed to be the thing that slows
    // you down rather than the thing that takes you further away.
    if (gs_track_outside(t, x, y) > 0) return GS_RUNOFF_SURFACE;

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

gs_fix gs_track_corner_at(const gs_track *t, uint8_t x, uint8_t y) {
    if (x > GS_TRACK_MAX || y > GS_TRACK_MAX) return 0;
    return (gs_fix)((int32_t)t->corner[(size_t)y * GS_CORNER_STRIDE + x] * 256);
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

static uint64_t gs_track_hash_of(const gs_track *t, bool with_route) {
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
    //
    // **Including whether it is a loop or a path**, which this said and did not
    // do. It hashed the gates and not what they mean, so a circuit and a sprint
    // over exactly the same ground were the same track - and the library is
    // content addressed, so saving one beside the other renamed the first and
    // threw the second away. A player who built a lap, saved it, turned it into
    // a run and saved that under a second name had one track afterwards, with
    // the second name on the first track. Their work, gone, silently.
    //
    // It is the most literal reading of driving the same ground the other way
    // round: on a circuit you cross gate zero again to finish a lap, on a
    // sprint you drive from the first gate to the last and stop. A best lap on
    // one is not a time you can put beside a best lap on the other.
    if (with_route) gs_hash_bytes(&h, &t->route, sizeof t->route);
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

uint64_t gs_track_hash(const gs_track *t) {
    return gs_track_hash_of(t, true);
}

// **What this used to answer**, for reading a share code written before the
// route was part of a track's identity.
//
// A code carries the hash of what it encodes so that a damaged one fails loudly
// rather than opening as a track nobody built. Changing what a track's identity
// *is* therefore stops every code already shared from opening - and one went
// out with v0.1.0-beta1. A reader that accepts either answer costs three lines
// and means nobody's link breaks; what it gives up is noticing a code whose
// route byte alone was corrupted, which is one bit of one byte out of a
// hundred, and the alternative was telling somebody their working code was
// damaged.
uint64_t gs_track_hash_before_route_kind(const gs_track *t) {
    return gs_track_hash_of(t, false);
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
// magic, version, w, h, gate count, route kind.
#define GS_TRACK_HEADER_BYTES (4 + 4 + 1 + 1 + 1 + 1)

// What a version 2 header was, before the route kind was in it.
#define GS_TRACK_HEADER_BYTES_V2 (4 + 4 + 1 + 1 + 1)

// x, y, half width, heading.
#define GS_GATE_BYTES (4 + 4 + 4 + 2)

static size_t gs_track_payload(const gs_track *t) {
    size_t corners = ((size_t)t->w + 1) * ((size_t)t->h + 1) * 2;  // int16 each
    size_t tiles = (size_t)t->w * (size_t)t->h * 2;                // surface, gravity
    size_t gates = (size_t)t->gate_count * GS_GATE_BYTES;
    return corners + tiles + gates;
}

uint8_t gs_track_finish_gate(const gs_track *t) {
    if (t->gate_count == 0) return 0;
    if (t->route == (uint8_t)GS_ROUTE_CIRCUIT) return 0;
    return (uint8_t)(t->gate_count - 1);
}

bool gs_track_is_circuit(const gs_track *t) {
    return t->route == (uint8_t)GS_ROUTE_CIRCUIT;
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
    *p++ = t->route;

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
    if (len < GS_TRACK_HEADER_BYTES_V2) return false;

    const uint8_t *p = buf;
    if (gs_get_u32(p) != GS_TRACK_MAGIC) return false;
    p += 4;

    // **Version 2 still loads.** Those files carry no route kind because there
    // was none to carry, and every one of them is a sprint: two gates, one at
    // each end. Refusing them would throw away every track anybody had saved to
    // add a byte.
    uint32_t version = gs_get_u32(p);
    p += 4;
    if (version != GS_TRACK_VERSION && version != 2u) return false;
    size_t header = version >= 3u ? GS_TRACK_HEADER_BYTES
                                  : (size_t)GS_TRACK_HEADER_BYTES_V2;
    if (len < header) return false;

    uint8_t w = *p++;
    uint8_t h = *p++;
    uint8_t gates = *p++;
    uint8_t kind = version >= 3u ? *p++ : (uint8_t)GS_ROUTE_SPRINT;
    if (w == 0 || h == 0 || w > GS_TRACK_MAX || h > GS_TRACK_MAX) return false;
    if (gates > GS_TRACK_MAX_GATES) return false;
    if (kind > (uint8_t)GS_ROUTE_CIRCUIT) return false;

    // Everything is checked before anything is written, so a refused file
    // leaves the caller's track exactly as it was. Half-loading is the failure
    // that matters here: it races, and it is not the track anybody built.
    size_t corners = ((size_t)w + 1) * ((size_t)h + 1) * 2;
    size_t tiles = (size_t)w * (size_t)h * 2;
    size_t route = (size_t)gates * GS_GATE_BYTES;
    if (len < header + corners + tiles + route) return false;

    gs_track_init(t, w, h, GS_SURF_PAVEMENT);
    t->route = kind;

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
    case GS_TRACK_GATE_FACING:      return "a gate faces across the route rather "
                                           "than along it";
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

    // **And every gate faces the way the route goes through it.**
    //
    // A gate is a plane whose normal is its heading: gs_gate_crossed wants the
    // car to start behind it and finish in front, and the arrow drawn on the
    // ground points the same way. Turn a gate ninety degrees and a car driving
    // the route travels *along* its plane rather than through it - the crossing
    // becomes a coin toss decided by which side of the centre the car happened
    // to be - and past ninety it cannot be crossed in the direction of travel
    // at all. The arrow, meanwhile, points somewhere nobody drives.
    //
    // Every hand-written stock track was authored with a heading of zero on
    // every gate, and `the crossing` - a figure of eight - shipped with all
    // four of its gates facing east, one of them square across the route. This
    // is the check that was missing: the route was asked whether it could be
    // *finished*, never whether its gates faced the way it went.
    //
    // Sixty degrees rather than the ninety the geometry forbids, because a gate
    // approached at sixty degrees has already lost half its width to the angle,
    // and a track that only just works is a track that will not survive being
    // edited. Both kinds are covered: the tangent through a loop wraps and a
    // path takes its ends from its ends.
    if (t->gate_count >= 2) {
        for (uint8_t i = 0; i < t->gate_count; i++) {
            if (!gs_faces_route(t, i)) {
                return (gs_track_issue){ GS_TRACK_GATE_FACING, (int)i, -1 };
            }
        }
    }

    return ok;
}

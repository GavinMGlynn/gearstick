// gs_parts.c - see gs_parts.h.

#include "core/gs_parts.h"

#include "core/gs_fixed.h"

const char *gs_part_name(gs_part_kind k) {
    switch (k) {
    case GS_PART_STRAIGHT:     return "straight";
    case GS_PART_CORNER:       return "corner";
    case GS_PART_RAMP:         return "ramp";
    case GS_PART_CREST:        return "crest";
    case GS_PART_DIP:          return "dip";
    case GS_PART_CROSSROADS:   return "crossroads";
    case GS_PART_START:        return "start line";
    case GS_PART_FINISH:       return "finish line";
    case GS_PART_START_FINISH: return "start / finish";
    case GS_PART_CHECKPOINT:   return "checkpoint";
    default:                   return "part";
    }
}

bool gs_part_is_road(gs_part_kind k) {
    return k <= GS_PART_CROSSROADS;
}

bool gs_part_is_route(gs_part_kind k) {
    return k >= GS_PART_START && k < GS_PART_COUNT;
}

gs_part gs_part_default(gs_part_kind kind) {
    gs_part p = { 0 };
    p.kind = (uint8_t)kind;
    p.turn = 0;
    p.width = 8;
    p.length = 10;
    p.surface = (uint8_t)GS_SURF_PAVEMENT;
    p.rise = 0;

    switch (kind) {
    case GS_PART_CORNER:
        // Square, because a quarter turn's length and width are the same arc.
        p.length = 12;
        p.width = 8;
        break;
    case GS_PART_RAMP:
        p.length = 10;
        p.rise = GS_INT(2);
        break;
    case GS_PART_CREST:
    case GS_PART_DIP:
        p.length = 14;
        p.rise = GS_INT(2);
        break;
    case GS_PART_CROSSROADS:
        // Square, and long enough either way that a car has road to arrive on
        // and road to leave by. Level, always: an intersection with a slope in
        // it is one where nobody can see what the other road is doing.
        p.length = 14;
        p.width = 8;
        break;
    default:
        break;
    }

    // A line is one tile deep and as wide as the road it crosses. Its length is
    // not a dial, because a start line as long as the straight it sits on is
    // not a start line.
    if (gs_part_is_route(kind)) {
        p.length = 1;
        p.width = 8;
    }
    return p;
}

// The way a part points, as the four quarter turns.
static gs_angle gs_part_heading(const gs_part *p) {
    return (gs_angle)((uint32_t)p->turn * (GS_TURN / 4u));
}

void gs_part_footprint(const gs_part *p, int32_t x, int32_t y,
                       int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1) {
    int32_t half = p->width / 2;
    int32_t len = gs_part_is_route((gs_part_kind)p->kind) ? 1 : p->length;

    // Laid from where it was dropped, running the way it points.
    int32_t ax = 0, ay = 0;      // along
    int32_t sx = 0, sy = 0;      // across
    switch (p->turn & 3u) {
    case 0: ax = 1;  ay = 0;  sx = 0;  sy = 1;  break;
    case 1: ax = 0;  ay = 1;  sx = -1; sy = 0;  break;
    case 2: ax = -1; ay = 0;  sx = 0;  sy = -1; break;
    default: ax = 0; ay = -1; sx = 1;  sy = 0;  break;
    }

    int32_t px[4], py[4];
    px[0] = x + sx * -half;                py[0] = y + sy * -half;
    px[1] = x + sx * half;                 py[1] = y + sy * half;
    px[2] = x + ax * len + sx * -half;     py[2] = y + ay * len + sy * -half;
    px[3] = x + ax * len + sx * half;      py[3] = y + ay * len + sy * half;

    // **A crossroads reaches both ways from where it was dropped**, because it
    // is the middle of the piece that goes under the pointer rather than one
    // end of it - an intersection is placed by its junction.
    if ((gs_part_kind)p->kind == GS_PART_CROSSROADS) {
        int32_t arm = len / 2;
        *x0 = x - arm; *x1 = x + arm;
        *y0 = y - arm; *y1 = y + arm;
        return;
    }

    // A corner turns, so its box is the square it turns inside.
    if ((gs_part_kind)p->kind == GS_PART_CORNER) {
        px[2] = x + ax * len + sx * len;   py[2] = y + ay * len + sy * len;
        px[3] = x + sx * len;              py[3] = y + sy * len;
    }

    *x0 = px[0]; *x1 = px[0];
    *y0 = py[0]; *y1 = py[0];
    for (int i = 1; i < 4; i++) {
        if (px[i] < *x0) *x0 = px[i];
        if (px[i] > *x1) *x1 = px[i];
        if (py[i] < *y0) *y0 = py[i];
        if (py[i] > *y1) *y1 = py[i];
    }
}

// How high the road is at `t` along a shaped piece, `t` being Q16.16 from zero
// at the near end to one at the far end.
static gs_fix gs_part_profile(const gs_part *p, gs_fix at) {
    switch ((gs_part_kind)p->kind) {
    case GS_PART_RAMP:
        return gs_fix_mul(p->rise, at);
    case GS_PART_CREST:
        // Up and over: a half turn of sine peaks in the middle and comes back.
        return gs_fix_mul(p->rise, gs_sin((gs_angle)(at / 2)));
    case GS_PART_DIP:
        return -gs_fix_mul(p->rise, gs_sin((gs_angle)(at / 2)));
    default:
        return 0;
    }
}

// Where the centreline of a road piece is, `at` being Q16.16 along it.
static void gs_part_centre(const gs_part *p, int32_t x, int32_t y, gs_fix at,
                           gs_fix *ox, gs_fix *oy) {
    gs_fix len = GS_INT(p->length);

    gs_angle h = gs_part_heading(p);
    gs_fix fx = gs_cos(h), fy = gs_sin(h);

    if ((gs_part_kind)p->kind == GS_PART_CORNER) {
        // A quarter circle of radius `length`, starting where the piece was
        // dropped and pointing the way it points, bending to the left of it.
        gs_fix sx = -fy, sy = fx;
        gs_angle a = (gs_angle)(at / 4);         // a quarter turn over the piece
        gs_fix s = gs_sin(a), c = gs_cos(a);

        *ox = GS_INT(x) + gs_fix_mul(len, gs_fix_mul(fx, s)) +
              gs_fix_mul(len, gs_fix_mul(sx, GS_ONE - c));
        *oy = GS_INT(y) + gs_fix_mul(len, gs_fix_mul(fy, s)) +
              gs_fix_mul(len, gs_fix_mul(sy, GS_ONE - c));
        return;
    }

    *ox = GS_INT(x) + gs_fix_mul(len, gs_fix_mul(fx, at));
    *oy = GS_INT(y) + gs_fix_mul(len, gs_fix_mul(fy, at));
}

// How finely a piece is walked when laying it. Fine enough that the stamps
// overlap into a continuous road round the tightest corner a piece can be.
#define GS_PART_STEPS 96

// A crossroads is two roads through the same middle at right angles, laid at
// one level so that neither arm tips a car crossing the other. **Flat, and only
// flat**: two roads at *different* heights - an overpass - is not something the
// terrain can hold, because it is one height per corner and an overpass needs
// two. That is a change to what a track is rather than another piece in the
// box, and it is not made here.
static bool gs_part_lay_cross(gs_edit_log *l, gs_track *t, const gs_part *p,
                              int32_t x, int32_t y) {
    int32_t half = p->width / 2;
    int32_t arm = p->length / 2;

    gs_fix level = gs_track_height(t, GS_INT(x), GS_INT(y));

    for (int32_t dy = -arm - half; dy <= arm + half; dy++) {
        for (int32_t dx = -arm - half; dx <= arm + half; dx++) {
            // Inside one arm or the other: a plus sign, not a square.
            bool along_x = (dx * dx <= (arm + half) * (arm + half)) &&
                           (dy * dy <= half * half);
            bool along_y = (dy * dy <= (arm + half) * (arm + half)) &&
                           (dx * dx <= half * half);
            if (!along_x && !along_y) continue;

            int32_t px = x + dx, py = y + dy;
            if (px < 0 || py < 0) continue;
            if (px > (int32_t)t->w || py > (int32_t)t->h) continue;

            if (!gs_edit_corner(l, t, (uint8_t)px, (uint8_t)py, level)) return false;
            if (px < (int32_t)t->w && py < (int32_t)t->h) {
                if (!gs_edit_surface(l, t, (uint8_t)px, (uint8_t)py,
                                     (gs_surface)p->surface)) {
                    return false;
                }
            }
        }
    }
    return true;
}

static bool gs_part_lay_road(gs_edit_log *l, gs_track *t, const gs_part *p,
                             int32_t x, int32_t y) {
    if ((gs_part_kind)p->kind == GS_PART_CROSSROADS) {
        return gs_part_lay_cross(l, t, p, x, y);
    }

    int32_t half = p->width / 2;

    // The ground the piece starts from, so a straight laid off the end of a
    // ramp continues from where the ramp left off rather than from zero.
    gs_fix base = gs_track_height(t, GS_INT(x), GS_INT(y));

    for (int i = 0; i <= GS_PART_STEPS; i++) {
        gs_fix at = (gs_fix)(((int64_t)i * GS_ONE) / GS_PART_STEPS);

        gs_fix cxf = 0, cyf = 0;
        gs_part_centre(p, x, y, at, &cxf, &cyf);
        gs_fix level = base + gs_part_profile(p, at);

        int32_t cx = gs_fix_floor(cxf), cy = gs_fix_floor(cyf);
        for (int32_t dy = -half; dy <= half + 1; dy++) {
            for (int32_t dx = -half; dx <= half + 1; dx++) {
                int32_t px = cx + dx, py = cy + dy;
                if (px < 0 || py < 0) continue;
                if (px > (int32_t)t->w || py > (int32_t)t->h) continue;
                if (dx * dx + dy * dy > half * half) continue;

                if (!gs_edit_corner(l, t, (uint8_t)px, (uint8_t)py, level)) {
                    return false;
                }
                if (px < (int32_t)t->w && py < (int32_t)t->h) {
                    if (!gs_edit_surface(l, t, (uint8_t)px, (uint8_t)py,
                                         (gs_surface)p->surface)) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

bool gs_part_place(gs_edit_log *l, gs_track *t, const gs_part *p,
                   int32_t x, int32_t y) {
    int32_t x0, y0, x1, y1;
    gs_part_footprint(p, x, y, &x0, &y0, &x1, &y1);
    if (x0 < 0 || y0 < 0) return false;
    if (x1 > (int32_t)t->w || y1 > (int32_t)t->h) return false;

    gs_edit_begin(l);

    bool ok = true;
    if (gs_part_is_road((gs_part_kind)p->kind)) {
        ok = gs_part_lay_road(l, t, p, x, y);
    } else {
        // **The lines, and what each one says about the track.**
        gs_part_kind kind = (gs_part_kind)p->kind;

        if (kind == GS_PART_START_FINISH) {
            ok = gs_edit_route_kind(l, t, GS_ROUTE_CIRCUIT);
        } else if (kind == GS_PART_START || kind == GS_PART_FINISH) {
            ok = gs_edit_route_kind(l, t, GS_ROUTE_SPRINT);
        }

        if (ok) {
            int at = gs_edit_add_gate(l, t, GS_INT(x), GS_INT(y),
                                      gs_part_heading(p),
                                      GS_INT(p->width) / 2);
            ok = at >= 0;

            // A start line is gate zero wherever it was dropped, because gate
            // zero is where a race begins. Dropped after the corners have been
            // laid it would otherwise be the last thing on the route, and the
            // track would start in the middle of itself.
            if (ok && at > 0 &&
                (kind == GS_PART_START || kind == GS_PART_START_FINISH)) {
                ok = gs_edit_move_gate(l, t, (uint8_t)at, 0);
            }
        }
    }

    gs_edit_end(l);

    // A part that would not fit in the history is a part that did not happen -
    // the same rule every other edit follows.
    if (!ok) gs_edit_undo(l, t);
    return ok;
}

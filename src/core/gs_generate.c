// gs_generate.c - tracks from a number.
//
// **A track is a draw from a matrix.** Ten dials - class, length, curviness,
// straightness, jumps, relief, relief range, gravity, dress, road width - each
// drawn from the seed in gs_generate_spec_for, plus a base surface. The route
// is then *grown* to satisfy the draw: a randomised self-avoiding walk over a
// coarse grid of cells, biased by the curviness and straightness dials, turned
// into straights and arcs a car can drive. How a track folds is an outcome of
// the dice, not a family it belongs to.
//
// This replaced four hard-coded terrain shapes and three fixed route layouts,
// after a player said what they were: "every track can't be the same shape...
// the tracks should be organic". The old generator chose scenery per seed and
// laid the identical serpentine under all of it.
//
// Downstream of the plan nothing changed: the route is carved into the ground
// (a ridge in the way becomes a jump), the lattice is relaxed until every slope
// is climbable, and the gates are laid on the centreline facing the way a car
// arrives.
#include "core/gs_generate.h"

#include "core/gs_sim.h"

#include <string.h>

// xorshift, because it is four lines and the same four lines everywhere. The
// quality of the randomness is not what makes a track good.
typedef struct gs_rng { uint32_t s; } gs_rng;

// **Every draw gets its own statement.** Two draws in one argument list are two
// draws in an order C does not define - gcc took them right to left and clang
// left to right, and the same seed then produced a different track depending on
// who built the binary. A generated track is identified by its seed, so that is
// the same class of break as a physics desync.

static uint32_t gs_next(gs_rng *r) {
    r->s ^= r->s << 13;
    r->s ^= r->s >> 17;
    r->s ^= r->s << 5;
    return r->s;
}

static uint32_t gs_pick(gs_rng *r, uint32_t n) {
    return n == 0 ? 0 : gs_next(r) % n;
}

// --- the spec ---------------------------------------------------------------

gs_track_spec gs_generate_spec_for(uint32_t seed) {
    gs_rng r = { seed != 0 ? seed : 0x9e3779b9u };
    for (int i = 0; i < 4; i++) gs_next(&r);      // shake off a poor first value

    gs_track_spec s;
    s.kind = (gs_gen_class)gs_pick(&r, GS_CLASS_COUNT);
    s.length = (gs_gen_length)gs_pick(&r, GS_LEN_COUNT);
    s.curve = (gs_gen_curve)gs_pick(&r, GS_CURVE_COUNT);
    s.straight = (gs_gen_straight)gs_pick(&r, GS_STRAIGHT_COUNT);
    s.jumps = (gs_gen_jumps)gs_pick(&r, GS_JUMPS_COUNT);
    s.relief = (gs_gen_relief)gs_pick(&r, GS_RELIEF_COUNT);
    s.range = (gs_gen_range)gs_pick(&r, GS_RANGE_COUNT);
    s.gravity = (gs_gen_gravity)gs_pick(&r, GS_GRAV_COUNT);
    s.dress = (gs_gen_dress)gs_pick(&r, GS_DRESS_COUNT);
    s.width = (gs_gen_width)gs_pick(&r, GS_WIDTH_COUNT);
    s.base = (gs_surface)gs_pick(&r, GS_SURF_COUNT);

    // **The vetoes.** Two dials contradicting each other is resolved here,
    // once, so a spec in hand is always a spec that can be built - and
    // resolved the same way for the same seed on every machine.

    // Flat ground with a relief range on it is a contradiction: the range
    // wins, and the ground it asked for gets a shape that can carry it.
    if (s.relief == GS_RELIEF_FLAT && s.range != GS_RANGE_SUBTLE) {
        s.relief = s.range == GS_RANGE_SEVERE ? GS_RELIEF_RIDGED
                                              : GS_RELIEF_ROLLING;
    }

    // Flat ground and no jumps is a billiard table: completable, differently
    // hashed from its neighbours, and worthless. The ground gives - gently.
    if (s.relief == GS_RELIEF_FLAT && s.jumps == GS_JUMPS_NONE) {
        s.relief = GS_RELIEF_ROLLING;
    }

    // A narrow road, constant corners and no grip is three dials that are each
    // fine and together a track nobody finishes. The road gives.
    if (s.width == GS_WIDTH_NARROW && s.curve == GS_CURVE_TECHNICAL &&
        (s.base == GS_SURF_ICE || s.base == GS_SURF_SLUSH)) {
        s.width = GS_WIDTH_STANDARD;
    }

    // Big jumps and moon gravity is a car that leaves the world; that veto is
    // applied where gravity is painted, as a floor of three quarters, so the
    // gravity dial itself stays honest here.

    return s;
}

uint8_t gs_spec_road(const gs_track_spec *spec) {
    switch (spec->width) {
    case GS_WIDTH_NARROW: return 3;
    case GS_WIDTH_WIDE:   return 6;
    case GS_WIDTH_STANDARD:
    case GS_WIDTH_COUNT:
    default:              return 4;
    }
}

// The one-line reason, after the 1985 manual: every track existed for a reason
// somebody could say in one line, and a set that cannot do that is a set of
// variations. Hand-rolled append because src/core links nothing, stdio
// included.
static size_t gs_say(char *out, size_t cap, size_t n, const char *word) {
    for (const char *p = word; *p != '\0' && n + 1 < cap; p++) out[n++] = *p;
    return n;
}

void gs_spec_line(const gs_track_spec *spec, char *out, size_t cap) {
    static const char *const kind[GS_CLASS_COUNT] = { "circuit", "path" };
    static const char *const len[GS_LEN_COUNT] = { "standard", "long", "epic" };
    static const char *const curve[GS_CURVE_COUNT] = {
        "flowing", "winding", "technical",
    };
    static const char *const straight[GS_STRAIGHT_COUNT] = {
        "broken straights", "balanced straights", "a power straight",
    };
    static const char *const jumps[GS_JUMPS_COUNT] = {
        "no jumps", "small jumps", "big jumps",
    };
    static const char *const relief[GS_RELIEF_COUNT] = {
        "flat", "rolling", "ridged", "a basin",
    };
    static const char *const range[GS_RANGE_COUNT] = {
        "subtle", "moderate", "severe",
    };
    static const char *const gravity[GS_GRAV_COUNT] = {
        "earth gravity", "light pockets", "heavy pockets", "split gravity",
    };
    static const char *const dress[GS_DRESS_COUNT] = {
        "plain", "banded", "patchwork",
    };
    static const char *const width[GS_WIDTH_COUNT] = {
        "narrow road", "standard road", "wide road",
    };
    static const char *const ground[GS_SURF_COUNT] = {
        "pavement", "dirt", "ice", "sand", "gravel", "rock", "dust", "slush",
        "grass",
    };

    size_t n = 0;
    n = gs_say(out, cap, n, curve[spec->curve]);
    n = gs_say(out, cap, n, " ");
    n = gs_say(out, cap, n, len[spec->length]);
    n = gs_say(out, cap, n, " ");
    n = gs_say(out, cap, n, kind[spec->kind]);
    n = gs_say(out, cap, n, " on ");
    n = gs_say(out, cap, n, ground[spec->base]);
    n = gs_say(out, cap, n, " | ");
    n = gs_say(out, cap, n, straight[spec->straight]);
    n = gs_say(out, cap, n, " | ");
    n = gs_say(out, cap, n, jumps[spec->jumps]);
    n = gs_say(out, cap, n, " | ");
    n = gs_say(out, cap, n, relief[spec->relief]);
    n = gs_say(out, cap, n, " ");
    n = gs_say(out, cap, n, range[spec->range]);
    n = gs_say(out, cap, n, " | ");
    n = gs_say(out, cap, n, gravity[spec->gravity]);
    n = gs_say(out, cap, n, " | ");
    n = gs_say(out, cap, n, dress[spec->dress]);
    n = gs_say(out, cap, n, " | ");
    n = gs_say(out, cap, n, width[spec->width]);
    if (cap > 0) out[n < cap ? n : cap - 1] = '\0';
}

// --- growing the route ------------------------------------------------------
//
// The route lives on a coarse grid of cells, pitch tiles apart, inset from the
// field's edge. A **path** is a self-avoiding walk over the cells, grown by
// depth-first search with backtracking until it has visited as many cells as
// the length dial asks for. A **circuit** starts as a two-by-two ring of cells
// and grows by *bumping*: an edge of the cycle is pushed sideways into two
// unused cells, which keeps it a cycle by construction and adds two cells per
// bump. Both are self-avoiding on the cell grid, so no two stretches of road
// come closer than the pitch - which is what lets the corridor be carved
// without biting into itself.
//
// The dials reach the walk as biases: curviness weights whether the walk
// prefers to keep going or to turn, straightness caps how long a run may be
// and whether growth protects or attacks the runs a cycle already has.

// How far in from the edge a route runs, so a car that goes wide has ground to
// go wide onto rather than a kerb and then the drop. Wider than a gate is half
// wide, or a gate on the outermost stretch ends past the edge of the world.
#define GS_GEN_INSET 10

// How wide the verge is: the tiles either side of the road where its level
// blends back into whatever the ground was doing. Six rather than four: the
// verge is also the ground a car that runs wide has to recover on, and on a
// severe field with poor grip a four-tile blend still ended in a hillside a
// wedged car could not climb back off.
#define GS_GEN_VERGE 6

// How finely the centreline is sampled when carving. Scaled to the route, not
// the picture: a thousand tiles of route at 2048 samples is one every half
// tile, and the stamped discs overlap into a continuous road.
#define GS_GEN_SAMPLES 2048

#define GS_GEN_GRID 10                          // most cells the grid has a side
#define GS_GEN_CELLS (GS_GEN_GRID * GS_GEN_GRID)
#define GS_GEN_PTS 4096                          // polyline points in a plan

typedef struct gs_walk {
    int8_t cx[GS_GEN_CELLS], cy[GS_GEN_CELLS];
    int count;
} gs_walk;

static const int gs_dx[4] = { 1, 0, -1, 0 };
static const int gs_dy[4] = { 0, 1, 0, -1 };

// The walk, depth first with backtracking. `want` cells, runs capped at
// `maxrun`, the first `seedrun` cells laid in a straight line so the start of
// every path - where the grid stands - is on a straight. The weights decide
// how often the walk keeps its heading against turning, which is the whole of
// the curviness dial at this level.
//
// A step budget rather than a proof: self-avoiding walks can wedge, and a
// wedged search that keeps digging is a generator that sometimes takes a
// minute. On failure the caller retries with fresh dice, then asks for less.
static bool gs_walk_path(gs_rng *r, int nx, int ny, int want, int seedrun,
                         int maxrun, int wkeep, int wturn, gs_walk *out) {
    if (want > nx * ny) want = nx * ny;
    if (want > GS_GEN_CELLS) want = GS_GEN_CELLS;
    if (seedrun > want) seedrun = want;
    if (seedrun < 1) seedrun = 1;

    bool visited[GS_GEN_GRID][GS_GEN_GRID];
    memset(visited, 0, sizeof visited);
    out->count = 0;

    // The seed straight: a direction that fits, then a start it fits from.
    int d = (int)gs_pick(r, 4);
    if ((gs_dx[d] != 0 && seedrun > nx) || (gs_dy[d] != 0 && seedrun > ny)) {
        d = (d + 1) % 4;
    }
    int sx, sy;
    if (gs_dx[d] != 0) {
        int room = nx - seedrun;
        sx = gs_dx[d] > 0
                 ? (int)gs_pick(r, (uint32_t)(room + 1))
                 : seedrun - 1 + (int)gs_pick(r, (uint32_t)(room + 1));
        sy = (int)gs_pick(r, (uint32_t)ny);
    } else {
        int room = ny - seedrun;
        sy = gs_dy[d] > 0
                 ? (int)gs_pick(r, (uint32_t)(room + 1))
                 : seedrun - 1 + (int)gs_pick(r, (uint32_t)(room + 1));
        sx = (int)gs_pick(r, (uint32_t)nx);
    }

    for (int i = 0; i < seedrun; i++) {
        int x = sx + gs_dx[d] * i, y = sy + gs_dy[d] * i;
        out->cx[out->count] = (int8_t)x;
        out->cy[out->count] = (int8_t)y;
        out->count++;
        visited[x][y] = true;
    }

    // One frame per cell placed past the seed: which moves remain to try from
    // it, heaviest-weighted first more often than not.
    typedef struct { int8_t order[3]; int8_t n, next; } gs_frame;
    static gs_frame st[GS_GEN_CELLS];
    int top = -1;
    int lastdir = d;
    int run = seedrun;
    int budget = 500000;

    for (;;) {
        if (out->count >= want) return true;
        if (--budget <= 0) return false;

        if (top < out->count - seedrun) {
            // Arrived at a new cell: build its frame.
            top++;
            gs_frame *f = &st[top];
            int8_t cand[3];
            int wts[3];
            int nc = 0;
            for (int dir = 0; dir < 4; dir++) {
                if (dir == ((lastdir + 2) % 4)) continue;    // no reversing
                int x = out->cx[out->count - 1] + gs_dx[dir];
                int y = out->cy[out->count - 1] + gs_dy[dir];
                if (x < 0 || y < 0 || x >= nx || y >= ny) continue;
                if (visited[x][y]) continue;
                if (dir == lastdir && run >= maxrun) continue;
                cand[nc] = (int8_t)dir;
                wts[nc] = dir == lastdir ? wkeep : wturn;
                nc++;
            }
            // Weighted order: draw without replacement, heavier first more
            // often. Three candidates at most, so this is a handful of draws.
            f->n = (int8_t)nc;
            f->next = 0;
            for (int k = 0; k < nc; k++) {
                int total = 0;
                for (int j = k; j < nc; j++) total += wts[j];
                int roll = (int)gs_pick(r, (uint32_t)total);
                int hit = k;
                for (int j = k; j < nc; j++) {
                    roll -= wts[j];
                    if (roll < 0) { hit = j; break; }
                }
                int8_t tc = cand[k]; cand[k] = cand[hit]; cand[hit] = tc;
                int tw = wts[k]; wts[k] = wts[hit]; wts[hit] = tw;
                f->order[k] = cand[k];
            }
            continue;
        }

        gs_frame *f = &st[top];
        if (f->next >= f->n) {
            // Exhausted here: back out of this cell.
            if (top == 0) return false;
            top--;
            out->count--;
            visited[out->cx[out->count]][out->cy[out->count]] = false;
            // Restore the heading and run the cell we are back on was placed
            // with, by reading them off the walk itself.
            lastdir = d;
            run = seedrun;
            if (out->count > seedrun) {
                int px = out->cx[out->count - 2], py = out->cy[out->count - 2];
                int cx = out->cx[out->count - 1], cy = out->cy[out->count - 1];
                for (int dir = 0; dir < 4; dir++) {
                    if (px + gs_dx[dir] == cx && py + gs_dy[dir] == cy) {
                        lastdir = dir;
                        break;
                    }
                }
                run = 1;
                for (int i = out->count - 2; i > 0; i--) {
                    if (out->cx[i] - out->cx[i - 1] == gs_dx[lastdir] &&
                        out->cy[i] - out->cy[i - 1] == gs_dy[lastdir]) {
                        run++;
                    } else {
                        break;
                    }
                }
            }
            continue;
        }

        int dir = f->order[f->next];
        f->next++;
        int x = out->cx[out->count - 1] + gs_dx[dir];
        int y = out->cy[out->count - 1] + gs_dy[dir];
        if (visited[x][y]) continue;                 // taken since frame built
        if (dir == lastdir && run >= maxrun) continue;
        out->cx[out->count] = (int8_t)x;
        out->cy[out->count] = (int8_t)y;
        out->count++;
        visited[x][y] = true;
        run = dir == lastdir ? run + 1 : 1;
        lastdir = dir;
    }
}

// A cycle over the cells, grown by bumping. `policy` is the straightness dial:
// power protects the runs the cycle has, broken attacks them, balanced lets
// the dice decide.
static bool gs_walk_cycle(gs_rng *r, int nx, int ny, int want,
                          gs_gen_straight policy, gs_walk *out) {
    if (nx < 2 || ny < 2) return false;
    if (want > (nx * ny * 3) / 4) want = (nx * ny * 3) / 4;
    if (want < 4) want = 4;

    bool used[GS_GEN_GRID][GS_GEN_GRID];
    memset(used, 0, sizeof used);

    int bx = (int)gs_pick(r, (uint32_t)(nx - 1));
    int by = (int)gs_pick(r, (uint32_t)(ny - 1));
    out->cx[0] = (int8_t)bx;       out->cy[0] = (int8_t)by;
    out->cx[1] = (int8_t)(bx + 1); out->cy[1] = (int8_t)by;
    out->cx[2] = (int8_t)(bx + 1); out->cy[2] = (int8_t)(by + 1);
    out->cx[3] = (int8_t)bx;       out->cy[3] = (int8_t)(by + 1);
    out->count = 4;
    used[bx][by] = used[bx + 1][by] = true;
    used[bx + 1][by + 1] = used[bx][by + 1] = true;

    while (out->count + 2 <= want) {
        // Every bump the cycle could take right now: an edge, and a side of it
        // where both flanking cells are free.
        typedef struct { int16_t edge; int8_t sx, sy; bool onrun; } gs_bump;
        static gs_bump opt[GS_GEN_CELLS * 2];
        int nopt = 0, nrun = 0;

        for (int i = 0; i < out->count; i++) {
            int j = (i + 1) % out->count;
            int ex = out->cx[j] - out->cx[i], ey = out->cy[j] - out->cy[i];

            // Is this edge part of a straight run of the cycle?
            int h = (i - 1 + out->count) % out->count;
            int k = (j + 1) % out->count;
            bool onrun =
                (out->cx[i] - out->cx[h] == ex &&
                 out->cy[i] - out->cy[h] == ey) ||
                (out->cx[k] - out->cx[j] == ex &&
                 out->cy[k] - out->cy[j] == ey);

            const int side[2][2] = { { -ey, ex }, { ey, -ex } };
            for (int s = 0; s < 2; s++) {
                int ax = out->cx[i] + side[s][0];
                int ay = out->cy[i] + side[s][1];
                int bx2 = out->cx[j] + side[s][0];
                int by2 = out->cy[j] + side[s][1];
                if (ax < 0 || ay < 0 || ax >= nx || ay >= ny) continue;
                if (bx2 < 0 || by2 < 0 || bx2 >= nx || by2 >= ny) continue;
                if (used[ax][ay] || used[bx2][by2]) continue;
                if (nopt >= (int)(sizeof opt / sizeof opt[0])) break;
                opt[nopt] = (gs_bump){ .edge = (int16_t)i,
                                       .sx = (int8_t)side[s][0],
                                       .sy = (int8_t)side[s][1],
                                       .onrun = onrun };
                if (onrun) nrun++;
                nopt++;
            }
        }
        if (nopt == 0) break;                                 // locked

        // The straightness dial, as a preference rather than a rule: prefer
        // the bumps that treat the runs the way the dial asks, fall back to
        // any bump rather than lock.
        bool wantrun = false;
        bool care = true;
        if (policy == GS_STRAIGHT_POWER) wantrun = false;      // keep the runs
        else if (policy == GS_STRAIGHT_BROKEN) wantrun = true; // break them up
        else care = false;

        int matching = wantrun ? nrun : nopt - nrun;
        int choice;
        if (care && matching > 0) {
            int nth = (int)gs_pick(r, (uint32_t)matching);
            choice = 0;
            for (int i = 0; i < nopt; i++) {
                if (opt[i].onrun == wantrun && nth-- == 0) { choice = i; break; }
            }
        } else {
            choice = (int)gs_pick(r, (uint32_t)nopt);
        }

        // Push the edge sideways: ... a, a', b', b ... - still a cycle, two
        // cells longer, by construction.
        int i = opt[choice].edge;
        int j = (i + 1) % out->count;
        int ax = out->cx[i] + opt[choice].sx;
        int ay = out->cy[i] + opt[choice].sy;
        int bx2 = out->cx[j] + opt[choice].sx;
        int by2 = out->cy[j] + opt[choice].sy;

        memmove(&out->cx[i + 3], &out->cx[i + 1],
                (size_t)(out->count - i - 1) * sizeof out->cx[0]);
        memmove(&out->cy[i + 3], &out->cy[i + 1],
                (size_t)(out->count - i - 1) * sizeof out->cy[0]);
        out->cx[i + 1] = (int8_t)ax;  out->cy[i + 1] = (int8_t)ay;
        out->cx[i + 2] = (int8_t)bx2; out->cy[i + 2] = (int8_t)by2;
        out->count += 2;
        used[ax][ay] = true;
        used[bx2][by2] = true;
    }

    return out->count >= 8;    // a bare 2x2 ring is an oval; ask for more
}

// Rotate a cycle so it starts in the middle of its longest straight run - the
// start line, and the grid behind it, want a straight to stand on.
static void gs_walk_rotate_to_straight(gs_walk *w) {
    int best = 0, bestlen = 0;
    for (int i = 0; i < w->count; i++) {
        int ex = w->cx[(i + 1) % w->count] - w->cx[i];
        int ey = w->cy[(i + 1) % w->count] - w->cy[i];
        int len = 1;
        for (int j = 1; j < w->count; j++) {
            int a = (i + j) % w->count, b = (i + j + 1) % w->count;
            if (w->cx[b] - w->cx[a] == ex && w->cy[b] - w->cy[a] == ey) len++;
            else break;
        }
        if (len > bestlen) { bestlen = len; best = i; }
    }

    int shift = (best + bestlen / 2) % w->count;
    if (shift == 0) return;

    static int8_t tx[GS_GEN_CELLS], ty[GS_GEN_CELLS];
    for (int i = 0; i < w->count; i++) {
        tx[i] = w->cx[(i + shift) % w->count];
        ty[i] = w->cy[(i + shift) % w->count];
    }
    memcpy(w->cx, tx, (size_t)w->count * sizeof tx[0]);
    memcpy(w->cy, ty, (size_t)w->count * sizeof ty[0]);
}

// --- from cells to geometry -------------------------------------------------
//
// Cell centres, jittered as far as the pitch allows, become waypoints; every
// corner between waypoints is rounded with an arc whose radius the curviness
// dial draws. What comes out is a dense polyline with distance along it, which
// is all the carving and the gates ever ask for.

typedef struct gs_route_plan {
    gs_fix   px[GS_GEN_PTS], py[GS_GEN_PTS];
    gs_fix   cum[GS_GEN_PTS];     // distance from the start to each point
    uint16_t count;
    gs_fix   total;               // the whole route, in tiles
    bool     loop;
} gs_route_plan;

static void gs_plan_point(gs_route_plan *p, gs_fix x, gs_fix y) {
    if (p->count >= GS_GEN_PTS) return;
    if (p->count > 0 && p->px[p->count - 1] == x && p->py[p->count - 1] == y) {
        return;
    }
    p->px[p->count] = x;
    p->py[p->count] = y;
    p->count++;
}

// An arc from `from`, around a centre, sweeping `turn` angle units, ending
// exactly on `to`. Sampled by rotating the spoke a fixed step at a time; the
// exact endpoint is emitted last so error never accumulates into the next
// straight.
static void gs_plan_arc(gs_route_plan *p, gs_fix cx, gs_fix cy, gs_fix fromx,
                        gs_fix fromy, int32_t turn, gs_fix tox, gs_fix toy) {
    int32_t mag = turn < 0 ? -turn : turn;
    int steps = (int)(mag / 2048) + 1;           // a point every ~11 degrees
    gs_angle step = (gs_angle)(turn / steps);
    gs_fix ca = gs_cos(step), sa = gs_sin(step);

    gs_fix vx = fromx - cx, vy = fromy - cy;
    for (int i = 1; i < steps; i++) {
        gs_fix nvx = gs_fix_mul(vx, ca) - gs_fix_mul(vy, sa);
        gs_fix nvy = gs_fix_mul(vx, sa) + gs_fix_mul(vy, ca);
        vx = nvx;
        vy = nvy;
        gs_plan_point(p, cx + vx, cy + vy);
    }
    gs_plan_point(p, tox, toy);
}

static void gs_plan_close(gs_route_plan *p) {
    p->cum[0] = 0;
    for (uint16_t i = 1; i < p->count; i++) {
        p->cum[i] = p->cum[i - 1] + gs_fix_len2(p->px[i] - p->px[i - 1],
                                                p->py[i] - p->py[i - 1]);
    }
    p->total = p->count > 0 ? p->cum[p->count - 1] : 0;
}

// One corner's rounding: where the arc enters, leaves, turns around and how
// hard. No arc means the corner is straight enough to drive through.
typedef struct gs_corner {
    gs_fix  ex, ey;      // entry - where the incoming straight hands over
    gs_fix  lx, ly;      // leave - where the outgoing straight picks up
    gs_fix  cx, cy;      // the arc's centre
    int32_t turn;        // signed, in angle units
    bool    arc;
} gs_corner;

// Round the corner at C between P and N with radius up to `rad`. The tangent
// length is r*tan(half the turn); when the turn nears a reversal the tangent
// runs away, so past about 78 degrees of half-angle the tangent is pinned to
// what the neighbouring straights can give and the radius derived from it
// instead - the same geometry, without the overflow.
static void gs_round_corner(gs_fix px, gs_fix py, gs_fix cx, gs_fix cy,
                            gs_fix nx, gs_fix ny, gs_fix rad, gs_corner *out) {
    gs_fix inx = cx - px, iny = cy - py;
    gs_fix outx = nx - cx, outy = ny - cy;
    gs_fix inlen = gs_fix_len2(inx, iny);
    gs_fix outlen = gs_fix_len2(outx, outy);
    if (inlen <= 0 || outlen <= 0) {
        out->arc = false;
        out->ex = out->lx = cx;
        out->ey = out->ly = cy;
        return;
    }

    gs_fix uix = gs_fix_div(inx, inlen), uiy = gs_fix_div(iny, inlen);
    gs_fix uox = gs_fix_div(outx, outlen), uoy = gs_fix_div(outy, outlen);

    gs_angle ain = gs_atan2(uiy, uix);
    gs_angle aout = gs_atan2(uoy, uox);
    int32_t turn = (int32_t)(int16_t)(uint16_t)(aout - ain);

    int32_t mag = turn < 0 ? -turn : turn;
    if (mag < 600) {                     // about three degrees: drive through
        out->arc = false;
        out->ex = out->lx = cx;
        out->ey = out->ly = cy;
        return;
    }

    gs_angle half = (gs_angle)(mag / 2);
    gs_fix sh = gs_sin(half), ch = gs_cos(half);

    gs_fix tmax = (inlen < outlen ? inlen : outlen) / 2 - GS_ONE / 4;
    if (tmax < GS_ONE / 4) tmax = GS_ONE / 4;

    gs_fix tlen, r;
    if (ch > GS_RATIO(20, 100)) {
        tlen = gs_fix_mul(rad, gs_fix_div(sh, ch));
        if (tlen > tmax) tlen = tmax;
        r = gs_fix_mul(tlen, gs_fix_div(ch, sh));
    } else {
        tlen = tmax;
        r = gs_fix_mul(tlen, gs_fix_div(ch, sh));
    }

    out->ex = cx - gs_fix_mul(uix, tlen);
    out->ey = cy - gs_fix_mul(uiy, tlen);
    out->lx = cx + gs_fix_mul(uox, tlen);
    out->ly = cy + gs_fix_mul(uoy, tlen);

    // The centre sits a radius to the side the route bends toward: the left
    // normal of the way in when the heading increases, the right when it
    // falls.
    gs_fix nxn = -uiy, nyn = uix;
    if (turn < 0) { nxn = uiy; nyn = -uix; }
    out->cx = out->ex + gs_fix_mul(nxn, r);
    out->cy = out->ey + gs_fix_mul(nyn, r);
    out->turn = turn;
    out->arc = true;
}

// Corner radius by curviness, in tiles: how gently a corner is taken when the
// ground gives it room. Capped per corner by the straights either side.
// The floors matter more than the ceilings: gates are laid about every dozen
// tiles and the corner test bounds how far the route may turn between two of
// them, so a radius small enough to hide a whole right angle between gates is
// a corner the suite refuses. Eight tiles spreads ninety degrees over
// thirteen-plus tiles of arc, which no twelve-tile window sees all of.
static gs_fix gs_corner_radius(gs_rng *r, gs_gen_curve curve) {
    switch (curve) {
    case GS_CURVE_FLOWING:
        return GS_INT(14) + GS_ONE * (gs_fix)gs_pick(r, 5);
    case GS_CURVE_TECHNICAL:
        return GS_INT(8) + GS_ONE * (gs_fix)gs_pick(r, 3);
    case GS_CURVE_WINDING:
    case GS_CURVE_COUNT:
    default:
        return GS_INT(11) + GS_ONE * (gs_fix)gs_pick(r, 4);
    }
}

static void gs_plan_from_walk(const gs_walk *wk, gs_rng *r,
                              const gs_track_spec *spec, int pitch, gs_fix ox,
                              gs_fix oy, int jmax_tiles, bool loop,
                              gs_route_plan *p) {
    static gs_fix wx[GS_GEN_CELLS], wy[GS_GEN_CELLS];
    int m = wk->count;

    // Waypoints: cell centres plus jitter, in quarter tiles so generated
    // geometry lands on the same lattice the editor paints on. The jitter is
    // capped so two corridors a pitch apart can never touch.
    static gs_fix jx[GS_GEN_CELLS], jy[GS_GEN_CELLS];
    int jq = jmax_tiles * 4;
    for (int i = 0; i < m; i++) {
        int qx = jq > 0 ? (int)gs_pick(r, (uint32_t)(2 * jq + 1)) - jq : 0;
        int qy = jq > 0 ? (int)gs_pick(r, (uint32_t)(2 * jq + 1)) - jq : 0;

        // **A power straight is straight.** The walk seeds one at the start
        // of every power path, and two tiles of organic wobble down a drag
        // strip is the dial being taken back a heading at a time. The draws
        // still happen, so every other waypoint lands where it always did.
        if (!loop && spec->straight == GS_STRAIGHT_POWER && i < 8) {
            qx = 0;
            qy = 0;
        }
        jx[i] = (gs_fix)((int64_t)qx * GS_ONE / 4);
        jy[i] = (gs_fix)((int64_t)qy * GS_ONE / 4);
        wx[i] = ox + GS_INT(wk->cx[i] * pitch) + GS_INT(pitch) / 2 + jx[i];
        wy[i] = oy + GS_INT(wk->cy[i] * pitch) + GS_INT(pitch) / 2 + jy[i];
    }

    // **No corner past a right angle.** The grid only ever turns ninety
    // degrees; it is the jitter that can push a corner past it, and a corner
    // past it can put more than ninety between two gates - which is the
    // reversal the suite exists to refuse. Wherever a corner has overturned,
    // the three waypoints that make it give half their jitter back, until it
    // has not: with no jitter at all the corner is the grid's own right
    // angle, so this always settles.
    for (int pass = 0; pass < 10; pass++) {
        bool clean = true;
        int d0 = loop ? 0 : 1;
        int d1 = loop ? m - 1 : m - 2;
        for (int i = d0; i <= d1; i++) {
            int prev = (i - 1 + m) % m;
            int next = (i + 1) % m;
            gs_angle a_in = gs_atan2(wy[i] - wy[prev], wx[i] - wx[prev]);
            gs_angle a_out = gs_atan2(wy[next] - wy[i], wx[next] - wx[i]);
            int32_t turn = (int32_t)(int16_t)(uint16_t)(a_out - a_in);
            if (turn < 0) turn = -turn;
            if (turn <= 16570) continue;             // 91 degrees, and change
            clean = false;
            const int trio[3] = { prev, i, next };
            for (int k = 0; k < 3; k++) {
                int at = trio[k];
                jx[at] /= 2;
                jy[at] /= 2;
                wx[at] = ox + GS_INT(wk->cx[at] * pitch) + GS_INT(pitch) / 2 +
                         jx[at];
                wy[at] = oy + GS_INT(wk->cy[at] * pitch) + GS_INT(pitch) / 2 +
                         jy[at];
            }
        }
        if (clean) break;
        if (pass == 8) {
            // The last word: strip the jitter entirely from anything still
            // over, which is the grid itself and cannot overturn.
            for (int i = 0; i < m; i++) {
                jx[i] = 0;
                jy[i] = 0;
                wx[i] = ox + GS_INT(wk->cx[i] * pitch) + GS_INT(pitch) / 2;
                wy[i] = oy + GS_INT(wk->cy[i] * pitch) + GS_INT(pitch) / 2;
            }
        }
    }

    static gs_corner corner[GS_GEN_CELLS];
    int c0 = loop ? 0 : 1;
    int c1 = loop ? m - 1 : m - 2;
    for (int i = c0; i <= c1; i++) {
        int prev = (i - 1 + m) % m;
        int next = (i + 1) % m;
        gs_fix rad = gs_corner_radius(r, spec->curve);
        gs_round_corner(wx[prev], wy[prev], wx[i], wy[i], wx[next], wy[next],
                        rad, &corner[i]);
    }

    p->count = 0;
    p->loop = loop;

    if (!loop) {
        gs_plan_point(p, wx[0], wy[0]);
        for (int i = 1; i <= m - 2; i++) {
            gs_plan_point(p, corner[i].ex, corner[i].ey);
            if (corner[i].arc) {
                gs_plan_arc(p, corner[i].cx, corner[i].cy, corner[i].ex,
                            corner[i].ey, corner[i].turn, corner[i].lx,
                            corner[i].ly);
            }
        }
        gs_plan_point(p, wx[m - 1], wy[m - 1]);
    } else {
        // Start where corner zero's arc lets go - the middle of the longest
        // straight, thanks to the rotation - and come all the way round to
        // the same point, so the polyline closes exactly.
        gs_plan_point(p, corner[0].lx, corner[0].ly);
        for (int i = 1; i < m; i++) {
            gs_plan_point(p, corner[i].ex, corner[i].ey);
            if (corner[i].arc) {
                gs_plan_arc(p, corner[i].cx, corner[i].cy, corner[i].ex,
                            corner[i].ey, corner[i].turn, corner[i].lx,
                            corner[i].ly);
            }
        }
        gs_plan_point(p, corner[0].ex, corner[0].ey);
        if (corner[0].arc) {
            gs_plan_arc(p, corner[0].cx, corner[0].cy, corner[0].ex,
                        corner[0].ey, corner[0].turn, corner[0].lx,
                        corner[0].ly);
        } else {
            gs_plan_point(p, corner[0].lx, corner[0].ly);
        }
    }

    gs_plan_close(p);
}

// A point on the plan, `along` running from zero to GS_ONE over the whole
// route - by distance, so the samples that carve the road are evenly spaced
// whatever the route is doing.
static void gs_route_at(const gs_route_plan *p, gs_fix along, gs_fix *x,
                        gs_fix *y) {
    *x = 0; *y = 0;
    if (p->count < 2 || p->total <= 0) return;

    if (along < 0) along = 0;
    if (along > GS_ONE) along = GS_ONE;
    gs_fix want = gs_fix_mul(p->total, along);

    uint16_t lo = 0, hi = (uint16_t)(p->count - 1);
    while (lo + 1 < hi) {
        uint16_t mid = (uint16_t)((lo + hi) / 2);
        if (p->cum[mid] < want) lo = mid;
        else hi = mid;
    }

    gs_fix seg = p->cum[hi] - p->cum[lo];
    gs_fix s = seg > 0 ? gs_fix_div(want - p->cum[lo], seg) : 0;
    if (s > GS_ONE) s = GS_ONE;
    *x = p->px[lo] + gs_fix_mul(p->px[hi] - p->px[lo], s);
    *y = p->py[lo] + gs_fix_mul(p->py[hi] - p->py[lo], s);
}

// --- the ground -------------------------------------------------------------

// The relief range, as an amplitude in tiles. The wavelengths below are chosen
// so even the severe band's steepest face stays under what a car can climb -
// and the relax pass after carving holds the line if a stamp lands on a slope.
// Sized to what a car can live with rather than to what looks dramatic: the
// rolling field peaks at about one and a half times this with its octave on
// top, and the first severe band shipped at three tiles wedged cars on ice
// hillsides they could not climb back off. Severe is meant to be felt in the
// suspension, not to end the race.
static gs_fix gs_range_amp(gs_rng *r, gs_gen_range range) {
    switch (range) {
    case GS_RANGE_SUBTLE:
        return GS_RATIO(30, 100) + GS_RATIO(1, 100) * (gs_fix)gs_pick(r, 26);
    case GS_RANGE_SEVERE:
        return GS_RATIO(160, 100) + GS_RATIO(1, 100) * (gs_fix)gs_pick(r, 61);
    case GS_RANGE_MODERATE:
    case GS_RANGE_COUNT:
    default:
        return GS_RATIO(80, 100) + GS_RATIO(1, 100) * (gs_fix)gs_pick(r, 61);
    }
}

// **The relief eases off toward the rim.** A hillside that keeps its full
// height to the very edge of the world is a slope that can point *off* it,
// and on ice a car that runs wide there slides the last twenty tiles without
// a say in the matter - which is how a severe field lost cars over the drop.
// Flat ground at the rim gives a car that has gone wide somewhere to stop.
// The basin keeps its full rim: its slope already points back into the play.
// Flat inside ten tiles of the edge, full relief from forty in: the
// outermost corridor of a route sits about twenty tiles from the rim, and a
// driver fighting full-height severe hills there wanders wide with no world
// left to wander into. Ten to forty puts calm ground under that corridor and
// the whole of the relief under everything inland of it.
static gs_fix gs_rim_ease(const gs_track *t, int32_t x, int32_t y) {
    int32_t d = x;
    if (y < d) d = y;
    if ((int32_t)t->w - x < d) d = (int32_t)t->w - x;
    if ((int32_t)t->h - y < d) d = (int32_t)t->h - y;
    d -= 10;
    if (d >= 30) return GS_ONE;
    if (d < 0) d = 0;
    return (gs_fix)(((int64_t)d * GS_ONE) / 30);
}

// **And past the ease, the world turns up at its edge.** The ease alone was
// not enough: a car that arrives at the rim already sliding - ice, a light
// pocket, a landing - crosses twenty flat tiles without slowing and goes over
// the drop. The basin never lost a car, because its rim rises into the play;
// every generated world gets that now, as a low lip over the last fifteen
// tiles. Gentle enough to drive out onto and be pushed back off.
static void gs_lay_rim(gs_track *t) {
    const gs_fix lip = GS_RATIO(90, 100);
    for (int32_t y = 0; y <= (int32_t)t->h; y++) {
        for (int32_t x = 0; x <= (int32_t)t->w; x++) {
            int32_t d = x;
            if (y < d) d = y;
            if ((int32_t)t->w - x < d) d = (int32_t)t->w - x;
            if ((int32_t)t->h - y < d) d = (int32_t)t->h - y;
            if (d >= 15) continue;
            if (d < 0) d = 0;
            gs_fix q = (gs_fix)(((int64_t)(15 - d) * GS_ONE) / 15);
            gs_fix add = gs_fix_mul(lip, gs_fix_mul(q, q));
            gs_track_set_corner(t, (uint8_t)x, (uint8_t)y,
                                gs_track_corner_at(t, (uint8_t)x, (uint8_t)y) +
                                    add);
        }
    }
}

static void gs_lay_relief(gs_track *t, gs_rng *r, const gs_track_spec *spec) {
    if (spec->relief == GS_RELIEF_FLAT) return;

    gs_fix amp = gs_range_amp(r, spec->range);

    if (spec->relief == GS_RELIEF_BASIN) {
        // High at the rim, low in the middle: a dished field the route climbs
        // in and out of, and one that nudges a wide car back toward the play.
        gs_fix hw = GS_INT(t->w) / 2, hh = GS_INT(t->h) / 2;
        for (int32_t y = 0; y <= (int32_t)t->h; y++) {
            for (int32_t x = 0; x <= (int32_t)t->w; x++) {
                gs_fix qx = gs_fix_div(GS_INT(x) - hw, hw);
                gs_fix qy = gs_fix_div(GS_INT(y) - hh, hh);
                gs_fix q = gs_fix_mul(qx, qx) + gs_fix_mul(qy, qy);
                if (q > GS_ONE) q = GS_ONE;
                gs_track_set_corner(t, (uint8_t)x, (uint8_t)y,
                                    gs_fix_mul(amp, q));
            }
        }
        return;
    }

    if (spec->relief == GS_RELIEF_RIDGED) {
        // Parallel ridges at an angle the world's axes know nothing about.
        gs_angle dir = (gs_angle)gs_pick(r, 65536);
        gs_angle phase = (gs_angle)gs_pick(r, 65536);
        int32_t lambda = 48 + (int32_t)gs_pick(r, 33);
        gs_fix c = gs_cos(dir), s = gs_sin(dir);
        for (int32_t y = 0; y <= (int32_t)t->h; y++) {
            for (int32_t x = 0; x <= (int32_t)t->w; x++) {
                gs_fix proj =
                    gs_fix_mul(GS_INT(x), c) + gs_fix_mul(GS_INT(y), s);
                gs_angle a = (gs_angle)(uint32_t)((int64_t)proj / lambda);
                gs_fix hgt = gs_fix_mul(amp, gs_sin((gs_angle)(a + phase)));
                gs_track_set_corner(t, (uint8_t)x, (uint8_t)y,
                                    gs_fix_mul(hgt, gs_rim_ease(t, x, y)));
            }
        }
        return;
    }

    // Rolling: two octaves of crossed sine, which is hills without a grain.
    int32_t l1 = 48 + (int32_t)gs_pick(r, 33);
    int32_t l2 = l1 / 2 < 24 ? 24 : l1 / 2;
    gs_angle p1 = (gs_angle)gs_pick(r, 65536);
    gs_angle p2 = (gs_angle)gs_pick(r, 65536);
    gs_angle p3 = (gs_angle)gs_pick(r, 65536);
    gs_angle p4 = (gs_angle)gs_pick(r, 65536);
    for (int32_t y = 0; y <= (int32_t)t->h; y++) {
        for (int32_t x = 0; x <= (int32_t)t->w; x++) {
            gs_angle ax1 = (gs_angle)((uint32_t)x * 65536u / (uint32_t)l1);
            gs_angle ay1 = (gs_angle)((uint32_t)y * 65536u / (uint32_t)l1);
            gs_angle ax2 = (gs_angle)((uint32_t)x * 65536u / (uint32_t)l2);
            gs_angle ay2 = (gs_angle)((uint32_t)y * 65536u / (uint32_t)l2);
            gs_fix hgt = gs_fix_mul(amp,
                             gs_fix_mul(gs_sin((gs_angle)(ax1 + p1)),
                                        gs_sin((gs_angle)(ay1 + p2)))) +
                         gs_fix_mul(amp / 2,
                             gs_fix_mul(gs_sin((gs_angle)(ax2 + p3)),
                                        gs_sin((gs_angle)(ay2 + p4))));
            gs_track_set_corner(t, (uint8_t)x, (uint8_t)y,
                                gs_fix_mul(hgt, gs_rim_ease(t, x, y)));
        }
    }
}

// A round hill stamped on the route, which the carve then cuts a road through:
// the along-route profile survives the cut, and that is a jump. Radius at
// least three times the height, so the faces are climbable before the relax
// pass has to say anything.
static void gs_stamp_hill(gs_track *t, gs_fix cx, gs_fix cy, int radius,
                          gs_fix height) {
    gs_fix r2 = GS_INT(radius * radius);
    int32_t tx = gs_fix_floor(cx), ty = gs_fix_floor(cy);
    for (int32_t y = ty - radius; y <= ty + radius + 1; y++) {
        for (int32_t x = tx - radius; x <= tx + radius + 1; x++) {
            if (x < 0 || y < 0 || x > (int32_t)t->w || y > (int32_t)t->h) {
                continue;
            }
            gs_fix dx = GS_INT(x) - cx, dy = GS_INT(y) - cy;
            gs_fix d2 = gs_fix_mul(dx, dx) + gs_fix_mul(dy, dy);
            if (d2 >= r2) continue;
            gs_fix q = gs_fix_div(d2, r2);
            gs_fix add = gs_fix_mul(height, GS_ONE - q);
            gs_track_set_corner(t, (uint8_t)x, (uint8_t)y,
                                gs_track_corner_at(t, (uint8_t)x, (uint8_t)y) +
                                    add);
        }
    }
}

// How much clear route a car gets before the first thing in its way. A grid
// stands at the start and a standing start crests nothing; jumps keep off the
// opening stretch.
#define GS_GEN_RUNUP 45

static void gs_lay_jumps(gs_track *t, gs_rng *r, const gs_track_spec *spec,
                         const gs_route_plan *plan) {
    if (spec->jumps == GS_JUMPS_NONE) return;

    bool big = spec->jumps == GS_JUMPS_BIG;
    int32_t spacing = big ? 130 + (int32_t)gs_pick(r, 41)
                          : 90 + (int32_t)gs_pick(r, 31);
    int32_t route = (int32_t)(plan->total / GS_ONE);
    int32_t tail = plan->loop ? 0 : 25;
    int32_t usable = route - GS_GEN_RUNUP - tail;
    if (usable < spacing) return;

    int32_t n = usable / spacing;
    for (int32_t i = 0; i < n; i++) {
        int32_t jitter = (int32_t)gs_pick(r, (uint32_t)(spacing / 5 + 1)) -
                         spacing / 10;
        int32_t at = GS_GEN_RUNUP + i * spacing + spacing / 2 + jitter;
        if (at < GS_GEN_RUNUP || at > route - tail) continue;

        gs_fix height =
            big ? GS_RATIO(200, 100) +
                      GS_RATIO(1, 100) * (gs_fix)gs_pick(r, 101)
                : GS_RATIO(90, 100) +
                      GS_RATIO(1, 100) * (gs_fix)gs_pick(r, 41);
        int radius = big ? 9 + (int)gs_pick(r, 3) : 5 + (int)gs_pick(r, 3);

        // A ramp on ground that is already severe launches off the sum of the
        // two, and the sum is what threw cars off the world. Two thirds keeps
        // the band's character without the compound interest.
        if (spec->range == GS_RANGE_SEVERE) {
            height = (gs_fix)((int64_t)height * 2 / 3);
        }

        gs_fix along = gs_fix_div(GS_INT(at), plan->total);
        gs_fix cx, cy;
        gs_route_at(plan, along, &cx, &cy);
        gs_stamp_hill(t, cx, cy, radius, height);
    }
}

static void gs_lay_gravity(gs_track *t, gs_rng *r, const gs_track_spec *spec,
                           const gs_route_plan *plan) {
    if (spec->gravity == GS_GRAV_EARTH) return;

    // **The one veto that lives at paint time**: big jumps put a floor of
    // three quarters under light gravity, because a third of Earth under a
    // big ramp is a car that leaves the world - and the bar a track has to
    // clear before shipping would only throw the seed away rather than make a
    // better one.
    bool floor75 = spec->jumps == GS_JUMPS_BIG;

    if (spec->gravity == GS_GRAV_SPLIT) {
        // Half the world one way, the other half left at Earth, split at an
        // angle the route crosses over and over.
        gs_angle dir = (gs_angle)gs_pick(r, 65536);
        gs_fix c = gs_cos(dir), s = gs_sin(dir);
        gs_fix cx = GS_INT(t->w) / 2, cy = GS_INT(t->h) / 2;
        uint32_t lightside = gs_pick(r, 2);
        uint32_t pc = floor75 ? 75u + gs_pick(r, 11) : 65u + gs_pick(r, 21);
        uint32_t hc = 120u + gs_pick(r, 41);
        gs_fix mul = lightside != 0 ? GS_RATIO((int32_t)pc, 100)
                                    : GS_RATIO((int32_t)hc, 100);
        for (int32_t y = 0; y < (int32_t)t->h; y++) {
            for (int32_t x = 0; x < (int32_t)t->w; x++) {
                gs_fix rel = gs_fix_mul(GS_INT(x) + GS_ONE / 2 - cx, c) +
                             gs_fix_mul(GS_INT(y) + GS_ONE / 2 - cy, s);
                if (rel > 0) {
                    gs_track_set_gravity(t, (uint8_t)x, (uint8_t)y, mul);
                }
            }
        }
        return;
    }

    // Pockets, centred on the route so they are driven through rather than
    // decorating a corner of the map.
    uint32_t pockets = 1 + gs_pick(r, 3);
    for (uint32_t i = 0; i < pockets; i++) {
        gs_fix along = (gs_fix)gs_pick(r, 65536);
        int radius = 10 + (int)gs_pick(r, 7);
        uint32_t pc;
        if (spec->gravity == GS_GRAV_LIGHT) {
            pc = floor75 ? 75u + gs_pick(r, 11) : 35u + gs_pick(r, 41);
        } else {
            pc = 130u + gs_pick(r, 71);
        }
        gs_fix mul = GS_RATIO((int32_t)pc, 100);

        gs_fix cx, cy;
        gs_route_at(plan, along, &cx, &cy);
        int32_t tx = gs_fix_floor(cx), ty = gs_fix_floor(cy);
        for (int32_t y = ty - radius; y <= ty + radius; y++) {
            for (int32_t x = tx - radius; x <= tx + radius; x++) {
                if (x < 0 || y < 0 || x >= (int32_t)t->w ||
                    y >= (int32_t)t->h) {
                    continue;
                }
                int32_t dx = x - tx, dy = y - ty;
                if (dx * dx + dy * dy > radius * radius) continue;
                gs_track_set_gravity(t, (uint8_t)x, (uint8_t)y, mul);
            }
        }
    }
}

// A surface that is not this one, for dressing: same draw, nudged off the base
// so a band or a patch is always visible against the ground it sits on.
static gs_surface gs_other_surface(gs_rng *r, gs_surface base) {
    gs_surface s = (gs_surface)gs_pick(r, GS_SURF_COUNT);
    if (s == base) s = (gs_surface)((base + 1) % GS_SURF_COUNT);
    return s;
}

static void gs_lay_dress(gs_track *t, gs_rng *r, const gs_track_spec *spec,
                         const gs_route_plan *plan) {
    if (spec->dress == GS_DRESS_PLAIN) return;

    if (spec->dress == GS_DRESS_BANDED) {
        gs_surface s2 = gs_other_surface(r, spec->base);
        bool across_x = gs_pick(r, 2) == 0;
        uint32_t bands = 2 + gs_pick(r, 3);
        for (uint32_t i = 0; i < bands; i++) {
            int32_t dim = across_x ? (int32_t)t->w : (int32_t)t->h;
            int32_t from = (int32_t)gs_pick(r, (uint32_t)dim);
            int32_t wide = 10 + (int32_t)gs_pick(r, 11);
            for (int32_t a = from; a < from + wide && a < dim; a++) {
                int32_t other = across_x ? (int32_t)t->h : (int32_t)t->w;
                for (int32_t b = 0; b < other; b++) {
                    uint8_t x = (uint8_t)(across_x ? a : b);
                    uint8_t y = (uint8_t)(across_x ? b : a);
                    gs_track_set_surface(t, x, y, s2);
                }
            }
        }
        return;
    }

    // Patchwork: discs of foreign ground centred on the route, so the surface
    // changes under the car rather than off in the scenery.
    uint32_t patches = 3 + gs_pick(r, 3);
    for (uint32_t i = 0; i < patches; i++) {
        gs_surface s2 = gs_other_surface(r, spec->base);
        gs_fix along = (gs_fix)gs_pick(r, 65536);
        int radius = 12 + (int)gs_pick(r, 9);

        gs_fix cx, cy;
        gs_route_at(plan, along, &cx, &cy);
        int32_t tx = gs_fix_floor(cx), ty = gs_fix_floor(cy);
        for (int32_t y = ty - radius; y <= ty + radius; y++) {
            for (int32_t x = tx - radius; x <= tx + radius; x++) {
                if (x < 0 || y < 0 || x >= (int32_t)t->w ||
                    y >= (int32_t)t->h) {
                    continue;
                }
                int32_t dx = x - tx, dy = y - ty;
                if (dx * dx + dy * dy > radius * radius) continue;
                gs_track_set_surface(t, (uint8_t)x, (uint8_t)y, s2);
            }
        }
    }
}

// --- carving, relaxing, gates -----------------------------------------------
//
// Unchanged in spirit from the first generator that carved: the route is cut
// *through* the terrain rather than dropped on top of it.

// Flatten the road across its width without flattening it along its length,
// and let it back down into the terrain either side.
//
// **A road is level across and not along.** The terrain is laid first and the
// route cut through it, so a ridge in the way becomes a ramp up one side and
// down the other - which is a jump, and jumps are the point - while the car is
// never tipped sideways by ground that happens to fall away under one set of
// wheels.
//
// **The verge is not decoration.** Stamping the road's level straight into the
// ground leaves a cliff at its edge wherever the road and the hillside
// disagree. The level is blended out over the verge instead, so the road meets
// the hillside on a slope something can drive up.
//
// Two passes, because one is order-dependent: the first records, for every
// corner, the nearest point of the route and what height the road has there;
// the second applies it. What comes out does not depend on which end the route
// was walked from.
static void gs_carve(gs_track *t, const gs_route_plan *plan, gs_surface road,
                     int half) {
    const int32_t reach = half + GS_GEN_VERGE;

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
                if (x < 0 || y < 0 || x > (int32_t)t->w ||
                    y > (int32_t)t->h) {
                    continue;
                }

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
            if (d <= half) {
                want = level[at];
            } else {
                // Out across the verge, back to whatever the ground was.
                gs_fix k = (gs_fix)(((int64_t)(d - half) * GS_ONE) /
                                    GS_GEN_VERGE);
                want = level[at] + gs_fix_mul(was - level[at], k);
            }
            gs_track_set_corner(t, (uint8_t)x, (uint8_t)y, want);

            // **And the road is a surface, not only a height.** Flattening
            // alone leaves a route you cannot see. Only the road itself, not
            // the verge, or the edge of it goes soft too.
            if (d <= half && x < (int32_t)t->w && y < (int32_t)t->h) {
                gs_track_set_surface(t, (uint8_t)x, (uint8_t)y, road);
            }
        }
    }
}

// **Nothing anywhere may be steeper than a car can climb.** Wherever two
// neighbouring corners differ by more than the limit, both move half the
// excess toward each other, repeatedly, until nothing moves - the excess
// spreads outward until there is room for it, which is what a hillside does
// anyway. The limit is the car's, less an eighth: sitting exactly on it means
// a car that can *just* climb every slope, and "just" is not a margin.
static void gs_relax(gs_track *t) {
    const gs_fix limit = GS_MAX_CLIMB - GS_MAX_CLIMB / 8;

    for (int pass = 0; pass < 96; pass++) {
        bool moved = false;

        for (int32_t y = 0; y <= (int32_t)t->h; y++) {
            for (int32_t x = 1; x <= (int32_t)t->w; x++) {
                gs_fix a = gs_track_corner_at(t, (uint8_t)(x - 1), (uint8_t)y);
                gs_fix b = gs_track_corner_at(t, (uint8_t)x, (uint8_t)y);
                gs_fix d = b - a;
                gs_fix over = (d > limit) ? d - limit
                                          : (d < -limit ? d + limit : 0);
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
                gs_fix over = (d > limit) ? d - limit
                                          : (d < -limit ? d + limit : 0);
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
// the way a car is travelling when it arrives. One every dozen tiles or so:
// close enough together that the gates describe the route rather than merely
// marking it, and **always wider than the road** - a gate narrower than the
// road it crosses is a checkpoint a car can pass beside, and gates count in
// order, so one missed gate is a finish line that does nothing.
static void gs_lay_gates(gs_track *t, gs_rng *r, const gs_route_plan *plan,
                         int half) {
    bool loop = plan->loop;
    int want = (int)(plan->total / GS_INT(12)) + (int)gs_pick(r, 3);
    if (want > GS_TRACK_MAX_GATES - 4) want = GS_TRACK_MAX_GATES - 4;
    if (want < 6) want = 6;
    uint8_t gates = (uint8_t)want;

    gs_fix wide = GS_INT(half + 2) + (gs_fix)((int64_t)GS_ONE * gs_pick(r, 2));

    for (uint8_t i = 0; i < gates; i++) {
        gs_fix along, ahead;
        if (loop) {
            along = (gs_fix)(((int64_t)i * 65536) / gates);
            ahead = along + (gs_fix)(65536 / (gates * 8));
        } else {
            along = (gs_fix)(((int64_t)i * GS_ONE) / (gates - 1));
            ahead = along + GS_ONE / (gates * 8);
            if (ahead > GS_ONE) {
                // The last gate faces the way the road was going as it
                // arrived, so a finish line is square to the road and not to
                // the world.
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

// --- the whole track --------------------------------------------------------

// How far a race in each length band runs, in tiles actually raced. The floor
// is GS_STOCK_MIN_ROUTE; the epic ceiling is the field's - GS_TRACK_MAX bounds
// the world, and about twelve hundred tiles is what an organic route can spend
// inside it without folding into mush.
// Drawn a few percent over the bands the header promises, because the
// geometry always spends less than the plan: fillets cut the corners short
// and a walk that cannot quite fill its grid gives back a couple of cells.
// Measured over the bands: what comes out lands on the promise.
static const int32_t gs_len_lo[GS_LEN_COUNT] = { 680, 840, 1030 };
static const int32_t gs_len_hi[GS_LEN_COUNT] = { 840, 1030, 1230 };

// The cell pitch the curviness dial would like, before the length dial has its
// say: bigger cells are longer runs and wider hairpins.
static int gs_pitch_pref(const gs_track_spec *s) {
    static const int circuit[GS_CURVE_COUNT] = { 40, 30, 24 };
    static const int path[GS_CURVE_COUNT] = { 32, 26, 21 };
    return s->kind == GS_CLASS_CIRCUIT ? circuit[s->curve] : path[s->curve];
}

void gs_generate_from_spec(gs_track *t, uint32_t seed,
                           const gs_track_spec *spec) {
    // Its own stream, distinct from the one the spec was drawn from, so the
    // spec's draws and the ground's draws never shift each other.
    gs_rng r = { (seed != 0 ? seed : 0x9e3779b9u) ^ 0x51ed2701u };
    for (int i = 0; i < 4; i++) gs_next(&r);

    // Most of the board, always: the length floor needs the room, whatever
    // the dials say. A little variation so the sets do not all frame
    // identically - except an epic draw, which takes the biggest fields
    // going, because an epic path is capped by how many cells the field
    // holds and nothing else.
    uint8_t w, h;
    if (spec->length == GS_LEN_EPIC) {
        w = (uint8_t)(184 + gs_pick(&r, 2) * 4);
        h = (uint8_t)(184 + gs_pick(&r, 2) * 4);
    } else {
        w = (uint8_t)(176 + gs_pick(&r, 4) * 4);
        h = (uint8_t)(176 + gs_pick(&r, 4) * 4);
    }
    gs_track_init(t, w, h, spec->base);

    gs_lay_relief(t, &r, spec);
    gs_lay_rim(t);

    // How far this race runs, and what the route has to supply of it: all of
    // it for a path, a lap's worth - a third - for a circuit.
    int32_t lo = gs_len_lo[spec->length], hi = gs_len_hi[spec->length];
    int32_t race = lo + (int32_t)gs_pick(&r, (uint32_t)(hi - lo + 1));
    int32_t route = spec->kind == GS_CLASS_CIRCUIT ? race / GS_STOCK_LAPS
                                                   : race;

    // The pitch: what the curviness dial prefers, shrunk until the grid can
    // actually hold a route this long. Length wins the argument, because a
    // short track is broken and a tight one is merely harder.
    int usable_x = (int)t->w - 2 * GS_GEN_INSET;
    int usable_y = (int)t->h - 2 * GS_GEN_INSET;
    int pitch = gs_pitch_pref(spec);
    int nx, ny, wantcells;
    bool loop = spec->kind == GS_CLASS_CIRCUIT;
    for (;;) {
        nx = usable_x / pitch;
        ny = usable_y / pitch;
        if (nx > GS_GEN_GRID) nx = GS_GEN_GRID;
        if (ny > GS_GEN_GRID) ny = GS_GEN_GRID;
        // Fillets cut corners short, so ask for a little over the target.
        wantcells = (route * 112 / 100) / pitch;
        if (loop) {
            wantcells = (wantcells + 1) & ~1;              // growth adds pairs
            if (wantcells < 8) wantcells = 8;
            if (wantcells <= (nx * ny * 3) / 4) break;
        } else {
            if (wantcells < 10) wantcells = 10;
            if (wantcells <= (nx * ny * 9) / 10) break;
        }
        if (pitch <= 18) {                       // the field has said its last
            wantcells = loop ? (nx * ny * 3) / 4 : (nx * ny * 9) / 10;
            if (loop) wantcells &= ~1;
            break;
        }
        pitch -= 2;
    }

    // Grow the walk. Fresh dice per attempt, fewer cells when even fresh dice
    // cannot make it fit - a track slightly short of its band is a track; a
    // generator that never returns is not.
    static gs_walk walk;
    int want = wantcells;
    bool grown = false;
    for (int attempt = 0; attempt < 24 && !grown; attempt++) {
        gs_rng wr = { gs_next(&r) ^ (0x9e3779b9u * (uint32_t)(attempt + 1)) };
        if (wr.s == 0) wr.s = 0x2545f491u;

        if (loop) {
            grown = gs_walk_cycle(&wr, nx, ny, want, spec->straight, &walk);
        } else {
            static const int wkeep[GS_CURVE_COUNT] = { 7, 2, 1 };
            static const int wturn[GS_CURVE_COUNT] = { 1, 1, 3 };
            static const int maxrun[GS_STRAIGHT_COUNT] = { 2, 4, 7 };

            // The run cap gives before the length does: a broken-straights
            // walk that cannot fill the grid at runs of two gets three, then
            // four, rather than shipping an epic path two thirds the length
            // its band promised. The dial softens; the track stays a track.
            int cap = maxrun[spec->straight];
            if (attempt >= 12) cap++;
            if (attempt >= 18) cap++;

            int seedrun = spec->straight == GS_STRAIGHT_POWER ? 7 : 3;
            if (seedrun > nx - 1) seedrun = nx - 1;
            if (seedrun > cap) seedrun = cap;
            grown = gs_walk_path(&wr, nx, ny, want, seedrun, cap,
                                 wkeep[spec->curve], wturn[spec->curve],
                                 &walk);
        }

        if (!grown && attempt >= 9 && attempt % 3 == 2 && want > 10) want -= 2;
    }
    if (!grown) {
        // The deterministic last resort: a plain ring or edge-hugging path of
        // the whole grid, which always exists. In practice the attempts above
        // do not all fail; this is here so the function has no failure mode.
        walk.count = 0;
        for (int x = 0; x < nx; x++) {
            walk.cx[walk.count] = (int8_t)x;
            walk.cy[walk.count++] = 0;
        }
        for (int y = 1; y < ny; y++) {
            walk.cx[walk.count] = (int8_t)(nx - 1);
            walk.cy[walk.count++] = (int8_t)y;
        }
        if (loop) {
            for (int x = nx - 2; x >= 0; x--) {
                walk.cx[walk.count] = (int8_t)x;
                walk.cy[walk.count++] = (int8_t)(ny - 1);
            }
            for (int y = ny - 2; y >= 1; y--) {
                walk.cx[walk.count] = 0;
                walk.cy[walk.count++] = (int8_t)y;
            }
        }
    }
    if (loop) gs_walk_rotate_to_straight(&walk);

    // Centre the cell grid in the field, and jitter the waypoints as far as
    // the corridors allow without touching.
    int half = (int)gs_spec_road(spec);
    gs_fix ox = GS_INT(GS_GEN_INSET) + GS_INT(usable_x - nx * pitch) / 2;
    gs_fix oy = GS_INT(GS_GEN_INSET) + GS_INT(usable_y - ny * pitch) / 2;
    static const int jdiv[GS_CURVE_COUNT] = { 8, 6, 4 };
    int jmax = (pitch - 2 * half - 2) / 2;
    int jlegs = (pitch - 17) / 2;      // legs stay long enough for the fillet
    if (jlegs < jmax) jmax = jlegs;
    int jwant = pitch / jdiv[spec->curve];
    if (jwant < jmax) jmax = jwant;
    if (jmax < 0) jmax = 0;

    static gs_route_plan plan;
    gs_plan_from_walk(&walk, &r, spec, pitch, ox, oy, jmax, loop, &plan);

    gs_lay_jumps(t, &r, spec, &plan);
    gs_lay_dress(t, &r, spec, &plan);
    gs_lay_gravity(t, &r, spec, &plan);

    // What the road is made of. Never the same as the ground it crosses, or
    // there is no road to see - and drawn from the surfaces a road is
    // plausibly made of, because a route surfaced in slush is a joke the
    // first time and an unraceable track every time after.
    static const gs_surface made[] = {
        GS_SURF_PAVEMENT, GS_SURF_DIRT, GS_SURF_GRAVEL, GS_SURF_ROCK,
    };
    gs_surface road = made[gs_pick(&r, 4)];
    if (road == spec->base) {
        road = spec->base == GS_SURF_PAVEMENT ? GS_SURF_DIRT
                                              : GS_SURF_PAVEMENT;
    }

    gs_carve(t, &plan, road, half);
    gs_relax(t);
    gs_lay_gates(t, &r, &plan, half);
}

void gs_generate(gs_track *t, uint32_t seed) {
    gs_track_spec spec = gs_generate_spec_for(seed);
    gs_generate_from_spec(t, seed, &spec);
}

// --- naming ----------------------------------------------------------------
//
// Two words from the seed. "Seed 2864434397" is not something anybody repeats
// out loud, and a name that comes back the same every time is worth more than
// a name that is merely pretty.

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
    n = gs_say(out, cap, n, a);
    n = gs_say(out, cap, n, " ");
    n = gs_say(out, cap, n, b);
    if (cap > 0) out[n < cap ? n : cap - 1] = '\0';
}

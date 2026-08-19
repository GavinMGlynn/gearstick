// test_gearstick.c - the facts the simulation is required to keep true.
//
// Tests are named as sentences stating the fact they pin, so a failure reads as
// a claim that stopped being true rather than as a function that went red.
//
// This file may use floating point; src/core/ may not. Comparing the fixed
// point trigonometry against double precision is exactly the job that needs it,
// and doing it here is why the rule can be absolute over there.

#include <math.h>
#include <stdio.h>
#include <string.h>

// C23 with extensions off does not declare M_PI. The reference value for
// the trigonometry test has to come from somewhere, so it comes from here.
#define GS_PI 3.14159265358979323846

#include "core/gs_ai.h"
#include "core/gs_analyse.h"
#include "core/gs_clock.h"
#include "core/gs_edit.h"
#include "core/gs_ghost.h"
#include "core/gs_net.h"
#include "core/gs_pack.h"
#include "core/gs_share.h"
#include "core/gs_replay.h"
#include "core/gs_sim.h"
#include "core/gs_track.h"

static int gs_failures = 0;
static const char *gs_current = "";

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("  FAIL %s\n    %s:%d: %s\n", gs_current, __FILE__,         \
                   __LINE__, #cond);                                           \
            gs_failures++;                                                     \
        }                                                                      \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                  \
    do {                                                                       \
        double _a = (a), _b = (b);                                             \
        if (fabs(_a - _b) > (tol)) {                                           \
            printf("  FAIL %s\n    %s:%d: %s = %.6f, expected %.6f +- %.6f\n", \
                   gs_current, __FILE__, __LINE__, #a, _a, _b, (double)(tol)); \
            gs_failures++;                                                     \
        }                                                                      \
    } while (0)

#define TEST(name)                                                             \
    static void name(void);                                                    \
    static void run_##name(void) {                                             \
        gs_current = #name;                                                    \
        name();                                                                \
    }                                                                          \
    static void name(void)

static double gs_to_double(gs_fix v) { return (double)v / (double)GS_ONE; }

// ---------------------------------------------------------------------------
// Arithmetic
// ---------------------------------------------------------------------------

TEST(the_trigonometry_agrees_with_double_precision_across_a_full_turn) {
    double worst = 0.0;
    for (int32_t a = 0; a < 65536; a += 7) {
        double want = sin((double)a / 65536.0 * 2.0 * GS_PI);
        double got = gs_to_double(gs_sin((gs_angle)a));
        double err = fabs(want - got);
        if (err > worst) worst = err;
    }
    // The table is 1024 steps to the quarter with the low bits interpolated;
    // anything above 1e-4 means the interpolation is wrong, not coarse.
    CHECK(worst < 1e-4);

    CHECK(gs_sin(0) == 0);
    CHECK(gs_sin(GS_QUARTER) == GS_ONE);
    CHECK(gs_cos(0) == GS_ONE);
    CHECK(gs_sin((gs_angle)(GS_QUARTER * 3)) == -GS_ONE);
}

TEST(atan2_inverts_the_sine_and_cosine_it_was_built_beside) {
    int32_t worst = 0;
    for (int32_t a = 0; a < 65536; a += 13) {
        gs_fix x = gs_fix_mul(gs_cos((gs_angle)a), GS_INT(100));
        gs_fix y = gs_fix_mul(gs_sin((gs_angle)a), GS_INT(100));
        gs_angle got = gs_atan2(y, x);
        int32_t err = gs_angle_delta((gs_angle)a, got);
        if (err < 0) err = -err;
        if (err > worst) worst = err;
    }
    // 65536 units to the turn, so 40 units is a fifth of a degree.
    CHECK(worst < 40);
    CHECK(gs_atan2(0, 0) == 0);
}

TEST(square_root_is_exact_on_squares_and_monotone_between_them) {
    for (int32_t n = 1; n <= 100; n++) {
        gs_fix want = GS_INT(n);
        gs_fix got = gs_fix_sqrt(gs_fix_mul(want, want));
        CHECK(got >= want - 2 && got <= want + 2);
    }
    CHECK(gs_fix_sqrt(0) == 0);
    CHECK(gs_fix_sqrt(-GS_ONE) == 0);
    CHECK_NEAR(gs_to_double(gs_fix_len2(GS_INT(3), GS_INT(4))), 5.0, 1e-3);
}

// ---------------------------------------------------------------------------
// The ground
// ---------------------------------------------------------------------------

// A ramp climbing `rise` tiles between x = x0 and x = x1, flat either side.
static void gs_build_ramp(gs_track *t, uint8_t x0, uint8_t x1, gs_fix rise) {
    gs_track_init(t, 32, 8, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= t->h; y++) {
        for (uint8_t x = 0; x <= t->w; x++) {
            gs_fix h;
            if (x <= x0) h = 0;
            else if (x >= x1) h = rise;
            else h = (gs_fix)((int64_t)rise * (x - x0) / (x1 - x0));
            gs_track_set_corner(t, x, y, h);
        }
    }
}

TEST(the_ground_is_continuous_across_a_tile_boundary) {
    static gs_track t;
    gs_build_ramp(&t, 8, 12, GS_INT(1));

    // Either side of the join at x = 10, a thousandth of a tile apart. A tile
    // atlas would step here; corner heights cannot.
    gs_fix below = gs_track_height(&t, GS_INT(10) - GS_RATIO(1, 1000), GS_INT(4));
    gs_fix above = gs_track_height(&t, GS_INT(10) + GS_RATIO(1, 1000), GS_INT(4));
    CHECK_NEAR(gs_to_double(below), gs_to_double(above), 0.002);

    // And the ramp really does rise a quarter tile per tile.
    CHECK_NEAR(gs_to_double(gs_track_height(&t, GS_INT(9), GS_INT(4))), 0.25, 0.001);
    CHECK_NEAR(gs_to_double(gs_track_height(&t, GS_INT(11), GS_INT(4))), 0.75, 0.001);
}

TEST(a_flat_tile_has_no_slope_and_a_ramp_has_the_slope_it_was_built_with) {
    static gs_track t;
    gs_build_ramp(&t, 8, 12, GS_INT(1));

    gs_fix dzdx = 0, dzdy = 0;
    gs_track_slope(&t, GS_INT(2), GS_INT(4), &dzdx, &dzdy);
    CHECK(dzdx == 0 && dzdy == 0);

    gs_track_slope(&t, GS_INT(10), GS_INT(4), &dzdx, &dzdy);
    CHECK_NEAR(gs_to_double(dzdx), 0.25, 0.001);
    CHECK_NEAR(gs_to_double(dzdy), 0.0, 0.001);
}

TEST(a_track_edit_changes_its_identity_and_an_undone_edit_restores_it) {
    static gs_track a, b;
    gs_track_init(&a, 16, 16, GS_SURF_DIRT);
    gs_track_init(&b, 16, 16, GS_SURF_DIRT);

    // Two tracks built independently to the same design are the same track.
    CHECK(gs_track_hash(&a) == gs_track_hash(&b));

    uint64_t before = gs_track_hash(&a);
    gs_track_set_corner(&a, 3, 3, GS_INT(2));
    CHECK(gs_track_hash(&a) != before);
    gs_track_set_corner(&a, 3, 3, 0);
    CHECK(gs_track_hash(&a) == before);

    gs_track_set_surface(&a, 5, 5, GS_SURF_ICE);
    CHECK(gs_track_hash(&a) != before);
}

TEST(a_track_survives_the_round_trip_through_its_file_format) {
    static gs_track built, loaded;
    static uint8_t buf[GS_TRACK_TILES * 4 + 4096];

    gs_build_ramp(&built, 8, 12, GS_INT(1));
    for (uint8_t x = 16; x < 24; x++)
        for (uint8_t y = 0; y < built.h; y++)
            gs_track_set_surface(&built, x, y, GS_SURF_ICE);
    for (uint8_t x = 4; x < 9; x++)
        for (uint8_t y = 0; y < built.h; y++)
            gs_track_set_gravity(&built, x, y, GS_RATIO(35, 100));
    // Ground below the datum, so the sign of a corner height has to survive too.
    gs_track_set_corner(&built, 2, 2, GS_INT(-3));

    size_t n = gs_track_serialize(&built, buf, sizeof buf);
    CHECK(n == gs_track_size(&built));
    CHECK(n > 0);

    CHECK(gs_track_deserialize(&loaded, buf, n));
    CHECK(loaded.w == built.w && loaded.h == built.h);

    // The hash is the identity, so this single line is the round-trip test:
    // every corner, every surface and every painted gravity value, or it fails.
    CHECK(gs_track_hash(&loaded) == gs_track_hash(&built));
    CHECK(gs_track_height(&loaded, GS_INT(2), GS_INT(2)) ==
          gs_track_height(&built, GS_INT(2), GS_INT(2)));

    // Only the used region is written - a 32x8 track is not a 22 KB file.
    CHECK(n < 2048);
}

TEST(a_corrupt_track_file_is_refused_rather_than_half_loaded) {
    static gs_track built, target;
    static uint8_t buf[GS_TRACK_TILES * 4 + 4096];

    gs_track_init(&built, 20, 20, GS_SURF_DIRT);
    gs_track_set_corner(&built, 5, 5, GS_INT(4));
    size_t n = gs_track_serialize(&built, buf, sizeof buf);

    // Something recognisable in the target, so a partial load would show.
    gs_track_init(&target, 8, 8, GS_SURF_ICE);
    uint64_t untouched = gs_track_hash(&target);

    CHECK(!gs_track_deserialize(&target, buf, 4));          // shorter than a header
    CHECK(gs_track_hash(&target) == untouched);

    CHECK(!gs_track_deserialize(&target, buf, n - 1));      // truncated payload
    CHECK(gs_track_hash(&target) == untouched);

    uint8_t bad_magic[64];
    for (size_t i = 0; i < sizeof bad_magic; i++) bad_magic[i] = buf[i];
    bad_magic[0] ^= 0xffu;
    CHECK(!gs_track_deserialize(&target, bad_magic, sizeof bad_magic));
    CHECK(gs_track_hash(&target) == untouched);

    static uint8_t bad_version[GS_TRACK_TILES * 4 + 4096];
    for (size_t i = 0; i < n; i++) bad_version[i] = buf[i];
    bad_version[4] = 99;                                    // a version from the future
    CHECK(!gs_track_deserialize(&target, bad_version, n));
    CHECK(gs_track_hash(&target) == untouched);

    // A surface byte that is not a surface would index the surface table off
    // its end. It is normalised on the way in, once.
    //
    // Checked on the *stored byte*, deliberately. The obvious version of this
    // asserts on gs_track_surface(), which has a clamp of its own - so it
    // passes whether or not the loader normalises anything, and pins nothing.
    static uint8_t bad_surface[GS_TRACK_TILES * 4 + 4096];
    for (size_t i = 0; i < n; i++) bad_surface[i] = buf[i];
    size_t first_tile = 10 + ((size_t)built.w + 1) * ((size_t)built.h + 1) * 2;
    bad_surface[first_tile] = 200;
    CHECK(gs_track_deserialize(&target, bad_surface, n));
    CHECK(target.surface[GS_TILE_INDEX(0, 0)] < GS_SURF_COUNT);

    // And the buffer has to be big enough to write into in the first place.
    CHECK(gs_track_serialize(&built, buf, 8) == 0);
}

TEST(two_tracks_built_the_same_way_share_an_identity_through_a_file) {
    static gs_track a, b, back;
    static uint8_t buf[GS_TRACK_TILES * 4 + 4096];

    // Built independently, by the same steps, in a different order.
    gs_track_init(&a, 24, 10, GS_SURF_PAVEMENT);
    gs_track_set_corner(&a, 3, 4, GS_INT(2));
    gs_track_set_surface(&a, 7, 2, GS_SURF_ICE);

    gs_track_init(&b, 24, 10, GS_SURF_PAVEMENT);
    gs_track_set_surface(&b, 7, 2, GS_SURF_ICE);
    gs_track_set_corner(&b, 3, 4, GS_INT(2));

    CHECK(gs_track_hash(&a) == gs_track_hash(&b));

    // And the identity survives the journey, which is what lets a shared track
    // aggregate ghosts and times without a server deciding anything.
    size_t n = gs_track_serialize(&a, buf, sizeof buf);
    CHECK(gs_track_deserialize(&back, buf, n));
    CHECK(gs_track_hash(&back) == gs_track_hash(&b));

    // One tile moved is a different track.
    gs_track_set_corner(&b, 3, 4, GS_INT(2) + 256);
    CHECK(gs_track_hash(&b) != gs_track_hash(&back));
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

// A union rather than a struct with a trailing array: this guarantees the
// storage is aligned for a gs_edit_log, which is what the flexible array member
// needs and what a hand-rolled byte buffer would not promise.
#define GS_TEST_EDITS 4096
static union {
    gs_edit_log log;
    unsigned char raw[sizeof(gs_edit_log) + GS_TEST_EDITS * sizeof(gs_edit)];
} gs_edit_storage;

static gs_edit_log *gs_fresh_log(uint32_t cap) {
    gs_edit_log *l = &gs_edit_storage.log;
    gs_edit_log_init(l, cap);
    return l;
}

// A spread of edits of all three kinds, deterministic and not aligned to
// anything convenient.
static void gs_make_edits(gs_edit_log *l, gs_track *t, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint8_t x = (uint8_t)((i * 7u) % t->w);
        uint8_t y = (uint8_t)((i * 11u) % t->h);
        switch (i % 3u) {
        case 0:
            gs_edit_corner(l, t, x, y, GS_INT((int32_t)(i % 5u)) + GS_HALF);
            break;
        case 1:
            gs_edit_surface(l, t, x, y, (gs_surface)(i % GS_SURF_COUNT));
            break;
        default:
            gs_edit_gravity(l, t, x, y, GS_RATIO(20 + (int32_t)(i % 200u), 100));
            break;
        }
    }
}

TEST(any_sequence_of_edits_undone_completely_restores_the_track) {
    static gs_track t;
    gs_track_init(&t, 24, 16, GS_SURF_PAVEMENT);
    uint64_t start = gs_track_hash(&t);

    gs_edit_log *l = gs_fresh_log(GS_TEST_EDITS);
    gs_make_edits(l, &t, 500);
    CHECK(gs_track_hash(&t) != start);

    uint32_t undone = 0;
    while (gs_edit_undo(l, &t)) undone++;

    CHECK(undone > 0);
    CHECK(!gs_edit_can_undo(l));
    // The whole claim, in one line: whatever was done, undoing it all leaves
    // the track it started as - not merely one that looks similar.
    CHECK(gs_track_hash(&t) == start);
}

TEST(redoing_everything_returns_the_track_to_where_it_was) {
    static gs_track t;
    gs_track_init(&t, 24, 16, GS_SURF_DIRT);

    gs_edit_log *l = gs_fresh_log(GS_TEST_EDITS);
    gs_make_edits(l, &t, 300);
    uint64_t edited = gs_track_hash(&t);

    while (gs_edit_undo(l, &t)) { }
    while (gs_edit_redo(l, &t)) { }

    CHECK(!gs_edit_can_redo(l));
    CHECK(gs_track_hash(&t) == edited);
}

TEST(a_brush_stroke_undoes_as_one_action) {
    static gs_track t;
    gs_track_init(&t, 20, 20, GS_SURF_PAVEMENT);
    uint64_t start = gs_track_hash(&t);

    gs_edit_log *l = gs_fresh_log(GS_TEST_EDITS);

    // Forty tiles painted in one drag. Undoing that a tile at a time would be
    // useless, which is what transactions are for.
    gs_edit_begin(l);
    for (uint8_t x = 2; x < 12; x++)
        for (uint8_t y = 2; y < 6; y++)
            gs_edit_surface(l, &t, x, y, GS_SURF_ICE);
    gs_edit_end(l);

    CHECK(gs_edit_undo_depth(l) == 1);
    CHECK(gs_track_hash(&t) != start);

    CHECK(gs_edit_undo(l, &t));
    CHECK(gs_track_hash(&t) == start);
    CHECK(!gs_edit_can_undo(l));

    CHECK(gs_edit_redo(l, &t));
    CHECK(gs_edit_undo_depth(l) == 1);
    CHECK(gs_track_surface(&t, GS_INT(5) + GS_HALF, GS_INT(3) + GS_HALF) == GS_SURF_ICE);
}

TEST(an_edit_after_an_undo_drops_what_was_ahead) {
    static gs_track t;
    gs_track_init(&t, 16, 16, GS_SURF_PAVEMENT);

    gs_edit_log *l = gs_fresh_log(GS_TEST_EDITS);
    gs_edit_corner(l, &t, 1, 1, GS_INT(1));
    gs_edit_corner(l, &t, 2, 2, GS_INT(2));
    gs_edit_corner(l, &t, 3, 3, GS_INT(3));

    CHECK(gs_edit_undo(l, &t));
    CHECK(gs_edit_undo(l, &t));
    CHECK(gs_edit_redo_depth(l) == 2);

    // History is a line, not a tree. Everyone expects this and nobody thanks
    // you for the alternative.
    gs_edit_corner(l, &t, 9, 9, GS_INT(4));
    CHECK(gs_edit_redo_depth(l) == 0);
    CHECK(!gs_edit_can_redo(l));

    CHECK(gs_edit_undo_depth(l) == 2);
}

TEST(an_edit_that_changes_nothing_leaves_no_step_in_the_history) {
    static gs_track t;
    gs_track_init(&t, 16, 16, GS_SURF_PAVEMENT);

    gs_edit_log *l = gs_fresh_log(GS_TEST_EDITS);
    gs_edit_corner(l, &t, 4, 4, GS_INT(2));
    CHECK(gs_edit_undo_depth(l) == 1);

    // Painting the same value over itself is what a brush does constantly while
    // the mouse sits still. None of it belongs in the history, or undo fills up
    // with steps that visibly do nothing.
    for (int i = 0; i < 50; i++) {
        gs_edit_corner(l, &t, 4, 4, GS_INT(2));
        gs_edit_surface(l, &t, 4, 4, GS_SURF_PAVEMENT);
    }
    CHECK(gs_edit_undo_depth(l) == 1);
}

TEST(a_full_log_refuses_the_edit_rather_than_applying_it) {
    static gs_track t;
    gs_track_init(&t, 16, 16, GS_SURF_PAVEMENT);

    gs_edit_log *l = gs_fresh_log(3);
    CHECK(gs_edit_corner(l, &t, 1, 1, GS_INT(1)));
    CHECK(gs_edit_corner(l, &t, 2, 2, GS_INT(1)));
    CHECK(gs_edit_corner(l, &t, 3, 3, GS_INT(1)));

    uint64_t full = gs_track_hash(&t);

    // An edit that cannot be undone is worse than an edit that did not happen.
    CHECK(!gs_edit_corner(l, &t, 4, 4, GS_INT(1)));
    CHECK(gs_track_hash(&t) == full);

    // And the history is still coherent afterwards.
    while (gs_edit_undo(l, &t)) { }
    CHECK(!gs_edit_can_undo(l));
}

TEST(the_far_corners_of_a_track_can_be_edited) {
    static gs_track t;
    gs_track_init(&t, 8, 6, GS_SURF_PAVEMENT);

    gs_edit_log *l = gs_fresh_log(GS_TEST_EDITS);

    // A 8x6 track has 9x7 corners. Editing the last one is the off-by-one that
    // would otherwise be discovered by a ramp that cannot reach the track edge.
    CHECK(gs_edit_corner(l, &t, 8, 6, GS_INT(3)));
    CHECK(gs_track_height(&t, GS_INT(8) - 1, GS_INT(6) - 1) > 0);

    // Tiles stop one short of that, and an edit off the end is ignored rather
    // than corrupting the neighbour.
    uint64_t before = gs_track_hash(&t);
    gs_edit_surface(l, &t, 8, 0, GS_SURF_ICE);
    gs_edit_gravity(l, &t, 0, 6, GS_INT(2));
    CHECK(gs_track_hash(&t) == before);
}

// ---------------------------------------------------------------------------
// The route
// ---------------------------------------------------------------------------

TEST(a_car_driving_through_a_gate_is_seen_to_cross_it) {
    // A gate at (10, 6) that cars pass through heading along +x, reaching two
    // tiles either side of centre.
    gs_gate g = { .x = GS_INT(10), .y = GS_INT(6), .half_width = GS_INT(2),
                  .heading = 0, .pad = 0 };

    // Straight through the middle.
    CHECK(gs_gate_crossed(&g, GS_INT(9), GS_INT(6), GS_INT(11), GS_INT(6)));

    // Through it near the edge, still a crossing.
    CHECK(gs_gate_crossed(&g, GS_INT(9), GS_INT(4) + GS_HALF,
                          GS_INT(11), GS_INT(4) + GS_HALF));

    // Past the end of it is not. A gate is a gate, not a tripwire strung across
    // the whole world - you can miss one, and missing one has to mean something.
    CHECK(!gs_gate_crossed(&g, GS_INT(9), GS_INT(1), GS_INT(11), GS_INT(1)));

    // Backwards is not a crossing either, which is what stops a player
    // reversing over the finish line to score laps.
    CHECK(!gs_gate_crossed(&g, GS_INT(11), GS_INT(6), GS_INT(9), GS_INT(6)));

    // Nor is driving up to it and stopping short.
    CHECK(!gs_gate_crossed(&g, GS_INT(8), GS_INT(6), GS_INT(9), GS_INT(6)));

    // A gate turned to face another way is crossed from that way instead.
    gs_gate north = { .x = GS_INT(10), .y = GS_INT(6), .half_width = GS_INT(2),
                      .heading = GS_QUARTER, .pad = 0 };
    CHECK(gs_gate_crossed(&north, GS_INT(10), GS_INT(5), GS_INT(10), GS_INT(7)));
    CHECK(!gs_gate_crossed(&north, GS_INT(9), GS_INT(6), GS_INT(11), GS_INT(6)));
}

TEST(a_route_is_part_of_the_track_and_survives_being_saved) {
    static gs_track a, b, back;
    static uint8_t buf[GS_TRACK_TILES * 4 + 4096];

    gs_track_init(&a, 24, 12, GS_SURF_PAVEMENT);
    gs_track_init(&b, 24, 12, GS_SURF_PAVEMENT);
    CHECK(gs_track_hash(&a) == gs_track_hash(&b));

    CHECK(gs_track_add_gate(&a, GS_INT(4), GS_INT(6), 0, GS_INT(3)) == 0);
    CHECK(gs_track_add_gate(&a, GS_INT(16), GS_INT(6), GS_QUARTER, GS_INT(3)) == 1);

    // The same ground with a route is not the same track: a lap time on it is
    // not comparable with a lap time on bare terrain.
    CHECK(gs_track_hash(&a) != gs_track_hash(&b));

    size_t n = gs_track_serialize(&a, buf, sizeof buf);
    CHECK(gs_track_deserialize(&back, buf, n));
    CHECK(back.gate_count == 2);
    CHECK(gs_track_hash(&back) == gs_track_hash(&a));
    CHECK(back.gate[1].heading == GS_QUARTER);

    // Direction is part of it too - the same gates the other way round are a
    // different track, which is the whole reason a gate has a heading.
    back.gate[0].heading = (gs_angle)(back.gate[0].heading + GS_QUARTER * 2);
    CHECK(gs_track_hash(&back) != gs_track_hash(&a));
}

TEST(removing_a_gate_closes_the_gap_and_leaves_the_order_alone) {
    static gs_track t;
    gs_track_init(&t, 24, 12, GS_SURF_PAVEMENT);

    for (int i = 0; i < 5; i++) {
        CHECK(gs_track_add_gate(&t, GS_INT(2 * i), GS_INT(6), (gs_angle)(i * 1000),
                                GS_INT(2)) == i);
    }
    CHECK(t.gate_count == 5);

    CHECK(gs_track_remove_gate(&t, 1));
    CHECK(t.gate_count == 4);
    // What was third is now second, and everything after it moved up with it.
    CHECK(t.gate[1].x == GS_INT(4));
    CHECK(t.gate[3].x == GS_INT(8));

    CHECK(!gs_track_remove_gate(&t, 9));

    // And the route has a ceiling that is refused rather than overrun.
    while (t.gate_count < GS_TRACK_MAX_GATES) {
        CHECK(gs_track_add_gate(&t, GS_INT(1), GS_INT(1), 0, GS_INT(1)) >= 0);
    }
    CHECK(gs_track_add_gate(&t, GS_INT(1), GS_INT(1), 0, GS_INT(1)) == -1);
    CHECK(t.gate_count == GS_TRACK_MAX_GATES);
}

// A route that has nothing wrong with it, to break in specific ways.
static void gs_sound_route(gs_track *t) {
    gs_track_init(t, 32, 24, GS_SURF_PAVEMENT);
    gs_track_add_gate(t, GS_INT(6), GS_INT(12), 0, GS_INT(4));
    gs_track_add_gate(t, GS_INT(16), GS_INT(12), 0, GS_INT(4));
    gs_track_add_gate(t, GS_INT(26), GS_INT(12), 0, GS_INT(4));
}

TEST(a_sound_route_is_accepted_and_a_broken_one_names_its_problem) {
    static gs_track t;
    gs_sound_route(&t);
    CHECK(gs_track_validate(&t).problem == GS_TRACK_OK);

    // Nothing at all. This is the state every new track starts in, so the
    // message has to be the one that tells a beginner what to do first.
    gs_track_init(&t, 32, 24, GS_SURF_PAVEMENT);
    gs_track_issue none = gs_track_validate(&t);
    CHECK(none.problem == GS_TRACK_NO_START);
    CHECK(none.gate == -1);

    // A start line and nowhere to go.
    gs_track_add_gate(&t, GS_INT(6), GS_INT(12), 0, GS_INT(4));
    CHECK(gs_track_validate(&t).problem == GS_TRACK_TOO_FEW_GATES);

    // Every problem says something, and says something different.
    CHECK(gs_track_problem_text(GS_TRACK_NO_START)[0] != '\0');
    CHECK(gs_track_problem_text(GS_TRACK_OK) !=
          gs_track_problem_text(GS_TRACK_NO_START));
}

TEST(a_gate_hanging_off_the_track_is_refused_and_says_which_one) {
    static gs_track t;
    gs_sound_route(&t);

    // The centre is comfortably on the track; one end of the span is not.
    // Checked because a gate a car can drive round the end of is worse than one
    // that is obviously wrong - it looks fine and quietly does nothing.
    t.gate[1].y = GS_INT(2);
    t.gate[1].half_width = GS_INT(4);

    gs_track_issue bad = gs_track_validate(&t);
    CHECK(bad.problem == GS_TRACK_GATE_OFF_TRACK);
    CHECK(bad.gate == 1);

    // And the centre being off is caught too.
    gs_sound_route(&t);
    t.gate[2].x = GS_INT(40);
    bad = gs_track_validate(&t);
    CHECK(bad.problem == GS_TRACK_GATE_OFF_TRACK);
    CHECK(bad.gate == 2);
}

TEST(a_gate_nothing_can_fit_through_is_refused) {
    static gs_track t;
    gs_sound_route(&t);

    t.gate[2].half_width = GS_ONE / 16;   // a slot, not a gate
    gs_track_issue bad = gs_track_validate(&t);
    CHECK(bad.problem == GS_TRACK_GATE_TOO_NARROW);
    CHECK(bad.gate == 2);
}

TEST(two_gates_in_the_same_place_are_refused_as_an_ambiguous_order) {
    static gs_track t;
    gs_sound_route(&t);

    // The route would then mean one thing to the game and another to whoever
    // built it, which is worse than it plainly not working.
    t.gate[2].x = t.gate[0].x;
    t.gate[2].y = t.gate[0].y;

    gs_track_issue bad = gs_track_validate(&t);
    CHECK(bad.problem == GS_TRACK_GATES_COINCIDE);
    CHECK(bad.gate == 0);
    CHECK(bad.other == 2);

    // Gates that are merely close are fine - a chicane is allowed to be tight.
    gs_sound_route(&t);
    t.gate[1].x = t.gate[0].x + GS_INT(2);
    t.gate[1].y = t.gate[0].y;
    CHECK(gs_track_validate(&t).problem == GS_TRACK_OK);
}

// ---------------------------------------------------------------------------
// Driving
// ---------------------------------------------------------------------------

static void gs_flat_world(gs_track *t, gs_world *w, gs_surface s) {
    gs_track_init(t, 32, 32, s);
    gs_world_init(w, GS_ONE);
    gs_world_add_car(w, t, GS_VEH_STOCK_CAR, GS_INT(4), GS_INT(16), 0);
}

TEST(a_car_accelerates_under_throttle_and_stops_under_the_brake) {
    static gs_track t;
    gs_world w;
    gs_flat_world(&t, &w, GS_SURF_PAVEMENT);

    gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, 0, 0, 0 };
    for (int i = 0; i < GS_TICK_HZ * 3; i++) gs_world_step(&w, &t, in);

    gs_fix cruising = gs_car_speed(&w.car[0]);
    CHECK(cruising > GS_INT(3));
    CHECK(cruising <= gs_vehicle(GS_VEH_STOCK_CAR)->top);

    // The brake takes it to a stop and then, held, backs it up - which is what
    // the original did and is why this watches the slowest moment rather than
    // the speed at the end of three seconds.
    in[0] = GS_IN_BRAKE;
    gs_fix slowest = cruising;
    for (int i = 0; i < GS_TICK_HZ * 3; i++) {
        gs_world_step(&w, &t, in);
        gs_fix s = gs_car_speed(&w.car[0]);
        if (s < slowest) slowest = s;
    }
    CHECK(slowest < gs_fix_mul(cruising, GS_RATIO(1, 4)));
    CHECK(w.car[0].vx < 0);
}

TEST(a_car_left_on_a_slope_rolls_downhill_and_one_on_the_flat_does_not) {
    static gs_track flat, hill;
    gs_world wf, wh;

    gs_track_init(&flat, 32, 8, GS_SURF_PAVEMENT);
    gs_world_init(&wf, GS_ONE);
    gs_world_add_car(&wf, &flat, GS_VEH_STOCK_CAR, GS_INT(16), GS_INT(4), 0);

    gs_build_ramp(&hill, 0, 32, GS_INT(8));   // a constant quarter-tile slope
    gs_world_init(&wh, GS_ONE);
    gs_world_add_car(&wh, &hill, GS_VEH_STOCK_CAR, GS_INT(16), GS_INT(4), 0);

    for (int i = 0; i < GS_TICK_HZ * 2; i++) {
        gs_world_step(&wf, &flat, nullptr);
        gs_world_step(&wh, &hill, nullptr);
    }

    CHECK(gs_car_speed(&wf.car[0]) == 0);
    CHECK(wf.car[0].x == GS_INT(16));

    // Downhill is -x here, because the ramp climbs with x.
    CHECK(wh.car[0].x < GS_INT(16));
    CHECK(gs_car_speed(&wh.car[0]) > GS_ONE);
}

TEST(a_car_turns_more_sharply_at_a_crawl_than_at_speed) {
    static gs_track t;
    gs_world slow, fast;
    gs_track_init(&t, 40, 40, GS_SURF_PAVEMENT);

    for (int i = 0; i < 2; i++) {
        gs_world *w = i == 0 ? &slow : &fast;
        gs_world_init(w, GS_ONE);
        gs_world_add_car(w, &t, GS_VEH_STOCK_CAR, GS_INT(20), GS_INT(20), 0);
        w->car[0].vx = i == 0 ? GS_ONE : GS_INT(6);
    }

    gs_input in[GS_MAX_CARS] = { GS_IN_LEFT, 0, 0, 0 };
    for (int i = 0; i < GS_TICK_HZ; i++) {
        gs_world_step(&slow, &t, in);
        gs_world_step(&fast, &t, in);
    }

    // Steering authority falls away with speed. Without that a car pirouettes
    // at 80, and with it the car is predictable in the hands after two corners
    // - which is the whole reason the rule is this simple.
    int32_t turned_slow = gs_angle_delta(0, slow.car[0].heading);
    int32_t turned_fast = gs_angle_delta(0, fast.car[0].heading);
    CHECK(turned_slow < 0 && turned_fast < 0);   // left is decreasing here
    CHECK(-turned_slow > -turned_fast);
}

TEST(ice_lets_go_of_a_sliding_car_long_after_pavement_has_caught_it) {
    static gs_track pave, ice;
    gs_world wp, wi;
    gs_flat_world(&pave, &wp, GS_SURF_PAVEMENT);
    gs_flat_world(&ice, &wi, GS_SURF_ICE);

    // Pointing along x, travelling diagonally: the y component is pure slip.
    for (int i = 0; i < 2; i++) {
        gs_world *w = i == 0 ? &wp : &wi;
        w->car[0].vx = GS_INT(4);
        w->car[0].vy = GS_INT(4);
    }

    // Two seconds. Pavement bears about 2.7 tiles/s^2 sideways, so it has the
    // slip gone well inside that; ice bears a sixth of it and is still sliding.
    for (int i = 0; i < GS_TICK_HZ * 2; i++) {
        gs_world_step(&wp, &pave, nullptr);
        gs_world_step(&wi, &ice, nullptr);
    }

    CHECK(gs_fix_abs(wp.car[0].vy) < gs_fix_abs(wi.car[0].vy));
    CHECK(gs_fix_abs(wp.car[0].vy) == 0);
    CHECK(gs_fix_abs(wi.car[0].vy) > GS_INT(2));
}

// Take the same corner at the same speed on a given surface, and report both
// halves of what happened: how far the car's *velocity* turned, and how far
// that lags where the car is pointing.
//
// The lag is the interesting number. A car on ice will point wherever you steer
// it while continuing in a straight line, so a test that watched the heading
// alone would call that a corner taken.
typedef struct gs_corner {
    double turned;   // degrees the velocity came round
    double slip;     // degrees between where it points and where it is going
} gs_corner;

static gs_corner gs_take_corner(gs_surface surface) {
    static gs_track t;
    gs_track_init(&t, 60, 60, surface);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(30), GS_INT(30), 0);
    w.car[0].vx = GS_INT(5);

    gs_angle before = gs_atan2(w.car[0].vy, w.car[0].vx);

    gs_input in[GS_MAX_CARS] = { (gs_input)(GS_IN_ACCEL | GS_IN_LEFT), 0, 0, 0 };
    for (int i = 0; i < GS_TICK_HZ * 3 / 2; i++) gs_world_step(&w, &t, in);

    gs_angle after = gs_atan2(w.car[0].vy, w.car[0].vx);

    int32_t turned = gs_angle_delta(before, after);
    int32_t slip = gs_angle_delta(after, w.car[0].heading);
    if (turned < 0) turned = -turned;
    if (slip < 0) slip = -slip;

    return (gs_corner){ (double)turned / 65536.0 * 360.0,
                        (double)slip / 65536.0 * 360.0 };
}

TEST(the_same_corner_at_the_same_speed_is_takeable_on_pavement_and_not_on_ice) {
    gs_corner pavement = gs_take_corner(GS_SURF_PAVEMENT);
    gs_corner dirt = gs_take_corner(GS_SURF_DIRT);
    gs_corner ice = gs_take_corner(GS_SURF_ICE);

    // On pavement the car goes exactly where it points: the corner is simply
    // taken, and there is nothing to correct for.
    CHECK(pavement.slip < 2.0);
    CHECK(pavement.turned > 35.0);

    // On ice it points into the corner and carries straight on, which is the
    // whole character of the surface and the reason a corner that is nothing on
    // pavement is a problem here.
    CHECK(ice.slip > 20.0);
    CHECK(ice.turned < pavement.turned / 3.0);

    // Dirt is between them on both counts, so the three are a spread rather
    // than two surfaces and a spare.
    CHECK(dirt.turned < pavement.turned && dirt.turned > ice.turned);
    CHECK(dirt.slip > pavement.slip && dirt.slip < ice.slip);
}

// ---------------------------------------------------------------------------
// Jumping - the part the whole game is about
// ---------------------------------------------------------------------------

// The arc a ramp throws a car into, measured rather than asserted.
//
// Drag and rolling resistance are dialled out, so what comes back is the
// ballistic answer and not a measurement of the aerodynamics.
//
// `force` overwrites the velocity at the instant of take-off. This matters more
// than it looks: a ramp does *not* hand back the same take-off speed under
// different gravity, because a car climbing it loses less speed when gravity is
// weaker. So a test about gravity alone has to hold the launch fixed, and a
// test about the ramp itself must not.
typedef struct gs_jump {
    double range;    // tiles between leaving the ground and touching it again
    double vx, vz;   // the take-off velocity, tiles per second
    double gravity;  // tiles per second squared where it flew
    bool   flew;
} gs_jump;

#define GS_LAUNCH_VX GS_INT(5)
#define GS_LAUNCH_VZ GS_RATIO(125, 100)

static gs_jump gs_jump_off_ramp(gs_fix gravity_scale, bool low_g_pocket, bool force) {
    static gs_track t;
    gs_build_ramp(&t, 8, 12, GS_INT(1));

    if (low_g_pocket) {
        // A stripe of one-fifth gravity painted across the flight path - the
        // brush, used exactly as a player would use it.
        for (uint8_t x = 13; x < 24; x++)
            for (uint8_t y = 0; y < t.h; y++)
                gs_track_set_gravity(&t, x, y, GS_RATIO(20, 100));
    }

    gs_world w;
    gs_world_init(&w, gravity_scale);
    w.drag_scale = 0;
    w.friction_scale = 0;
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(2), GS_INT(4), 0);
    w.car[0].vx = GS_LAUNCH_VX;

    gs_jump j = { 0 };
    j.gravity = gs_to_double(w.gravity);

    gs_fix launch_x = 0;
    for (int i = 0; i < GS_TICK_HZ * 20; i++) {
        bool was_air = !w.car[0].grounded;
        gs_world_step(&w, &t, nullptr);
        bool is_air = !w.car[0].grounded;

        if (!was_air && is_air) {
            if (force) {
                w.car[0].vx = GS_LAUNCH_VX;
                w.car[0].vy = 0;
                w.car[0].vz = GS_LAUNCH_VZ;
            }
            launch_x = w.car[0].x;
            j.vx = gs_to_double(w.car[0].vx);
            j.vz = gs_to_double(w.car[0].vz);
            j.flew = true;
        }
        if (was_air && !is_air && j.flew) {
            j.range = gs_to_double(w.car[0].x - launch_x);
            return j;
        }
    }
    return j;
}

TEST(a_ramp_throws_a_car_into_the_air_without_being_told_it_is_a_ramp) {
    // No ramp tile type exists. A jump happens because the ground fell away
    // faster than gravity could hold the car to it, which is what lets any
    // shape a player builds be one.
    gs_jump j = gs_jump_off_ramp(GS_ONE, false, false);
    CHECK(j.flew);
    CHECK(j.range > 1.0);
    CHECK(j.vz > 0.0);
}

TEST(a_jump_lands_where_the_closed_form_says_it_should) {
    gs_jump j = gs_jump_off_ramp(GS_ONE, false, true);
    CHECK(j.flew);

    // Take off and land at the same height, so the time aloft is 2*vz/g and the
    // range is that times the horizontal speed. The tolerance is for the 120 Hz
    // discretisation of take-off and touchdown, nothing else.
    double predicted = 2.0 * j.vx * j.vz / j.gravity;
    CHECK_NEAR(j.range, predicted, predicted * 0.05);
}

TEST(halving_gravity_doubles_a_jump_from_the_same_take_off) {
    gs_jump earth = gs_jump_off_ramp(GS_ONE, false, true);
    gs_jump half = gs_jump_off_ramp(GS_HALF, false, true);

    CHECK(earth.flew && half.flew);
    CHECK_NEAR(half.range, earth.range * 2.0, earth.range * 0.05);
}

TEST(a_ramp_throws_a_car_further_under_lower_gravity_than_the_arc_alone_explains) {
    // The launches are *not* held fixed here, and that is the point: under
    // weaker gravity the car also loses less speed climbing the ramp, so it
    // arrives at the crest faster and leaves it faster. The two effects
    // compound, which is why halving gravity on a real ramp does more than
    // double the jump.
    gs_jump earth = gs_jump_off_ramp(GS_ONE, false, false);
    gs_jump half = gs_jump_off_ramp(GS_HALF, false, false);

    CHECK(earth.flew && half.flew);
    CHECK(half.vx > earth.vx);
    CHECK(half.range > earth.range * 2.0);
}

TEST(a_low_gravity_tile_lengthens_a_jump_that_crosses_it) {
    gs_jump normal = gs_jump_off_ramp(GS_ONE, false, true);
    gs_jump painted = gs_jump_off_ramp(GS_ONE, true, true);

    // The brush: gravity is sampled where the car is, every tick, so a pocket
    // painted under the flight path changes the arc without touching the race
    // setting.
    CHECK(painted.flew);
    CHECK(painted.range > normal.range * 1.4);
}

TEST(the_same_jump_hurts_less_landed_on_a_downslope_than_landed_flat) {
    // What hurts is not falling speed but the *mismatch* between how fast the
    // car is coming down and how fast the ground is falling away beneath it.
    // That is why building a downhill landing is worth doing and a flat one is
    // a mistake, and it is the whole reason to shape a landing at all.
    uint8_t damage[2];

    for (int variant = 0; variant < 2; variant++) {
        static gs_track t;
        gs_track_init(&t, 40, 8, GS_SURF_PAVEMENT);

        // A table top eight tiles up, ending at x = 8. Beyond it either a flat
        // floor or a floor that falls away at the angle the car is descending.
        for (uint8_t y = 0; y <= t.h; y++) {
            for (uint8_t x = 0; x <= t.w; x++) {
                gs_fix h;
                if (x <= 8) h = GS_INT(8);
                else if (variant == 0) h = 0;
                else h = GS_INT(8) - (gs_fix)((int64_t)GS_INT(1) * (x - 8) * 8 / 10);
                gs_track_set_corner(&t, x, y, h);
            }
        }

        gs_world w;
        gs_world_init(&w, GS_ONE);
        w.drag_scale = 0;
        w.friction_scale = 0;
        gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(2), GS_INT(4), 0);
        w.car[0].vx = GS_INT(6);

        for (int i = 0; i < GS_TICK_HZ * 8; i++) gs_world_step(&w, &t, nullptr);
        damage[variant] = w.car[0].damage;
    }

    CHECK(damage[0] > 0);
    CHECK(damage[1] < damage[0]);
}

// ---------------------------------------------------------------------------
// The dials
// ---------------------------------------------------------------------------

// Each of these runs the *same* input log and changes one dial, so anything
// that moves is that dial's doing. Swept across several values rather than
// compared at two, because the claim is that they are continuous: a dial with
// three settings pretending to be a slider would pass a two-point test.

static gs_world gs_dialled(gs_fix gravity, gs_fix drag, gs_fix friction,
                           gs_fix damage, const gs_track *t, uint32_t ticks,
                           gs_input held) {
    gs_world w;
    gs_world_init(&w, gravity);
    w.drag_scale = drag;
    w.friction_scale = friction;
    w.damage_scale = damage;
    gs_world_add_car(&w, t, GS_VEH_STOCK_CAR, GS_INT(4), GS_INT(20), 0);

    gs_input in[GS_MAX_CARS] = { held, 0, 0, 0 };
    for (uint32_t i = 0; i < ticks; i++) gs_world_step(&w, t, in);
    return w;
}

TEST(air_drag_is_a_dial_and_more_of_it_means_less_speed) {
    static gs_track t;
    gs_track_init(&t, 60, 40, GS_SURF_PAVEMENT);

    gs_fix last = INT32_MAX;
    for (int32_t setting = 0; setting <= 8; setting++) {
        gs_world w = gs_dialled(GS_ONE, GS_INT(setting), GS_ONE, GS_ONE, &t,
                                GS_TICK_HZ * 8, GS_IN_ACCEL);
        gs_fix speed = gs_car_speed(&w.car[0]);

        // Strictly slower every step of the way: no plateau, no dead zone.
        CHECK(speed < last);
        last = speed;
    }
    CHECK(last > 0);   // and it never stops the car outright
}

// How far the velocity comes round in one second of full lock, at a given
// friction setting.
static double gs_turn_at_friction(gs_fix friction) {
    static gs_track t;
    gs_track_init(&t, 60, 60, GS_SURF_PAVEMENT);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    w.friction_scale = friction;
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(30), GS_INT(30), 0);
    w.car[0].vx = GS_INT(5);

    gs_angle before = gs_atan2(w.car[0].vy, w.car[0].vx);
    gs_input in[GS_MAX_CARS] = { (gs_input)(GS_IN_ACCEL | GS_IN_LEFT), 0, 0, 0 };
    for (int i = 0; i < GS_TICK_HZ; i++) gs_world_step(&w, &t, in);

    int32_t turned = gs_angle_delta(before, gs_atan2(w.car[0].vy, w.car[0].vx));
    return (double)(turned < 0 ? -turned : turned) / 65536.0 * 360.0;
}

TEST(the_friction_scale_is_a_dial_and_more_of_it_means_more_grip_until_it_does_not) {
    // While traction is what limits the corner, every step up the dial turns
    // the car further, with no flat spots.
    double last = -1.0;
    for (int32_t setting = 1; setting <= 9; setting++) {
        double deg = gs_turn_at_friction(GS_RATIO(setting, 10));
        CHECK(deg > last);
        last = deg;
    }

    // Past full grip it stops mattering, because the limit is no longer the
    // tyres - it is how fast the car can be steered. That plateau is the model
    // being honest rather than the dial being broken, and it is worth pinning:
    // a version of this that kept climbing would mean grip was buying
    // something it should not.
    double full = gs_turn_at_friction(GS_ONE);
    double double_grip = gs_turn_at_friction(GS_INT(2));
    double quadruple = gs_turn_at_friction(GS_INT(4));

    CHECK(full > last);
    CHECK(double_grip - full < 0.5);
    CHECK(quadruple - full < 0.5);

    // And the whole range is worth having: full grip turns several times as
    // much as a tenth of it.
    CHECK(full > gs_turn_at_friction(GS_RATIO(1, 10)) * 4.0);
}

TEST(the_damage_multiplier_is_a_dial_and_more_of_it_hurts_more) {
    // The same fall, every time. Only the dial moves.
    static gs_track t;
    gs_track_init(&t, 40, 12, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= t.h; y++)
        for (uint8_t x = 0; x <= t.w; x++)
            gs_track_set_corner(&t, x, y, x <= 6 ? GS_INT(9) : 0);

    int32_t last = -1;
    for (int32_t setting = 2; setting <= 14; setting += 2) {
        gs_world w;
        gs_world_init(&w, GS_ONE);
        w.damage_scale = GS_RATIO(setting, 10);
        gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(2), GS_INT(6), 0);
        w.car[0].vx = GS_INT(5);

        for (int i = 0; i < GS_TICK_HZ * 6; i++) gs_world_step(&w, &t, nullptr);

        CHECK((int32_t)w.car[0].damage >= last);
        last = (int32_t)w.car[0].damage;
    }
    // At the top of the range the same landing that was survivable is not.
    CHECK(last > 0);
}

TEST(gravity_is_a_dial_and_it_is_continuous_between_the_planets) {
    static gs_track t;
    gs_build_ramp(&t, 8, 12, GS_INT(1));

    // Range of a jump against gravity: closed form says it is proportional to
    // 1/g, so every step up the dial must shorten it, with no flat spots where
    // a range of settings does the same thing.
    double last = 1e9;
    for (int32_t setting = 2; setting <= 20; setting += 2) {
        gs_world w;
        gs_world_init(&w, GS_RATIO(setting, 10));
        w.drag_scale = 0;
        w.friction_scale = 0;
        gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(2), GS_INT(4), 0);
        w.car[0].vx = GS_INT(5);

        double launch = 0, range = 0;
        bool flew = false;
        for (int i = 0; i < GS_TICK_HZ * 30; i++) {
            bool was_air = !w.car[0].grounded;
            gs_world_step(&w, &t, nullptr);
            bool is_air = !w.car[0].grounded;
            if (!was_air && is_air) { launch = gs_to_double(w.car[0].x); flew = true; }
            if (was_air && !is_air && flew) {
                range = gs_to_double(w.car[0].x) - launch;
                break;
            }
        }
        CHECK(flew);
        CHECK(range < last);
        last = range;
    }

    // And the presets are named, because "Jupiter" tells a player something a
    // number does not - which was the point of keeping them.
    CHECK(GS_GRAVITY_PRESETS >= 6);
    for (int i = 0; i < GS_GRAVITY_PRESETS; i++) {
        CHECK(gs_gravity_presets[i].name != nullptr);
        CHECK(gs_gravity_presets[i].scale > 0);
        if (i > 0) CHECK(gs_gravity_presets[i].scale > gs_gravity_presets[i - 1].scale);
    }
}

// ---------------------------------------------------------------------------
// Wear
// ---------------------------------------------------------------------------

// Drive the same corner, on the same line, `laps` times over, and report how
// far round the car got on the last one. Everything is identical between runs
// except what the earlier laps did to the ground.
static double gs_lap_on_worn_ground(gs_surface surface, int laps) {
    static gs_track t;
    gs_track_init(&t, 60, 60, surface);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(30), GS_INT(30), 0);

    double turned = 0.0;
    for (int lap = 0; lap < laps; lap++) {
        // Same start, same speed, same steering, every lap. Only the ground
        // remembers.
        w.car[0].x = GS_INT(30);
        w.car[0].y = GS_INT(30);
        w.car[0].heading = 0;
        w.car[0].vx = GS_INT(5);
        w.car[0].vy = 0;
        w.car[0].vz = 0;

        gs_angle before = gs_atan2(w.car[0].vy, w.car[0].vx);
        gs_input in[GS_MAX_CARS] = { (gs_input)(GS_IN_ACCEL | GS_IN_LEFT), 0, 0, 0 };
        for (int i = 0; i < GS_TICK_HZ; i++) gs_world_step(&w, &t, in);

        int32_t d = gs_angle_delta(before, gs_atan2(w.car[0].vy, w.car[0].vx));
        turned = (double)(d < 0 ? -d : d) / 65536.0 * 360.0;
    }
    return turned;
}

TEST(lap_five_on_dirt_is_slower_on_the_used_line_than_lap_one) {
    double first = gs_lap_on_worn_ground(GS_SURF_DIRT, 1);
    double fifth = gs_lap_on_worn_ground(GS_SURF_DIRT, 5);

    // The line everyone has been taking has churned into ruts, and the corner
    // that was there on lap one is not there any more.
    CHECK(fifth < first);
    CHECK(first - fifth > 1.0);
}

TEST(pavement_does_not_care_how_many_laps_you_have_done_on_it) {
    double first = gs_lap_on_worn_ground(GS_SURF_PAVEMENT, 1);
    double fifth = gs_lap_on_worn_ground(GS_SURF_PAVEMENT, 5);

    // Which is what makes pavement the surface you can plan around.
    CHECK(fabs(fifth - first) < 0.001);
}

TEST(ice_polishes_into_something_faster_and_looser) {
    static gs_track t;
    gs_track_init(&t, 60, 60, GS_SURF_ICE);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(4), GS_INT(30), 0);

    // Wear a strip in, driving straight.
    gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, 0, 0, 0 };
    for (int i = 0; i < GS_TICK_HZ * 12; i++) gs_world_step(&w, &t, in);

    // Sampled where the car started rather than where it ended up: it wears
    // the tiles it *spent time on*, and on ice it accelerates so slowly that
    // the first few are the deepest. The tile under it at the end is one it has
    // only just reached.
    gs_fix worn = gs_world_wear(&w, GS_INT(4), GS_INT(30));
    CHECK(worn > 0);

    // Polished ice holds a car less well than fresh ice does. Both are awful;
    // the used line is worse, which is the point.
    CHECK(gs_surfaces[GS_SURF_ICE].wear_grip < GS_ONE);
    // And it rolls more freely, so the used line is *faster* in a straight
    // line - the nastiest combination of the three surfaces.
    CHECK(gs_surfaces[GS_SURF_ICE].wear_rolling < GS_ONE);
}

TEST(a_sliding_tyre_churns_the_ground_more_than_a_rolling_one) {
    // Why the racing line goes off before the rest of the track does: it is not
    // that more cars have been over it, it is that they were all *working* on
    // it, and a tyre scrubbing sideways marks the ground far harder than one
    // rolling straight over it.
    static gs_track t;
    gs_track_init(&t, 60, 60, GS_SURF_DIRT);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    // Same longitudinal speed, same tile, same time. One is sliding and the
    // other is not, and that is the only difference between them.
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(10), GS_INT(10), 0);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(10), GS_INT(40), 0);

    w.car[0].vx = GS_INT(3);
    w.car[1].vx = GS_INT(3);
    w.car[1].vy = GS_INT(3);      // sliding sideways as well as travelling

    for (int i = 0; i < 20; i++) gs_world_step(&w, &t, nullptr);

    gs_fix rolled = gs_world_wear(&w, GS_INT(10), GS_INT(10));
    gs_fix scrubbed = gs_world_wear(&w, GS_INT(10), GS_INT(40));

    CHECK(rolled > 0);
    CHECK(scrubbed > rolled);
}

TEST(wear_belongs_to_the_race_and_not_to_the_track) {
    static gs_track t;
    gs_track_init(&t, 40, 40, GS_SURF_DIRT);
    uint64_t fresh = gs_track_hash(&t);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(4), GS_INT(20), 0);

    gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, 0, 0, 0 };
    for (int i = 0; i < GS_TICK_HZ * 8; i++) gs_world_step(&w, &t, in);

    CHECK(gs_world_wear(&w, w.car[0].x, w.car[0].y) > 0);

    // The track is untouched: reload it and the ground is fresh again, which is
    // exactly right - ruts are what happened during a race, not what somebody
    // built. A track's identity must not drift because it was driven on.
    CHECK(gs_track_hash(&t) == fresh);

    // But it is world state, so it travels in a snapshot and it is hashed.
    gs_world snap;
    memcpy(&snap, &w, sizeof snap);
    CHECK(gs_world_hash(&snap) == gs_world_hash(&w));

    gs_world clean;
    gs_world_init(&clean, GS_ONE);
    gs_world_add_car(&clean, &t, GS_VEH_STOCK_CAR, GS_INT(4), GS_INT(20), 0);
    CHECK(gs_world_hash(&clean) != gs_world_hash(&w));
}

// ---------------------------------------------------------------------------
// Hitting each other
// ---------------------------------------------------------------------------

TEST(a_head_on_at_speed_sends_both_cars_somewhere) {
    static gs_track t;
    gs_track_init(&t, 60, 20, GS_SURF_PAVEMENT);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(20), GS_INT(10), 0);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(30), GS_INT(10),
                     (gs_angle)(GS_QUARTER * 2));

    // Driving straight at each other at five tiles a second.
    w.car[0].vx = GS_INT(5);
    w.car[1].vx = -GS_INT(5);

    // Watch the *closest* they ever get, not where they finish. Two cars that
    // pass through each other and are shoved apart afterwards end up looking
    // exactly like two cars that never overlapped.
    gs_fix closest = INT32_MAX;
    for (int i = 0; i < GS_TICK_HZ * 3; i++) {
        gs_world_step(&w, &t, nullptr);
        gs_fix d = gs_fix_len2(w.car[0].x - w.car[1].x, w.car[0].y - w.car[1].y);
        if (d < closest) closest = d;
    }

    // Never meaningfully inside one another: they are pushed apart on the tick
    // they touch, rather than left to trade impulses from inside each other.
    CHECK(closest > GS_CAR_RADIUS * 2 - GS_ONE / 4);
    CHECK(w.car[0].x < w.car[1].x);       // still on their own sides of it

    // Both are going *backwards* now, which is the whole point: a hit is an
    // event that sends people somewhere, not a two-second penalty.
    CHECK(w.car[0].vx < 0);
    CHECK(w.car[1].vx > 0);

    // And with more energy than they arrived with, because the bounce is
    // deliberately exaggerated - real cars absorb and stop, which is correct
    // physics and the wrong game.
    CHECK(gs_fix_abs(w.car[0].vx) > GS_INT(4));
}

TEST(the_same_collision_happens_the_same_way_every_time) {
    static gs_track t;
    gs_track_init(&t, 60, 20, GS_SURF_PAVEMENT);

    uint64_t first = 0;
    for (int run = 0; run < 3; run++) {
        gs_world w;
        gs_world_init(&w, GS_ONE);
        gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(20), GS_INT(10), 0);
        gs_world_add_car(&w, &t, GS_VEH_DUNE_BUGGY, GS_INT(30), GS_INT(10) + GS_HALF,
                         (gs_angle)(GS_QUARTER * 2));
        w.car[0].vx = GS_INT(6);
        w.car[1].vx = -GS_INT(4);

        for (int i = 0; i < GS_TICK_HZ * 4; i++) {
            gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, GS_IN_ACCEL, 0, 0 };
            gs_world_step(&w, &t, in);
        }

        uint64_t h = gs_world_hash(&w);
        if (run == 0) first = h;
        CHECK(h == first);
    }
}

TEST(cars_that_are_already_overlapping_push_apart_rather_than_sit_inside_each_other) {
    // The case the separation step exists for, and the one an impulse cannot
    // handle: two cars occupying the same ground and *not* approaching. There
    // is no closing speed, so there is no impulse - without a push they would
    // sit inside one another for the rest of the race.
    static gs_track t;
    gs_track_init(&t, 40, 20, GS_SURF_PAVEMENT);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(20), GS_INT(10), 0);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(20), GS_INT(10), 0);

    // Exactly coincident, which is also the divide-by-zero the normal would hit
    // if nothing handled it.
    CHECK(w.car[0].x == w.car[1].x && w.car[0].y == w.car[1].y);

    for (int i = 0; i < GS_TICK_HZ; i++) gs_world_step(&w, &t, nullptr);

    gs_fix apart = gs_fix_len2(w.car[0].x - w.car[1].x, w.car[0].y - w.car[1].y);
    CHECK(apart >= GS_CAR_RADIUS * 2 - GS_ONE / 16);
}

TEST(a_car_can_be_destroyed_by_driving_and_by_being_hit) {
    // Both halves of the item, side by side, because "a car can be destroyed"
    // is two different claims and only one of them is about the ground.
    static gs_track cliff;
    gs_track_init(&cliff, 40, 12, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= cliff.h; y++)
        for (uint8_t x = 0; x <= cliff.w; x++)
            gs_track_set_corner(&cliff, x, y, x <= 8 ? GS_INT(40) : 0);

    gs_world alone;
    gs_world_init(&alone, GS_ONE);
    gs_world_add_car(&alone, &cliff, GS_VEH_MOTORCYCLE, GS_INT(2), GS_INT(6), 0);
    alone.car[0].vx = GS_INT(6);
    for (int i = 0; i < GS_TICK_HZ * 8; i++) gs_world_step(&alone, &cliff, nullptr);

    // Driving alone, off a cliff: nobody else involved.
    CHECK(alone.car[0].wrecked);

    // And by being hit, on flat ground where the landing can take no credit.
    static gs_track flat;
    gs_track_init(&flat, 80, 20, GS_SURF_PAVEMENT);

    gs_world hit;
    gs_world_init(&hit, GS_ONE);
    gs_world_add_car(&hit, &flat, GS_VEH_MOTORCYCLE, GS_INT(20), GS_INT(10), 0);
    gs_world_add_car(&hit, &flat, GS_VEH_STOCK_CAR, GS_INT(60), GS_INT(10),
                     (gs_angle)(GS_QUARTER * 2));

    bool ever_flew = false;
    for (int i = 0; i < GS_TICK_HZ * 20; i++) {
        // Both hold the throttle, facing each other, so every bounce is
        // followed by another run at it - a derby rather than one crash.
        gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, GS_IN_ACCEL, 0, 0 };
        gs_world_step(&hit, &flat, in);
        if (!hit.car[0].grounded) ever_flew = true;
    }

    CHECK(hit.car[0].wrecked);       // destroyed by being hit, on flat ground
    CHECK(ever_flew);

    // And the damage is the *collision's*, not a landing's afterwards. A gentle
    // shunt leaves both cars on the ground and still costs something, which is
    // the only way to know the two sources are separate.
    gs_world nudge;
    gs_world_init(&nudge, GS_ONE);
    gs_world_add_car(&nudge, &flat, GS_VEH_MOTORCYCLE, GS_INT(20), GS_INT(10), 0);
    // Just outside touching distance, so a slow closing speed still reaches.
    gs_world_add_car(&nudge, &flat, GS_VEH_MOTORCYCLE, GS_INT(21) + GS_HALF,
                     GS_INT(10), (gs_angle)(GS_QUARTER * 2));
    nudge.car[0].vx = GS_ONE;
    nudge.car[1].vx = -GS_ONE;

    bool left_ground = false;
    for (int i = 0; i < GS_TICK_HZ; i++) {
        gs_world_step(&nudge, &flat, nullptr);
        if (!nudge.car[0].grounded || !nudge.car[1].grounded) left_ground = true;
    }
    CHECK(!left_ground);
    CHECK(nudge.car[0].damage > 0);

    // The fragile one takes far more of it than the sturdy one from the very
    // same collisions, which is toughness meaning something in a fight and not
    // only on a landing.
    CHECK(hit.car[0].damage > hit.car[1].damage);
}

TEST(a_wreck_is_scenery_that_the_living_bounce_off) {
    static gs_track t;
    gs_track_init(&t, 60, 20, GS_SURF_PAVEMENT);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(20), GS_INT(10), 0);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(30), GS_INT(10), 0);

    // One of them is already finished.
    w.car[1].wrecked = true;
    w.car[1].damage = 255;
    gs_fix wreck_x = w.car[1].x, wreck_y = w.car[1].y;

    w.car[0].vx = GS_INT(6);
    for (int i = 0; i < GS_TICK_HZ * 3; i++) gs_world_step(&w, &t, nullptr);

    // The wreck did not move. Being able to shove a dead car around the track
    // would make debris a toy rather than an obstacle - and Phase 7 wants it to
    // become part of the course.
    CHECK(w.car[1].x == wreck_x);
    CHECK(w.car[1].y == wreck_y);

    // And the living car came off it rather than through it.
    CHECK(w.car[0].vx < 0);
    CHECK(w.car[0].x < wreck_x);
}

TEST(a_hard_enough_hit_puts_a_car_in_the_air) {
    static gs_track t;
    gs_track_init(&t, 60, 20, GS_SURF_PAVEMENT);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(20), GS_INT(10), 0);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(24), GS_INT(10),
                     (gs_angle)(GS_QUARTER * 2));
    w.car[0].vx = GS_INT(7);
    w.car[1].vx = -GS_INT(7);

    bool flew = false;
    for (int i = 0; i < GS_TICK_HZ * 2; i++) {
        gs_world_step(&w, &t, nullptr);
        if (!w.car[0].grounded || !w.car[1].grounded) flew = true;
    }
    CHECK(flew);
}

TEST(a_car_flying_over_another_does_not_hit_it) {
    static gs_track t;
    gs_track_init(&t, 60, 20, GS_SURF_PAVEMENT);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(20), GS_INT(10), 0);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(20), GS_INT(10), 0);

    // One of them well overhead and staying there.
    w.car[1].z = GS_INT(3);
    w.car[1].grounded = false;
    w.car[1].vz = GS_INT(2);

    gs_fix was = w.car[0].x;
    for (int i = 0; i < 10; i++) gs_world_step(&w, &t, nullptr);

    // Being swatted sideways by something passing overhead would be the least
    // readable thing in the game.
    CHECK(w.car[0].x == was);
    CHECK(gs_car_speed(&w.car[0]) == 0);
}

// ---------------------------------------------------------------------------
// What you leave behind
// ---------------------------------------------------------------------------

// Drive a car straight across a patch of ground and report how much of a turn
// it can still make there. `dropper` is who left the hazard, if any.
static double gs_turn_over_hazard(gs_hazard_kind kind, int dropper) {
    static gs_track t;
    gs_track_init(&t, 60, 60, GS_SURF_PAVEMENT);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(30), GS_INT(30), 0);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(30), GS_INT(50), 0);

    if (kind != GS_HAZ_NONE) {
        // Dropped exactly where the first car is about to be.
        w.car[(uint8_t)dropper].x = GS_INT(30);
        w.car[(uint8_t)dropper].y = GS_INT(30);
        CHECK(gs_world_drop(&w, (uint8_t)dropper, kind));
        w.car[1].x = GS_INT(30);
        w.car[1].y = GS_INT(50);
    }

    w.car[0].x = GS_INT(30);
    w.car[0].y = GS_INT(30);
    w.car[0].vx = GS_INT(4);

    gs_angle before = gs_atan2(w.car[0].vy, w.car[0].vx);
    gs_input in[GS_MAX_CARS] = { (gs_input)(GS_IN_ACCEL | GS_IN_LEFT), 0, 0, 0 };
    for (int i = 0; i < GS_TICK_HZ / 3; i++) gs_world_step(&w, &t, in);

    int32_t d = gs_angle_delta(before, gs_atan2(w.car[0].vy, w.car[0].vx));
    return (double)(d < 0 ? -d : d) / 65536.0 * 360.0;
}

TEST(a_hazard_dropped_by_one_car_affects_the_other_and_not_the_dropper) {
    double clean = gs_turn_over_hazard(GS_HAZ_NONE, 0);
    double theirs = gs_turn_over_hazard(GS_HAZ_OIL, 1);   // car one dropped it
    double mine = gs_turn_over_hazard(GS_HAZ_OIL, 0);     // car zero dropped it

    CHECK(clean > 5.0);

    // Somebody else's oil takes the grip away.
    CHECK(theirs < clean / 2.0);

    // Your own does nothing to you. Driving into what you dropped would make
    // the weapon a way of hurting yourself, and nobody would ever use it.
    CHECK(mine > clean - 0.5);
}

TEST(oil_gives_the_grip_back_the_moment_you_are_off_it) {
    static gs_track t;
    gs_track_init(&t, 80, 20, GS_SURF_PAVEMENT);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(10), GS_INT(10), 0);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(20), GS_INT(10), 0);
    CHECK(gs_world_drop(&w, 1, GS_HAZ_OIL));

    // Drive the first car through the slick and out the far side.
    w.car[1].x = GS_INT(60);
    w.car[0].vx = GS_INT(6);
    for (int i = 0; i < GS_TICK_HZ * 3; i++) {
        gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, 0, 0, 0 };
        gs_world_step(&w, &t, in);
    }

    // Well past it now, and cornering normally again: a slick is something to
    // be driven through, not a penalty to be served.
    CHECK(w.car[0].x > GS_INT(25));

    gs_world clean;
    gs_world_init(&clean, GS_ONE);
    gs_world_add_car(&clean, &t, GS_VEH_STOCK_CAR, w.car[0].x, GS_INT(10), 0);
    clean.car[0].vx = w.car[0].vx;

    gs_input turn[GS_MAX_CARS] = { (gs_input)(GS_IN_ACCEL | GS_IN_LEFT), 0, 0, 0 };
    for (int i = 0; i < GS_TICK_HZ / 4; i++) {
        gs_world_step(&w, &t, turn);
        gs_world_step(&clean, &t, turn);
    }
    CHECK(gs_angle_delta(clean.car[0].heading, w.car[0].heading) == 0);
}

TEST(a_mine_goes_off_once_and_hurts_whoever_found_it) {
    static gs_track t;
    gs_track_init(&t, 60, 20, GS_SURF_PAVEMENT);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(10), GS_INT(10), 0);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(20), GS_INT(10), 0);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(10), GS_INT(16), 0);

    // Car one leaves a mine where it stands, then gets out of the way.
    CHECK(gs_world_drop(&w, 1, GS_HAZ_MINE));
    w.car[1].x = GS_INT(50);
    w.car[1].y = GS_INT(16);

    w.car[0].vx = GS_INT(5);
    w.car[2].vx = GS_INT(5);

    bool launched = false;
    for (int i = 0; i < GS_TICK_HZ * 4; i++) {
        gs_world_step(&w, &t, nullptr);
        if (!w.car[0].grounded) launched = true;
    }

    CHECK(launched);
    CHECK(w.car[0].damage > 0);

    // One use. The third car drives over the same ground afterwards and finds
    // nothing there, because a mine that keeps going off is a wall.
    CHECK(w.car[2].damage == 0);
    CHECK(w.hazard[0].spent);
}

TEST(holding_the_button_leaves_a_trail_and_not_a_carpet) {
    static gs_track t;
    gs_track_init(&t, 80, 20, GS_SURF_PAVEMENT);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(4), GS_INT(10), 0);

    gs_input in[GS_MAX_CARS] = { (gs_input)(GS_IN_ACCEL | GS_IN_FIRE), 0, 0, 0 };
    for (int i = 0; i < GS_TICK_HZ * 5; i++) gs_world_step(&w, &t, in);

    // Five seconds of holding it down is about five, not six hundred.
    CHECK(w.hazard_count >= 4);
    CHECK(w.hazard_count <= 7);

    // And they are spread along the road rather than heaped in one place.
    gs_fix span = w.hazard[w.hazard_count - 1].x - w.hazard[0].x;
    CHECK(span > GS_INT(4));
}

TEST(destruction_mode_ends_when_one_car_is_left_driving) {
    static gs_track t;
    gs_track_init(&t, 40, 20, GS_SURF_PAVEMENT);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_set_mode(&w, GS_MODE_DESTRUCTION);
    for (int i = 0; i < 3; i++) {
        gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(5 + 8 * i), GS_INT(10), 0);
    }

    CHECK(!w.over);
    CHECK(w.winner == GS_NO_WINNER);

    // Two of them go. Nothing about the third changes.
    w.car[0].wrecked = true;
    w.car[0].damage = 255;
    gs_world_step(&w, &t, nullptr);
    CHECK(!w.over);

    w.car[2].wrecked = true;
    w.car[2].damage = 255;
    gs_world_step(&w, &t, nullptr);

    CHECK(w.over);
    CHECK(w.winner == 1);

    // Settled. A winner who drives off a cliff in the silence afterwards has
    // still won, and taking it back would be absurd.
    w.car[1].wrecked = true;
    for (int i = 0; i < GS_TICK_HZ; i++) gs_world_step(&w, &t, nullptr);
    CHECK(w.over);
    CHECK(w.winner == 1);
}

TEST(everybody_going_at_once_is_a_draw_rather_than_a_win) {
    static gs_track t;
    gs_track_init(&t, 40, 20, GS_SURF_PAVEMENT);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_set_mode(&w, GS_MODE_DESTRUCTION);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(5), GS_INT(10), 0);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(15), GS_INT(10), 0);

    w.car[0].wrecked = true;
    w.car[1].wrecked = true;
    gs_world_step(&w, &t, nullptr);

    CHECK(w.over);
    CHECK(w.winner == GS_NO_WINNER);
}

TEST(a_race_does_not_end_just_because_somebody_was_wrecked) {
    static gs_track t;
    gs_track_init(&t, 40, 20, GS_SURF_PAVEMENT);

    gs_world w;
    gs_world_init(&w, GS_ONE);          // race mode, which is the default
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(5), GS_INT(10), 0);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(15), GS_INT(10), 0);

    w.car[0].wrecked = true;
    w.car[1].wrecked = true;
    for (int i = 0; i < GS_TICK_HZ; i++) gs_world_step(&w, &t, nullptr);

    // Racing is decided by the flag, not by who is still moving - and this is
    // the same track, cars and physics with one toggle between them.
    CHECK(!w.over);
    CHECK(w.winner == GS_NO_WINNER);
}

TEST(a_destruction_race_fought_out_between_two_cars_finishes_by_itself) {
    // Not staged: two cars driven into each other until one of them stops.
    static gs_track t;
    gs_track_init(&t, 80, 20, GS_SURF_PAVEMENT);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_set_mode(&w, GS_MODE_DESTRUCTION);
    gs_world_add_car(&w, &t, GS_VEH_MOTORCYCLE, GS_INT(20), GS_INT(10), 0);
    gs_world_add_car(&w, &t, GS_VEH_BAJA_BUG, GS_INT(60), GS_INT(10),
                     (gs_angle)(GS_QUARTER * 2));

    for (int i = 0; i < GS_TICK_HZ * 30 && !w.over; i++) {
        gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, GS_IN_ACCEL, 0, 0 };
        gs_world_step(&w, &t, in);
    }

    CHECK(w.over);
    // The baja bug is built for this and the motorcycle is not.
    CHECK(w.winner == 1);
}

// Drive a lap of the same line with the same inputs, and report where it ended
// up and how far off the straight it was pushed. `blocked` puts a wreck on the
// line first.
typedef struct gs_line { gs_fix x, y; double drift; } gs_line;

static gs_line gs_drive_the_line(bool blocked) {
    static gs_track t;
    gs_track_init(&t, 80, 24, GS_SURF_PAVEMENT);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(4), GS_INT(12), 0);

    if (blocked) {
        // Somebody died here last lap, on the line everyone takes.
        gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(30), GS_INT(12), 0);
        w.car[1].wrecked = true;
        w.car[1].damage = 255;
    }

    double drift = 0.0;
    for (int i = 0; i < GS_TICK_HZ * 6; i++) {
        gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, 0, 0, 0 };
        gs_world_step(&w, &t, in);

        double off = (double)(w.car[0].y - GS_INT(12)) / (double)GS_ONE;
        if (off < 0) off = -off;
        if (off > drift) drift = off;
    }
    return (gs_line){ w.car[0].x, w.car[0].y, drift };
}

TEST(a_wreck_changes_the_racing_line_for_the_rest_of_the_race) {
    gs_line clear_run = gs_drive_the_line(false);
    gs_line blocked = gs_drive_the_line(true);

    // With nothing in the way the car runs dead straight down the line.
    CHECK(clear_run.drift < 0.01);
    CHECK(clear_run.x > GS_INT(30));

    // With a wreck on it, the same inputs no longer take the same path: the
    // course has been reshaped by something that happened during the race, and
    // winning the fight has changed the track.
    CHECK(blocked.x != clear_run.x);
    CHECK(blocked.x < clear_run.x);      // it cost time as well as position
}

TEST(a_wreck_is_still_there_much_later_and_has_not_moved) {
    static gs_track t;
    gs_track_init(&t, 80, 24, GS_SURF_PAVEMENT);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(4), GS_INT(12), 0);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(30), GS_INT(12), 0);
    w.car[1].wrecked = true;
    w.car[1].damage = 255;

    gs_fix where_x = w.car[1].x, where_y = w.car[1].y;

    // A long race, with the live car repeatedly driving into it.
    for (int i = 0; i < GS_TICK_HZ * 40; i++) {
        gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, 0, 0, 0 };
        gs_world_step(&w, &t, in);
    }

    // Still exactly where it died. Debris that drifted would stop being track
    // geometry and start being another car.
    CHECK(w.car[1].x == where_x);
    CHECK(w.car[1].y == where_y);
    CHECK(w.car[1].wrecked);

    // And it is still an obstacle rather than having quietly become passable.
    gs_fix gap = gs_fix_len2(w.car[0].x - where_x, w.car[0].y - where_y);
    CHECK(gap >= GS_CAR_RADIUS * 2 - GS_ONE / 4);
}

// ---------------------------------------------------------------------------
// Somebody to race
// ---------------------------------------------------------------------------

// Lay out a rectangular circuit of gates on a plain of the given surface, and
// let the AI drive it. Reports how many laps it managed.
typedef struct gs_ai_run { uint16_t laps; gs_fix travelled; bool wrecked; } gs_ai_run;

static void gs_circuit(gs_track *t, gs_surface surface) {
    gs_track_init(t, 60, 60, surface);
    // Four corners, driven anticlockwise, each gate facing the way you go
    // through it.
    gs_track_add_gate(t, GS_INT(45), GS_INT(15), 0, GS_INT(6));
    gs_track_add_gate(t, GS_INT(45), GS_INT(45), GS_QUARTER, GS_INT(6));
    gs_track_add_gate(t, GS_INT(15), GS_INT(45), (gs_angle)(GS_QUARTER * 2), GS_INT(6));
    gs_track_add_gate(t, GS_INT(15), GS_INT(15), (gs_angle)(GS_QUARTER * 3), GS_INT(6));
}

static gs_ai_run gs_ai_laps(const gs_track *t, gs_fix gravity, uint8_t vehicle,
                            uint32_t seconds) {
    gs_world w;
    gs_world_init(&w, gravity);
    gs_world_add_car(&w, t, vehicle, GS_INT(20), GS_INT(15), 0);

    gs_fix last_x = w.car[0].x, last_y = w.car[0].y, travelled = 0;
    for (uint32_t i = 0; i < GS_TICK_HZ * seconds; i++) {
        gs_input in[GS_MAX_CARS] = { gs_ai_drive(&w, t, 0), 0, 0, 0 };
        gs_world_step(&w, t, in);
        travelled += gs_fix_len2(w.car[0].x - last_x, w.car[0].y - last_y);
        last_x = w.car[0].x;
        last_y = w.car[0].y;
    }
    return (gs_ai_run){ w.car[0].laps, travelled, w.car[0].wrecked };
}

TEST(the_ai_steers_both_ways) {
    // Pinned directly rather than inferred from a lap. The circuit below turns
    // the same way at every corner, so an AI that had lost the ability to steer
    // one of the two directions still got round it - which is a test proving
    // half of what it claims.
    static gs_track t;
    gs_track_init(&t, 60, 60, GS_SURF_PAVEMENT);
    gs_track_add_gate(&t, GS_INT(30), GS_INT(40), 0, GS_INT(4));
    gs_track_add_gate(&t, GS_INT(30), GS_INT(20), 0, GS_INT(4));

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(30), GS_INT(30), 0);

    // Facing along +x with the gate off to one side, then the other.
    w.car[0].next_gate = 0;                       // the gate at greater y
    gs_input toward_y = gs_ai_drive(&w, &t, 0);
    CHECK((toward_y & GS_IN_RIGHT) != 0);
    CHECK((toward_y & GS_IN_LEFT) == 0);

    w.car[0].next_gate = 1;                       // the gate at lesser y
    gs_input away_from_y = gs_ai_drive(&w, &t, 0);
    CHECK((away_from_y & GS_IN_LEFT) != 0);
    CHECK((away_from_y & GS_IN_RIGHT) == 0);

    // And straight at it, no steering at all - a driver that cannot hold a line
    // weaves down every straight.
    w.car[0].y = GS_INT(20);
    gs_input dead_ahead = gs_ai_drive(&w, &t, 0);
    CHECK((dead_ahead & (gs_input)(GS_IN_LEFT | GS_IN_RIGHT)) == 0);
}

TEST(the_ai_gets_round_a_circuit_it_has_never_seen) {
    static gs_track t;
    gs_circuit(&t, GS_SURF_PAVEMENT);

    gs_ai_run run = gs_ai_laps(&t, GS_ONE, (uint8_t)GS_VEH_STOCK_CAR, 90);

    // Round, repeatedly, without help and without a recorded line.
    CHECK(run.laps >= 2);
    CHECK(!run.wrecked);
}

TEST(the_ai_gets_round_on_surfaces_and_vehicles_it_was_not_tuned_for) {
    static gs_track dirt, ice;
    gs_circuit(&dirt, GS_SURF_DIRT);
    gs_circuit(&ice, GS_SURF_ICE);

    // Dirt has two thirds of the grip and ice has a sixth of it. Nothing about
    // the driver changes; what changes is what it works out it can do.
    CHECK(gs_ai_laps(&dirt, GS_ONE, (uint8_t)GS_VEH_DUNE_BUGGY, 90).laps >= 1);
    CHECK(gs_ai_laps(&ice, GS_ONE, (uint8_t)GS_VEH_MOTORCYCLE, 150).laps >= 1);

    // And in a machine with completely different numbers.
    CHECK(gs_ai_laps(&dirt, GS_ONE, (uint8_t)GS_VEH_LUNAR_ROVER, 150).laps >= 1);
}

// How far from the corner the AI decides it must slow down.
//
// The car is held at a constant speed rather than left to drive, so what is
// measured is the *decision* and not the approach. Otherwise stronger gravity
// changes how fast it arrives as well as how late it brakes, and the two are
// impossible to tell apart.
static double gs_braking_distance(gs_fix gravity, uint8_t vehicle, gs_surface surface) {
    static gs_track t;
    gs_track_init(&t, 60, 60, surface);

    // A gate square across the road, so reaching it means a right angle.
    gs_track_add_gate(&t, GS_INT(40), GS_INT(30), GS_QUARTER, GS_INT(5));
    gs_track_add_gate(&t, GS_INT(40), GS_INT(50), GS_QUARTER, GS_INT(5));

    gs_world w;
    gs_world_init(&w, gravity);
    gs_world_add_car(&w, &t, vehicle, GS_INT(6), GS_INT(30), 0);

    for (int i = 0; i < GS_TICK_HZ * 20; i++) {
        // Held at a steady five tiles a second, pointing down the straight.
        w.car[0].vx = GS_INT(5);
        w.car[0].vy = 0;
        w.car[0].heading = 0;

        gs_input want = gs_ai_drive(&w, &t, 0);
        if ((want & GS_IN_BRAKE) != 0) {
            gs_fix dx = t.gate[0].x - w.car[0].x;
            gs_fix dy = t.gate[0].y - w.car[0].y;
            return (double)gs_fix_len2(dx, dy) / (double)GS_ONE;
        }

        gs_input in[GS_MAX_CARS] = { want, 0, 0, 0 };
        gs_world_step(&w, &t, in);
    }
    return -1.0;   // never braked at all
}

TEST(the_same_corner_is_braked_for_differently_under_different_gravity) {
    double light = gs_braking_distance(GS_RATIO(4, 10), (uint8_t)GS_VEH_STOCK_CAR,
                                       GS_SURF_PAVEMENT);
    double heavy = gs_braking_distance(GS_RATIO(18, 10), (uint8_t)GS_VEH_STOCK_CAR,
                                       GS_SURF_PAVEMENT);

    CHECK(light > 0.0);
    CHECK(heavy > 0.0);

    // Grip is a multiple of gravity, so heavier gravity means more of it and a
    // corner that can be left later. A baked speed profile would give the same
    // answer for both, which is the failure this is looking for: it would look
    // right and be wrong the moment somebody moved the dial.
    CHECK(light > heavy * 1.5);
}

TEST(the_same_corner_is_braked_for_differently_in_a_different_car) {
    double slippery = gs_braking_distance(GS_ONE, (uint8_t)GS_VEH_SPRINT_CAR,
                                          GS_SURF_PAVEMENT);
    double grippy = gs_braking_distance(GS_ONE, (uint8_t)GS_VEH_LUNAR_ROVER,
                                        GS_SURF_PAVEMENT);

    CHECK(slippery > 0.0 && grippy > 0.0);

    // The sprint car has the least grip in the roster and the rover the most,
    // so this should be a wide gap and not a photo finish.
    //
    // Written as a bare `slippery > grippy` first, and it passed with the
    // vehicle's tyres taken out of the AI entirely: the two came to 3.961 and
    // 3.946, and the four-thousandths between them was the cars' *drag*
    // pushing them along the straight at fractionally different rates. A
    // comparison that can be satisfied by noise is not a comparison.
    CHECK(slippery > grippy * 1.5);

    // And the surface counts as well as the machine - ice by a mile.
    double on_ice = gs_braking_distance(GS_ONE, (uint8_t)GS_VEH_STOCK_CAR,
                                        GS_SURF_ICE);
    double on_road = gs_braking_distance(GS_ONE, (uint8_t)GS_VEH_STOCK_CAR,
                                         GS_SURF_PAVEMENT);
    CHECK(on_ice > on_road * 3.0);
}

TEST(a_quicker_driver_carries_more_speed_through_the_same_corner) {
    // Difficulty is one number: how much of the available grip the driver is
    // willing to use. Not extra power, not rubber-banding, not cheating on the
    // physics - the same thing that separates two people.
    static gs_track t;
    gs_track_init(&t, 60, 60, GS_SURF_PAVEMENT);
    gs_track_add_gate(&t, GS_INT(40), GS_INT(30), GS_QUARTER, GS_INT(5));
    gs_track_add_gate(&t, GS_INT(40), GS_INT(50), GS_QUARTER, GS_INT(5));

    gs_world w;
    gs_world_init(&w, GS_ONE);
    // Four tiles short of the corner at five tiles a second, which is exactly
    // where a cautious driver has already decided and a quick one has not.
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(36), GS_INT(30), 0);
    w.car[0].vx = GS_INT(5);

    gs_input careful = gs_ai_drive_at(&w, &t, 0, GS_AI_CAUTIOUS);
    gs_input quick = gs_ai_drive_at(&w, &t, 0, GS_AI_QUICK);

    CHECK((careful & GS_IN_BRAKE) != 0);
    CHECK((quick & GS_IN_BRAKE) == 0);
    CHECK((quick & GS_IN_ACCEL) != 0);

    // And the default sits between them rather than at one end, which is what
    // makes it worth racing rather than a formality in either direction.
    CHECK(GS_AI_CAUTIOUS < GS_AI_NORMAL);
    CHECK(GS_AI_NORMAL < GS_AI_QUICK);
    CHECK(GS_AI_QUICK < GS_ONE);
}

TEST(an_ai_race_is_deterministic_like_every_other_race) {
    static gs_track t;
    gs_circuit(&t, GS_SURF_DIRT);

    uint64_t first = 0;
    for (int run = 0; run < 3; run++) {
        gs_world w;
        gs_world_init(&w, GS_ONE);
        gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(20), GS_INT(15), 0);
        gs_world_add_car(&w, &t, GS_VEH_DUNE_BUGGY, GS_INT(20), GS_INT(18), 0);

        for (int i = 0; i < GS_TICK_HZ * 30; i++) {
            gs_input in[GS_MAX_CARS] = {
                gs_ai_drive(&w, &t, 0), gs_ai_drive(&w, &t, 1), 0, 0
            };
            gs_world_step(&w, &t, in);
        }
        uint64_t h = gs_world_hash(&w);
        if (run == 0) first = h;
        CHECK(h == first);
    }
}

// ---------------------------------------------------------------------------
// Asking questions about a track
// ---------------------------------------------------------------------------

static gs_analysis gs_report;   // 8 KB, so not on the stack

TEST(the_analyser_calls_a_jump_nobody_can_clear_impossible) {
    // A ramp to a cliff edge, a chasm, and the landing on the far side. Clearing
    // it is a matter of how far a car flies, which is a matter of gravity - so
    // this is completable at the light end of the dial and not at the heavy one,
    // and that is exactly the answer a designer needs before anybody drives it.
    static gs_track t;
    gs_track_init(&t, 64, 16, GS_SURF_PAVEMENT);

    for (uint8_t y = 0; y <= t.h; y++) {
        for (uint8_t x = 0; x <= t.w; x++) {
            gs_fix z;
            if (x < 12) z = 0;                                       // run up
            else if (x < 18) z = (gs_fix)((int64_t)GS_INT(2) * (x - 12) / 6);  // ramp
            else if (x < 34) z = -GS_INT(30);                        // the chasm
            else z = 0;                                              // the landing
            gs_track_set_corner(&t, x, y, z);
        }
    }
    gs_track_add_gate(&t, GS_INT(4), GS_INT(8), 0, GS_INT(6));
    gs_track_add_gate(&t, GS_INT(50), GS_INT(8), 0, GS_INT(6));

    gs_analyse(&t, 25, &gs_report);

    // Somebody, somewhere in the range, can do it - otherwise this would be
    // testing that a broken track is broken.
    CHECK(gs_report.completable);

    // And not everybody: the envelope has a top to it, which is the whole
    // point of reporting one.
    CHECK(gs_report.completed[0] > 0);
    CHECK(gs_report.completed[GS_ANALYSIS_STEPS - 1] == 0);
    CHECK(gs_report.heaviest < gs_report.gravity[GS_ANALYSIS_STEPS - 1]);
}

TEST(the_analyser_says_so_when_a_track_cannot_be_got_round_at_all) {
    // A wall across the road, too tall and too steep for anything.
    static gs_track t;
    gs_track_init(&t, 64, 16, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= t.h; y++)
        for (uint8_t x = 0; x <= t.w; x++)
            gs_track_set_corner(&t, x, y, (x >= 24 && x <= 27) ? GS_INT(60) : 0);

    gs_track_add_gate(&t, GS_INT(4), GS_INT(8), 0, GS_INT(6));
    gs_track_add_gate(&t, GS_INT(50), GS_INT(8), 0, GS_INT(6));

    gs_analyse(&t, 20, &gs_report);
    CHECK(!gs_report.completable);
}

TEST(the_heatmap_shows_where_everybody_actually_went) {
    static gs_track t;
    gs_circuit(&t, GS_SURF_PAVEMENT);

    gs_analyse(&t, 30, &gs_report);
    CHECK(gs_report.completable);
    CHECK(gs_report.busiest > 0);

    // The corners of the circuit are driven; the middle of it is not. A racing
    // line you can see is the thing that changes how a track gets built.
    gs_fix on_the_line = gs_analysis_heat(&gs_report, 45, 15);
    gs_fix in_the_middle = gs_analysis_heat(&gs_report, 30, 30);

    CHECK(on_the_line > 0);
    CHECK(in_the_middle < on_the_line);

    // And somewhere is the busiest, at full heat, so the scale means something.
    bool saw_peak = false;
    for (uint8_t x = 0; x < t.w && !saw_peak; x++) {
        for (uint8_t y = 0; y < t.h; y++) {
            if (gs_analysis_heat(&gs_report, x, y) == GS_ONE) { saw_peak = true; break; }
        }
    }
    CHECK(saw_peak);
}

// ---------------------------------------------------------------------------
// Ghosts
// ---------------------------------------------------------------------------

// A short scripted race on a flat track: two cars, one of them turning, so a
// recording of it has something in it worth comparing.
static void gs_ghost_scene(gs_track *t, gs_world *w) {
    gs_track_init(t, 40, 16, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= t->h; y++)
        for (uint8_t x = 0; x <= t->w; x++) gs_track_set_corner(t, x, y, 0);

    gs_world_init(w, GS_ONE);
    gs_world_add_car(w, t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(4), GS_INT(5), 0);
    gs_world_add_car(w, t, (uint8_t)GS_VEH_DUNE_BUGGY, GS_INT(4), GS_INT(11),
                     GS_DEG(45));
}

static void gs_ghost_inputs(uint32_t tick, gs_input *in) {
    in[0] = GS_IN_ACCEL | ((tick / 40u) % 2u == 0u ? GS_IN_LEFT : GS_IN_RIGHT);
    in[1] = GS_IN_ACCEL;
    in[2] = 0;
    in[3] = 0;
}

TEST(a_ghost_is_the_race_that_made_it_not_a_picture_of_it) {
    static gs_track t;
    gs_world w;
    gs_ghost_scene(&t, &w);

    static gs_replay rec;
    gs_replay_begin(&rec, &w, &t);

    const uint32_t ticks = GS_TICK_HZ * 4;
    for (uint32_t i = 0; i < ticks; i++) {
        gs_input in[GS_MAX_CARS];
        gs_ghost_inputs(i, in);
        gs_replay_record(&rec, in);
        gs_world_step(&w, &t, in);
    }

    // Out to the wire and back, which is the only form a ghost ever arrives in.
    static uint8_t bytes[sizeof(gs_replay) + 4096];
    size_t n = gs_replay_serialize(&rec, bytes, sizeof bytes);
    CHECK(n > 0);

    static gs_ghost g;
    CHECK(gs_ghost_load(&g, &t, bytes, n));
    CHECK(g.ready);
    CHECK(gs_ghost_length(&g) == ticks);

    // Stepped beside a fresh run of the same race, it has to be in the same
    // place at *every* tick. Agreeing only at the end would be a ghost you
    // could not race against - it would drift and then arrive.
    gs_world beside;
    gs_ghost_scene(&t, &beside);
    for (uint32_t i = 0; i < ticks; i++) {
        gs_input in[GS_MAX_CARS];
        gs_ghost_inputs(i, in);
        gs_world_step(&beside, &t, in);
        gs_ghost_step(&g, &t);
        CHECK(gs_world_hash(&g.world) == gs_world_hash(&beside));
    }
    CHECK(g.finished);

    // And it went somewhere. A ghost that agrees perfectly by not moving would
    // pass everything above.
    const gs_car *c = gs_ghost_car(&g);
    CHECK(c != nullptr);
    if (c != nullptr) CHECK(c->x > GS_INT(10));
}

TEST(a_ghost_from_another_track_is_refused_rather_than_raced) {
    static gs_track t, other;
    gs_world w;
    gs_ghost_scene(&t, &w);

    static gs_replay rec;
    gs_replay_begin(&rec, &w, &t);
    for (uint32_t i = 0; i < 240; i++) {
        gs_input in[GS_MAX_CARS];
        gs_ghost_inputs(i, in);
        gs_replay_record(&rec, in);
        gs_world_step(&w, &t, in);
    }

    static uint8_t bytes[sizeof(gs_replay) + 4096];
    size_t n = gs_replay_serialize(&rec, bytes, sizeof bytes);

    // One corner different. The hash is the whole track, so this is a different
    // track and the same inputs would be a different race on it.
    gs_ghost_scene(&other, &w);
    gs_track_set_corner(&other, 20, 8, GS_INT(2));
    CHECK(gs_track_hash(&other) != gs_track_hash(&t));

    static gs_ghost g;
    CHECK(!gs_ghost_load(&g, &other, bytes, n));
    CHECK(!g.ready);
    CHECK(gs_ghost_car(&g) == nullptr);   // and it draws nothing

    // The bytes were kept, though: the right track afterwards works.
    CHECK(gs_ghost_load(&g, &t, bytes, n));
    CHECK(g.ready);
}

TEST(a_ghost_carries_its_own_starting_grid) {
    static gs_track t;
    gs_world w;
    gs_ghost_scene(&t, &w);

    // Move the grid somewhere nothing would guess, so that a replay which
    // quietly re-created the default one would be caught.
    w.car[0].x = GS_INT(17);
    w.car[0].y = GS_INT(3);
    w.car[0].heading = GS_DEG(90);
    w.car[1].x = GS_INT(29);
    w.car[1].y = GS_INT(13);

    static gs_replay rec;
    gs_replay_begin(&rec, &w, &t);
    for (uint32_t i = 0; i < 120; i++) {
        gs_input in[GS_MAX_CARS];
        gs_ghost_inputs(i, in);
        gs_replay_record(&rec, in);
        gs_world_step(&w, &t, in);
    }

    static uint8_t bytes[sizeof(gs_replay) + 4096];
    size_t n = gs_replay_serialize(&rec, bytes, sizeof bytes);

    // Nothing here is told where the cars stood. It has to come out of the
    // recording, or a ghost somebody sends you starts from the wrong square.
    static gs_ghost g;
    CHECK(gs_ghost_load(&g, &t, bytes, n));
    const gs_car *c = gs_ghost_car(&g);
    CHECK(c != nullptr);
    if (c != nullptr) {
        CHECK(c->x == GS_INT(17));
        CHECK(c->y == GS_INT(3));
        CHECK(c->heading == GS_DEG(90));
    }

    gs_world played;
    CHECK(gs_replay_playback(&rec, &t, &played));
    CHECK(gs_world_hash(&played) == gs_world_hash(&w));
}

TEST(a_ghost_that_runs_out_stays_where_it_finished) {
    static gs_track t;
    gs_world w;
    gs_ghost_scene(&t, &w);

    static gs_replay rec;
    gs_replay_begin(&rec, &w, &t);
    for (uint32_t i = 0; i < 200; i++) {
        gs_input in[GS_MAX_CARS];
        gs_ghost_inputs(i, in);
        gs_replay_record(&rec, in);
        gs_world_step(&w, &t, in);
    }

    static gs_ghost g;
    CHECK(gs_ghost_take(&g, &rec, &t));
    for (uint32_t i = 0; i < 200; i++) gs_ghost_step(&g, &t);
    CHECK(g.finished);

    // The last thing anybody wants to know is where it beat them, so it stops
    // rather than vanishing or coasting on under no input.
    gs_fix x = gs_ghost_car(&g)->x;
    for (uint32_t i = 0; i < 600; i++) gs_ghost_step(&g, &t);
    CHECK(gs_ghost_car(&g)->x == x);

    // And it can be sent back to the start line to run again.
    gs_ghost_reset(&g, &t);
    CHECK(!g.finished);
    CHECK(gs_ghost_car(&g)->x == GS_INT(4));
}

// ---------------------------------------------------------------------------
// Sharing a track
// ---------------------------------------------------------------------------

static void gs_share_track(gs_track *t) {
    gs_track_init(t, 40, 24, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= t->h; y++) {
        for (uint8_t x = 0; x <= t->w; x++) {
            gs_fix h = 0;
            if (x > 10 && x < 15) h = (gs_fix)((int64_t)GS_INT(1) * (x - 10) / 4);
            else if (x >= 15 && x < 22) h = GS_INT(1);
            gs_track_set_corner(t, x, y, h);
        }
    }
    for (uint8_t x = 0; x < t->w; x++) {
        for (uint8_t y = 0; y < t->h; y++) {
            if (x >= 26 && x < 34) gs_track_set_surface(t, x, y, GS_SURF_ICE);
            else if (y < 6) gs_track_set_surface(t, x, y, GS_SURF_DIRT);
            if (x >= 15 && x < 21) gs_track_set_gravity(t, x, y, GS_RATIO(35, 100));
        }
    }
    gs_track_add_gate(t, GS_INT(6), GS_INT(12), 0, GS_INT(5));
    gs_track_add_gate(t, GS_INT(34), GS_INT(12), 0, GS_INT(5));
}

TEST(the_packer_gives_back_exactly_what_it_was_given) {
    static uint8_t in[9000], packed[GS_PACK_BOUND(sizeof in)], out[sizeof in];

    // Four shapes of input, because a packer that only handles the easy one is
    // a packer that corrupts a track somebody spent an evening on.
    for (int shape = 0; shape < 4; shape++) {
        size_t n = sizeof in;
        uint32_t seed = 7u;
        for (size_t i = 0; i < n; i++) {
            seed = seed * 1103515245u + 12345u;
            switch (shape) {
            case 0: in[i] = 0; break;                             // all one byte
            case 1: in[i] = (uint8_t)(i & 1u); break;             // a two-byte pattern
            case 2: in[i] = (uint8_t)(seed >> 16); break;         // nothing repeated
            default: in[i] = (uint8_t)((i / 64u) & 0xffu); break; // long stretches
            }
        }

        size_t pn = gs_pack(in, n, packed, sizeof packed);
        CHECK(pn > 0);
        CHECK(gs_unpack(packed, pn, out, sizeof out) == n);
        CHECK(memcmp(in, out, n) == 0);

        // And the repetitive shapes have to actually be smaller, or the packer
        // is only proving it can copy.
        if (shape != 2) CHECK(pn < n / 4);
    }

    // Nothing at all, which is the input every codec gets wrong once.
    CHECK(gs_pack(in, 0, packed, sizeof packed) == 0);
    CHECK(gs_unpack(packed, 0, out, sizeof out) == 0);

    // And a buffer too small is refused rather than written past.
    CHECK(gs_pack(in, sizeof in, packed, 4) == 0);
}

TEST(a_track_survives_being_pasted_into_a_chat_window) {
    static gs_track t, back;
    gs_share_track(&t);

    static char code[GS_SHARE_MAX];
    size_t n = gs_track_to_code(&t, code, sizeof code);
    CHECK(n > 0);

    // Short enough to be a code rather than a file. A track is four kilobytes
    // of mostly flat ground; if this is not a fraction of that, sharing it in a
    // message is not a thing anybody will do.
    static uint8_t raw[GS_TRACK_TILES * 4 + 4096];
    size_t raw_n = gs_track_serialize(&t, raw, sizeof raw);
    CHECK(n < raw_n / 4);

    CHECK(gs_track_from_code(&back, code));
    CHECK(gs_track_hash(&back) == gs_track_hash(&t));

    // Not merely the same hash - the same track, corner by corner and tile by
    // tile, including the gravity somebody painted and the route they laid.
    CHECK(back.w == t.w && back.h == t.h);
    for (uint8_t y = 0; y <= t.h; y++) {
        for (uint8_t x = 0; x <= t.w; x++) {
            size_t c = (size_t)y * (GS_TRACK_MAX + 1) + x;
            CHECK(back.corner[c] == t.corner[c]);
        }
    }
    for (uint8_t x = 0; x < t.w; x++) {
        for (uint8_t y = 0; y < t.h; y++) {
            gs_fix cx = GS_INT(x) + GS_HALF, cy = GS_INT(y) + GS_HALF;
            CHECK(gs_track_surface(&back, cx, cy) == gs_track_surface(&t, cx, cy));
            CHECK(gs_track_gravity(&back, cx, cy) == gs_track_gravity(&t, cx, cy));
        }
    }
    CHECK(back.gate_count == t.gate_count);

    // As a URL, which is the other thing people send.
    static char url[GS_SHARE_MAX];
    CHECK(gs_track_to_url(&t, url, sizeof url) > 0);
    CHECK(gs_track_from_code(&back, url));
    CHECK(gs_track_hash(&back) == gs_track_hash(&t));

    // And with whatever the chat client wrapped it in.
    static char messy[GS_SHARE_MAX + 8];
    snprintf(messy, sizeof messy, "  \n\t%s\n ", code);
    CHECK(gs_track_from_code(&back, messy));
    CHECK(gs_track_hash(&back) == gs_track_hash(&t));

    // Things that are not codes are refused, not guessed at.
    CHECK(!gs_track_from_code(&back, ""));
    CHECK(!gs_track_from_code(&back, "GST1"));
    CHECK(!gs_track_from_code(&back, "hello"));
    CHECK(!gs_track_from_code(&back, "GST2AAAAAAAAAAAA"));
    CHECK(!gs_track_from_code(&back, nullptr));
}

TEST(a_damaged_code_never_becomes_a_different_track) {
    static gs_track t, back;
    gs_share_track(&t);

    static char code[GS_SHARE_MAX];
    size_t n = gs_track_to_code(&t, code, sizeof code);
    uint64_t want = gs_track_hash(&t);

    // Every single-character change, one at a time. Each has to be either
    // refused or the same track: what must never happen is a code that lost a
    // character in a chat window quietly becoming a track nobody built.
    int accepted = 0;
    for (size_t i = 4; i < n; i++) {
        char save = code[i];
        code[i] = (save == 'A') ? 'B' : 'A';
        if (gs_track_from_code(&back, code)) {
            accepted++;
            CHECK(gs_track_hash(&back) == want);
        }
        code[i] = save;
    }
    // Some changes genuinely encode the same track - the track format has slack
    // in it. This just pins that the loop did something.
    CHECK(accepted < (int)n);

    // A truncated code, which is what a line-wrapping chat client produces.
    for (size_t cut = 8; cut < n; cut += 37) {
        char save = code[cut];
        code[cut] = '\0';
        if (gs_track_from_code(&back, code)) CHECK(gs_track_hash(&back) == want);
        code[cut] = save;
    }
}

TEST(a_code_is_the_same_code_on_every_machine) {
    // A small fixed track and the exact string it encodes to. A code is a wire
    // format between two people, so it is pinned the same way the golden replay
    // is: if this moves, every code anybody has shared has stopped working, and
    // that should be a decision rather than a surprise.
    static const char *want =
        "GST1nO3tcjgrKaH_R1RSSwIAAAATCAYFAQADAQkGEQ8RDzARDxEPEQ8RDgFAAQ8BDUEC"
        "Ew8BDwEPAQJqAwMDAALoAwYNCA";

    static gs_track t, back;
    gs_track_init(&t, 8, 6, GS_SURF_DIRT);
    for (uint8_t y = 0; y <= t.h; y++)
        for (uint8_t x = 0; x <= t.w; x++)
            gs_track_set_corner(&t, x, y, x == 4 ? GS_INT(1) : 0);
    gs_track_set_surface(&t, 2, 2, GS_SURF_ICE);
    gs_track_add_gate(&t, GS_INT(1), GS_INT(3), 0, GS_INT(2));
    gs_track_add_gate(&t, GS_INT(6), GS_INT(3), 0, GS_INT(2));

    static char code[GS_SHARE_MAX];
    CHECK(gs_track_to_code(&t, code, sizeof code) > 0);
    CHECK(strcmp(code, want) == 0);

    // And the committed string still reads back as the track it was taken from.
    CHECK(gs_track_from_code(&back, want));
    CHECK(gs_track_hash(&back) == gs_track_hash(&t));
}

// ---------------------------------------------------------------------------
// Rollback netcode
// ---------------------------------------------------------------------------

// A link with latency, jitter and loss, and no allocation - a fixed ring of
// datagrams, each with the tick it becomes deliverable on. Deterministic, so a
// failure is a failure somebody else can reproduce rather than a bad afternoon.
#define GS_LINK_MAX 512
#define GS_LINK_MTU 96

typedef struct gs_link {
    struct {
        uint8_t  bytes[GS_LINK_MTU];
        size_t   len;
        uint32_t due;
        bool     live;
    } slot[GS_LINK_MAX];

    uint32_t seed;
    uint32_t latency;     // ticks
    uint32_t jitter;      // ticks, added at random
    uint32_t loss_pct;
    uint32_t sent, dropped, delivered;
} gs_link;

static uint32_t gs_link_rand(gs_link *l) {
    l->seed = l->seed * 1103515245u + 12345u;
    return (l->seed >> 16) & 0x7fffu;
}

static void gs_link_send(gs_link *l, uint32_t now, const uint8_t *b, size_t n) {
    l->sent++;
    if (n > GS_LINK_MTU) return;
    if (l->loss_pct > 0 && gs_link_rand(l) % 100u < l->loss_pct) {
        l->dropped++;
        return;
    }
    for (int i = 0; i < GS_LINK_MAX; i++) {
        if (l->slot[i].live) continue;
        memcpy(l->slot[i].bytes, b, n);
        l->slot[i].len  = n;
        l->slot[i].due  = now + l->latency +
                          (l->jitter ? gs_link_rand(l) % (l->jitter + 1u) : 0u);
        l->slot[i].live = true;
        return;
    }
    l->dropped++;   // the link is full, which is also a kind of loss
}

// Everything that has come due, in slot order - which is not arrival order, so
// this delivers out of order as well as late. Both are things a real link does
// and both are things rollback has to survive.
static void gs_link_deliver(gs_link *l, uint32_t now, gs_net *to, const gs_track *t) {
    for (int i = 0; i < GS_LINK_MAX; i++) {
        if (!l->slot[i].live || l->slot[i].due > now) continue;
        gs_net_receive(to, t, l->slot[i].bytes, l->slot[i].len);
        l->slot[i].live = false;
        l->delivered++;
    }
}

static void gs_net_scene(gs_track *t, gs_world *w) {
    gs_track_init(t, 48, 20, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= t->h; y++)
        for (uint8_t x = 0; x <= t->w; x++)
            gs_track_set_corner(t, x, y, x > 20 && x < 26 ? GS_INT(1) : 0);

    static const uint8_t grid[GS_MAX_CARS] = {
        (uint8_t)GS_VEH_STOCK_CAR, (uint8_t)GS_VEH_DUNE_BUGGY,
        (uint8_t)GS_VEH_SPRINT_CAR, (uint8_t)GS_VEH_BAJA_BUG,
    };
    gs_world_init(w, GS_ONE);
    for (uint8_t i = 0; i < GS_MAX_CARS; i++) {
        gs_world_add_car(w, t, grid[i], GS_INT(4), GS_INT(5) + GS_INT(3) * i, 0);
    }
}

// What each player does. Deliberately not constant: a player who never changes
// their input is a player whose every prediction is right, which would test
// nothing.
static gs_input gs_net_drive(uint8_t player, uint32_t tick) {
    // Four different drivers on four different rhythms, so no two players ever
    // change their minds together and a session that confused one for another
    // would be caught rather than accidentally agreeing.
    uint32_t turn = 23u + player * 14u;
    uint32_t brake = 53u + player * 19u;

    gs_input in = GS_IN_ACCEL;
    if ((tick / turn) % 3u == 0u) in |= GS_IN_LEFT;
    else if ((tick / turn) % 3u == 1u) in |= GS_IN_RIGHT;
    if ((tick / brake) % 4u == 0u) in |= GS_IN_BRAKE;
    return in;
}

// Run both machines for `ticks`, over a link with the given conditions, and
// hand back what each ended up believing. Static because two worlds and two
// sessions are the better part of a hundred kilobytes.
static void gs_net_race(uint32_t ticks, uint32_t latency, uint32_t jitter,
                        uint32_t loss_pct, gs_net *a, gs_net *b, gs_link *ab,
                        gs_link *ba, gs_track *t) {
    gs_world w;
    gs_net_scene(t, &w);

    gs_net_begin(a, &w, 2, 0);
    gs_net_begin(b, &w, 2, 1);

    *ab = (gs_link){ 0 };
    *ba = (gs_link){ 0 };
    ab->seed = 0x1234u; ba->seed = 0x9876u;
    ab->latency = ba->latency = latency;
    ab->jitter  = ba->jitter  = jitter;
    ab->loss_pct = ba->loss_pct = loss_pct;

    for (uint32_t tick = 0; tick < ticks; tick++) {
        gs_link_deliver(ab, tick, b, t);
        gs_link_deliver(ba, tick, a, t);

        gs_net_local_input(a, gs_net_drive(0, tick));
        gs_net_local_input(b, gs_net_drive(1, tick));

        uint8_t buf[GS_LINK_MTU];
        size_t n = gs_net_packet(a, buf, sizeof buf);
        gs_link_send(ab, tick, buf, n);
        n = gs_net_packet(b, buf, sizeof buf);
        gs_link_send(ba, tick, buf, n);

        gs_net_step(a, t);
        gs_net_step(b, t);
    }

    // Let the tail of the link drain, so both machines have every input and can
    // confirm the whole race. Without this the last few ticks are still guesses
    // on both sides and comparing them would be comparing predictions.
    for (uint32_t tick = ticks; tick < ticks + latency + jitter + 8u; tick++) {
        gs_link_deliver(ab, tick, b, t);
        gs_link_deliver(ba, tick, a, t);
    }
}

TEST(two_machines_race_to_the_same_finish_over_a_bad_connection) {
    static gs_net a, b;
    static gs_link ab, ba;
    static gs_track t;

    // 24 ticks of latency is 200 ms each way at 120 Hz - a bad connection by any
    // standard - with 40 ms of jitter on top and one packet in eight thrown
    // away. Nothing here asks for a retransmission; the redundancy in each
    // datagram is what absorbs the loss.
    const uint32_t ticks = GS_TICK_HZ * 12;
    gs_net_race(ticks, 24, 5, 12, &a, &b, &ab, &ba, &t);

    // The whole race is confirmed on both machines...
    CHECK(a.confirmed_tick == ticks);
    CHECK(b.confirmed_tick == ticks);

    // ...and it is the same race. This is the claim: two machines, a bad link,
    // no shared authority, and the same world at the end.
    CHECK(gs_world_hash(&a.confirmed) == gs_world_hash(&b.confirmed));
    CHECK(!a.desynced);
    CHECK(!b.desynced);

    // And it is the race that would have happened on one machine with no
    // network at all, which is the stronger claim and the one that says the
    // rollback is correct rather than merely consistent.
    gs_world solo;
    gs_net_scene(&t, &solo);
    for (uint32_t tick = 0; tick < ticks; tick++) {
        gs_input in[GS_MAX_CARS] = { 0 };
        in[0] = gs_net_drive(0, tick);
        in[1] = gs_net_drive(1, tick);
        gs_world_step(&solo, &t, in);
    }
    CHECK(gs_world_hash(&solo) == gs_world_hash(&a.confirmed));

    // The link really was bad and the rollback really did work for its living.
    CHECK(ab.dropped > 0);
    CHECK(a.rollbacks > 0);
    CHECK(b.rollbacks > 0);

    // Nobody stalled: a quarter second of redundancy covers this, so the race
    // never had to wait for anything.
    CHECK(a.stalls == 0);
    CHECK(b.stalls == 0);

    // And the cost is bounded. Both players change input a few times a second,
    // and roughly that many rollbacks is what a correct predictor produces over
    // twelve seconds. An order of magnitude more means it is rewinding on ticks
    // it guessed right about, which is how this was first written.
    CHECK(a.rollbacks < ticks / 8u);
    CHECK(b.rollbacks < ticks / 8u);

    // And the cars went somewhere, so this is a race rather than a grid.
    CHECK(a.confirmed.car[0].x > GS_INT(20));
}

TEST(four_machines_race_to_the_same_finish_over_a_bad_connection) {
    // Four players is what this game is for, and four players is a different
    // problem from two: every machine is guessing about *three* other people at
    // once, and a rollback is triggered by whichever of them changed their mind
    // first. Twelve links rather than two, all of them bad.
    static gs_net net[4];
    static gs_link link[4][4];
    static gs_track t;
    static gs_world w;

    gs_net_scene(&t, &w);
    for (int i = 0; i < 4; i++) gs_net_begin(&net[i], &w, 4, (uint8_t)i);

    for (int a = 0; a < 4; a++) {
        for (int b = 0; b < 4; b++) {
            link[a][b] = (gs_link){ 0 };
            link[a][b].seed = 0x2000u + (uint32_t)a * 37u + (uint32_t)b * 11u;
            link[a][b].latency = 18u + (uint32_t)(a + b) * 3u;   // all different
            link[a][b].jitter = 6;
            link[a][b].loss_pct = 10;
        }
    }

    const uint32_t ticks = GS_TICK_HZ * 8;
    for (uint32_t tick = 0; tick < ticks; tick++) {
        for (int a = 0; a < 4; a++) {
            for (int b = 0; b < 4; b++) {
                if (a != b) gs_link_deliver(&link[a][b], tick, &net[b], &t);
            }
        }

        for (int i = 0; i < 4; i++) {
            gs_net_local_input(&net[i], gs_net_drive((uint8_t)i, tick));

            uint8_t buf[GS_LINK_MTU];
            size_t n = gs_net_packet(&net[i], buf, sizeof buf);

            // A mesh: the same packet to each of the other three, which is what
            // the socket layer does with one gs_wire_send.
            for (int b = 0; b < 4; b++) {
                if (b != i) gs_link_send(&link[i][b], tick, buf, n);
            }
            gs_net_step(&net[i], &t);
        }
    }

    for (uint32_t tick = ticks; tick < ticks + 80u; tick++) {
        for (int a = 0; a < 4; a++) {
            for (int b = 0; b < 4; b++) {
                if (a != b) gs_link_deliver(&link[a][b], tick, &net[b], &t);
            }
        }
    }

    // All four confirmed the whole race, and all four agree about it.
    for (int i = 0; i < 4; i++) {
        CHECK(net[i].confirmed_tick == ticks);
        CHECK(!net[i].desynced);
        CHECK(gs_world_hash(&net[i].confirmed) == gs_world_hash(&net[0].confirmed));
        CHECK(net[i].stalls == 0);
    }

    // And it is the race one machine with no network would have run.
    gs_world solo;
    gs_net_scene(&t, &solo);
    for (uint32_t tick = 0; tick < ticks; tick++) {
        gs_input in[GS_MAX_CARS];
        for (uint8_t i = 0; i < 4; i++) in[i] = gs_net_drive(i, tick);
        gs_world_step(&solo, &t, in);
    }
    CHECK(gs_world_hash(&solo) == gs_world_hash(&net[0].confirmed));

    // The links really were bad, and rollback really did work for its living -
    // more than in the two-player case, because there are three people to be
    // wrong about rather than one.
    CHECK(link[0][1].dropped > 0);
    CHECK(net[0].rollbacks > 0);
}

TEST(rollback_costs_nothing_when_the_guess_is_right) {
    static gs_net a, b;
    static gs_link ab, ba;
    static gs_track t;

    // A perfect link and a player who never changes their input: every
    // prediction is right, so there is nothing to rewind. If this rolls back at
    // all, the rollback is firing on agreement rather than on disagreement, and
    // every real race is paying for it.
    gs_world w;
    gs_net_scene(&t, &w);
    gs_net_begin(&a, &w, 2, 0);
    gs_net_begin(&b, &w, 2, 1);
    ab = (gs_link){ 0 };
    ba = (gs_link){ 0 };
    ab.latency = ba.latency = 16;

    for (uint32_t tick = 0; tick < 600; tick++) {
        gs_link_deliver(&ab, tick, &b, &t);
        gs_link_deliver(&ba, tick, &a, &t);

        gs_net_local_input(&a, GS_IN_ACCEL);
        gs_net_local_input(&b, GS_IN_ACCEL);

        uint8_t buf[GS_LINK_MTU];
        size_t n = gs_net_packet(&a, buf, sizeof buf);
        gs_link_send(&ab, tick, buf, n);
        n = gs_net_packet(&b, buf, sizeof buf);
        gs_link_send(&ba, tick, buf, n);

        gs_net_step(&a, &t);
        gs_net_step(&b, &t);
    }

    // Exactly one, and it is the unavoidable one: on the very first ticks
    // nothing at all is known about the other player, so they are assumed idle
    // and they were not. Every prediction after that is right, and *that* is
    // what must cost nothing - a rollback per tick on a steady input means
    // rollback is firing on agreement, and every real race pays for it.
    CHECK(a.rollbacks <= 1);
    CHECK(b.rollbacks <= 1);
    CHECK(a.resimulated < 32);

    // The confirmed state still followed along behind, a latency's distance
    // back - which is what "no rollback" has to mean, rather than "no progress".
    CHECK(a.confirmed_tick > 600u - 64u);
}

TEST(a_machine_that_goes_quiet_stalls_the_race_rather_than_desyncing) {
    static gs_net a;
    static gs_track t;
    gs_world w;
    gs_net_scene(&t, &w);
    gs_net_begin(&a, &w, 2, 0);

    // The other machine never says anything at all. Running on regardless would
    // mean building a race on nothing but guesses and having no way back when
    // the truth arrived, so the window is the limit and the race stops there.
    for (uint32_t tick = 0; tick < GS_NET_WINDOW * 2u; tick++) {
        gs_net_local_input(&a, GS_IN_ACCEL);
        gs_net_step(&a, &t);
    }

    CHECK(a.local_tick == GS_NET_WINDOW - 1u);
    CHECK(a.stalls > 0);
    CHECK(a.confirmed_tick == 0);
    CHECK(!a.desynced);
}

TEST(a_desync_is_noticed_rather_than_lived_with) {
    static gs_net a, b;
    static gs_link ab, ba;
    static gs_track t;
    gs_world w;
    gs_net_scene(&t, &w);

    gs_net_begin(&a, &w, 2, 0);

    // The other machine starts from a world that is one nudge different. Every
    // input will agree and every state will not, which is exactly the shape of
    // a real desync: nothing complains, and the two people are watching
    // different races.
    gs_world nudged = w;
    nudged.car[1].x += GS_RATIO(1, 64);
    gs_net_begin(&b, &nudged, 2, 1);

    ab = (gs_link){ 0 };
    ba = (gs_link){ 0 };
    ab.latency = ba.latency = 4;

    for (uint32_t tick = 0; tick < 400; tick++) {
        gs_link_deliver(&ab, tick, &b, &t);
        gs_link_deliver(&ba, tick, &a, &t);

        gs_net_local_input(&a, gs_net_drive(0, tick));
        gs_net_local_input(&b, gs_net_drive(1, tick));

        uint8_t buf[GS_LINK_MTU];
        size_t n = gs_net_packet(&a, buf, sizeof buf);
        gs_link_send(&ab, tick, buf, n);
        n = gs_net_packet(&b, buf, sizeof buf);
        gs_link_send(&ba, tick, buf, n);

        gs_net_step(&a, &t);
        gs_net_step(&b, &t);
    }

    CHECK(a.desynced);
    CHECK(b.desynced);
    CHECK(gs_world_hash(&a.confirmed) != gs_world_hash(&b.confirmed));
}

TEST(the_analyser_gives_the_same_answer_twice) {
    static gs_track t;
    gs_circuit(&t, GS_SURF_DIRT);

    static gs_analysis first;
    gs_analyse(&t, 12, &first);
    gs_analyse(&t, 12, &gs_report);

    // A design tool that disagreed with itself between runs would be worse
    // than none: you could not tell a change to the track from noise.
    CHECK(first.completable == gs_report.completable);
    CHECK(first.lightest == gs_report.lightest);
    CHECK(first.heaviest == gs_report.heaviest);
    CHECK(first.busiest == gs_report.busiest);
    for (int i = 0; i < GS_ANALYSIS_STEPS; i++) {
        CHECK(first.completed[i] == gs_report.completed[i]);
    }
}

// ---------------------------------------------------------------------------
// Determinism - the property everything else is built on
// ---------------------------------------------------------------------------

// A scripted race: enough throttle, steering and airtime that a change to any
// part of the physics moves the answer.
static void gs_script_inputs(uint32_t tick, uint8_t cars, gs_input *in) {
    for (uint8_t c = 0; c < GS_MAX_CARS; c++) in[c] = 0;

    in[0] = GS_IN_ACCEL;
    if ((tick / 40u) % 3u == 1u) in[0] |= GS_IN_LEFT;
    if ((tick / 55u) % 4u == 2u) in[0] |= GS_IN_RIGHT;
    if (cars > 1) {
        in[1] = GS_IN_ACCEL;
        if ((tick / 33u) % 2u == 0u) in[1] |= GS_IN_RIGHT;
    }
}

static void gs_scripted_race(gs_world *w, const gs_track *t, uint32_t ticks,
                             gs_replay *rec) {
    for (uint32_t i = 0; i < ticks; i++) {
        gs_input in[GS_MAX_CARS];
        gs_script_inputs(i, w->car_count, in);
        if (rec != nullptr) gs_replay_record(rec, in);
        gs_world_step(w, t, in);
    }
}

static void gs_demo_track(gs_track *t) {
    gs_build_ramp(t, 8, 12, GS_INT(1));
    for (uint8_t x = 16; x < 24; x++)
        for (uint8_t y = 0; y < t->h; y++)
            gs_track_set_surface(t, x, y, GS_SURF_ICE);
}

// Feed the clock a sequence of frames and report how many simulation steps it
// asked for in total. Bounded by construction: a broken clock makes this return
// the wrong number, and never makes it fail to return. An earlier version of
// this test drove the world until a tick count was reached, which *hung* rather
// than failed when the clock stopped making progress - and a test that hangs is
// worse than no test, because CI just sits there.
static uint64_t gs_clock_steps(uint64_t frame_ns, uint32_t frames, uint64_t tail_ns) {
    gs_clock c;
    gs_clock_init(&c);

    uint64_t total = 0;
    for (uint32_t i = 0; i < frames; i++) total += gs_clock_advance(&c, frame_ns);
    if (tail_ns != 0) total += gs_clock_advance(&c, tail_ns);
    return total;
}

TEST(the_clock_delivers_the_same_ticks_however_the_time_is_chopped_up) {
    // Four frame rates, each chopping the *same* one second of wall clock into
    // different pieces. The tail makes each sequence sum to exactly
    // 1,000,000,000 ns, so this compares like with like rather than comparing
    // rounding.
    uint64_t at30 = gs_clock_steps(33333333ULL, 30, 10ULL);
    uint64_t at60 = gs_clock_steps(16666666ULL, 60, 40ULL);
    uint64_t at144 = gs_clock_steps(6944444ULL, 144, 64ULL);
    uint64_t at240 = gs_clock_steps(4166666ULL, 240, 160ULL);

    // One second at 120 Hz.
    CHECK(at30 == 120);
    CHECK(at60 == 120);
    CHECK(at144 == 120);
    CHECK(at240 == 120);

    // 240 is the one that matters most: every frame there is *shorter* than a
    // tick, so the leftover in the accumulator is the only thing that makes the
    // simulation advance at all. A clock that discards its remainder scores
    // zero here and looks fine at 30.
    CHECK(at240 > 0);
}

TEST(a_race_paced_by_the_clock_is_the_race_the_simulation_would_have_run) {
    static gs_track t;
    gs_demo_track(&t);

    // Driven exactly as the frontend drives it: frames in, steps out, and the
    // input for a tick decided by the tick.
    gs_world paced;
    gs_world_init(&paced, GS_ONE);
    gs_world_add_car(&paced, &t, GS_VEH_STOCK_CAR, GS_INT(2), GS_INT(3), 0);
    gs_world_add_car(&paced, &t, GS_VEH_MOTORCYCLE, GS_INT(2), GS_INT(5), 0);

    gs_clock c;
    gs_clock_init(&c);

    uint32_t done = 0;
    for (uint32_t frame = 0; frame < 240; frame++) {          // one second at 240 fps
        uint32_t steps = gs_clock_advance(&c, 4166666ULL);
        for (uint32_t i = 0; i < steps; i++) {
            gs_input in[GS_MAX_CARS];
            gs_script_inputs(done, paced.car_count, in);
            gs_world_step(&paced, &t, in);
            done++;
        }
    }

    gs_world direct;
    gs_world_init(&direct, GS_ONE);
    gs_world_add_car(&direct, &t, GS_VEH_STOCK_CAR, GS_INT(2), GS_INT(3), 0);
    gs_world_add_car(&direct, &t, GS_VEH_MOTORCYCLE, GS_INT(2), GS_INT(5), 0);
    gs_scripted_race(&direct, &t, done, nullptr);

    // A second of frames is a second of simulation, near enough that the
    // truncation of a 240 fps frame length is the only difference.
    CHECK(done >= 119 && done <= 120);
    CHECK(gs_world_hash(&paced) == gs_world_hash(&direct));
}

TEST(a_stalled_frame_makes_the_game_go_slow_rather_than_mad) {
    gs_clock c;
    gs_clock_init(&c);

    // A breakpoint, a window drag, a laptop lid: ten seconds of nothing. Run
    // uncapped that is 1200 ticks in one frame, and the game lurches across the
    // track. Capped, it is a quarter second of catching up.
    uint32_t steps = gs_clock_advance(&c, 10000000000ULL);
    CHECK(steps <= GS_TICK_HZ / 4);
    CHECK(steps > 0);
}

TEST(the_interpolation_fraction_never_reaches_the_next_tick) {
    gs_clock c;
    gs_clock_init(&c);

    // Deliberately awkward frame lengths - nothing that divides evenly into a
    // tick - so the leftover lands all over the range.
    for (uint32_t i = 0; i < 5000; i++) {
        gs_clock_advance(&c, 3000000ULL + (i % 37u) * 211111ULL);
        gs_fix a = gs_clock_alpha(&c);
        CHECK(a >= 0 && a < GS_ONE);
    }
}

TEST(the_same_inputs_produce_the_same_world_every_time) {
    static gs_track t;
    gs_demo_track(&t);

    uint64_t first = 0;
    for (int run = 0; run < 3; run++) {
        gs_world w;
        gs_world_init(&w, GS_ONE);
        gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(2), GS_INT(3), 0);
        gs_world_add_car(&w, &t, GS_VEH_MOTORCYCLE, GS_INT(2), GS_INT(5), 0);
        gs_scripted_race(&w, &t, 900, nullptr);

        uint64_t h = gs_world_hash(&w);
        if (run == 0) first = h;
        CHECK(h == first);
    }
}

TEST(a_world_snapshot_is_a_memory_copy_and_restores_exactly) {
    static gs_track t;
    gs_demo_track(&t);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(2), GS_INT(3), 0);
    gs_scripted_race(&w, &t, 200, nullptr);

    // The whole of rollback, in three lines: copy, run on, put it back, run the
    // same inputs again, and land in the same place.
    gs_world snapshot;
    memcpy(&snapshot, &w, sizeof snapshot);

    gs_scripted_race(&w, &t, 300, nullptr);
    uint64_t after = gs_world_hash(&w);

    memcpy(&w, &snapshot, sizeof w);
    CHECK(gs_world_hash(&w) == gs_world_hash(&snapshot));

    gs_scripted_race(&w, &t, 300, nullptr);
    CHECK(gs_world_hash(&w) == after);
}

TEST(a_replay_re_races_to_the_same_world_it_recorded) {
    static gs_track t;
    static gs_replay rec;
    gs_demo_track(&t);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_DUNE_BUGGY, GS_INT(2), GS_INT(3), 0);
    gs_replay_begin(&rec, &w, &t);
    gs_scripted_race(&w, &t, 600, &rec);
    uint64_t want = gs_world_hash(&w);

    gs_world back;
    CHECK(gs_replay_restore(&rec, &back, &t));
    gs_world_add_car(&back, &t, GS_VEH_DUNE_BUGGY, GS_INT(2), GS_INT(3), 0);
    gs_replay_playback(&rec, &t, &back);

    CHECK(gs_world_hash(&back) == want);
}

TEST(a_replay_survives_the_round_trip_through_its_wire_format) {
    static gs_track t;
    static gs_replay rec, back;
    static uint8_t buf[GS_REPLAY_MAX_TICKS * GS_MAX_CARS + 256];
    gs_demo_track(&t);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_BAJA_BUG, GS_INT(2), GS_INT(3), 0);
    gs_replay_begin(&rec, &w, &t);
    gs_scripted_race(&w, &t, 400, &rec);

    size_t n = gs_replay_serialize(&rec, buf, sizeof buf);
    CHECK(n == gs_replay_size(&rec));
    CHECK(gs_replay_deserialize(&back, buf, n));

    CHECK(back.meta.track_hash == rec.meta.track_hash);
    CHECK(back.meta.tick_count == rec.meta.tick_count);
    CHECK(back.meta.gravity == rec.meta.gravity);
    CHECK(memcmp(back.input, rec.input,
                 (size_t)rec.meta.tick_count * GS_MAX_CARS) == 0);

    // A replay against a track it was not recorded on is refused rather than
    // quietly re-raced somewhere else.
    static gs_track other;
    gs_track_init(&other, 16, 16, GS_SURF_DIRT);
    gs_world elsewhere;
    CHECK(!gs_replay_restore(&back, &elsewhere, &other));
}

TEST(a_wrecked_car_stops_moving) {
    static gs_track t;
    gs_track_init(&t, 32, 8, GS_SURF_PAVEMENT);
    // A cliff: level for eight tiles, then the floor drops away by forty.
    for (uint8_t y = 0; y <= t.h; y++)
        for (uint8_t x = 0; x <= t.w; x++)
            gs_track_set_corner(&t, x, y, x <= 8 ? GS_INT(40) : 0);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_MOTORCYCLE, GS_INT(2), GS_INT(4), 0);
    w.car[0].vx = GS_INT(6);

    for (int i = 0; i < GS_TICK_HZ * 10; i++) gs_world_step(&w, &t, nullptr);

    CHECK(w.car[0].damage > 0);
    CHECK(w.car[0].wrecked);

    gs_fix x = w.car[0].x;
    for (int i = 0; i < GS_TICK_HZ; i++) gs_world_step(&w, &t, nullptr);
    CHECK(w.car[0].x == x);
}

// ---------------------------------------------------------------------------

int main(void) {
    printf("gearstick tests\n");

    run_the_trigonometry_agrees_with_double_precision_across_a_full_turn();
    run_atan2_inverts_the_sine_and_cosine_it_was_built_beside();
    run_square_root_is_exact_on_squares_and_monotone_between_them();
    run_the_ground_is_continuous_across_a_tile_boundary();
    run_a_flat_tile_has_no_slope_and_a_ramp_has_the_slope_it_was_built_with();
    run_a_track_edit_changes_its_identity_and_an_undone_edit_restores_it();
    run_a_track_survives_the_round_trip_through_its_file_format();
    run_a_corrupt_track_file_is_refused_rather_than_half_loaded();
    run_two_tracks_built_the_same_way_share_an_identity_through_a_file();
    run_any_sequence_of_edits_undone_completely_restores_the_track();
    run_redoing_everything_returns_the_track_to_where_it_was();
    run_a_brush_stroke_undoes_as_one_action();
    run_an_edit_after_an_undo_drops_what_was_ahead();
    run_an_edit_that_changes_nothing_leaves_no_step_in_the_history();
    run_a_full_log_refuses_the_edit_rather_than_applying_it();
    run_the_far_corners_of_a_track_can_be_edited();
    run_a_car_driving_through_a_gate_is_seen_to_cross_it();
    run_a_sound_route_is_accepted_and_a_broken_one_names_its_problem();
    run_a_gate_hanging_off_the_track_is_refused_and_says_which_one();
    run_a_gate_nothing_can_fit_through_is_refused();
    run_two_gates_in_the_same_place_are_refused_as_an_ambiguous_order();
    run_a_route_is_part_of_the_track_and_survives_being_saved();
    run_removing_a_gate_closes_the_gap_and_leaves_the_order_alone();
    run_a_car_accelerates_under_throttle_and_stops_under_the_brake();
    run_a_car_left_on_a_slope_rolls_downhill_and_one_on_the_flat_does_not();
    run_a_car_turns_more_sharply_at_a_crawl_than_at_speed();
    run_ice_lets_go_of_a_sliding_car_long_after_pavement_has_caught_it();
    run_the_same_corner_at_the_same_speed_is_takeable_on_pavement_and_not_on_ice();
    run_a_ramp_throws_a_car_into_the_air_without_being_told_it_is_a_ramp();
    run_a_jump_lands_where_the_closed_form_says_it_should();
    run_halving_gravity_doubles_a_jump_from_the_same_take_off();
    run_a_ramp_throws_a_car_further_under_lower_gravity_than_the_arc_alone_explains();
    run_a_low_gravity_tile_lengthens_a_jump_that_crosses_it();
    run_the_same_jump_hurts_less_landed_on_a_downslope_than_landed_flat();
    run_air_drag_is_a_dial_and_more_of_it_means_less_speed();
    run_the_friction_scale_is_a_dial_and_more_of_it_means_more_grip_until_it_does_not();
    run_the_damage_multiplier_is_a_dial_and_more_of_it_hurts_more();
    run_gravity_is_a_dial_and_it_is_continuous_between_the_planets();
    run_lap_five_on_dirt_is_slower_on_the_used_line_than_lap_one();
    run_pavement_does_not_care_how_many_laps_you_have_done_on_it();
    run_ice_polishes_into_something_faster_and_looser();
    run_a_sliding_tyre_churns_the_ground_more_than_a_rolling_one();
    run_wear_belongs_to_the_race_and_not_to_the_track();
    run_a_head_on_at_speed_sends_both_cars_somewhere();
    run_the_same_collision_happens_the_same_way_every_time();
    run_cars_that_are_already_overlapping_push_apart_rather_than_sit_inside_each_other();
    run_a_car_can_be_destroyed_by_driving_and_by_being_hit();
    run_a_wreck_is_scenery_that_the_living_bounce_off();
    run_a_hard_enough_hit_puts_a_car_in_the_air();
    run_a_car_flying_over_another_does_not_hit_it();
    run_a_hazard_dropped_by_one_car_affects_the_other_and_not_the_dropper();
    run_oil_gives_the_grip_back_the_moment_you_are_off_it();
    run_a_mine_goes_off_once_and_hurts_whoever_found_it();
    run_holding_the_button_leaves_a_trail_and_not_a_carpet();
    run_destruction_mode_ends_when_one_car_is_left_driving();
    run_everybody_going_at_once_is_a_draw_rather_than_a_win();
    run_a_race_does_not_end_just_because_somebody_was_wrecked();
    run_a_destruction_race_fought_out_between_two_cars_finishes_by_itself();
    run_a_wreck_changes_the_racing_line_for_the_rest_of_the_race();
    run_a_wreck_is_still_there_much_later_and_has_not_moved();
    run_the_ai_steers_both_ways();
    run_the_ai_gets_round_a_circuit_it_has_never_seen();
    run_the_ai_gets_round_on_surfaces_and_vehicles_it_was_not_tuned_for();
    run_the_same_corner_is_braked_for_differently_under_different_gravity();
    run_the_same_corner_is_braked_for_differently_in_a_different_car();
    run_a_quicker_driver_carries_more_speed_through_the_same_corner();
    run_an_ai_race_is_deterministic_like_every_other_race();
    run_the_analyser_calls_a_jump_nobody_can_clear_impossible();
    run_the_analyser_says_so_when_a_track_cannot_be_got_round_at_all();
    run_the_heatmap_shows_where_everybody_actually_went();
    run_the_analyser_gives_the_same_answer_twice();
    run_a_ghost_is_the_race_that_made_it_not_a_picture_of_it();
    run_a_ghost_from_another_track_is_refused_rather_than_raced();
    run_a_ghost_carries_its_own_starting_grid();
    run_a_ghost_that_runs_out_stays_where_it_finished();
    run_the_packer_gives_back_exactly_what_it_was_given();
    run_a_track_survives_being_pasted_into_a_chat_window();
    run_a_damaged_code_never_becomes_a_different_track();
    run_a_code_is_the_same_code_on_every_machine();
    run_two_machines_race_to_the_same_finish_over_a_bad_connection();
    run_four_machines_race_to_the_same_finish_over_a_bad_connection();
    run_rollback_costs_nothing_when_the_guess_is_right();
    run_a_machine_that_goes_quiet_stalls_the_race_rather_than_desyncing();
    run_a_desync_is_noticed_rather_than_lived_with();
    run_the_same_inputs_produce_the_same_world_every_time();
    run_the_clock_delivers_the_same_ticks_however_the_time_is_chopped_up();
    run_a_race_paced_by_the_clock_is_the_race_the_simulation_would_have_run();
    run_a_stalled_frame_makes_the_game_go_slow_rather_than_mad();
    run_the_interpolation_fraction_never_reaches_the_next_tick();
    run_a_world_snapshot_is_a_memory_copy_and_restores_exactly();
    run_a_replay_re_races_to_the_same_world_it_recorded();
    run_a_replay_survives_the_round_trip_through_its_wire_format();
    run_a_wrecked_car_stops_moving();

    if (gs_failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d check%s failed\n", gs_failures, gs_failures == 1 ? "" : "s");
    return 1;
}

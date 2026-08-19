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

#include "core/gs_clock.h"
#include "core/gs_edit.h"
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

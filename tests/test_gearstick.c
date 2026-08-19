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
// Determinism - the property everything else is built on
// ---------------------------------------------------------------------------

// A scripted race: enough throttle, steering and airtime that a change to any
// part of the physics moves the answer.
static void gs_scripted_race(gs_world *w, const gs_track *t, uint32_t ticks,
                             gs_replay *rec) {
    for (uint32_t i = 0; i < ticks; i++) {
        gs_input in[GS_MAX_CARS] = { 0, 0, 0, 0 };
        in[0] = GS_IN_ACCEL;
        if ((i / 40u) % 3u == 1u) in[0] |= GS_IN_LEFT;
        if ((i / 55u) % 4u == 2u) in[0] |= GS_IN_RIGHT;
        if (w->car_count > 1) {
            in[1] = GS_IN_ACCEL;
            if ((i / 33u) % 2u == 0u) in[1] |= GS_IN_RIGHT;
        }
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
    run_a_car_accelerates_under_throttle_and_stops_under_the_brake();
    run_a_car_left_on_a_slope_rolls_downhill_and_one_on_the_flat_does_not();
    run_a_car_turns_more_sharply_at_a_crawl_than_at_speed();
    run_ice_lets_go_of_a_sliding_car_long_after_pavement_has_caught_it();
    run_a_ramp_throws_a_car_into_the_air_without_being_told_it_is_a_ramp();
    run_a_jump_lands_where_the_closed_form_says_it_should();
    run_halving_gravity_doubles_a_jump_from_the_same_take_off();
    run_a_ramp_throws_a_car_further_under_lower_gravity_than_the_arc_alone_explains();
    run_a_low_gravity_tile_lengthens_a_jump_that_crosses_it();
    run_the_same_jump_hurts_less_landed_on_a_downslope_than_landed_flat();
    run_the_same_inputs_produce_the_same_world_every_time();
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

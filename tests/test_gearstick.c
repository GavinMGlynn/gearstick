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
#include "core/gs_parts.h"
#include "core/gs_generate.h"
#include "core/gs_generate.h"
#include "core/gs_ghost.h"
#include "core/gs_library.h"
#include "core/gs_blake2s.h"
#include "core/gs_net.h"
#include "core/gs_pack.h"
#include "core/gs_profile.h"
#include "core/gs_records.h"
#include "net/gs_carrier.h"
#include "net/gs_verify.h"
#include "core/gs_share.h"
#include "core/gs_stunts.h"
#include "core/gs_replay.h"
#include "core/gs_sim.h"
#include "core/gs_track.h"

static int gs_failures = 0;
static const char *gs_current = "";

// Little-endian writers, so a test can lay out a saved file by hand. Building
// the bytes explicitly is the point: a fixture produced by today's writer would
// agree with today's reader even when both are wrong about a length.
static void gs_test_put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xffu);
}

static void gs_test_put64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xffu);
}

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

// Is this byte one the hash is meant to read? The used region and nothing else:
// a track's identity is what is on it, not what the arrays have room for.
static bool gs_byte_counts(const gs_track *t, size_t at) {
    const size_t corner = offsetof(gs_track, corner);
    const size_t surface = offsetof(gs_track, surface);
    const size_t gravity = offsetof(gs_track, gravity);
    const size_t gate = offsetof(gs_track, gate);

    if (at == offsetof(gs_track, route)) return true;
    if (at == offsetof(gs_track, gate_count)) return true;

    if (at >= corner && at < corner + sizeof t->corner) {
        const size_t i = (at - corner) / sizeof t->corner[0];
        const size_t x = i % GS_CORNER_STRIDE, y = i / GS_CORNER_STRIDE;
        return x <= t->w && y <= t->h;      // corners are one more than tiles
    }
    if (at >= surface && at < surface + sizeof t->surface) {
        const size_t i = at - surface;
        return (i % GS_TRACK_MAX) < t->w && (i / GS_TRACK_MAX) < t->h;
    }
    if (at >= gravity && at < gravity + sizeof t->gravity) {
        const size_t i = at - gravity;
        return (i % GS_TRACK_MAX) < t->w && (i / GS_TRACK_MAX) < t->h;
    }
    if (at >= gate && at < gate + sizeof t->gate) {
        const size_t i = (at - gate) / sizeof t->gate[0];
        if (i >= t->gate_count) return false;      // a gate nobody laid
        const size_t in = (at - gate) % sizeof t->gate[0];
        // Everything but the padding the struct carries for alignment.
        return in < offsetof(gs_gate, pad);
    }
    return false;       // w, h and anything the compiler put between fields
}

TEST(a_race_is_identified_by_everything_that_decides_it) {
    // **The same sweep as the track below, on the thing determinism is for.**
    // `gs_world_hash` is what two machines compare to find out they have
    // stopped agreeing, and it is written field by field - so a field added to
    // gs_world and not added here is a disagreement neither machine can see.
    // That is exactly what happened to a track's route byte.
    //
    // Every byte of a raced world is flipped and the hash has to move, except
    // where it is padding the compiler inserted, a car or a hazard nobody has
    // added, or the one field named below.
    static gs_track t;
    gs_track_init(&t, 24, 16, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= t.h; y++)
        for (uint8_t x = 0; x <= t.w; x++) gs_track_set_corner(&t, x, y, 0);

    static gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(4), GS_INT(4), 0);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_SPRINT_CAR, GS_INT(8), GS_INT(4), 0);
    gs_world_set_mode(&w, GS_MODE_RACE);
    gs_world_set_countdown(&w, 200);
    for (int i = 0; i < 60; i++) gs_world_step(&w, &t, nullptr);
    CHECK(gs_world_countdown(&w) > 0);      // still on the grid, lights red

    const uint64_t was = gs_world_hash(&w);

    // **The one field the hash does not read, named with its reason.**
    //
    // `green_tick` is when the lights go green. It gates whether any input
    // reaches a car at all, so it is state that decides the race - and it is
    // not in the hash. It is not *added* to the hash because that number is
    // written into every networked recording as the state the peers agreed the
    // race ended in, and moving it would have every recording made with
    // v0.1.0-beta1 rejected as a different race.
    //
    // What covers it is asserted below rather than assumed, which is the whole
    // point of writing it down here: while the lights are red every car's lap
    // clock is pinned to the green, so two worlds that disagree about when it
    // comes disagree in the hash through that. Take the pinning away and this
    // test goes red, and whoever does that has to hash `green_tick` instead.
    const size_t green = offsetof(gs_world, green_tick);

    int moved = 0, still = 0, wrong = 0, in_cars = 0;
    uint8_t *raw = (uint8_t *)&w;
    for (size_t at = 0; at < sizeof w; at++) {
        // These two say how much of the arrays after them is real. A flipped
        // one says 255 cars, and the hash then reads off the end of an array
        // holding four - a test crashing, not a fault found. Checked apart.
        if (at == offsetof(gs_world, car_count) ||
            at == offsetof(gs_world, hazard_count)) {
            continue;
        }

        const uint8_t keep = raw[at];
        raw[at] = (uint8_t)(keep ^ 0xffu);
        const bool shifted = gs_world_hash(&w) != was;
        raw[at] = keep;

        if (shifted) { moved++; continue; }
        still++;

        // **Inside a car somebody is driving, every named field has to move
        // it.** Padding does not, and there is no portable way to say where a
        // compiler put padding - so what is checked instead is the arithmetic:
        // a car's hashed fields add up to so many bytes, the struct is so many
        // bytes, and the difference is what may sit still. Add a field to
        // gs_car and the difference grows and this fails, whatever the offsets
        // happen to be on the machine it is built on.
        const size_t cars_at = offsetof(gs_world, car);
        if (at >= cars_at && at < cars_at + sizeof w.car[0] * w.car_count) {
            in_cars++;
            continue;
        }
        if (at >= green && at < green + sizeof w.green_tick) continue;
        if (at < cars_at) continue;             // padding around car_count
        if (at >= cars_at + sizeof w.car[0] * w.car_count &&
            at < offsetof(gs_world, wear)) {
            continue;                           // cars and hazards nobody added
        }
        if (at >= offsetof(gs_world, wear) + sizeof w.wear) {
            continue;                           // padding on the end of it all
        }

        wrong++;
        printf("  RACE byte %zu is in the world and not in its hash\n", at);
    }

    // What one car has that the hash reads. The list is the hash's own, and
    // adding to gs_car without adding here is the failure this exists for.
    const gs_car *any = &w.car[0];
    const size_t car_field[] = {
        sizeof any->x, sizeof any->y, sizeof any->z,
        sizeof any->vx, sizeof any->vy, sizeof any->vz,
        sizeof any->heading, sizeof any->vehicle, sizeof any->damage,
        sizeof any->grounded, sizeof any->wrecked, sizeof any->active,
        sizeof any->air_ticks, sizeof any->drop_cooldown, sizeof any->next_gate,
        sizeof any->laps, sizeof any->finish_tick, sizeof any->lap_start,
        sizeof any->best_lap,
        // What it is carrying, what a tap would leave, and how long the button
        // has been down. Added the day weapons went in - and this test is how
        // it was noticed that they had not been added to the hash yet, which is
        // exactly the job it was written to do.
        sizeof any->ammo, sizeof any->selected,
        sizeof any->fire_held, sizeof any->fire_cycled,
    };
    size_t named = 0;
    for (size_t i = 0; i < GS_ARRAY_LEN(car_field); i++) named += car_field[i];

    const size_t slack = (sizeof w.car[0] - named) * w.car_count;
    if ((size_t)in_cars != slack) {
        printf("  RACE %d bytes inside the cars are not hashed; a car is %zu "
               "bytes and the hash reads %zu of them, so %zu should be\n",
               in_cars, sizeof w.car[0], named, slack);
    }
    CHECK((size_t)in_cars == slack);

    printf("  RACE %zu bytes of a raced world: %d decide it, %d are padding, "
           "cars nobody added, or green_tick. A car is %zu bytes and %zu of "
           "them are read\n", sizeof w, moved, still, sizeof w.car[0], named);
    CHECK(wrong == 0);
    CHECK(moved > 0);

    // The two counted apart: both say how much is real and both must be read.
    const uint8_t cars = w.car_count, hazards = w.hazard_count;
    w.car_count = (uint8_t)(cars - 1);
    CHECK(gs_world_hash(&w) != was);
    w.car_count = cars;
    w.hazard_count = (uint8_t)(hazards + 1);
    CHECK(gs_world_hash(&w) != was);
    w.hazard_count = hazards;
    CHECK(gs_world_hash(&w) == was);

    // **And the thing that covers green_tick, proved rather than believed.**
    // Two worlds alike but for when the lights come: while they are still red,
    // the lap clocks say so and the hashes differ.
    static gs_world later;
    later = w;
    later.green_tick = w.green_tick + 30u;
    for (uint8_t i = 0; i < later.car_count; i++) {
        later.car[i].lap_start = later.green_tick;   // what a held step does
    }
    CHECK(gs_world_hash(&later) != was);
}

TEST(a_track_is_identified_by_everything_that_is_on_it) {
    // **The guard the route byte got past.** A field was added to gs_track and
    // the function that says what a track *is* was not told about it, so a
    // circuit and a sprint over the same ground were one track and a library
    // ate somebody's work. A test naming that one case catches that one case.
    // This is the general one: every byte of a track is flipped, and whether
    // the identity moves has to match what the hash claims to cover.
    //
    // So a field added next year is either read by the hash or is named here as
    // deliberately not part of what a track is. It cannot be neither.
    static gs_track t;
    gs_track_init(&t, 9, 7, GS_SURF_DIRT);       // smaller than the arrays hold
    for (uint8_t y = 0; y <= t.h; y++) {
        for (uint8_t x = 0; x <= t.w; x++) {
            gs_track_set_corner(&t, x, y, GS_INT((x + y) % 3));
        }
    }
    gs_track_set_surface(&t, 3, 3, GS_SURF_ICE);
    gs_track_set_gravity(&t, 4, 2, GS_RATIO(40, 100));
    CHECK(gs_track_add_gate(&t, GS_INT(2), GS_INT(3), 0, GS_INT(2)) >= 0);
    CHECK(gs_track_add_gate(&t, GS_INT(7), GS_INT(3),
                            (gs_angle)(GS_TURN / 4), GS_INT(2)) >= 0);
    t.route = (uint8_t)GS_ROUTE_CIRCUIT;

    const uint64_t was = gs_track_hash(&t);

    // **`w`, `h` and `gate_count` are changed to other legal values** rather
    // than flipped. They say how much of the arrays is real, and a flipped one
    // says 255 tiles - which sends the hash reading off the end of a 64-tile
    // array. That is a test crashing, not a fault found.
    int read = 0, ignored = 0, wrong = 0;
    for (size_t at = 0; at < sizeof t; at++) {
        if (at == offsetof(gs_track, w) || at == offsetof(gs_track, h) ||
            at == offsetof(gs_track, gate_count)) {
            continue;
        }

        uint8_t *raw = (uint8_t *)&t;
        const uint8_t keep = raw[at];
        raw[at] = (uint8_t)(keep ^ 0xffu);
        const bool moved = gs_track_hash(&t) != was;
        raw[at] = keep;

        const bool should = gs_byte_counts(&t, at);
        if (moved) read++; else ignored++;
        if (moved == should) continue;

        wrong++;
        printf("  IDENTITY byte %zu %s the hash and %s\n", at,
               moved ? "moves" : "does not move",
               should ? "should" : "should not");
    }

    // And the three counted apart, because they decide how much of the rest is
    // real and every one of them has to be part of what a track is.
    const uint8_t w = t.w, h = t.h, gates = t.gate_count;
    t.w = (uint8_t)(w - 1);
    CHECK(gs_track_hash(&t) != was);
    t.w = w;
    t.h = (uint8_t)(h - 1);
    CHECK(gs_track_hash(&t) != was);
    t.h = h;
    t.gate_count = (uint8_t)(gates - 1);
    CHECK(gs_track_hash(&t) != was);
    t.gate_count = gates;
    CHECK(gs_track_hash(&t) == was);

    printf("  IDENTITY %zu bytes of a track: %d are what it is, %d are room "
           "the arrays have and nobody filled\n", sizeof t, read, ignored);
    CHECK(wrong == 0);
    CHECK(read > 0);
    CHECK(ignored > 0);
}

TEST(a_loop_and_a_path_over_the_same_ground_are_two_tracks) {
    // **Found by walking every kind of edit through undo.** Changing a track
    // from a path to a loop did not change its hash, which the test above reads
    // as "that edit did nothing" - and it is worse than that, because the hash
    // is what a track *is*.
    //
    // The comment over gs_track_hash says the route is part of a track's
    // identity, "the same ground driven the other way round is a different
    // track, and its times are not comparable". It hashes the gates. It did not
    // hash whether the gates make a loop or a path, which is the most literal
    // reading of driving the same ground a different way round: on a circuit
    // you cross gate zero again to finish a lap, on a sprint you drive from the
    // first gate to the last and stop.
    static gs_track path;
    gs_track_init(&path, 16, 12, GS_SURF_PAVEMENT);
    CHECK(gs_track_add_gate(&path, GS_INT(2), GS_INT(6), 0, GS_INT(2)) >= 0);
    CHECK(gs_track_add_gate(&path, GS_INT(8), GS_INT(6), 0, GS_INT(2)) >= 0);
    CHECK(gs_track_add_gate(&path, GS_INT(14), GS_INT(6), 0, GS_INT(2)) >= 0);
    path.route = (uint8_t)GS_ROUTE_SPRINT;

    static gs_track loop;
    loop = path;
    loop.route = (uint8_t)GS_ROUTE_CIRCUIT;

    const uint64_t as_path = gs_track_hash(&path);
    const uint64_t as_loop = gs_track_hash(&loop);
    if (as_path == as_loop) {
        printf("  TRACK a sprint and a circuit over the same ground both hash "
               "%016llx\n", (unsigned long long)as_path);
    }
    CHECK(as_path != as_loop);

    // **And the cost of that was somebody's work.** The library is content
    // addressed - "the same track twice is one track" - so putting the loop in
    // beside the path found the path already there, renamed it, and threw the
    // loop away. A player who built a circuit, saved it, turned it into a
    // sprint and saved that under a second name had one track in their library
    // afterwards, with the second name on the first track.
    static gs_library lib;
    gs_library_clear(&lib);
    CHECK(gs_library_put(&lib, &path, "the long way round", "gavin") >= 0);
    CHECK(lib.count == 1);
    CHECK(gs_library_put(&lib, &loop, "the same, as a lap", "gavin") >= 0);
    if (lib.count != 2) {
        printf("  TRACK the library folded a sprint and a circuit into one "
               "entry, called '%s'\n", lib.entry[0].name);
    }
    CHECK(lib.count == 2);

    // Both are there, and each is the track it says it is.
    int as_sprint = 0, as_circuit = 0;
    for (uint16_t i = 0; i < lib.count; i++) {
        if (lib.entry[i].track.route == (uint8_t)GS_ROUTE_SPRINT) as_sprint++;
        if (lib.entry[i].track.route == (uint8_t)GS_ROUTE_CIRCUIT) as_circuit++;
    }
    CHECK(as_sprint == 1);
    CHECK(as_circuit == 1);

    // And a record set on one is not offered against the other. A best lap on
    // a circuit is a lap of the loop; on a sprint it is one end to the other.
    // Pooling them puts a time on a screen next to a time it cannot be
    // compared with, which is the whole reason a track has an identity.
    CHECK(gs_track_hash(&lib.entry[0].track) !=
          gs_track_hash(&lib.entry[1].track));
}

TEST(every_kind_of_edit_can_be_taken_back_and_put_back_again) {
    // **Everything you can do, you can take back.** That is the promise the
    // construction set makes, and it was tested on three of the seven kinds of
    // edit there are. Two of the seven had never been called by *any* test at
    // all: moving a gate in the route, and changing whether the track is a loop
    // or a path. Both are undoable in principle and nothing had ever checked
    // that they are - which for the route kind means the one edit that decides
    // how a lap is scored.
    //
    // Walked from GS_EDIT_COUNT rather than a list, so an eighth kind of edit
    // is in this test the day it exists rather than the day somebody remembers.
    int walked = 0;
    bool seen[GS_EDIT_COUNT] = { false };

    for (int kind = 0; kind < GS_EDIT_COUNT; kind++) {
        static gs_track t;
        gs_track_init(&t, 16, 12, GS_SURF_PAVEMENT);

        // Whatever this kind of edit needs to exist before it can happen, laid
        // in directly so it is part of the track rather than part of the
        // history - undo has to come back to *this*, not to an empty track.
        if (kind == GS_EDIT_GATE_REMOVE || kind == GS_EDIT_GATE_MOVE) {
            CHECK(gs_track_add_gate(&t, GS_INT(4), GS_INT(4), 0,
                                    GS_INT(2)) >= 0);
            CHECK(gs_track_add_gate(&t, GS_INT(8), GS_INT(4),
                                    (gs_angle)(GS_TURN / 4), GS_INT(2)) >= 0);
            CHECK(gs_track_add_gate(&t, GS_INT(12), GS_INT(8), 0,
                                    GS_INT(2)) >= 0);
        }

        gs_edit_log *l = gs_fresh_log(GS_TEST_EDITS);
        const uint64_t before = gs_track_hash(&t);
        CHECK(!gs_edit_can_undo(l));

        bool did = false;
        switch ((gs_edit_kind)kind) {
        case GS_EDIT_CORNER:
            did = gs_edit_corner(l, &t, 3, 3, GS_INT(2) + GS_HALF);
            break;
        case GS_EDIT_SURFACE:
            did = gs_edit_surface(l, &t, 2, 2, GS_SURF_ICE);
            break;
        case GS_EDIT_GRAVITY:
            did = gs_edit_gravity(l, &t, 2, 2, GS_RATIO(38, 100));
            break;
        case GS_EDIT_GATE_ADD:
            did = gs_edit_add_gate(l, &t, GS_INT(6), GS_INT(6), 0,
                                   GS_INT(2)) >= 0;
            break;
        case GS_EDIT_GATE_REMOVE:
            did = gs_edit_remove_gate(l, &t, 1);
            break;
        case GS_EDIT_GATE_MOVE:
            // Last to first, which is what dropping a start line in after the
            // corners have been laid actually does.
            did = gs_edit_move_gate(l, &t, 2, 0);
            break;
        case GS_EDIT_ROUTE_KIND:
            did = gs_edit_route_kind(l, &t, GS_ROUTE_CIRCUIT);
            break;
        case GS_EDIT_COUNT:
            break;
        }
        CHECK(did);
        seen[kind] = true;

        // **It changed something.** An edit that did nothing would pass every
        // assertion below without any of them meaning anything.
        const uint64_t after = gs_track_hash(&t);
        if (after == before) {
            printf("  EDIT kind %d changed nothing\n", kind);
        }
        CHECK(after != before);
        CHECK(gs_edit_can_undo(l));
        CHECK(gs_edit_undo_depth(l) == 1);

        // Taken back: the track is what it was, to the bit.
        CHECK(gs_edit_undo(l, &t));
        if (gs_track_hash(&t) != before) {
            printf("  EDIT kind %d could not be undone: %016llx, was %016llx\n",
                   kind, (unsigned long long)gs_track_hash(&t),
                   (unsigned long long)before);
        }
        CHECK(gs_track_hash(&t) == before);
        CHECK(!gs_edit_can_undo(l));
        CHECK(gs_edit_can_redo(l));

        // And put back again: the same track the edit made, not a near miss.
        CHECK(gs_edit_redo(l, &t));
        if (gs_track_hash(&t) != after) {
            printf("  EDIT kind %d could not be redone: %016llx, wanted "
                   "%016llx\n", kind, (unsigned long long)gs_track_hash(&t),
                   (unsigned long long)after);
        }
        CHECK(gs_track_hash(&t) == after);
        CHECK(!gs_edit_can_redo(l));

        // And round again, because once through proves the pair works and not
        // that the log is left in a state that can do it twice.
        CHECK(gs_edit_undo(l, &t));
        CHECK(gs_track_hash(&t) == before);
        CHECK(gs_edit_redo(l, &t));
        CHECK(gs_track_hash(&t) == after);

        walked++;
    }

    for (int kind = 0; kind < GS_EDIT_COUNT; kind++) CHECK(seen[kind]);
    printf("  EDITS all %d kinds undone and redone\n", walked);
    CHECK(walked == GS_EDIT_COUNT);
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

TEST(nobody_drives_before_the_lights_go_green) {
    // **A race used to simply be, from tick zero**, so arriving at a track
    // meant already being late and the only way to know a race had begun was
    // that the car under you had started moving. There is a countdown now, and
    // the rule it exists to enforce is this one: full throttle held down
    // through the whole of it moves the car not at all, and the same throttle
    // moves it the moment the lights go green.
    static gs_track t;
    gs_world w;
    gs_flat_world(&t, &w, GS_SURF_PAVEMENT);
    gs_world_set_countdown(&w, GS_COUNTDOWN_TICKS);

    gs_fix started_at_x = w.car[0].x;
    gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL | GS_IN_RIGHT, 0, 0, 0 };

    CHECK(gs_world_held(&w));
    CHECK(gs_world_countdown(&w) == GS_COUNTDOWN_TICKS);

    for (uint32_t i = 0; i < GS_COUNTDOWN_TICKS; i++) {
        gs_world_step(&w, &t, in);
        CHECK(gs_car_speed(&w.car[0]) == 0);
    }

    // Not a step of it, and not a degree of steering either: the whole input is
    // held, not only the throttle.
    CHECK(w.car[0].x == started_at_x);
    CHECK(!gs_world_held(&w));
    CHECK(gs_world_countdown(&w) == 0);

    // And the clock reads zero at the flag rather than three seconds, which is
    // what it read while the cars sat still on the grid.
    CHECK(w.car[0].lap_start == (uint32_t)w.tick);

    for (int i = 0; i < GS_TICK_HZ; i++) gs_world_step(&w, &t, in);
    CHECK(gs_car_speed(&w.car[0]) > GS_INT(1));

    // A race nobody counted down is not held for an instant - which is every
    // replay recorded before there were lights, and why none of them moved.
    gs_world plain;
    gs_flat_world(&t, &plain, GS_SURF_PAVEMENT);
    CHECK(!gs_world_held(&plain));
    for (int i = 0; i < GS_TICK_HZ; i++) gs_world_step(&plain, &t, in);
    CHECK(gs_car_speed(&plain.car[0]) > GS_INT(1));
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

        // **Measured at the landing, which is the thing being compared.** Eight
        // seconds of running on afterwards used to be harmless, because the
        // ground went on for ever; now the car drives off the end of the track,
        // over the run-off, and both variants come back wrecked at 255 - which
        // is equal, and says nothing about landings.
        bool flew = false;
        for (int i = 0; i < GS_TICK_HZ * 8; i++) {
            gs_world_step(&w, &t, nullptr);
            if (!w.car[0].grounded) flew = true;
            if (flew && w.car[0].grounded) break;
        }
        CHECK(flew);
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

    // **In the middle, with room either side.** A derby needs the two of them to
    // keep meeting, and a car that bounces off towards an edge finds a run-off
    // and then a drop - so a pair set up near the boundary stops being a derby
    // and becomes two cars leaving the world in opposite directions.
    gs_world hit;
    gs_world_init(&hit, GS_ONE);
    gs_world_add_car(&hit, &flat, GS_VEH_MOTORCYCLE, GS_INT(26), GS_INT(10), 0);
    gs_world_add_car(&hit, &flat, GS_VEH_STOCK_CAR, GS_INT(38), GS_INT(10),
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
        // Dropped exactly where the first car is about to be. Armed first,
        // because a car carrying none of something drops none of it - which is
        // what a race with the weapons turned off is.
        gs_world_arm(&w, (uint8_t)dropper, kind, 4);
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
    gs_world_arm(&w, 1, GS_HAZ_OIL, 4);
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
    gs_world_arm(&w, 1, GS_HAZ_MINE, 4);
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

// Hold the fire button down for `ticks`, then let go, stepping the world all
// the way through. Returns the world so the caller can look at what happened.
static void gs_fire_for(gs_world *w, const gs_track *t, int ticks) {
    gs_input in[GS_MAX_CARS] = { (gs_input)(GS_IN_ACCEL | GS_IN_FIRE), 0, 0, 0 };
    for (int i = 0; i < ticks; i++) gs_world_step(w, t, in);
    in[0] = (gs_input)GS_IN_ACCEL;
    gs_world_step(w, t, in);
}

TEST(a_tap_leaves_a_hazard_and_a_hold_changes_which_one) {
    // **One button, four things to leave behind.** A tap drops what is
    // selected; holding for half a second moves the selection on instead and
    // drops nothing. That is the whole control, and it is the same control on a
    // pad as on a keyboard, because the binding for it carries a key and a
    // button and gs_input_poll ors them together.
    //
    // This replaces a test that held the button down and counted a trail. That
    // was the rule before there was more than one thing to drop.
    static gs_track t;
    gs_track_init(&t, 80, 20, GS_SURF_PAVEMENT);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(4), GS_INT(10), 0);
    gs_world_arm(&w, 0, GS_HAZ_OIL, 3);
    gs_world_arm(&w, 0, GS_HAZ_MINE, 2);

    // Armed with two kinds, the first of them is what a tap would leave.
    CHECK(gs_car_selected(&w.car[0]) == GS_HAZ_OIL);

    // **A tap leaves one.** Short - well under the half second that means
    // "change" - and the drop lands when the button comes up.
    gs_fire_for(&w, &t, 6);
    CHECK(w.hazard_count == 1);
    CHECK(w.hazard[0].kind == (uint8_t)GS_HAZ_OIL);
    CHECK(gs_car_ammo(&w.car[0], GS_HAZ_OIL) == 2);
    CHECK(gs_car_selected(&w.car[0]) == GS_HAZ_OIL);

    // **A hold leaves nothing and changes what is selected.**
    gs_fire_for(&w, &t, GS_FIRE_HOLD + 4);
    CHECK(w.hazard_count == 1);                       // still just the one
    CHECK(gs_car_selected(&w.car[0]) == GS_HAZ_MINE);
    CHECK(gs_car_ammo(&w.car[0], GS_HAZ_OIL) == 2);   // and nothing was spent

    // And the next tap leaves the thing that is now selected.
    for (int i = 0; i < GS_TICK_HZ; i++) gs_world_step(&w, &t, nullptr);
    gs_fire_for(&w, &t, 6);
    CHECK(w.hazard_count == 2);
    CHECK(w.hazard[1].kind == (uint8_t)GS_HAZ_MINE);
    CHECK(gs_car_ammo(&w.car[0], GS_HAZ_MINE) == 1);

    // **Holding goes round rather than stopping at the end**, and it moves the
    // selection once per hold rather than racing through everything in half a
    // second - which would make the control unusable.
    gs_fire_for(&w, &t, GS_FIRE_HOLD * 3);
    CHECK(gs_car_selected(&w.car[0]) == GS_HAZ_OIL);
}

TEST(fire_burns_while_you_are_in_it_and_then_burns_out) {
    // **A mine punishes arriving; fire punishes staying.** That is the whole
    // difference between them, and it is why fire is not spent by being found:
    // what ends it is its own clock.
    static gs_track t;
    gs_track_init(&t, 60, 20, GS_SURF_PAVEMENT);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(20), GS_INT(10), 0);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(40), GS_INT(10), 0);

    gs_world_arm(&w, 1, GS_HAZ_FLAME, 1);
    w.car[1].x = GS_INT(20);
    w.car[1].y = GS_INT(10);
    CHECK(gs_world_drop(&w, 1, GS_HAZ_FLAME));
    w.car[1].x = GS_INT(40);           // and out of its own fire

    CHECK(w.car[0].damage == 0);

    // A second of standing in it costs something, and two seconds cost more -
    // which is what "while you are in it" means and a mine cannot do.
    for (int i = 0; i < GS_TICK_HZ; i++) gs_world_step(&w, &t, nullptr);
    const uint8_t after_one = w.car[0].damage;
    CHECK(after_one > 0);

    for (int i = 0; i < GS_TICK_HZ; i++) gs_world_step(&w, &t, nullptr);
    CHECK(w.car[0].damage > after_one);

    // **The one who lit it walks through it.** Same rule as oil: a weapon that
    // hurts the person carrying it is a weapon nobody uses.
    CHECK(w.car[1].damage == 0);

    // And it burns out. Long after it should have, the car parked in it stops
    // taking damage at all.
    for (int i = 0; i < GS_TICK_HZ * 6; i++) gs_world_step(&w, &t, nullptr);
    const uint8_t burnt_out = w.car[0].damage;
    for (int i = 0; i < GS_TICK_HZ * 2; i++) gs_world_step(&w, &t, nullptr);
    CHECK(w.car[0].damage == burnt_out);
}

TEST(smoke_hides_the_ground_and_does_nothing_to_the_car) {
    // **Smoke is the one that is not physics.** It is where it is and it goes
    // out after a while, and both of those are world state that two machines
    // have to agree about - but a car driving through it drives exactly as it
    // would have. What it changes is what the driver behind can see, and that
    // belongs to the renderer.
    static gs_track t;
    gs_track_init(&t, 60, 60, GS_SURF_PAVEMENT);

    const double clean = gs_turn_over_hazard(GS_HAZ_NONE, 0);
    const double smoked = gs_turn_over_hazard(GS_HAZ_SMOKE, 1);
    CHECK(clean > 5.0);
    CHECK(smoked > clean - 0.5);      // somebody else's smoke: no difference
    CHECK(smoked < clean + 0.5);

    // And it clears, which is what stops a track being fog by lap three.
    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(30), GS_INT(30), 0);
    gs_world_arm(&w, 0, GS_HAZ_SMOKE, 1);
    CHECK(gs_world_drop(&w, 0, GS_HAZ_SMOKE));
    CHECK(w.hazard[0].spent == 0);
    CHECK(w.hazard[0].life > 0);

    for (int i = 0; i < GS_TICK_HZ * 9; i++) gs_world_step(&w, &t, nullptr);
    CHECK(w.hazard[0].spent == 1);
}

TEST(oil_and_mines_stay_where_they_were_left) {
    // The other half of the same rule: what has no clock does not get one.
    // Oil and a mine are still there at the end of a long race, which is what
    // makes them worth placing early.
    static gs_track t;
    gs_track_init(&t, 60, 60, GS_SURF_PAVEMENT);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(30), GS_INT(30), 0);
    gs_world_arm(&w, 0, GS_HAZ_OIL, 1);
    gs_world_arm(&w, 0, GS_HAZ_MINE, 1);

    CHECK(gs_world_drop(&w, 0, GS_HAZ_OIL));
    for (int i = 0; i < GS_TICK_HZ; i++) gs_world_step(&w, &t, nullptr);
    CHECK(gs_world_drop(&w, 0, GS_HAZ_MINE));

    for (int i = 0; i < GS_TICK_HZ * 60; i++) gs_world_step(&w, &t, nullptr);
    CHECK(w.hazard_count == 2);
    for (uint8_t i = 0; i < w.hazard_count; i++) {
        CHECK(w.hazard[i].life == 0);
        CHECK(w.hazard[i].spent == 0);
    }
}

TEST(every_kind_of_hazard_can_be_carried_dropped_and_told_apart) {
    // Walked from GS_HAZ_COUNT rather than a list, so a fifth kind is in this
    // test the day it exists. Two of the four could not be dropped by anybody
    // at all before this: the mine was written, hashed and unreachable, and
    // smoke and fire did not exist.
    int walked = 0;
    for (int kind = GS_HAZ_NONE + 1; kind < GS_HAZ_COUNT; kind++) {
        static gs_track t;
        gs_track_init(&t, 60, 20, GS_SURF_PAVEMENT);

        gs_world w;
        gs_world_init(&w, GS_ONE);
        gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(20), GS_INT(10), 0);

        // Carried, selected without anybody pressing anything, and dropped by
        // a tap - the whole way a weapon reaches the ground in a real race.
        gs_world_arm(&w, 0, (gs_hazard_kind)kind, 2);
        CHECK(gs_car_selected(&w.car[0]) == (gs_hazard_kind)kind);
        CHECK(gs_car_ammo(&w.car[0], (gs_hazard_kind)kind) == 2);

        gs_fire_for(&w, &t, 6);
        if (w.hazard_count != 1) {
            printf("  HAZARD kind %d was carried and could not be dropped\n",
                   kind);
        }
        CHECK(w.hazard_count == 1);
        CHECK(w.hazard[0].kind == (uint8_t)kind);
        CHECK(w.hazard[0].owner == 0);
        CHECK(gs_car_ammo(&w.car[0], (gs_hazard_kind)kind) == 1);
        walked++;
    }
    printf("  HAZARDS all %d kinds carried, selected and dropped by a tap\n",
           walked);
    CHECK(walked == GS_HAZ_COUNT - 1);
}

TEST(a_car_carrying_nothing_leaves_nothing) {
    // **A race with the weapons turned off is every car carrying zero**, and
    // that is what makes the setting a setting rather than a branch in the
    // simulation. Nothing is dropped, the button does nothing, and nothing
    // selects itself.
    static gs_track t;
    gs_track_init(&t, 80, 20, GS_SURF_PAVEMENT);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(4), GS_INT(10), 0);

    CHECK(gs_car_selected(&w.car[0]) == GS_HAZ_NONE);
    for (int k = 0; k < GS_HAZ_COUNT; k++) {
        CHECK(gs_car_ammo(&w.car[0], (gs_hazard_kind)k) == 0);
    }

    gs_fire_for(&w, &t, 6);                       // a tap
    CHECK(w.hazard_count == 0);
    gs_fire_for(&w, &t, GS_FIRE_HOLD * 2);        // and a hold
    CHECK(w.hazard_count == 0);
    CHECK(gs_car_selected(&w.car[0]) == GS_HAZ_NONE);
}

TEST(a_weapon_runs_out_and_the_button_moves_on) {
    // Spending the last of something moves the selection to whatever is left,
    // so the button keeps doing something instead of going dead in the hand.
    static gs_track t;
    gs_track_init(&t, 80, 20, GS_SURF_PAVEMENT);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(4), GS_INT(10), 0);
    gs_world_arm(&w, 0, GS_HAZ_OIL, 1);
    gs_world_arm(&w, 0, GS_HAZ_SMOKE, 1);
    CHECK(gs_car_selected(&w.car[0]) == GS_HAZ_OIL);

    gs_fire_for(&w, &t, 6);
    CHECK(w.hazard_count == 1);
    CHECK(gs_car_ammo(&w.car[0], GS_HAZ_OIL) == 0);
    CHECK(gs_car_selected(&w.car[0]) == GS_HAZ_SMOKE);

    // The last one of all, and then there is nothing to select.
    for (int i = 0; i < GS_TICK_HZ; i++) gs_world_step(&w, &t, nullptr);
    gs_fire_for(&w, &t, 6);
    CHECK(w.hazard_count == 2);
    CHECK(gs_car_selected(&w.car[0]) == GS_HAZ_NONE);

    // And now the button does nothing at all, however it is pressed.
    for (int i = 0; i < GS_TICK_HZ; i++) gs_world_step(&w, &t, nullptr);
    gs_fire_for(&w, &t, 6);
    CHECK(w.hazard_count == 2);
}

TEST(one_a_second_however_fast_the_button_is_tapped) {
    // The cooldown is what stops a road being paved. It was written when the
    // button dropped while held; tapping is the way to do it quickly now, so
    // this is the rule stated against tapping.
    static gs_track t;
    gs_track_init(&t, 80, 20, GS_SURF_PAVEMENT);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(4), GS_INT(10), 0);
    gs_world_arm(&w, 0, GS_HAZ_OIL, 200);

    // Tapped as fast as a button can be tapped: down one tick, up the next,
    // for five seconds.
    for (int i = 0; i < GS_TICK_HZ * 5 / 2; i++) gs_fire_for(&w, &t, 1);

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

TEST(how_many_are_still_driving_is_one_number_with_one_definition) {
    // **The question a derby asks**, and it was being counted in two places
    // that did not know about each other: the rule that ends the race, and
    // nowhere else - because the screen was not showing it at all. It told the
    // player their position in a running order instead, with the wrecked cars
    // counted among the opposition.
    static gs_track t;
    gs_track_init(&t, 32, 32, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= t.h; y++) {
        for (uint8_t x = 0; x <= t.w; x++) gs_track_set_corner(&t, x, y, 0);
    }
    gs_track_add_gate(&t, GS_INT(8), GS_INT(16), 0, GS_INT(6));
    gs_track_add_gate(&t, GS_INT(24), GS_INT(16), 0, GS_INT(6));

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_set_mode(&w, GS_MODE_DESTRUCTION);
    for (uint8_t i = 0; i < GS_MAX_CARS; i++) {
        gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR,
                         GS_INT(6) + GS_INT(3) * i, GS_INT(14), 0);
    }

    uint8_t last = 0;
    CHECK(gs_world_driving(&w, &last) == GS_MAX_CARS);
    CHECK(last == GS_MAX_CARS - 1);          // the last one still going

    // Wrecked is out of it, whatever it is doing and wherever it is.
    w.car[0].wrecked = true;
    CHECK(gs_world_driving(&w, &last) == GS_MAX_CARS - 1);
    CHECK(last == GS_MAX_CARS - 1);

    w.car[GS_MAX_CARS - 1].wrecked = true;
    CHECK(gs_world_driving(&w, &last) == GS_MAX_CARS - 2);
    CHECK(last == GS_MAX_CARS - 2);

    // **And it is the same number the race is decided by.** Two definitions of
    // "out of it" is a screen saying two are left over a race that has already
    // been won. Two are still going here, so it is not over; wreck one and it
    // is, and the winner is the one the count hands back.
    {
        gs_input in[GS_MAX_CARS] = { 0 };
        gs_world_step(&w, &t, in);
    }
    CHECK(gs_world_driving(&w, &last) == 2);
    CHECK(!w.over);

    w.car[1].wrecked = true;
    CHECK(gs_world_driving(&w, &last) == 1);
    {
        gs_input in[GS_MAX_CARS] = { 0 };
        gs_world_step(&w, &t, in);
    }
    CHECK(w.over);
    CHECK(w.winner == last);

    // **And everybody out at once is nobody's win.** A fresh race, because the
    // one above is settled and settled is forever - a winner who then drives
    // off a cliff in the silence afterwards has still won.
    gs_world all_out;
    gs_world_init(&all_out, GS_ONE);
    gs_world_set_mode(&all_out, GS_MODE_DESTRUCTION);
    for (uint8_t i = 0; i < GS_MAX_CARS; i++) {
        gs_world_add_car(&all_out, &t, (uint8_t)GS_VEH_STOCK_CAR,
                         GS_INT(6) + GS_INT(3) * i, GS_INT(14), 0);
        all_out.car[i].wrecked = true;
    }

    // Nobody driving hands back nobody, rather than the last index it saw.
    last = 3;
    CHECK(gs_world_driving(&all_out, &last) == 0);
    CHECK(last == GS_NO_WINNER);

    {
        gs_input in[GS_MAX_CARS] = { 0 };
        gs_world_step(&all_out, &t, in);
    }
    CHECK(all_out.over);
    CHECK(all_out.winner == GS_NO_WINNER);
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

// How long a lap takes at a given point on the skill dial, in ticks. Timed
// from the first crossing, so the standing start is not counted against it.
static uint32_t gs_ai_lap_ticks(const gs_track *t, gs_fix gravity,
                                uint8_t vehicle, int skill, uint32_t laps) {
    gs_world w;
    gs_world_init(&w, gravity);
    gs_world_add_car(&w, t, vehicle, GS_INT(20), GS_INT(15), 0);

    const gs_ai_style style = gs_ai_skill_style(skill);
    uint64_t started = 0;
    for (uint32_t i = 0; i < GS_TICK_HZ * 600u; i++) {
        gs_input in[GS_MAX_CARS] = { gs_ai_drive_style(&w, t, 0, style), 0, 0, 0 };
        gs_world_step(&w, t, in);
        if (started == 0 && w.car[0].laps == 1) started = w.tick;
        if (started != 0 && w.car[0].laps == 1 + laps) {
            return (uint32_t)(w.tick - started);
        }
    }
    return 0;
}

// How fast this driver arrives at the second gate, and how far off its centre.
// Both are what a person watching would call the line into the corner.
typedef struct gs_ai_entry {
    gs_fix speed;       // when the gate is crossed
    gs_fix offset;      // across the gate from its middle, absolute
    uint32_t tick;
} gs_ai_entry;

static gs_ai_entry gs_ai_corner_entry(const gs_track *t, gs_fix gravity,
                                      uint8_t vehicle, int skill) {
    gs_world w;
    gs_world_init(&w, gravity);
    gs_world_add_car(&w, t, vehicle, GS_INT(20), GS_INT(15), 0);

    const gs_ai_style style = gs_ai_skill_style(skill);
    const uint8_t want = 2;              // the gate after the first corner

    for (uint32_t i = 0; i < GS_TICK_HZ * 200u; i++) {
        gs_input in[GS_MAX_CARS] = { gs_ai_drive_style(&w, t, 0, style), 0, 0, 0 };
        uint8_t was = w.car[0].next_gate;
        gs_world_step(&w, t, in);
        if (was == want - 1 && w.car[0].next_gate == want) {
            const gs_gate *g = &t->gate[want - 1];
            // Across the gate is at right angles to the way it faces.
            gs_fix nx = -gs_sin(g->heading), ny = gs_cos(g->heading);
            gs_fix off = gs_fix_mul(w.car[0].x - g->x, nx) +
                         gs_fix_mul(w.car[0].y - g->y, ny);
            return (gs_ai_entry){ gs_car_speed(&w.car[0]), gs_fix_abs(off),
                                  (uint32_t)w.tick };
        }
    }
    return (gs_ai_entry){ 0, 0, 0 };
}

// A ramp up onto a plateau, with a gate at each end - so the driver has a
// reason to come down it at speed and something to launch off at the top. The
// break where the slope meets the flat is what does the launching, which is the
// same thing the jump tests above use.
static void gs_ai_jump(gs_track *t) {
    gs_build_ramp(t, 8, 12, GS_INT(1));
    gs_track_add_gate(t, GS_INT(4), GS_INT(4), 0, GS_INT(3));
    gs_track_add_gate(t, GS_INT(16), GS_INT(4), 0, GS_INT(3));
}

// Where the car comes down after the ramp, and how fast it left it.
typedef struct gs_ai_flight { gs_fix take_off; gs_fix landed; } gs_ai_flight;

static gs_ai_flight gs_ai_over_the_jump(const gs_track *t, int skill) {
    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(2), GS_INT(4), 0);

    const gs_ai_style style = gs_ai_skill_style(skill);
    gs_ai_flight f = { 0, 0 };
    bool flew = false;

    for (uint32_t i = 0; i < GS_TICK_HZ * 60u; i++) {
        bool was_air = !w.car[0].grounded;
        gs_input in[GS_MAX_CARS] = { gs_ai_drive_style(&w, t, 0, style), 0, 0, 0 };
        gs_world_step(&w, t, in);
        bool is_air = !w.car[0].grounded;

        if (!was_air && is_air && w.car[0].x > GS_INT(10)) {
            f.take_off = gs_car_speed(&w.car[0]);
            flew = true;
        }
        if (was_air && !is_air && flew) {
            f.landed = w.car[0].x;
            return f;
        }
    }
    return f;
}

TEST(the_skill_dial_changes_how_they_drive_and_not_only_how_fast) {
    // **A dial that only scales the top speed is a handicap, not a driver.**
    //
    // What it moves is where they lift, how much of the grip they ask for once
    // they are in the corner, and how straight they hold it - so two settings a
    // tenth of a second apart over a lap are still visibly doing different
    // things at the same corner and over the same jump.
    static gs_track pav, jump;
    gs_circuit(&pav, GS_SURF_PAVEMENT);
    gs_ai_jump(&jump);

    static gs_ai_entry entry[GS_AI_SKILL_STEPS + 1];
    static gs_ai_flight flight[GS_AI_SKILL_STEPS + 1];
    static uint32_t lap[GS_AI_SKILL_STEPS + 1];

    for (int sk = 0; sk <= GS_AI_SKILL_STEPS; sk++) {
        entry[sk] = gs_ai_corner_entry(&pav, GS_ONE, (uint8_t)GS_VEH_STOCK_CAR, sk);
        flight[sk] = gs_ai_over_the_jump(&jump, sk);
        lap[sk] = gs_ai_lap_ticks(&pav, GS_ONE, (uint8_t)GS_VEH_STOCK_CAR, sk, 3);

        CHECK(entry[sk].tick > 0);          // it got to the corner
        CHECK(flight[sk].take_off > 0);     // and it left the ground
        CHECK(flight[sk].landed > 0);       // and came back down
        CHECK(lap[sk] > 0);
    }

    // **No two neighbours drive the same jump.** Not "the fast one is faster" -
    // every adjacent pair on the dial leaves the ramp at a different speed and
    // lands somewhere else, which is what makes it twenty-one drivers rather
    // than one driver with twenty-one handicaps.
    int same_take_off = 0, same_landing = 0;
    for (int sk = 1; sk <= GS_AI_SKILL_STEPS; sk++) {
        if (flight[sk].take_off == flight[sk - 1].take_off) same_take_off++;
        if (flight[sk].landed == flight[sk - 1].landed) same_landing++;
    }
    CHECK(same_take_off == 0);
    CHECK(same_landing == 0);

    // **And the closest pair of all still differs.** Whichever two settings lap
    // nearest each other - found rather than chosen, so it stays the hardest
    // case when the numbers move.
    int closest = 1;
    for (int sk = 2; sk <= GS_AI_SKILL_STEPS; sk++) {
        if (lap[sk - 1] - lap[sk] < lap[closest - 1] - lap[closest]) closest = sk;
    }
    const uint32_t gap = lap[closest - 1] - lap[closest];
    printf("  STYLE closest pair is %d and %d, %u ticks apart over three laps; "
           "they leave the ramp at %.3f and %.3f and land %.3f apart\n",
           closest - 1, closest, gap,
           (double)flight[closest - 1].take_off / 65536.0,
           (double)flight[closest].take_off / 65536.0,
           (double)gs_fix_abs(flight[closest].landed - flight[closest - 1].landed) /
               65536.0);
    CHECK(gap * 200u < lap[closest]);       // within half a percent of each other
    CHECK(flight[closest].take_off != flight[closest - 1].take_off);
    CHECK(flight[closest].landed != flight[closest - 1].landed);

    // **Across the whole dial it is not subtle.** The quick one arrives at the
    // corner several times faster than the timid one, and lands most of a tile
    // further down the road.
    printf("  STYLE corner entry %.3f at the bottom against %.3f at the top; "
           "landing %.3f against %.3f\n",
           (double)entry[0].speed / 65536.0,
           (double)entry[GS_AI_SKILL_STEPS].speed / 65536.0,
           (double)flight[0].landed / 65536.0,
           (double)flight[GS_AI_SKILL_STEPS].landed / 65536.0);
    CHECK(entry[GS_AI_SKILL_STEPS].speed > gs_fix_mul(entry[0].speed, GS_INT(5)));
    CHECK(flight[GS_AI_SKILL_STEPS].landed - flight[0].landed > GS_HALF);
}

TEST(every_step_of_the_skill_dial_is_a_faster_lap_than_the_one_below_it) {
    // **The dial is a dial, and it is checked as one.** Not three settings and
    // not the two ends: every step of it, in four sets of conditions, each one
    // strictly quicker than the step below - no ties anywhere, which is the
    // difference between a dial and a dropdown with twenty-one identical
    // entries on it.
    //
    // Four conditions rather than one because the driver is not tuned per
    // track: pavement is where it is easy, dirt has two thirds of the grip,
    // the Moon has a sixth of the weight, and a different machine has
    // completely different numbers.
    static gs_track pav, dirt;
    gs_circuit(&pav, GS_SURF_PAVEMENT);
    gs_circuit(&dirt, GS_SURF_DIRT);

    static const struct {
        const char *name;
        gs_track   *track;
        gs_fix      gravity;
        uint8_t     vehicle;
    } runs[] = {
        { "pavement, Earth", &pav,  GS_ONE,             (uint8_t)GS_VEH_STOCK_CAR },
        { "dirt, Earth",     &dirt, GS_ONE,             (uint8_t)GS_VEH_STOCK_CAR },
        { "pavement, Moon",  &pav,  GS_RATIO(17, 100),  (uint8_t)GS_VEH_STOCK_CAR },
        { "a dune buggy",    &pav,  GS_ONE,             (uint8_t)GS_VEH_DUNE_BUGGY },
    };

    int checked = 0;
    for (size_t r = 0; r < (sizeof runs / sizeof runs[0]); r++) {
        uint32_t last = 0;
        for (int sk = 0; sk <= GS_AI_SKILL_STEPS; sk++) {
            uint32_t lap = gs_ai_lap_ticks(runs[r].track, runs[r].gravity,
                                           runs[r].vehicle, sk, 3);

            // **Every setting gets round**, the timid one included. A driver
            // who brakes far too early is still a driver; one that cannot
            // complete a lap is a setting nobody should be offered.
            if (lap == 0) printf("  STUCK at skill %d on %s\n", sk, runs[r].name);
            CHECK(lap > 0);

            if (sk > 0) {
                if (lap >= last) {
                    printf("  NOT FASTER skill %d on %s: %u against %u\n",
                           sk, runs[r].name, lap, last);
                }
                CHECK(lap < last);
            }
            last = lap;
            checked++;
        }
    }

    printf("  DIAL %d settings timed, over %d sets of conditions\n",
           checked, (int)(sizeof runs / sizeof runs[0]));
    CHECK(checked == (GS_AI_SKILL_STEPS + 1) * (int)(sizeof runs / sizeof runs[0]));
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
    // Difficulty is confidence and nothing else: how much of the available grip
    // the driver is willing to use, and how late they are willing to leave the
    // braking. Not extra power, not rubber-banding, not cheating on the physics
    // - the same things that separate two people.
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

    gs_input careful = gs_ai_drive_style(&w, &t, 0, gs_ai_skill_style(0));
    gs_input quick = gs_ai_drive_style(&w, &t, 0,
                                       gs_ai_skill_style(GS_AI_SKILL_STEPS));

    CHECK((careful & GS_IN_BRAKE) != 0);
    CHECK((quick & GS_IN_BRAKE) == 0);
    CHECK((quick & GS_IN_ACCEL) != 0);

    // And the default sits between them rather than at one end, which is what
    // makes it worth racing rather than a formality in either direction.
    // **The dial rises all the way and never reaches the limit.** Every step
    // asks for more grip than the one below it, and the top of it still leaves
    // something in hand - a driver exactly at the limit is a driver about to be
    // over it, and the estimate it is working from is a chord approximation.
    for (int s = 1; s <= GS_AI_SKILL_STEPS; s++) {
        CHECK(gs_ai_skill_margin(s - 1) < gs_ai_skill_margin(s));
    }
    CHECK(gs_ai_skill_margin(GS_AI_SKILL_STEPS) < GS_ONE);

    // And it is clamped rather than trusted: a setting off either end is the
    // end it is off.
    CHECK(gs_ai_skill_margin(-5) == gs_ai_skill_margin(0));
    CHECK(gs_ai_skill_margin(GS_AI_SKILL_STEPS + 5) ==
          gs_ai_skill_margin(GS_AI_SKILL_STEPS));
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

TEST(a_longer_route_is_given_longer_to_get_round) {
    // A constant here reported big tracks impossible: the same twenty seconds
    // that is generous on a forty tile sprint runs out halfway along a fifty
    // two tile out-and-back, and the verdict then describes the clock rather
    // than the track.
    static gs_track small, big;

    gs_track_init(&small, 24, 16, GS_SURF_PAVEMENT);
    gs_track_add_gate(&small, GS_INT(4), GS_INT(8), 0, GS_INT(4));
    gs_track_add_gate(&small, GS_INT(20), GS_INT(8), 0, GS_INT(4));

    gs_track_init(&big, 64, 48, GS_SURF_PAVEMENT);
    gs_track_add_gate(&big, GS_INT(4), GS_INT(24), 0, GS_INT(4));
    gs_track_add_gate(&big, GS_INT(60), GS_INT(24), 0, GS_INT(4));

    CHECK(gs_analyse_seconds(&big) > gs_analyse_seconds(&small));

    // Floor and ceiling: a tiny track still gets a fair go, and no single track
    // can turn a fifty track sweep into a coffee break.
    CHECK(gs_analyse_seconds(&small) >= 20);
    CHECK(gs_analyse_seconds(&big) <= 90);

    // A track with no route at all still answers rather than dividing by it.
    static gs_track bare;
    gs_track_init(&bare, 32, 32, GS_SURF_PAVEMENT);
    CHECK(gs_analyse_seconds(&bare) >= 20);

    // And the number is what it is for: the big track can be got round.
    gs_analyse(&big, gs_analyse_seconds(&big), &gs_report);
    CHECK(gs_report.completable);
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
    //
    // **Moved once, deliberately**, when a track gained the thing that says
    // whether it is a loop or a path - see gs_route_kind, and the note in
    // src/frontend/cli/golden.h about why the tracks had to change at all. The
    // header grew a byte, so every code is a byte longer and none of the old
    // ones spell the same string.
    //
    // Old codes still *read*, which is the half of it that matters: the version
    // two code this test used to pin is below, and it still decodes to the same
    // track it always did. Nobody's shared link stopped working - they just do
    // not spell the same thing any more.
    //
    // **And moved a second time, deliberately**, when whether a track is a loop
    // or a path became part of its identity. A code carries the hash of what it
    // encodes, so a new code spells a different string - but the reader takes
    // the answer that hash used to give as well, and the version two code below
    // is the proof: it was shared with v0.1.0-beta1 and it still opens. See the
    // note over gs_track_hash_before_route_kind.
    static const char *want =
        "GST1TvZUKuduud3_R1RSSwMAAAAnCAYCBQAABAEKBxEPYBEPEQ8RDxEPEQ0BQAEPAgEN"
        "AhMPAQ8BDwECagPrAQLpAwYNCA";

    static const char *version_two =
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
    CHECK(gs_track_is_circuit(&back) == gs_track_is_circuit(&t));

    // A code shared before the route kind existed still opens, and opens as a
    // path - which is what every one of those tracks was.
    static gs_track old_one;
    CHECK(gs_track_from_code(&old_one, version_two));
    CHECK(old_one.w == t.w && old_one.h == t.h);
    CHECK(old_one.gate_count == t.gate_count);
    CHECK(!gs_track_is_circuit(&old_one));
}

// ---------------------------------------------------------------------------
// Rollback netcode
// ---------------------------------------------------------------------------

// A link with latency, jitter and loss, and no allocation - a fixed ring of
// datagrams, each with the tick it becomes deliverable on. Deterministic, so a
// failure is a failure somebody else can reproduce rather than a bad afternoon.
#define GS_LINK_MAX 512
// Exactly what the protocol produces, deliberately. A test link with a smaller
// one silently swallows every packet that outgrows it, and the race then fails
// as a stall - which reads as a network fault and is in fact a constant
// somebody changed. `gs_wire.h` separately asserts the real wire is at least
// this big.
#define GS_LINK_MTU GS_NET_MTU

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

    // Packets too big for the link. Not loss - loss is deliberate here and has
    // its own counter. This one means the datagram outgrew what carries it, and
    // a race that lets it pass unremarked reports the result as a stall.
    uint32_t oversize;
} gs_link;

static uint32_t gs_link_rand(gs_link *l) {
    l->seed = l->seed * 1103515245u + 12345u;
    return (l->seed >> 16) & 0x7fffu;
}

static void gs_link_send(gs_link *l, uint32_t now, const uint8_t *b, size_t n) {
    l->sent++;
    if (n > GS_LINK_MTU) { l->oversize++; return; }
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
// A secret per peer, fixed so a failing race can be re-run and get the same
// salts. A real client draws one at random; a test that did the same would be a
// test that fails differently every time.
static const uint8_t *gs_test_secret(uint8_t who) {
    static uint8_t s[GS_MAX_CARS][GS_NET_SECRET_BYTES];
    for (unsigned i = 0; i < GS_NET_SECRET_BYTES; i++) {
        s[who][i] = (uint8_t)((0x5au + who * 31u + i * 7u) & 0xffu);
    }
    return s[who];
}

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

    gs_net_begin(a, &w, 2, 0, gs_test_secret(0));
    gs_net_begin(b, &w, 2, 1, gs_test_secret(1));

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

    // **The race is over on both machines, so what is still owed goes out.**
    // While a race is on, the reveals run a fixed distance behind the
    // commitments - that gap is the whole mechanism - which leaves the last
    // dozen ticks promised and never shown. A real client flushes them when it
    // reaches the results screen; a test that did not would be measuring a race
    // whose ending nobody ever proved.
    gs_net_finish(a);
    gs_net_finish(b);

    // Long enough for the flush to actually start: a finished race spends its
    // first several dozen datagrams repeating the promises for the last ticks
    // before it reveals them, because a flushing datagram's own promises are
    // inadmissible. A settle shorter than that ends before the reveals do.
    uint32_t settle = ticks + 96u + latency + jitter;
    for (uint32_t tick = ticks; tick < settle; tick++) {
        gs_link_deliver(ab, tick, b, t);
        gs_link_deliver(ba, tick, a, t);

        uint8_t buf[GS_LINK_MTU];
        size_t n = gs_net_packet(a, buf, sizeof buf);
        gs_link_send(ab, tick, buf, n);
        n = gs_net_packet(b, buf, sizeof buf);
        gs_link_send(ba, tick, buf, n);
    }

    // And then let the tail of the link drain, so both machines have every
    // input and can confirm the whole race. Without this the last few ticks are
    // still guesses on both sides and comparing them would be comparing
    // predictions.
    for (uint32_t tick = settle; tick < settle + latency + jitter + 8u; tick++) {
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

    // The link really was bad and the rollback really did work for its living -
    // and nothing was lost to being too big, which is a different fault wearing
    // a stall's clothes.
    CHECK(ab.oversize == 0);
    CHECK(ba.oversize == 0);
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

TEST(a_race_with_nobody_else_in_it_never_stalls) {
    // **Found by playing it.** A player on a server of their own drove one
    // race, and two and an eighth seconds in - tick 255, one window - it froze
    // with the controls dead. Confirmation only ever happened when a datagram
    // arrived, and a race with one car in it never hears one, so nothing was
    // confirmed, the window filled, and the stall that is meant to say "the
    // other machine has gone quiet" fired forever at a race that had no other
    // machine in it.
    static gs_track t;
    gs_world w;
    gs_net_scene(&t, &w);

    static gs_net solo;
    gs_net_begin(&solo, &w, 1, 0, gs_test_secret(0));

    // Three windows and a bit, so this fails at the first one if the fault
    // comes back and does not pass by not getting that far.
    uint32_t ticks = GS_NET_WINDOW * 3u + 17u;
    for (uint32_t tick = 0; tick < ticks; tick++) {
        gs_net_local_input(&solo, gs_net_drive(0, tick));

        // A packet is still built and thrown away, exactly as the frontend
        // does it: a race with nobody in it still talks, and if building the
        // packet were what confirmed a tick this test would be measuring the
        // wrong thing.
        uint8_t buf[GS_LINK_MTU];
        (void)gs_net_packet(&solo, buf, sizeof buf);

        CHECK(gs_net_step(&solo, &t));
        if (solo.stalls > 0) break;
    }

    CHECK(solo.stalls == 0);
    CHECK(solo.local_tick == ticks);

    // And the confirmed state keeps up with what is being driven, because it is
    // the only thing that can: there is nobody else's input to wait for.
    CHECK(solo.confirmed_tick == ticks);

    // The race it produced is the race one machine with no network would have
    // driven, which is the claim that makes a solo online race worth having.
    gs_world alone;
    gs_net_scene(&t, &alone);
    for (uint32_t tick = 0; tick < ticks; tick++) {
        gs_input in[GS_MAX_CARS] = { 0 };
        in[0] = gs_net_drive(0, tick);
        gs_world_step(&alone, &t, in);
    }
    CHECK(gs_world_hash(&alone) == gs_world_hash(&solo.confirmed));
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
    for (int i = 0; i < 4; i++) {
        gs_net_begin(&net[i], &w, 4, (uint8_t)i, gs_test_secret((uint8_t)i));
    }

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

    // All four have finished, so the reveals still owed go out - see the note
    // in gs_net_race. Then the links drain.
    for (int i = 0; i < 4; i++) gs_net_finish(&net[i]);

    for (uint32_t tick = ticks; tick < ticks + 80u; tick++) {
        for (int a = 0; a < 4; a++) {
            for (int b = 0; b < 4; b++) {
                if (a != b) gs_link_deliver(&link[a][b], tick, &net[b], &t);
            }
        }
        for (int i = 0; i < 4; i++) {
            uint8_t buf[GS_LINK_MTU];
            size_t n = gs_net_packet(&net[i], buf, sizeof buf);
            for (int b = 0; b < 4; b++) {
                if (b != i) gs_link_send(&link[i][b], tick, buf, n);
            }
        }
    }

    for (uint32_t tick = ticks + 80u; tick < ticks + 160u; tick++) {
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
    CHECK(link[0][1].oversize == 0);
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
    gs_net_begin(&a, &w, 2, 0, gs_test_secret(0));
    gs_net_begin(&b, &w, 2, 1, gs_test_secret(1));
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

// --- BLAKE2s, checked against people who are not us --------------------------

static bool gs_digest_is(const uint8_t *got, uint8_t len, const char *hex) {
    for (uint8_t i = 0; i < len; i++) {
        char pair[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
        unsigned want = 0;
        for (int k = 0; k < 2; k++) {
            char c = pair[k];
            unsigned d = (c >= '0' && c <= '9') ? (unsigned)(c - '0')
                                                : (unsigned)(c - 'a' + 10);
            want = want * 16u + d;
        }
        if (got[i] != (uint8_t)want) return false;
    }
    return true;
}

TEST(blake2s_produces_the_digests_the_rfc_and_an_independent_library_say) {
    // **The first line is the published one.** RFC 7693 appendix B gives the
    // digest of "abc", and an implementation of a standard that has only ever
    // been checked against itself is an implementation of something else.
    //
    // The rest come from Python's `hashlib`, which is a different implementation
    // written by different people. They are here for the lengths the RFC does
    // not cover, and those lengths are chosen rather than arbitrary: sixty-four
    // bytes is exactly one block, and it is the case that a buffered hash gets
    // wrong when it compresses a full block eagerly instead of waiting to find
    // out whether anything follows it.
    static const struct { const char *msg; size_t len; const char *want; } v[] = {
        { "",    0,  "69217a3079908094e11121d042354a7c1f55b6482ca1a51e1b250dfd1ed0eef9" },
        { "abc", 3,  "508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982" },
    };
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        uint8_t out[GS_BLAKE2S_BYTES];
        gs_blake2s_hash(out, GS_BLAKE2S_BYTES, v[i].msg, v[i].len);
        CHECK(gs_digest_is(out, GS_BLAKE2S_BYTES, v[i].want));
    }

    static uint8_t a[256];
    for (size_t i = 0; i < sizeof a; i++) a[i] = 'a';

    struct { size_t len; const char *want; } runs[] = {
        {  63, "9a4267618070af968ff2a0fdaecc62b5c15ab91cb4a56424ba9fcad20aab417c" },
        {  64, "651d2f5f20952eacaea2fba2f2af2bcd633e511ea2d2e4c9ae2ac0d9ffb7b252" },
        {  65, "045f8ae18932119bd051ac7ba5c73db59892055fad5c32f82d79a6543d92a497" },
        { 128, "3ac477e27353f9019b81694afe60c8049403784f91a58288428ea318bfa82809" },
    };
    for (size_t i = 0; i < sizeof runs / sizeof runs[0]; i++) {
        uint8_t out[GS_BLAKE2S_BYTES];
        gs_blake2s_hash(out, GS_BLAKE2S_BYTES, a, runs[i].len);
        CHECK(gs_digest_is(out, GS_BLAKE2S_BYTES, runs[i].want));
    }

    // Every byte value, so a table indexed wrongly shows up.
    static uint8_t all[256];
    for (size_t i = 0; i < sizeof all; i++) all[i] = (uint8_t)i;
    uint8_t out[GS_BLAKE2S_BYTES];
    gs_blake2s_hash(out, GS_BLAKE2S_BYTES, all, sizeof all);
    CHECK(gs_digest_is(out, GS_BLAKE2S_BYTES,
                       "5fdeb59f681d975f52c8e69c5502e02a12a3afcc5836ba58f42784c439228781"));

    // A shorter digest is a different hash, not a truncation of the long one -
    // the length goes into the parameter block before a byte is taken in.
    uint8_t half[16];
    gs_blake2s_hash(half, 16, "abc", 3);
    CHECK(gs_digest_is(half, 16, "aa4938119b1dc7b87cbad0ffd200d0ae"));
    gs_blake2s_hash(out, GS_BLAKE2S_BYTES, "abc", 3);
    CHECK(memcmp(half, out, 16) != 0);

    // Fed in pieces, it is the same hash. Anything that buffers has an off-by-one
    // waiting in it, and this is where it would show.
    gs_blake2s s;
    gs_blake2s_init(&s, GS_BLAKE2S_BYTES);
    for (size_t i = 0; i < 128; i++) gs_blake2s_update(&s, a + i, 1);
    uint8_t piecemeal[GS_BLAKE2S_BYTES];
    gs_blake2s_final(&s, piecemeal);
    gs_blake2s_hash(out, GS_BLAKE2S_BYTES, a, 128);
    CHECK(memcmp(piecemeal, out, GS_BLAKE2S_BYTES) == 0);
}

// --- commit, then reveal -----------------------------------------------------

// Where the interesting bytes are in a rollback datagram. The format is in
// gs_net.c; this repeats only as much of it as a test needs to tamper.
#define GS_PKT_COMMITS(p)  ((size_t)(p)[5])
#define GS_PKT_REVEALS(p)  ((size_t)(p)[10])
#define GS_PKT_REVEAL_AT(p, i) \
    (GS_NET_HEAD + GS_PKT_COMMITS(p) * 8u + (size_t)(i) * 9u)
#define GS_PKT_COMMIT_AT(p, i) (GS_NET_HEAD + (size_t)(i) * 8u)

// Run two peers honestly over a perfect link for a while, so there is a real
// race in progress to tell a lie inside.
// `quiet` ticks at the end during which b runs on and a hears none of it. That
// tail is what leaves a with ticks it has not confirmed - and only an
// unconfirmed tick can still be lied about, because a confirmed one has already
// been folded into a state that cannot be rewound to.
static void gs_commit_warmup(gs_net *a, gs_net *b, gs_track *t, uint32_t ticks,
                             uint32_t quiet) {
    gs_world w;
    gs_net_scene(t, &w);
    gs_net_begin(a, &w, 2, 0, gs_test_secret(0));
    gs_net_begin(b, &w, 2, 1, gs_test_secret(1));

    for (uint32_t tick = 0; tick < ticks + quiet; tick++) {
        gs_net_local_input(a, gs_net_drive(0, tick));
        gs_net_local_input(b, gs_net_drive(1, tick));

        uint8_t pa[GS_NET_MTU], pb[GS_NET_MTU];
        size_t na = gs_net_packet(a, pa, sizeof pa);
        size_t nb = gs_net_packet(b, pb, sizeof pb);
        gs_net_receive(b, t, pa, na);
        if (tick < ticks) gs_net_receive(a, t, pb, nb);
        gs_net_step(a, t);
        gs_net_step(b, t);
    }
}

TEST(a_peer_that_reveals_an_input_it_did_not_commit_to_is_caught) {
    // **The hole this closes.** Rollback hands every peer the others' inputs
    // for a tick, so a modified client can wait, look, and then decide. Nothing
    // desyncs when it does - everybody simulates the dishonest input faithfully
    // - so the state-hash check sees nothing wrong. The promise is what sees it.
    static gs_net a, b;
    static gs_track t;
    gs_commit_warmup(&a, &b, &t, 90, 8);

    CHECK(!a.cheated);
    CHECK(a.confirmed_tick > 0);       // an honest race really was under way

    // b's next datagram, with one revealed input changed to something else.
    // Everything else about it is honest, including the promise it no longer
    // matches.
    uint8_t pkt[GS_NET_MTU];
    size_t n = gs_net_packet(&b, pkt, sizeof pkt);
    CHECK(n > 0);
    CHECK(GS_PKT_REVEALS(pkt) > 0);

    // **The newest revealed tick that a is actually in a position to check** -
    // one it has not already confirmed, and one it holds a promise for. Picking
    // an index by arithmetic instead would be a test that stops testing its rule
    // the day a constant moves, and passes quietly while it does.
    uint32_t reveal_base = (uint32_t)pkt[11] | ((uint32_t)pkt[12] << 8) |
                           ((uint32_t)pkt[13] << 16) | ((uint32_t)pkt[14] << 24);
    size_t lie = SIZE_MAX;
    for (size_t i = GS_PKT_REVEALS(pkt); i-- > 0; ) {
        uint32_t tick = reveal_base + (uint32_t)i;
        if (tick < a.confirmed_tick) break;
        if (a.committed[tick % GS_NET_WINDOW] & (uint8_t)(1u << 1)) { lie = i; break; }
    }
    CHECK(lie != SIZE_MAX);
    if (lie == SIZE_MAX) return;

    size_t at = GS_PKT_REVEAL_AT(pkt, lie);
    pkt[at] = (uint8_t)(pkt[at] ^ GS_IN_BRAKE);

    gs_net_receive(&a, &t, pkt, n);

    CHECK(a.cheated);
    CHECK(a.cheat_by == 1);

    // And the race stops. Not stalls - stops: a stall ends when a datagram
    // arrives, and there is no datagram that makes this all right.
    uint32_t was = a.local_tick;
    gs_net_local_input(&a, GS_IN_ACCEL);
    CHECK(!gs_net_step(&a, &t));
    CHECK(a.local_tick == was);
}

TEST(a_peer_that_promises_two_different_things_for_one_tick_is_caught) {
    // The other way to wriggle out of a promise: make several, and see which
    // one it turns out to be convenient to have made. An honest peer repeats
    // the same eight bytes in every datagram that carries them.
    static gs_net a, b;
    static gs_track t;
    gs_commit_warmup(&a, &b, &t, 90, 8);

    uint8_t pkt[GS_NET_MTU];
    size_t n = gs_net_packet(&b, pkt, sizeof pkt);
    CHECK(n > 0);
    CHECK(GS_PKT_COMMITS(pkt) > 0);

    // The honest one first, so the promise is on the record...
    gs_net_receive(&a, &t, pkt, n);
    CHECK(!a.cheated);

    // ...and then the same tick promised differently.
    size_t at = GS_PKT_COMMIT_AT(pkt, 0);
    pkt[at] = (uint8_t)(pkt[at] ^ 0xffu);
    gs_net_receive(&a, &t, pkt, n);

    CHECK(a.cheated);
    CHECK(a.cheat_by == 1);
}

TEST(a_promise_shown_in_the_same_breath_as_its_proof_buys_nothing) {
    // **The rule the whole thing rests on.** If a peer may promise and prove in
    // one datagram, the promise costs nothing to make: it can wait to see
    // everybody else's input, choose, and then build both at once. So a promise
    // only counts when it arrives in a datagram that does not also prove it,
    // which is what forces the two apart in time.
    //
    // A peer that ignores the gap is not caught lying - it has not lied. It
    // simply never says anything that can be checked, and nothing it sends is
    // ever accepted.
    static gs_net a, b;
    static gs_track t;
    gs_world w;
    gs_net_scene(&t, &w);
    gs_net_begin(&a, &w, 2, 0, gs_test_secret(0));
    gs_net_begin(&b, &w, 2, 1, gs_test_secret(1));

    // b reveals everything the moment it commits to it, from the first tick.
    //
    // `flush_wait` is cleared by hand, which is the whole point of the test: a
    // real client repeats its promises in ordinary datagrams before it reveals
    // anything, and this one deliberately does not. Without that line b spends
    // its first several dozen datagrams behaving honestly and the rule under
    // test never comes up.
    gs_net_finish(&b);
    b.flush_wait = 0;

    for (uint32_t tick = 0; tick < 300; tick++) {
        gs_net_local_input(&a, gs_net_drive(0, tick));
        gs_net_local_input(&b, gs_net_drive(1, tick));

        uint8_t pa[GS_NET_MTU], pb[GS_NET_MTU];
        size_t na = gs_net_packet(&a, pa, sizeof pa);
        size_t nb = gs_net_packet(&b, pb, sizeof pb);
        gs_net_receive(&b, &t, pa, na);
        gs_net_receive(&a, &t, pb, nb);
        gs_net_step(&a, &t);
        gs_net_step(&b, &t);
    }

    // Nothing b said was ever usable, so a confirmed nothing and stalled at the
    // window - the same as if b had never spoken at all, which is the honest
    // outcome for a peer whose word cannot be checked.
    CHECK(!a.cheated);
    CHECK(a.confirmed_tick == 0);
    CHECK(a.stalls > 0);

    // And a, which does keep the gap, was believed by b throughout.
    CHECK(b.confirmed_tick > 0);
    CHECK(!b.cheated);
}

// --- the whole race, not just the winning lap --------------------------------

// What the frontend does every frame of a networked race: write down the ticks
// everybody has agreed on, and nothing else. The visible world is built partly
// on guesses about the other cars, and most of those are rolled back, so a
// recording taken from it would be a recording of things that did not happen.
static void gs_record_agreed(gs_replay *rec, const gs_net *n, uint32_t *cursor) {
    uint32_t upto = gs_net_confirmed_tick(n);
    while (*cursor < upto) {
        const gs_input *in = gs_net_confirmed_input(n, *cursor);
        if (in == nullptr) break;
        if (!gs_replay_record(rec, in)) break;
        (*cursor)++;
    }
}

static gs_replay gs_agreed[4];

TEST(a_four_player_race_produces_a_log_that_re_races_to_the_ending_they_agreed_on) {
    // **Nearly free, because the simulation is exactly reproducible.** That is
    // the argument for having built it this way, collected: four machines, a
    // bad link, no referee, and afterwards a single log that a server can drive
    // through the same physics and arrive exactly where they all did.
    static gs_net net[4];
    static gs_link link[4][4];
    static gs_track t;
    static uint32_t cursor[4];

    gs_world w;
    gs_net_scene(&t, &w);

    for (int i = 0; i < 4; i++) {
        gs_net_begin(&net[i], &w, 4, (uint8_t)i, gs_test_secret((uint8_t)i));
        gs_replay_begin(&gs_agreed[i], &w, &t);
        cursor[i] = 0;
    }
    for (int a = 0; a < 4; a++) {
        for (int b = 0; b < 4; b++) {
            link[a][b] = (gs_link){ 0 };
            link[a][b].seed = 0x7000u + (uint32_t)a * 41u + (uint32_t)b * 13u;
            link[a][b].latency = 14u + (uint32_t)(a + b) * 2u;
            link[a][b].jitter = 5;
            link[a][b].loss_pct = 9;
        }
    }

    const uint32_t ticks = GS_TICK_HZ * 6;
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
            for (int b = 0; b < 4; b++) {
                if (b != i) gs_link_send(&link[i][b], tick, buf, n);
            }
            gs_net_step(&net[i], &t);
            gs_record_agreed(&gs_agreed[i], &net[i], &cursor[i]);
        }
    }

    for (int i = 0; i < 4; i++) gs_net_finish(&net[i]);
    for (uint32_t tick = ticks; tick < ticks + 160u; tick++) {
        for (int a = 0; a < 4; a++) {
            for (int b = 0; b < 4; b++) {
                if (a != b) gs_link_deliver(&link[a][b], tick, &net[b], &t);
            }
        }
        for (int i = 0; i < 4; i++) {
            if (tick < ticks + 80u) {
                uint8_t buf[GS_LINK_MTU];
                size_t n = gs_net_packet(&net[i], buf, sizeof buf);
                for (int b = 0; b < 4; b++) {
                    if (b != i) gs_link_send(&link[i][b], tick, buf, n);
                }
            }
            gs_record_agreed(&gs_agreed[i], &net[i], &cursor[i]);
        }
    }

    // All four wrote down the same race, to the same length, ending in the same
    // state. Everything below rests on this.
    uint64_t agreed = gs_net_agreed_hash(&net[0]);
    for (int i = 0; i < 4; i++) {
        CHECK(!net[i].desynced);
        CHECK(!net[i].cheated);
        CHECK(gs_net_confirmed_tick(&net[i]) == ticks);
        CHECK(gs_agreed[i].meta.tick_count == ticks);
        CHECK(gs_net_agreed_hash(&net[i]) == agreed);
        gs_replay_set_agreed(&gs_agreed[i], agreed);
    }

    // A claim that asserts no time at all. **Deliberately**: it means every
    // check that existed before this one passes whatever the log says, so the
    // only thing that can catch a doctored log is the ending it has to produce.
    // A claim with times in it would be caught by the lap check as well, and
    // then the test would not be testing this rule.
    gs_claim claim = { 0 };
    claim.track = gs_track_hash(&t);
    claim.conditions = gs_conditions_hash(gs_net_confirmed(&net[0]));
    claim.laps = gs_agreed[0].meta.laps_to_win;
    claim.car = 0;

    for (int i = 0; i < 4; i++) {
        claim.car = (uint8_t)i;
        CHECK(gs_verify(&gs_agreed[i], &t, &claim, nullptr) == GS_VERDICT_OK);
    }

    // --- One bit, somewhere in the middle, in somebody else's car.
    claim.car = 0;
    static gs_replay doctored;
    doctored = gs_agreed[0];
    uint32_t at = ticks / 2u;
    doctored.input[at][2] = (gs_input)(doctored.input[at][2] ^ GS_IN_LEFT);

    CHECK(gs_verify(&doctored, &t, &claim, nullptr) == GS_VERDICT_NOT_THAT_RACE);

    // **And the same doctored log, with no agreed ending on it, passes.** That
    // is not a bug being demonstrated - it is the size of the hole this closes.
    // Every check that came before is about one car and one lap, and a log
    // altered anywhere else walked straight past all of them.
    doctored.meta.agreed_hash = 0;
    CHECK(gs_verify(&doctored, &t, &claim, nullptr) == GS_VERDICT_OK);

    // A recording that does not say is not failed for it, which is what lets a
    // race run on one machine - and every recording made before version five -
    // still be verified for the lap it claims.
    static gs_replay silent;
    silent = gs_agreed[0];
    silent.meta.agreed_hash = 0;
    CHECK(gs_verify(&silent, &t, &claim, nullptr) == GS_VERDICT_OK);

    // **The rule, stated exactly.** A log is refused when it re-races to
    // somewhere other than the agreed ending - not merely when its bytes have
    // been changed. Those are different claims, and the sweep below is what
    // keeps them apart.
    //
    // Not every changed byte is a changed race: by five hundred ticks in, some
    // cars are wrecked, and a wrecked car is not taking input. Flipping the
    // accelerator for one of those alters the log and alters nothing that
    // happened, and answering "verified" is right. A test asserting every flip
    // is caught would be asserting something untrue and would have to be
    // loosened until it stopped saying anything.
    static const uint32_t where[] = { 0, 1, 37, 200, 500 };
    int moved = 0, checked = 0;
    for (size_t k = 0; k < sizeof where / sizeof where[0]; k++) {
        if (where[k] >= ticks) continue;
        for (uint8_t car = 0; car < 4; car++) {
            doctored = gs_agreed[0];
            doctored.input[where[k]][car] =
                (gs_input)(doctored.input[where[k]][car] ^ GS_IN_ACCEL);

            gs_world after;
            CHECK(gs_replay_playback(&doctored, &t, &after));
            bool different = gs_world_hash(&after) != agreed;

            doctored.meta.agreed_hash = agreed;
            gs_verdict v = gs_verify(&doctored, &t, &claim, nullptr);
            CHECK(v == (different ? GS_VERDICT_NOT_THAT_RACE : GS_VERDICT_OK));

            checked++;
            if (different) moved++;
        }
    }

    // And the sweep has teeth: most of those flips really did land the race
    // somewhere else, so the line above is doing work rather than agreeing with
    // itself about nothing.
    CHECK(checked >= 16);
    CHECK(moved > checked / 2);
}

TEST(a_recording_carries_its_agreed_ending_across_a_serialise) {
    // The ending has to survive the wire, and a reader of an older recording
    // has to come back with "does not say" rather than a number it invented.
    static gs_replay r;
    static gs_track t;
    gs_world w;
    gs_net_scene(&t, &w);
    gs_replay_begin(&r, &w, &t);
    for (uint32_t i = 0; i < 40; i++) {
        gs_input in[GS_MAX_CARS];
        for (uint8_t c = 0; c < GS_MAX_CARS; c++) in[c] = gs_net_drive(c, i);
        CHECK(gs_replay_record(&r, in));
    }
    gs_replay_set_agreed(&r, 0xfeedfacecafebeefull);

    static uint8_t bytes[sizeof(gs_replay) + 4096];
    size_t n = gs_replay_serialize(&r, bytes, sizeof bytes);
    CHECK(n > 0);

    static gs_replay back;
    CHECK(gs_replay_deserialize(&back, bytes, n));
    CHECK(back.meta.agreed_hash == 0xfeedfacecafebeefull);
    CHECK(back.meta.tick_count == 40);

    // A version four recording says nothing about an ending, and zero is the
    // honest answer for it - not a hash invented by the reader.
    bytes[4] = 4;                     // the version word, little-endian
    static gs_replay older;
    CHECK(gs_replay_deserialize(&older, bytes, n));
    CHECK(older.meta.agreed_hash == 0);
    CHECK(older.meta.tick_count == 40);
}

TEST(a_machine_that_goes_quiet_stalls_the_race_rather_than_desyncing) {
    static gs_net a;
    static gs_track t;
    gs_world w;
    gs_net_scene(&t, &w);
    gs_net_begin(&a, &w, 2, 0, gs_test_secret(0));

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

    gs_net_begin(&a, &w, 2, 0, gs_test_secret(0));

    // The other machine starts from a world that is one nudge different. Every
    // input will agree and every state will not, which is exactly the shape of
    // a real desync: nothing complains, and the two people are watching
    // different races.
    gs_world nudged = w;
    nudged.car[1].x += GS_RATIO(1, 64);
    gs_net_begin(&b, &nudged, 2, 1, gs_test_secret(1));

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

// ---------------------------------------------------------------------------
// Finishing a race
// ---------------------------------------------------------------------------

TEST(a_race_that_is_over_stays_over_until_a_new_one_replaces_it) {
    // **A finished world outlives the screen showing it.** The front end used
    // to clear its "the results have been worked out" flag on the way back to
    // the lobby - but the world it belongs to is the finished one, still loaded
    // until a new race replaces it. So the next frame found the race over and
    // the flag clear, ran the whole end-of-race path a second time, submitted
    // the same result again and put the screen back on the results. Somebody
    // who left the results for the tracks screen, chose a track and pressed
    // race was thrown straight back to the results they had just left.
    //
    // The simulation's half of that promise is what is pinned here: once a race
    // is over it stays over, however long it is stepped, so anything asking
    // "has this finished" keeps getting the same answer until the world is
    // replaced.
    static gs_track t;
    gs_track_init(&t, 40, 16, GS_SURF_PAVEMENT);
    gs_track_add_gate(&t, GS_INT(6), GS_INT(8), 0, GS_INT(6));
    gs_track_add_gate(&t, GS_INT(30), GS_INT(8), 0, GS_INT(6));

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_set_mode(&w, GS_MODE_RACE);
    gs_world_set_laps(&w, 1);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_SPRINT_CAR, GS_INT(4), GS_INT(8), 0);

    for (uint32_t i = 0; i < (uint32_t)GS_TICK_HZ * 120u && !w.over; i++) {
        gs_input in[GS_MAX_CARS] = { gs_ai_drive(&w, &t, 0), 0, 0, 0 };
        gs_world_step(&w, &t, in);
    }
    CHECK(w.over);

    uint32_t finished_at = w.car[0].finish_tick;
    uint8_t won = w.winner;
    CHECK(finished_at != 0);

    // Kept stepping, as a front end that has not started a new race does.
    for (uint32_t i = 0; i < (uint32_t)GS_TICK_HZ * 10u; i++) {
        gs_input in[GS_MAX_CARS] = { gs_ai_drive(&w, &t, 0), 0, 0, 0 };
        gs_world_step(&w, &t, in);
    }

    // Still over, still the same result: a car that has finished is timed once
    // and never again, so there is nothing here that would make a second
    // submission look like a different race.
    CHECK(w.over);
    CHECK(w.winner == won);
    CHECK(w.car[0].finish_tick == finished_at);

    // And a new race is what clears it, which is the only moment it is honestly
    // clear: when there is one for it to describe.
    gs_world_init(&w, GS_ONE);
    CHECK(!w.over);
    CHECK(w.winner == GS_NO_WINNER);
}

TEST(a_race_ends_when_everybody_has_finished_and_the_first_one_wins) {
    static gs_track t;
    gs_track_init(&t, 40, 16, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= t.h; y++)
        for (uint8_t x = 0; x <= t.w; x++) gs_track_set_corner(&t, x, y, 0);

    // Two gates, so a lap is out and back - and a track that is *lapped* is a
    // circuit, which is what says gate zero is where a lap begins and ends.
    // Left as a path this would be finished by arriving at the far gate once,
    // because that is what a path is.
    gs_track_add_gate(&t, GS_INT(6), GS_INT(8), 0, GS_INT(6));
    gs_track_add_gate(&t, GS_INT(30), GS_INT(8), 0, GS_INT(6));
    t.route = (uint8_t)GS_ROUTE_CIRCUIT;

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_set_mode(&w, GS_MODE_RACE);
    gs_world_set_laps(&w, 2);

    // A quick car and a slow one, so the order is decided by the racing rather
    // than by which index went first.
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_SPRINT_CAR, GS_INT(4), GS_INT(7), 0);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_LUNAR_ROVER, GS_INT(4), GS_INT(9), 0);

    CHECK(!w.over);
    CHECK(w.winner == GS_NO_WINNER);

    for (uint32_t i = 0; i < GS_TICK_HZ * 120 && !w.over; i++) {
        gs_input in[GS_MAX_CARS] = { 0 };
        for (uint8_t c = 0; c < w.car_count; c++) in[c] = gs_ai_drive(&w, &t, c);
        gs_world_step(&w, &t, in);
    }

    CHECK(w.over);
    CHECK(w.winner == 0);                      // the sprint car

    // Everybody has a time, and the winner's is the smallest of them.
    for (uint8_t i = 0; i < w.car_count; i++) {
        CHECK(w.car[i].finish_tick > 0);
        CHECK(w.car[i].laps >= 2);
    }
    CHECK(w.car[0].finish_tick < w.car[1].finish_tick);

    // **The clock does not stop on the winner.** A race that ended the moment
    // the first car crossed would have no time for anybody else, and the times
    // are the thing people argue about afterwards.
    CHECK(w.car[1].finish_tick > 0);

    // And a finished car is timed once. Its time does not creep as it drives on
    // through the silence afterwards.
    uint32_t settled = w.car[0].finish_tick;
    for (uint32_t i = 0; i < GS_TICK_HZ * 5; i++) {
        gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, GS_IN_ACCEL, 0, 0 };
        gs_world_step(&w, &t, in);
    }
    CHECK(w.car[0].finish_tick == settled);
    CHECK(w.winner == 0);
}

TEST(every_other_dial_on_the_setup_screen_reaches_the_race) {
    // **The rest of what the setup screen chooses**, walked in full rather than
    // at whatever it happened to be left on: every mode, every player count the
    // grid has room for, and every vehicle in the parts list - against a race
    // actually built from them.
    int checked = 0;

    static gs_track t;
    gs_track_init(&t, 40, 24, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= t.h; y++) {
        for (uint8_t x = 0; x <= t.w; x++) gs_track_set_corner(&t, x, y, 0);
    }
    gs_track_add_gate(&t, GS_INT(8), GS_INT(12), 0, GS_INT(8));
    gs_track_add_gate(&t, GS_INT(30), GS_INT(12), 32768, GS_INT(8));

    // **Every mode.** A race is won by arriving first and a demolition by being
    // the last one moving, and which of those it is has to survive being set.
    for (int mode = 0; mode < 2; mode++) {
        gs_world w;
        gs_world_init(&w, GS_ONE);
        gs_world_set_mode(&w, mode == 0 ? GS_MODE_RACE : GS_MODE_DESTRUCTION);
        CHECK(w.mode == (uint8_t)(mode == 0 ? GS_MODE_RACE : GS_MODE_DESTRUCTION));
        CHECK(!w.over);
        CHECK(w.winner == GS_NO_WINNER);
        checked++;
    }

    // **Every player count the grid has room for**, each car on its own slot
    // and no two in the same place - four cars stacked on one square is a
    // four-car pile-up before the lights go out.
    for (uint8_t players = 1; players <= GS_MAX_CARS; players++) {
        gs_world w;
        gs_world_init(&w, GS_ONE);
        gs_world_set_mode(&w, GS_MODE_RACE);

        for (uint8_t i = 0; i < players; i++) {
            gs_fix x, y;
            gs_angle heading;
            gs_track_grid(&t, i, &x, &y, &heading);
            gs_world_add_car(&w, &t, (uint8_t)GS_VEH_SPRINT_CAR, x, y, heading);
        }
        CHECK(w.car_count == players);

        for (uint8_t a = 0; a < players; a++) {
            CHECK(w.car[a].active);
            for (uint8_t b = (uint8_t)(a + 1u); b < players; b++) {
                CHECK(w.car[a].x != w.car[b].x || w.car[a].y != w.car[b].y);
            }
        }
        checked++;
    }

    // **Every vehicle**, and the car that arrives is the one that was chosen -
    // with that vehicle's numbers, not the first entry's.
    for (uint8_t v = 0; v < GS_VEH_COUNT; v++) {
        gs_world w;
        gs_world_init(&w, GS_ONE);
        gs_world_set_mode(&w, GS_MODE_RACE);
        gs_world_add_car(&w, &t, v, GS_INT(8), GS_INT(12), 0);

        CHECK(w.car_count == 1);
        CHECK(w.car[0].vehicle == v);
        CHECK(w.car[0].active);
        CHECK(!w.car[0].wrecked);
        CHECK(w.car[0].damage == 0);
        CHECK(gs_vehicles[v].name != nullptr);
        CHECK(gs_vehicles[v].power > 0);
        CHECK(gs_vehicles[v].top > 0);

        // It drives, under its own power, in the direction it is pointed.
        for (uint32_t i = 0; i < GS_TICK_HZ * 3u; i++) {
            gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, 0, 0, 0 };
            gs_world_step(&w, &t, in);
        }
        CHECK(w.car[0].x > GS_INT(8));
        checked++;
    }

    printf("  DIALS %d setup values checked\n", checked);
    CHECK(checked == 2 + GS_MAX_CARS + GS_VEH_COUNT);
}

TEST(every_lap_count_the_dial_offers_is_the_race_that_is_run) {
    // **One to twenty, which is the whole of the lap dial**, and until now
    // exactly one value of it had ever been raced. The rule is easy to say and
    // easy to get wrong by one: a car starts *behind* the line and crosses it
    // on the way out, so that first crossing is the run up to the line and not
    // a lap anybody drove. Getting it wrong ends a three-lap race after two.
    //
    // **Driven on a constant lock rather than by the racing AI.** What is under
    // test is the lap rule, and the AI is not part of it - on a bare rectangle
    // it laps twice, wanders into the run-off and sits there, which reads
    // exactly like a lap counter stuck at ten. A car held on full lock drives a
    // circle and will do it all day, which is all this needs. The throttle is
    // held to a speed rather than pinned down, because a car that keeps
    // accelerating turns wider every lap and spirals off the edge of the world.
    for (uint16_t want = 1; want <= 20; want++) {
        static gs_track t;
        gs_track_init(&t, 64, 64, GS_SURF_PAVEMENT);
        for (uint8_t y = 0; y <= t.h; y++) {
            for (uint8_t x = 0; x <= t.w; x++) gs_track_set_corner(&t, x, y, 0);
        }

        // Two gates on the circle the car will drive, wide enough that it goes
        // through them rather than past them.
        gs_track_add_gate(&t, GS_INT(32), GS_INT(32), 0, GS_INT(4));
        // **Facing the way the car will be going when it gets there.** Half a
        // circle later it is travelling in the opposite direction, and a gate
        // is a line with a side to it - pointed the same way as the first, the
        // car sails past the back of it and the lap never completes.
        gs_track_add_gate(&t, GS_INT(22), GS_INT(46), 32768, GS_INT(4));
        t.route = (uint8_t)GS_ROUTE_CIRCUIT;

        gs_world w;
        gs_world_init(&w, GS_ONE);
        gs_world_set_mode(&w, GS_MODE_RACE);
        gs_world_set_laps(&w, want);
        CHECK(gs_world_laps_needed(&w, &t) == want);

        gs_world_add_car(&w, &t, (uint8_t)GS_VEH_SPRINT_CAR,
                         GS_INT(32), GS_INT(32), 0);

        // A lap of this circle is about seventeen seconds, so twenty of them is
        // under six minutes; this allows fifteen and expects to stop long
        // before it.
        for (uint32_t i = 0; i < GS_TICK_HZ * 60u * 15u && !w.over; i++) {
            const gs_fix vx = w.car[0].vx;
            const gs_fix vy = w.car[0].vy;
            const gs_fix speed_sq = gs_fix_mul(vx, vx) + gs_fix_mul(vy, vy);

            gs_input in[GS_MAX_CARS] = { 0 };
            in[0] = (gs_input)((speed_sq < GS_INT(16) ? (unsigned)GS_IN_ACCEL : 0u) |
                               (unsigned)GS_IN_RIGHT);
            gs_world_step(&w, &t, in);
        }

        CHECK(w.over);
        CHECK(w.winner == 0);
        CHECK(w.car[0].finish_tick > 0);

        // **Finished on the lap it was asked for, not near it.** This is the
        // check that fails if the run-up crossing is ever counted as a lap.
        CHECK(gs_car_laps_done(&t, &w.car[0]) == want);

        // And the crossings are one ahead of the laps, for the same reason.
        CHECK(w.car[0].laps == (uint16_t)(want + 1u));
    }
}

TEST(a_race_with_no_lap_target_never_ends) {
    static gs_track t;
    gs_track_init(&t, 40, 16, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= t.h; y++)
        for (uint8_t x = 0; x <= t.w; x++) gs_track_set_corner(&t, x, y, 0);
    gs_track_add_gate(&t, GS_INT(6), GS_INT(8), 0, GS_INT(6));
    gs_track_add_gate(&t, GS_INT(30), GS_INT(8), 0, GS_INT(6));

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_set_mode(&w, GS_MODE_RACE);
    // No target set at all, which is what a test drive from the editor is.

    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_SPRINT_CAR, GS_INT(4), GS_INT(8), 0);

    for (uint32_t i = 0; i < GS_TICK_HZ * 90; i++) {
        gs_input in[GS_MAX_CARS] = { 0 };
        in[0] = gs_ai_drive(&w, &t, 0);
        gs_world_step(&w, &t, in);
    }

    // Laps counted, nobody finished, nothing over. Driving around is not a
    // race until somebody says how long it is.
    CHECK(w.car[0].laps > 1);
    CHECK(w.car[0].finish_tick == 0);
    CHECK(!w.over);
    CHECK(w.winner == GS_NO_WINNER);
}

TEST(a_replay_carries_what_race_it_was) {
    static gs_track t;
    gs_track_init(&t, 40, 16, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= t.h; y++)
        for (uint8_t x = 0; x <= t.w; x++) gs_track_set_corner(&t, x, y, 0);
    gs_track_add_gate(&t, GS_INT(6), GS_INT(8), 0, GS_INT(6));
    gs_track_add_gate(&t, GS_INT(30), GS_INT(8), 0, GS_INT(6));

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_set_mode(&w, GS_MODE_RACE);
    gs_world_set_laps(&w, 3);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_SPRINT_CAR, GS_INT(4), GS_INT(8), 0);

    static gs_replay rec;
    gs_replay_begin(&rec, &w, &t);
    for (uint32_t i = 0; i < GS_TICK_HZ * 60; i++) {
        gs_input in[GS_MAX_CARS] = { 0 };
        in[0] = gs_ai_drive(&w, &t, 0);
        gs_replay_record(&rec, in);
        gs_world_step(&w, &t, in);
    }

    // Through the bytes, because that is the only form a shared race arrives
    // in. A recording that did not carry the lap target would re-race the same
    // driving under different rules and finish somewhere else.
    static uint8_t buf[sizeof(gs_replay) + 4096];
    size_t n = gs_replay_serialize(&rec, buf, sizeof buf);
    CHECK(n > 0);

    static gs_replay back;
    CHECK(gs_replay_deserialize(&back, buf, n));
    CHECK(back.meta.laps_to_win == 3);
    CHECK(back.meta.mode == (uint8_t)GS_MODE_RACE);

    gs_world played;
    CHECK(gs_replay_playback(&back, &t, &played));
    CHECK(gs_world_hash(&played) == gs_world_hash(&w));
    CHECK(played.car[0].finish_tick == w.car[0].finish_tick);
}

// ---------------------------------------------------------------------------
// Records and the people who set them
// ---------------------------------------------------------------------------

static gs_records gs_rec;

// A world under the standard dials, for hashing conditions from.
static void gs_conditions(gs_world *w, gs_fix gravity) {
    gs_world_init(w, GS_ONE);
    w->gravity = gravity;
}

TEST(a_record_belongs_to_a_track_and_the_conditions_it_was_set_under) {
    gs_records_clear(&gs_rec);

    gs_world earth, moon;
    gs_conditions(&earth, GS_ONE);
    gs_conditions(&moon, GS_RATIO(17, 100));

    uint64_t e = gs_conditions_hash(&earth);
    uint64_t m = gs_conditions_hash(&moon);
    CHECK(e != m);

    const uint64_t track = 0xabcdef0123456789ULL;

    // Nothing has been done here yet.
    CHECK(gs_records_best_lap(&gs_rec, track, e) == nullptr);

    gs_record_beat first = gs_records_submit(&gs_rec, track, e,
                                             (uint8_t)GS_VEH_STOCK_CAR,
                                             (uint8_t)GS_MODE_RACE, 3,
                                             5040, 15900, "gavin", 1700000000ull);
    CHECK(first.lap);      // the first time round is a record by definition
    CHECK(first.race);

    const gs_record *best = gs_records_best_lap(&gs_rec, track, e);
    CHECK(best != nullptr);
    if (best != nullptr) {
        CHECK(best->lap == 5040);
        CHECK(strcmp(best->who, "gavin") == 0);
    }

    // **A lap at a sixth of gravity is not a lap.** The same track under
    // different dials is a different table, or every record would eventually be
    // set on the moon.
    CHECK(gs_records_best_lap(&gs_rec, track, m) == nullptr);

    // And a different track, one bit apart, is a different table too - which is
    // the whole reason the key is the content hash.
    CHECK(gs_records_best_lap(&gs_rec, track ^ 1u, e) == nullptr);
}

TEST(beating_a_record_is_reported_and_not_beating_one_is_not) {
    gs_records_clear(&gs_rec);
    gs_world w;
    gs_conditions(&w, GS_ONE);
    uint64_t c = gs_conditions_hash(&w);
    const uint64_t track = 0x1111ULL;

    gs_records_submit(&gs_rec, track, c, 0, (uint8_t)GS_MODE_RACE, 3, 5000, 16000, "ann", 1700000000ull);

    // Slower. Nothing is beaten, and the table still says what it said.
    gs_record_beat slower = gs_records_submit(&gs_rec, track, c, 0,
                                              (uint8_t)GS_MODE_RACE, 3,
                                              5200, 16400, "bob", 1700000000ull);
    CHECK(!slower.lap);
    CHECK(!slower.race);
    CHECK(gs_records_best_lap(&gs_rec, track, c)->lap == 5000);

    // Quicker on the lap but not over the race, which happens constantly: one
    // brilliant lap and three ordinary ones.
    gs_record_beat mixed = gs_records_submit(&gs_rec, track, c, 0,
                                             (uint8_t)GS_MODE_RACE, 3,
                                             4800, 16900, "bob", 1700000000ull);
    CHECK(mixed.lap);
    CHECK(!mixed.race);
    CHECK(gs_records_best_lap(&gs_rec, track, c)->lap == 4800);
    CHECK(gs_records_best_race(&gs_rec, track, c, 3)->race == 16000);

    // One row per person, not one per race: bob has been round twice and
    // appears once, holding his best rather than his latest.
    CHECK(gs_rec.count == 2);
    int bob = -1;
    for (uint16_t i = 0; i < gs_rec.count; i++) {
        if (strcmp(gs_rec.entry[i].who, "bob") == 0) bob = (int)i;
    }
    CHECK(bob >= 0);
    if (bob >= 0) CHECK(gs_rec.entry[bob].lap == 4800);

    // A race time only means anything against a race of the same length, so a
    // five-lap time does not take a three-lap record.
    gs_record_beat longer = gs_records_submit(&gs_rec, track, c, 0,
                                              (uint8_t)GS_MODE_RACE, 5,
                                              4900, 26000, "ann", 1700000000ull);
    CHECK(!longer.lap);
    CHECK(longer.race);         // the first five-lap race, so a record for that
    CHECK(gs_records_best_race(&gs_rec, track, c, 3)->race == 16000);
}

TEST(records_survive_being_written_and_read_back) {
    gs_records_clear(&gs_rec);
    gs_world w;
    gs_conditions(&w, GS_ONE);
    uint64_t c = gs_conditions_hash(&w);

    for (int i = 0; i < 40; i++) {
        char who[GS_NAME_MAX];
        snprintf(who, sizeof who, "driver%d", i);
        gs_records_submit(&gs_rec, 0x2000ULL + (uint64_t)(unsigned)(i % 7), c,
                          (uint8_t)(i % GS_VEH_COUNT), (uint8_t)GS_MODE_RACE, 3,
                          (uint32_t)(4000 + i * 13), (uint32_t)(14000 + i * 40), who,
                          1700000000ull);
    }
    uint16_t before = gs_rec.count;
    CHECK(before == 40);

    static uint8_t buf[sizeof(gs_records) + 4096];
    size_t n = gs_records_serialize(&gs_rec, buf, sizeof buf);
    CHECK(n > 0);

    static gs_records back;
    CHECK(gs_records_deserialize(&back, buf, n));
    CHECK(back.count == before);
    CHECK(memcmp(gs_rec.entry, back.entry, (size_t)before * sizeof(gs_record)) == 0);

    // Rubbish is refused rather than half-read.
    CHECK(!gs_records_deserialize(&back, buf, 4));
    buf[0] ^= 0xffu;
    CHECK(!gs_records_deserialize(&back, buf, n));
}

TEST(a_profile_is_a_person_rather_than_a_settings_entry) {
    static gs_profiles p;
    gs_profiles_clear(&p);

    CHECK(gs_profile_add(&p, "gavin", GS_COLOUR_ORANGE, (uint8_t)GS_VEH_BAJA_BUG) == 0);
    CHECK(gs_profile_add(&p, "ann", GS_COLOUR_PURPLE, (uint8_t)GS_VEH_MOTORCYCLE) == 1);

    // Two people called the same thing would be two people sharing a record.
    CHECK(gs_profile_add(&p, "gavin", GS_COLOUR_WHITE, 0) == -1);
    CHECK(gs_profile_add(&p, "", GS_COLOUR_WHITE, 0) == -1);
    CHECK(p.count == 2);

    CHECK(gs_profile_find(&p, "ann") == 1);
    CHECK(gs_profile_find(&p, "nobody") == -1);
    CHECK(p.entry[0].colour == GS_COLOUR_ORANGE);
    CHECK(p.entry[0].vehicle == (uint8_t)GS_VEH_BAJA_BUG);

    // A history, which is what makes it a person.
    gs_profile_raced(&p, 0, true, true, false, 420, 1700000000ull);
    gs_profile_raced(&p, 0, false, true, true, 380, 1700000001ull);
    CHECK(p.entry[0].races == 2);
    CHECK(p.entry[0].wins == 1);
    CHECK(p.entry[0].podiums == 2);
    CHECK(p.entry[0].wrecks == 1);
    CHECK(p.entry[0].tiles == 800);

    static uint8_t buf[sizeof(gs_profiles) + 1024];
    size_t n = gs_profiles_serialize(&p, buf, sizeof buf);
    CHECK(n > 0);

    static gs_profiles back;
    CHECK(gs_profiles_deserialize(&back, buf, n));
    CHECK(back.count == 2);
    CHECK(strcmp(back.entry[0].name, "gavin") == 0);
    CHECK(back.entry[0].wins == 1);
    CHECK(back.entry[0].tiles == 800);
    CHECK(back.entry[1].colour == GS_COLOUR_PURPLE);

    // Removing somebody keeps the rest in order - and says nothing about their
    // records, which belong to the track rather than to the roster.
    CHECK(gs_profile_remove(&p, 0));
    CHECK(p.count == 1);
    CHECK(strcmp(p.entry[0].name, "ann") == 0);
    CHECK(!gs_profile_remove(&p, 7));
}

TEST(a_roster_written_before_passwords_existed_still_loads) {
    // **Built by hand rather than by an older binary**, because the point is
    // the bytes: a version-two row is 50 of them and ends at the date. If the
    // reader ever gets that length wrong it will read the next row's name as
    // this row's lock, and a test that generated the file with today's writer
    // could not tell, having got the length wrong in both directions at once.
    enum { V2_ROW = GS_PROFILE_NAME + 1 + 1 + 4 + 4 + 4 + 4 + 8 + 8 };
    static uint8_t buf[12 + V2_ROW * 2];
    memset(buf, 0, sizeof buf);

    uint8_t *q = buf;
    gs_test_put32(q, 0x50525347u); q += 4;      // magic
    gs_test_put32(q, 2u);          q += 4;      // the version before the lock
    gs_test_put32(q, 2u);          q += 4;      // two people

    const char *names[2] = { "ada", "bez" };
    for (int i = 0; i < 2; i++) {
        memcpy(q, names[i], strlen(names[i])); q += GS_PROFILE_NAME;
        *q++ = (uint8_t)GS_COLOUR_GREEN;
        *q++ = (uint8_t)GS_VEH_DUNE_BUGGY;
        gs_test_put32(q, 7u);  q += 4;          // races
        gs_test_put32(q, 3u);  q += 4;          // wins
        gs_test_put32(q, 5u);  q += 4;          // podiums
        gs_test_put32(q, 1u);  q += 4;          // wrecks
        gs_test_put64(q, 1234u); q += 8;        // tiles
        gs_test_put64(q, 1700000000ull); q += 8;// last raced
    }
    CHECK((size_t)(q - buf) == sizeof buf);

    static gs_profiles old;
    CHECK(gs_profiles_deserialize(&old, buf, sizeof buf));
    CHECK(old.count == 2);
    CHECK(strcmp(old.entry[0].name, "ada") == 0);
    CHECK(strcmp(old.entry[1].name, "bez") == 0);
    CHECK(old.entry[1].wins == 3);
    CHECK(old.entry[1].tiles == 1234);

    // **Unlocked, and that is the truth about the file rather than a default.**
    // Somebody who has been playing since before there were passwords is not
    // locked out by the upgrade.
    CHECK(old.entry[0].password[0] == '\0');
    CHECK(old.entry[0].totp_len == 0);
    CHECK(old.entry[1].password[0] == '\0');
}

TEST(a_lock_survives_being_written_out_and_read_back) {
    static gs_profiles p;
    gs_profiles_clear(&p);
    CHECK(gs_profile_add(&p, "gavin", GS_COLOUR_RED, (uint8_t)GS_VEH_BAJA_BUG) == 0);
    CHECK(gs_profile_add(&p, "ada", GS_COLOUR_BLUE, (uint8_t)GS_VEH_SPRINT_CAR) == 1);

    // A profile starts unlocked, which is a supported state: one person on one
    // machine should not have to type anything to play.
    CHECK(p.entry[0].password[0] == '\0');

    // Not a real Argon2id hash - core cannot make one, it links nothing. What
    // is pinned here is that the field survives the round trip whole, which is
    // the only part of this core is responsible for. That the string verifies
    // is gs_auth's business and is tested where sodium is linked.
    static const char stand_in[] =
        "$argon2id$v=19$m=65536,t=2,p=1$abcdefghijklmnop$"
        "0123456789012345678901234567890123456789012";
    memcpy(p.entry[0].password, stand_in, sizeof stand_in);
    for (uint8_t i = 0; i < GS_PROFILE_TOTP; i++) p.entry[0].totp[i] = (uint8_t)(i + 1);
    p.entry[0].totp_len = GS_PROFILE_TOTP;

    static uint8_t buf[sizeof(gs_profiles) + 1024];
    size_t n = gs_profiles_serialize(&p, buf, sizeof buf);
    CHECK(n > 0);

    static gs_profiles back;
    CHECK(gs_profiles_deserialize(&back, buf, n));
    CHECK(back.count == 2);
    CHECK(strcmp(back.entry[0].password, stand_in) == 0);
    CHECK(back.entry[0].totp_len == GS_PROFILE_TOTP);
    for (uint8_t i = 0; i < GS_PROFILE_TOTP; i++)
        CHECK(back.entry[0].totp[i] == (uint8_t)(i + 1));

    // The second profile is still unlocked, so one person setting a password
    // does not quietly lock everybody on the machine.
    CHECK(back.entry[1].password[0] == '\0');
    CHECK(back.entry[1].totp_len == 0);
    CHECK(strcmp(back.entry[1].name, "ada") == 0);
}

TEST(the_race_that_sets_a_record_is_the_race_that_reports_it) {
    // End to end: a real race, its times taken from the simulation, submitted,
    // and a second slower race that does not beat it. This is the join between
    // the two halves - the simulation times laps and the table keeps them - and
    // it is the join that a test of either half alone would miss.
    static gs_track t;
    gs_track_init(&t, 40, 16, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= t.h; y++)
        for (uint8_t x = 0; x <= t.w; x++) gs_track_set_corner(&t, x, y, 0);
    gs_track_add_gate(&t, GS_INT(6), GS_INT(8), 0, GS_INT(6));
    gs_track_add_gate(&t, GS_INT(30), GS_INT(8), 0, GS_INT(6));

    gs_records_clear(&gs_rec);

    // Two real races, and **which of them is quicker is discovered rather than
    // assumed**. The first version raced a sprint car and then a lunar rover and
    // took it as read that the rover was slower, which the roster sweep says of
    // the machines and which stopped being true of these two *laps* the moment
    // leaving the track cost something: the quick car runs wider, and eight
    // tiles into a sand trap is worth more than the difference between them.
    //
    // That is the game working. It is not what this test is about - the join
    // between a race that times itself and a table that keeps the time - so the
    // fact pinned here is the one that is actually claimed: a better time beats
    // the record and a worse one does not.
    uint32_t times[2] = { 0, 0 };
    uint32_t races[2] = { 0, 0 };
    uint8_t  machine[2] = { (uint8_t)GS_VEH_SPRINT_CAR, (uint8_t)GS_VEH_LUNAR_ROVER };
    uint64_t conditions = 0;

    for (int run = 0; run < 2; run++) {
        gs_world w;
        gs_world_init(&w, GS_ONE);
        gs_world_set_mode(&w, GS_MODE_RACE);
        gs_world_set_laps(&w, 3);
        gs_world_add_car(&w, &t, machine[run], GS_INT(4), GS_INT(8), 0);

        for (uint32_t i = 0; i < GS_TICK_HZ * 200 && !w.over; i++) {
            gs_input in[GS_MAX_CARS] = { 0 };
            in[0] = gs_ai_drive(&w, &t, 0);
            gs_world_step(&w, &t, in);
        }
        CHECK(w.over);
        CHECK(w.car[0].best_lap > 0);
        times[run] = w.car[0].best_lap;
        races[run] = w.car[0].finish_tick;
        conditions = gs_conditions_hash(&w);
    }

    // The quicker of the two goes in first, so the second submission is the one
    // that has to be refused.
    int quick = times[0] <= times[1] ? 0 : 1;
    int slow  = 1 - quick;
    CHECK(times[slow] > times[quick]);

    gs_record_beat first = gs_records_submit(
        &gs_rec, gs_track_hash(&t), conditions, machine[quick],
        (uint8_t)GS_MODE_RACE, 3, times[quick], races[quick], "quick", 1700000000ull);
    CHECK(first.lap);
    CHECK(first.race);

    gs_record_beat second = gs_records_submit(
        &gs_rec, gs_track_hash(&t), conditions, machine[slow],
        (uint8_t)GS_MODE_RACE, 3, times[slow], races[slow], "slow", 1700000000ull);
    CHECK(!second.lap);

    const gs_record *best = gs_records_best_lap(&gs_rec, gs_track_hash(&t),
                                                0xffffffffffffffffULL);
    CHECK(best == nullptr);        // wrong conditions, no record
}

// ---------------------------------------------------------------------------
// A track, in pieces
// ---------------------------------------------------------------------------

static gs_carrier gs_carry;

static void gs_carried_track(gs_track *t) {
    gs_track_init(t, 48, 24, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= t->h; y++) {
        for (uint8_t x = 0; x <= t->w; x++) {
            gs_fix h = 0;
            if (x > 12 && x < 18) h = (gs_fix)((int64_t)GS_INT(2) * (x - 12) / 6);
            gs_track_set_corner(t, x, y, h);
        }
    }
    for (uint8_t x = 0; x < t->w; x++) {
        for (uint8_t y = 0; y < t->h; y++) {
            if (x >= 30) gs_track_set_surface(t, x, y, GS_SURF_ICE);
            if (x >= 12 && x < 18) gs_track_set_gravity(t, x, y, GS_RATIO(40, 100));
        }
    }
    gs_track_add_gate(t, GS_INT(4), GS_INT(12), 0, GS_INT(6));
    gs_track_add_gate(t, GS_INT(40), GS_INT(12), 0, GS_INT(6));
}

TEST(a_track_arrives_in_pieces_and_is_the_same_track) {
    static gs_track sent, got;
    gs_carried_track(&sent);

    static uint8_t bytes[GS_CARRIER_MAX_BYTES];
    size_t len = gs_track_serialize(&sent, bytes, sizeof bytes);
    CHECK(len > 0);

    uint64_t hash = gs_track_hash(&sent);
    uint16_t chunks = gs_carrier_chunks(len);
    CHECK(chunks > 1);          // or this is not testing reassembly at all

    gs_carrier_expect(&gs_carry, hash);
    CHECK(!gs_carrier_done(&gs_carry));

    // Deliberately out of order, because a datagram socket makes no promises
    // and a reassembler that only works in order works only on a good day.
    static const int order[] = { 3, 0, 2, 1 };
    for (size_t k = 0; k < sizeof order / sizeof order[0]; k++) {
        uint16_t c = (uint16_t)order[k];
        if (c >= chunks) continue;

        uint8_t dg[GS_PROTO_MTU];
        size_t n = gs_carrier_chunk(dg, sizeof dg, hash, bytes, len, c);
        CHECK(n > 0);
        CHECK(gs_carrier_take(&gs_carry, dg, n));
    }
    for (uint16_t c = 0; c < chunks; c++) {
        uint8_t dg[GS_PROTO_MTU];
        size_t n = gs_carrier_chunk(dg, sizeof dg, hash, bytes, len, c);
        gs_carrier_take(&gs_carry, dg, n);
    }

    CHECK(gs_carrier_done(&gs_carry));
    CHECK(gs_carrier_track(&gs_carry, &got));

    // The same track, not merely a track. Every corner, every tile, the route.
    CHECK(gs_track_hash(&got) == hash);
    CHECK(got.w == sent.w && got.h == sent.h);
    CHECK(got.gate_count == sent.gate_count);
    for (uint8_t y = 0; y <= sent.h; y++) {
        for (uint8_t x = 0; x <= sent.w; x++) {
            size_t c = (size_t)y * GS_CORNER_STRIDE + x;
            CHECK(got.corner[c] == sent.corner[c]);
        }
    }
    for (uint8_t x = 0; x < sent.w; x++) {
        for (uint8_t y = 0; y < sent.h; y++) {
            gs_fix cx = GS_INT(x) + GS_HALF, cy = GS_INT(y) + GS_HALF;
            CHECK(gs_track_surface(&got, cx, cy) == gs_track_surface(&sent, cx, cy));
            CHECK(gs_track_gravity(&got, cx, cy) == gs_track_gravity(&sent, cx, cy));
        }
    }
}

TEST(a_chunk_the_reassembler_refuses_does_not_poison_the_transfer) {
    // **Everything is checked before anything is kept.** The declared chunk
    // count used to be written down before the remaining checks had run, so a
    // datagram that was about to be refused still left its number behind - and
    // every honest chunk that followed was turned away for disagreeing with a
    // count that came from the chunk nobody accepted. One malformed datagram,
    // and that track could never be received again.
    static uint8_t bytes[4096];
    for (size_t i = 0; i < sizeof bytes; i++) bytes[i] = (uint8_t)(i * 7u);

    uint64_t hash = 0xfeed0001u;
    gs_carrier_expect(&gs_carry, hash);

    uint8_t dg[GS_PROTO_MTU];
    uint8_t payload[GS_CHUNK_BYTES];
    memcpy(payload, bytes, sizeof payload);

    // The last chunk of the largest transfer that could exist, at full length -
    // which puts its final byte past the end of the buffer. Well formed as far
    // as the protocol reader is concerned, and refused here.
    size_t n = gs_proto_track_chunk(dg, sizeof dg, hash,
                                    (uint16_t)(GS_CARRIER_MAX_CHUNKS - 1),
                                    (uint16_t)GS_CARRIER_MAX_CHUNKS, payload,
                                    (uint16_t)GS_CHUNK_BYTES);
    CHECK(n > 0);
    CHECK(!gs_carrier_take(&gs_carry, dg, n));

    // A count far past the array, which is refused for that reason alone.
    n = gs_proto_track_chunk(dg, sizeof dg, hash, 3,
                             (uint16_t)(GS_CARRIER_MAX_CHUNKS + 100), payload,
                             (uint16_t)GS_CHUNK_BYTES);
    CHECK(n > 0);
    CHECK(!gs_carrier_take(&gs_carry, dg, n));

    // And now the honest transfer of that same track, which has to work. This
    // is the assertion: a refusal left nothing behind.
    size_t len = (size_t)GS_CHUNK_BYTES * 3u + 17u;
    for (uint16_t c = 0; c < 4; c++) {
        n = gs_carrier_chunk(dg, sizeof dg, hash, bytes, len, c);
        CHECK(n > 0);
        CHECK(gs_carrier_take(&gs_carry, dg, n));
    }
    CHECK(gs_carrier_done(&gs_carry));
    CHECK(gs_carry.len == len);
    CHECK(memcmp(gs_carry.bytes, bytes, len) == 0);
}

TEST(the_chunk_reader_refuses_a_datagram_that_does_not_add_up) {
    // **Where the reassembler's safety actually comes from today.** It indexes
    // an array by a number off the network, and what keeps that number in range
    // is this reader refusing a chunk that is not below its own count. The
    // reassembler bounds the index itself as well, but that line is unreachable
    // while these hold - so these are the ones worth pinning, because if one of
    // them went the belt would be doing the work alone and nobody would know.
    uint8_t dg[GS_PROTO_MTU];
    uint8_t payload[GS_CHUNK_BYTES];
    memset(payload, 0xab, sizeof payload);

    uint64_t hash = 0;
    uint16_t chunk = 0, chunks = 0, data_len = 0;
    const uint8_t *data = nullptr;

    // The well-formed one, so the refusals below mean something.
    size_t n = gs_proto_track_chunk(dg, sizeof dg, 0x99u, 2, 5, payload, 64);
    CHECK(n > 0);
    CHECK(gs_proto_read_track_chunk(dg, n, &hash, &chunk, &chunks, &data, &data_len));
    CHECK(hash == 0x99u && chunk == 2 && chunks == 5 && data_len == 64);

    // A chunk number that is not below the count. This is the one the
    // reassembler leans on.
    n = gs_proto_track_chunk(dg, sizeof dg, 0x99u, 5, 5, payload, 64);
    CHECK(n > 0);
    CHECK(!gs_proto_read_track_chunk(dg, n, &hash, &chunk, &chunks, &data, &data_len));

    n = gs_proto_track_chunk(dg, sizeof dg, 0x99u, 60000, 5, payload, 64);
    CHECK(n > 0);
    CHECK(!gs_proto_read_track_chunk(dg, n, &hash, &chunk, &chunks, &data, &data_len));

    // A transfer of no chunks at all.
    n = gs_proto_track_chunk(dg, sizeof dg, 0x99u, 0, 0, payload, 64);
    CHECK(n > 0);
    CHECK(!gs_proto_read_track_chunk(dg, n, &hash, &chunk, &chunks, &data, &data_len));

    // A length longer than the datagram holding it - believing that is how a
    // reader walks off the end of somebody else's packet.
    n = gs_proto_track_chunk(dg, sizeof dg, 0x99u, 0, 5, payload, 64);
    CHECK(n > 0);
    CHECK(!gs_proto_read_track_chunk(dg, n - 32, &hash, &chunk, &chunks, &data, &data_len));

    // A truncated datagram, one byte at a time. None of them is a chunk and
    // none of them may be read as one.
    n = gs_proto_track_chunk(dg, sizeof dg, 0x99u, 1, 5, payload, 200);
    CHECK(n > 0);
    for (size_t cut = 0; cut < n; cut++) {
        CHECK(!gs_proto_read_track_chunk(dg, cut, &hash, &chunk, &chunks,
                                         &data, &data_len));
    }
}
TEST(a_track_that_arrives_damaged_is_refused_rather_than_raced) {
    static gs_track sent, got;
    gs_carried_track(&sent);

    static uint8_t bytes[GS_CARRIER_MAX_BYTES];
    size_t len = gs_track_serialize(&sent, bytes, sizeof bytes);
    uint64_t hash = gs_track_hash(&sent);
    uint16_t chunks = gs_carrier_chunks(len);

    // Every chunk arrives, and one of them is wrong. **This is the case the
    // hash is for**: the pieces being counted is not the same as the track
    // being right, and two machines racing on tracks they both believe are the
    // same one is the one failure rollback cannot absorb.
    gs_carrier_expect(&gs_carry, hash);
    for (uint16_t c = 0; c < chunks; c++) {
        uint8_t dg[GS_PROTO_MTU];
        size_t n = gs_carrier_chunk(dg, sizeof dg, hash, bytes, len, c);
        if (c == 1) dg[n - 1] ^= 0xffu;      // one byte, in the middle
        CHECK(gs_carrier_take(&gs_carry, dg, n));
    }

    CHECK(gs_carrier_done(&gs_carry));          // all the pieces are here...
    CHECK(!gs_carrier_track(&gs_carry, &got));  // ...and it is not the track

    // A chunk belonging to a different track is ignored rather than mixed in.
    gs_carrier_expect(&gs_carry, hash);
    uint8_t dg[GS_PROTO_MTU];
    size_t n = gs_carrier_chunk(dg, sizeof dg, hash ^ 1u, bytes, len, 0);
    CHECK(!gs_carrier_take(&gs_carry, dg, n));
    CHECK(gs_carry.got == 0);

    // Missing a piece is not done, however many times the others arrive.
    gs_carrier_expect(&gs_carry, hash);
    for (int repeat = 0; repeat < 3; repeat++) {
        for (uint16_t c = 1; c < chunks; c++) {
            size_t m = gs_carrier_chunk(dg, sizeof dg, hash, bytes, len, c);
            gs_carrier_take(&gs_carry, dg, m);
        }
    }
    CHECK(!gs_carrier_done(&gs_carry));
    CHECK(gs_carrier_progress(&gs_carry) < 1.0f);
    CHECK(!gs_carrier_track(&gs_carry, &got));

    // And the missing one completes it.
    size_t m = gs_carrier_chunk(dg, sizeof dg, hash, bytes, len, 0);
    CHECK(gs_carrier_take(&gs_carry, dg, m));
    CHECK(gs_carrier_done(&gs_carry));
    CHECK(gs_carrier_track(&gs_carry, &got));
    CHECK(gs_track_hash(&got) == hash);
}

// ---------------------------------------------------------------------------
// Is that time real?
// ---------------------------------------------------------------------------

static gs_replay gs_proof;

// A real race, recorded: an AI driver round a real track until the flag. What
// comes back is the recording and the truth about what it did.
static void gs_honest_race(gs_track *t, gs_claim *claim, uint16_t laps) {
    gs_track_init(t, 40, 16, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= t->h; y++)
        for (uint8_t x = 0; x <= t->w; x++) gs_track_set_corner(t, x, y, 0);
    gs_track_add_gate(t, GS_INT(6), GS_INT(8), 0, GS_INT(6));
    gs_track_add_gate(t, GS_INT(30), GS_INT(8), 0, GS_INT(6));

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_set_mode(&w, GS_MODE_RACE);
    gs_world_set_laps(&w, laps);
    gs_world_add_car(&w, t, (uint8_t)GS_VEH_SPRINT_CAR, GS_INT(4), GS_INT(8), 0);

    gs_replay_begin(&gs_proof, &w, t);
    for (uint32_t i = 0; i < GS_TICK_HZ * 200 && !w.over; i++) {
        gs_input in[GS_MAX_CARS] = { 0 };
        in[0] = gs_ai_drive(&w, t, 0);
        gs_replay_record(&gs_proof, in);
        gs_world_step(&w, t, in);
    }

    memset(claim, 0, sizeof *claim);
    claim->track = gs_track_hash(t);
    claim->conditions = gs_conditions_hash(&w);
    claim->laps = laps;
    claim->car = 0;
    claim->lap_ticks = w.car[0].best_lap;
    claim->race_ticks = w.car[0].finish_tick;
}

TEST(an_honest_time_is_verified_and_a_doctored_one_is_not) {
    static gs_track t;
    gs_claim claim;
    gs_honest_race(&t, &claim, 3);

    CHECK(claim.race_ticks > 0);
    CHECK(claim.lap_ticks > 0);

    // **The whole point.** The server re-races what was pressed and checks it
    // produces what was claimed - which is possible only because a race is
    // exactly reproducible from its inputs.
    CHECK(gs_verify(&gs_proof, &t, &claim, nullptr) == GS_VERDICT_OK);

    // A better time than was driven. This is the cheat, and it is the one
    // thing that must never get through.
    gs_claim faster = claim;
    faster.race_ticks = claim.race_ticks - 1;
    CHECK(gs_verify(&gs_proof, &t, &faster, nullptr) == GS_VERDICT_RACE_TOO_GOOD);

    faster = claim;
    faster.race_ticks = claim.race_ticks / 2;
    CHECK(gs_verify(&gs_proof, &t, &faster, nullptr) == GS_VERDICT_RACE_TOO_GOOD);

    faster = claim;
    faster.lap_ticks = claim.lap_ticks - 1;
    CHECK(gs_verify(&gs_proof, &t, &faster, nullptr) == GS_VERDICT_LAP_TOO_GOOD);

    // A *slower* claim is fine. Somebody's own honest mistake costs them the
    // record they did not take and nothing else, and rejecting it would be
    // punishing a player for being wrong in their own favour's opposite.
    gs_claim slower = claim;
    slower.race_ticks = claim.race_ticks + 500;
    slower.lap_ticks = claim.lap_ticks + 500;
    CHECK(gs_verify(&gs_proof, &t, &slower, nullptr) == GS_VERDICT_OK);
}

TEST(a_time_from_a_different_race_is_not_this_record) {
    static gs_track t, other;
    gs_claim claim;
    gs_honest_race(&t, &claim, 3);

    // Somewhere else. The recording says which track it is and the track says
    // what it is, so this is a comparison rather than a matter of trust.
    gs_track_init(&other, 40, 16, GS_SURF_DIRT);
    for (uint8_t y = 0; y <= other.h; y++)
        for (uint8_t x = 0; x <= other.w; x++) gs_track_set_corner(&other, x, y, 0);
    gs_track_add_gate(&other, GS_INT(6), GS_INT(8), 0, GS_INT(6));
    gs_track_add_gate(&other, GS_INT(30), GS_INT(8), 0, GS_INT(6));
    CHECK(gs_track_hash(&other) != gs_track_hash(&t));
    CHECK(gs_verify(&gs_proof, &other, &claim, nullptr) == GS_VERDICT_WRONG_TRACK);

    // A different distance. A two-lap time is not a three-lap record, however
    // real it is.
    gs_claim wrong_laps = claim;
    wrong_laps.laps = 5;
    CHECK(gs_verify(&gs_proof, &t, &wrong_laps, nullptr) == GS_VERDICT_WRONG_RULES);

    // **Different dials.** A lap driven on the Moon cannot pay for a claim
    // about Earth, and the conditions are in the recording rather than in the
    // claim, so saying otherwise does not help.
    gs_claim wrong_gravity = claim;
    wrong_gravity.conditions ^= 0xffull;
    CHECK(gs_verify(&gs_proof, &t, &wrong_gravity, nullptr) == GS_VERDICT_WRONG_RULES);

    // A car that was not in the race.
    gs_claim ghost_car = claim;
    ghost_car.car = 3;
    CHECK(gs_verify(&gs_proof, &t, &ghost_car, nullptr) == GS_VERDICT_NO_SUCH_CAR);

    // And rubbish is not a recording.
    static uint8_t junk[512];
    for (size_t i = 0; i < sizeof junk; i++) junk[i] = (uint8_t)(i * 13u);
    CHECK(gs_verify_bytes(junk, sizeof junk, &t, &claim, nullptr) ==
          GS_VERDICT_NOT_A_REPLAY);
}

TEST(a_time_survives_the_wire_and_is_still_verifiable) {
    // The proof arrives as bytes, so that is how it is checked. A verifier that
    // only worked on the recording it was handed in memory would be a verifier
    // no server could use.
    static gs_track t;
    gs_claim claim;
    gs_honest_race(&t, &claim, 2);

    static uint8_t bytes[sizeof(gs_replay) + 4096];
    size_t n = gs_replay_serialize(&gs_proof, bytes, sizeof bytes);
    CHECK(n > 0);

    gs_world produced;
    CHECK(gs_verify_bytes(bytes, n, &t, &claim, &produced) == GS_VERDICT_OK);

    // And the world handed back is the race that actually happened, which is
    // what a server should believe rather than what it was told.
    CHECK(produced.car[0].finish_tick == claim.race_ticks);
    CHECK(produced.car[0].best_lap == claim.lap_ticks);
    CHECK(produced.over);

    // --- What corruption is and is not caught, stated exactly.
    //
    // This used to say "one byte different in the middle and it is not a
    // recording any more" and flip the byte at the halfway mark. That passed by
    // luck. **Most single bytes in a replay change nothing anybody can detect,
    // and correctly so**: three quarters of the file is input for cars that were
    // not in the race, and a byte that makes the recorded driver *quicker*
    // leaves the claim slower than what the recording produces, which is not a
    // lie and is accepted on purpose. Probing forty bytes across the file caught
    // one.
    //
    // So the facts pinned here are the ones that are true.

    // The header is structure, and structure has to survive intact.
    for (size_t i = 0; i < 8; i++) {
        uint8_t was = bytes[i];
        bytes[i] ^= 0xffu;
        CHECK(gs_verify_bytes(bytes, n, &t, &claim, nullptr) == GS_VERDICT_NOT_A_REPLAY);
        bytes[i] = was;
    }

    // A recording that stops early is not the recording of a finished race.
    CHECK(gs_verify_bytes(bytes, n - 100, &t, &claim, nullptr) != GS_VERDICT_OK);

    // And driving that was not driven does not produce the time that was
    // claimed. Half the inputs scrambled rather than one byte flipped, because
    // one byte is a nudge and the question is whether the *race* is checked.
    static uint8_t scrambled[sizeof(gs_replay) + 4096];
    memcpy(scrambled, bytes, n);
    for (size_t i = n / 2; i < n; i++) scrambled[i] ^= 0x5au;
    CHECK(gs_verify_bytes(scrambled, n, &t, &claim, nullptr) != GS_VERDICT_OK);

    // The untouched original still passes, so none of the above was the test
    // breaking the bytes it was given.
    CHECK(gs_verify_bytes(bytes, n, &t, &claim, nullptr) == GS_VERDICT_OK);
}

// ---------------------------------------------------------------------------
// The library
// ---------------------------------------------------------------------------

static gs_library gs_lib;

// A track that differs from its neighbours by something real, so two of them
// are never accidentally the same track.
static void gs_make_track(gs_track *t, uint8_t seed) {
    gs_track_init(t, (uint8_t)(32 + seed), 16, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= t->h; y++) {
        for (uint8_t x = 0; x <= t->w; x++) {
            gs_fix h = (x % (uint8_t)(4 + seed) == 0) ? GS_INT(1) : 0;
            gs_track_set_corner(t, x, y, h);
        }
    }
    gs_track_add_gate(t, GS_INT(4), GS_INT(8), 0, GS_INT(5));
    gs_track_add_gate(t, GS_INT(20), GS_INT(8), 0, GS_INT(5));
}

TEST(three_tracks_are_kept_and_all_three_survive_a_restart) {
    static gs_track a, b, c;
    gs_make_track(&a, 1);
    gs_make_track(&b, 2);
    gs_make_track(&c, 3);

    gs_library_clear(&gs_lib);
    CHECK(gs_library_put(&gs_lib, &a, "the loop", "ada") == 0);
    CHECK(gs_library_put(&gs_lib, &b, "the drop", "bez") == 1);
    CHECK(gs_library_put(&gs_lib, &c, "the wall", "cy") == 2);
    CHECK(gs_lib.count == 3);

    // Three different tracks, or this proves nothing.
    CHECK(gs_track_hash(&a) != gs_track_hash(&b));
    CHECK(gs_track_hash(&b) != gs_track_hash(&c));

    // **The same track twice is one track.** Content addressing, doing what it
    // is for - and a name is a rename rather than a second copy.
    CHECK(gs_library_put(&gs_lib, &a, "the loop, again", "ada") == 0);
    CHECK(gs_lib.count == 3);
    CHECK(strcmp(gs_library_at(&gs_lib, 0)->name, "the loop, again") == 0);

    // Out to disk and back, which is the only form "still there after a
    // restart" ever takes.
    static uint8_t buf[GS_LIBRARY_MAX * (GS_TRACK_TILES * 4 + 4096) + 4096];
    size_t n = gs_library_serialize(&gs_lib, buf, sizeof buf);
    CHECK(n > 0);

    // Far smaller than the library in memory: the tracks are serialised rather
    // than copied whole.
    CHECK(n < sizeof(gs_library) / 4);

    static gs_library back;
    CHECK(gs_library_deserialize(&back, buf, n));
    CHECK(back.count == 3);

    for (int i = 0; i < 3; i++) {
        const gs_library_entry *was = gs_library_at(&gs_lib, i);
        const gs_library_entry *now = gs_library_at(&back, i);
        CHECK(now != nullptr);
        if (now == nullptr) continue;

        CHECK(now->hash == was->hash);
        CHECK(strcmp(now->name, was->name) == 0);
        CHECK(strcmp(now->author, was->author) == 0);

        // The track itself, not merely its name.
        CHECK(gs_track_hash(&now->track) == gs_track_hash(&was->track));
        CHECK(now->track.w == was->track.w);
        CHECK(now->track.gate_count == was->track.gate_count);
    }

    // Rubbish is refused rather than half-read.
    CHECK(!gs_library_deserialize(&back, buf, 6));
    buf[0] ^= 0xffu;
    CHECK(!gs_library_deserialize(&back, buf, n));
}

TEST(editing_one_track_leaves_the_others_alone) {
    static gs_track a, b, c;
    gs_make_track(&a, 1);
    gs_make_track(&b, 2);
    gs_make_track(&c, 3);

    gs_library_clear(&gs_lib);
    gs_library_put(&gs_lib, &a, "one", "ada");
    gs_library_put(&gs_lib, &b, "two", "bez");
    gs_library_put(&gs_lib, &c, "three", "cy");

    uint64_t was = gs_track_hash(&b);
    uint64_t untouched_a = gs_track_hash(&a);
    uint64_t untouched_c = gs_track_hash(&c);

    // Edit the middle one.
    gs_track edited = b;
    gs_track_set_corner(&edited, 10, 8, GS_INT(3));
    CHECK(gs_track_hash(&edited) != was);

    int at = gs_library_replace(&gs_lib, was, &edited);
    CHECK(at == 1);
    CHECK(gs_lib.count == 3);

    // The slot followed the edit, and kept its name.
    CHECK(gs_library_at(&gs_lib, 1)->hash == gs_track_hash(&edited));
    CHECK(strcmp(gs_library_at(&gs_lib, 1)->name, "two") == 0);
    CHECK(gs_library_find(&gs_lib, was) < 0);

    // And the other two are exactly as they were.
    CHECK(gs_library_at(&gs_lib, 0)->hash == untouched_a);
    CHECK(gs_library_at(&gs_lib, 2)->hash == untouched_c);
    CHECK(strcmp(gs_library_at(&gs_lib, 0)->name, "one") == 0);
    CHECK(strcmp(gs_library_at(&gs_lib, 2)->name, "three") == 0);

    // Editing something that is not here changes nothing.
    CHECK(gs_library_replace(&gs_lib, 0xdeadbeefull, &edited) == -1);
    CHECK(gs_lib.count == 3);

    // Editing one track *into* another leaves one entry rather than two of the
    // same thing.
    CHECK(gs_library_replace(&gs_lib, gs_track_hash(&edited), &c) == 
          gs_library_find(&gs_lib, untouched_c));
    CHECK(gs_lib.count == 2);
    CHECK(gs_library_find(&gs_lib, untouched_a) >= 0);
    CHECK(gs_library_find(&gs_lib, untouched_c) >= 0);

    // Removing keeps the rest in order.
    CHECK(gs_library_remove(&gs_lib, untouched_a));
    CHECK(gs_lib.count == 1);
    CHECK(gs_library_at(&gs_lib, 0)->hash == untouched_c);
    CHECK(!gs_library_remove(&gs_lib, 0xabcdull));
}

TEST(a_track_that_came_with_the_game_is_not_yours_to_change) {
    // **The library a player came with is still there after an afternoon of
    // building.** A shipped track is not theirs to rename, edit in place or
    // throw away - editing one takes a copy, which *is* theirs from the first
    // keystroke. That is better than refusing outright, and much better than
    // letting them change it and finding out at save time.
    static gs_library l;
    gs_library_clear(&l);

    static gs_track shipped, mine;
    gs_track_init(&shipped, 24, 24, GS_SURF_PAVEMENT);
    gs_track_set_corner(&shipped, 4, 4, GS_INT(1));

    gs_track_init(&mine, 24, 24, GS_SURF_DIRT);
    gs_track_set_corner(&mine, 6, 6, GS_INT(2));

    CHECK(gs_library_put_builtin(&l, &shipped, "first light", "gearstick") == 0);
    CHECK(gs_library_put(&l, &mine, "mine", "gavin") == 1);

    CHECK(gs_library_is_builtin(&l, gs_track_hash(&shipped)));
    CHECK(!gs_library_is_builtin(&l, gs_track_hash(&mine)));

    // Nothing here at all is not "not protected": a hash the library has never
    // heard of is not a shipped track, and saying so the other way round would
    // make every unknown track unchangeable.
    static gs_track stranger;
    gs_track_init(&stranger, 8, 8, GS_SURF_ICE);
    CHECK(!gs_library_is_builtin(&l, gs_track_hash(&stranger)));

    // **It survives the round trip**, or the protection lasts until the game is
    // next started and then quietly stops.
    static uint8_t buf[1u << 20];
    size_t wrote = gs_library_serialize(&l, buf, sizeof buf);
    CHECK(wrote > 0);

    static gs_library back;
    CHECK(gs_library_deserialize(&back, buf, wrote));
    CHECK(back.count == 2);
    CHECK(gs_library_is_builtin(&back, gs_track_hash(&shipped)));
    CHECK(!gs_library_is_builtin(&back, gs_track_hash(&mine)));

    // And a copy of a shipped track is the player's own, because what makes an
    // entry the game's is where it came from and not what is in it.
    static gs_library copied;
    gs_library_clear(&copied);
    CHECK(gs_library_put_builtin(&copied, &shipped, "first light", "gearstick") == 0);
    CHECK(gs_library_put(&copied, &mine, "first light (copy)", "") == 1);
    CHECK(!gs_library_is_builtin(&copied, gs_track_hash(&mine)));
}

TEST(a_library_that_is_full_says_so_rather_than_losing_something) {
    gs_library_clear(&gs_lib);

    for (int i = 0; i < GS_LIBRARY_MAX; i++) {
        static gs_track t;
        gs_make_track(&t, (uint8_t)(i + 1));
        char name[GS_LIBRARY_NAME];
        snprintf(name, sizeof name, "track %d", i);
        CHECK(gs_library_put(&gs_lib, &t, name, "ada") == i);
    }
    CHECK(gs_lib.count == GS_LIBRARY_MAX);

    static gs_track one_too_many;
    gs_make_track(&one_too_many, GS_LIBRARY_MAX + 1);
    CHECK(gs_library_put(&gs_lib, &one_too_many, "nope", "ada") == -1);

    // And nothing was dropped to make room for it.
    CHECK(gs_lib.count == GS_LIBRARY_MAX);
    CHECK(strcmp(gs_library_at(&gs_lib, 0)->name, "track 0") == 0);
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
// A recording knows who drove it
// ---------------------------------------------------------------------------

TEST(a_replay_recorded_by_one_driver_cannot_be_claimed_by_another) {
    // **The most serious hole docs/THREATS.md listed.** A recording carried the
    // track, the dials, the grid and the machines and not the driver - so an
    // honest replay was a bearer token: anyone who obtained one could hand it in
    // and the verifier would correctly agree the time had been driven. It had.
    // Just not by them.
    static gs_track t;
    gs_claim claim;
    gs_honest_race(&t, &claim, 2);
    gs_replay_set_driver(&gs_proof, 0, "ada");

    // Ada, who drove it, is believed.
    memcpy(claim.who, "ada", 4);
    CHECK(gs_verify(&gs_proof, &t, &claim, nullptr) == GS_VERDICT_OK);

    // Bez, who found it, is not - and is told exactly why rather than being
    // given a vague refusal.
    memcpy(claim.who, "bez", 4);
    CHECK(gs_verify(&gs_proof, &t, &claim, nullptr) == GS_VERDICT_WRONG_DRIVER);

    // Nor a name that is a prefix of the real one, which a sloppy comparison
    // would let through.
    memcpy(claim.who, "ad", 3);
    CHECK(gs_verify(&gs_proof, &t, &claim, nullptr) == GS_VERDICT_WRONG_DRIVER);

    // **And the same replay handed in twice sets one record, not two.** A
    // verified time is still only a time: the table is keyed on who set it under
    // which conditions, so submitting it again improves nothing and adds
    // nothing. Without that, a replay would be worth as many entries as somebody
    // had patience for.
    gs_records_clear(&gs_rec);
    memcpy(claim.who, "ada", 4);
    CHECK(gs_verify(&gs_proof, &t, &claim, nullptr) == GS_VERDICT_OK);

    for (int again = 0; again < 3; again++) {
        gs_record_beat beat = gs_records_submit(
            &gs_rec, claim.track, claim.conditions, (uint8_t)GS_VEH_SPRINT_CAR,
            (uint8_t)GS_MODE_RACE, claim.laps, claim.lap_ticks, claim.race_ticks,
            claim.who, 1700000000ull);

        // The first sets it; the rest beat nothing, because it is already theirs.
        CHECK(beat.lap == (again == 0));
        CHECK(beat.race == (again == 0));
        CHECK(gs_rec.count == 1);
    }
}

TEST(a_recording_that_names_nobody_backs_nobodys_claim) {
    // A version three replay does not know who drove it. **"It does not say" is
    // not "it says you"** - a claim of identity against a blank is refused,
    // because accepting it would leave every old recording a bearer token and
    // make the whole exercise decorative.
    static gs_track t;
    gs_claim claim;
    gs_honest_race(&t, &claim, 2);
    gs_replay_set_driver(&gs_proof, 0, "");

    memcpy(claim.who, "ada", 4);
    CHECK(gs_verify(&gs_proof, &t, &claim, nullptr) == GS_VERDICT_WRONG_DRIVER);

    // And a caller who is not asserting an identity at all - a local ghost, an
    // offline analysis - still gets an answer about the driving.
    claim.who[0] = '\0';
    CHECK(gs_verify(&gs_proof, &t, &claim, nullptr) == GS_VERDICT_OK);
}

TEST(the_driver_survives_the_wire_and_an_older_recording_still_loads) {
    static gs_track t;
    gs_claim claim;
    gs_honest_race(&t, &claim, 2);
    gs_replay_set_driver(&gs_proof, 0, "ada");
    gs_replay_set_driver(&gs_proof, 1, "bez");

    static uint8_t bytes[sizeof(gs_replay) + 4096];
    size_t n = gs_replay_serialize(&gs_proof, bytes, sizeof bytes);
    CHECK(n > 0);

    static gs_replay back;
    CHECK(gs_replay_deserialize(&back, bytes, n));
    CHECK(strcmp(gs_replay_driver(&back, 0), "ada") == 0);
    CHECK(strcmp(gs_replay_driver(&back, 1), "bez") == 0);
    CHECK(gs_replay_driver(&back, 2)[0] == '\0');

    // A name longer than fits is cut rather than refused: a recording that
    // silently had no driver would be the bearer token again.
    gs_replay_set_driver(&gs_proof, 0, "a-name-far-longer-than-sixteen");
    CHECK(strlen(gs_replay_driver(&gs_proof, 0)) == GS_REPLAY_NAME - 1);

    // And version three, which did not carry drivers, still loads - with the
    // names blank, which is the truth about it.
    static uint8_t older[sizeof(gs_replay) + 4096];
    memcpy(older, bytes, n);
    older[4] = 3; older[5] = 0; older[6] = 0; older[7] = 0;

    // Its header is shorter by exactly the names, so the inputs move up.
    size_t names = (size_t)GS_MAX_CARS * GS_REPLAY_NAME;
    size_t head = (size_t)(n - (size_t)gs_proof.meta.tick_count * GS_MAX_CARS);
    memmove(older + head - names, older + head, n - head);

    static gs_replay old_one;
    CHECK(gs_replay_deserialize(&old_one, older, n - names));
    CHECK(old_one.meta.tick_count == gs_proof.meta.tick_count);
    CHECK(gs_replay_driver(&old_one, 0)[0] == '\0');
}

// ---------------------------------------------------------------------------
// A track from somebody else's game
// ---------------------------------------------------------------------------

static gs_stunts_report gs_st_report;

// A Stunts file built here, byte by byte, from the published layout. **CI is
// exercised against this and never against a downloaded track**: the corpus is
// somebody else's and does not ship, per docs/ASSETS.md rule 1, and a test that
// needed a download would be a test that does not run.
static void gs_stunts_file(uint8_t *out, uint8_t horizon) {
    for (size_t i = 0; i < GS_STUNTS_BYTES; i++) out[i] = 0;
    out[GS_STUNTS_HORIZON_AT] = horizon;

    uint8_t *road = out + GS_STUNTS_TRACK_AT;
    uint8_t *ground = out + GS_STUNTS_TERRAIN_AT;

    // A paved straight along one row, a dirt straight along another, an icy one
    // along a third - the three surfaces the donor and this project share.
    // The road plane is stored bottom to top, hence the flip.
    for (int x = 4; x < 26; x++) {
        road[(size_t)(GS_STUNTS_SIDE - 1 - 10) * GS_STUNTS_SIDE + (size_t)x] = 0x05;
        road[(size_t)(GS_STUNTS_SIDE - 1 - 14) * GS_STUNTS_SIDE + (size_t)x] = 0x0f;
        road[(size_t)(GS_STUNTS_SIDE - 1 - 18) * GS_STUNTS_SIDE + (size_t)x] = 0x19;
    }
    // A start line, a crossroads, and a piece from a part of the game this
    // project has no equivalent of - a loop, which becomes road.
    road[(size_t)(GS_STUNTS_SIDE - 1 - 10) * GS_STUNTS_SIDE + 4] = 0x01;
    road[(size_t)(GS_STUNTS_SIDE - 1 - 10) * GS_STUNTS_SIDE + 15] = 0x4a;
    road[(size_t)(GS_STUNTS_SIDE - 1 - 10) * GS_STUNTS_SIDE + 20] = 0x40;

    // A raised plateau with a slope up to it, and a lake.
    for (int y = 2; y < 8; y++) {
        for (int x = 2; x < 12; x++) ground[(size_t)y * GS_STUNTS_SIDE + (size_t)x] = 0x06;
    }
    for (int x = 2; x < 12; x++) ground[(size_t)8 * GS_STUNTS_SIDE + (size_t)x] = 0x07;
    for (int y = 22; y < 27; y++) {
        for (int x = 18; x < 27; x++) ground[(size_t)y * GS_STUNTS_SIDE + (size_t)x] = 0x01;
    }
}

TEST(a_track_from_stunts_reads_as_a_track) {
    static uint8_t file[GS_STUNTS_BYTES];
    gs_stunts_file(file, GS_STUNTS_ALPINE);

    static gs_track t;
    CHECK(gs_stunts_read(&t, file, sizeof file, &gs_st_report));
    CHECK(gs_st_report.ok);

    // The shape of the donor, kept.
    CHECK(t.w == GS_STUNTS_SIDE);
    CHECK(t.h == GS_STUNTS_SIDE);
    CHECK(gs_st_report.horizon == GS_STUNTS_ALPINE);

    // The three surfaces it shares with this project, on the rows they were on.
    CHECK(gs_track_surface(&t, GS_INT(10) + GS_HALF, GS_INT(10) + GS_HALF) == GS_SURF_PAVEMENT);
    CHECK(gs_track_surface(&t, GS_INT(10) + GS_HALF, GS_INT(14) + GS_HALF) == GS_SURF_DIRT);
    CHECK(gs_track_surface(&t, GS_INT(10) + GS_HALF, GS_INT(18) + GS_HALF) == GS_SURF_ICE);

    // And what is not road is not road.
    CHECK(gs_track_surface(&t, GS_INT(10) + GS_HALF, GS_INT(12) + GS_HALF) == GS_SURF_GRASS);

    // A loop has no equivalent here and is not in the table this reader was
    // written from. It becomes road rather than a hole - a car should be able to
    // drive where the donor put one - and it is *counted*, because "it imported"
    // means nothing without knowing how much was approximated.
    CHECK(gs_track_surface(&t, GS_INT(20) + GS_HALF, GS_INT(10) + GS_HALF) == GS_SURF_PAVEMENT);
    CHECK(gs_st_report.unknown_pieces == 1);
    CHECK(gs_st_report.road_tiles > 60);

    // The ground came across: a plateau, a lake, and flat between them.
    CHECK(gs_track_height(&t, GS_INT(6), GS_INT(5)) > GS_ONE);
    CHECK(gs_track_height(&t, GS_INT(22), GS_INT(24)) < 0);
    CHECK(gs_track_height(&t, GS_INT(28), GS_INT(2)) == 0);
    CHECK(gs_st_report.raised_tiles > 60);
}

TEST(an_imported_track_can_be_validated_and_driven) {
    // **The point of importing at all.** A track nobody here designed, put
    // through the analyser and the AI - which have only ever been shown terrain
    // built by the same hands that wrote them.
    static uint8_t file[GS_STUNTS_BYTES];
    gs_stunts_file(file, GS_STUNTS_DESERT);

    static gs_track t;
    CHECK(gs_stunts_read(&t, file, sizeof file, nullptr));

    // A route, because Stunts describes one with its road pieces and this
    // project describes one with gates - so importing the ground is the part
    // that transfers and the route is the importer's caller's business.
    gs_track_add_gate(&t, GS_INT(5), GS_INT(10) + GS_HALF, 0, GS_INT(4));
    gs_track_add_gate(&t, GS_INT(24), GS_INT(10) + GS_HALF, 0, GS_INT(4));

    CHECK(gs_track_validate(&t).problem == GS_TRACK_OK);

    gs_analyse(&t, gs_analyse_seconds(&t), &gs_report);
    CHECK(gs_report.completable);
}

TEST(bytes_that_are_not_a_stunts_track_are_refused) {
    static uint8_t file[GS_STUNTS_BYTES + 16];
    gs_stunts_file(file, 0);

    static gs_track t;

    // The length is the only thing that can be said for certain - every byte
    // value in range is a legal picture of something - so it is the only thing
    // refused, and it is refused firmly.
    CHECK(!gs_stunts_read(&t, file, GS_STUNTS_BYTES - 1, &gs_st_report));
    CHECK(!gs_st_report.ok);
    CHECK(!gs_stunts_read(&t, file, GS_STUNTS_BYTES + 1, nullptr));
    CHECK(!gs_stunts_read(&t, nullptr, GS_STUNTS_BYTES, nullptr));
    CHECK(gs_stunts_read(&t, file, GS_STUNTS_BYTES, nullptr));
}

TEST(a_road_piece_this_reader_does_not_know_is_counted_rather_than_hidden) {
    // Half a track somebody can look at beats a refusal they cannot act on -
    // but they have to be told how much of it was thrown away, or "it imported"
    // means nothing.
    static uint8_t file[GS_STUNTS_BYTES];
    gs_stunts_file(file, 0);

    // What the file already contains that this reader cannot name - measured
    // rather than assumed, so adding a piece to the fixture later does not
    // quietly turn this into a test of a number.
    static gs_track t;
    CHECK(gs_stunts_read(&t, file, sizeof file, &gs_st_report));
    uint16_t before = gs_st_report.unknown_pieces;

    file[(size_t)(GS_STUNTS_SIDE - 1 - 3) * GS_STUNTS_SIDE + 3] = 0xb0;
    file[(size_t)(GS_STUNTS_SIDE - 1 - 3) * GS_STUNTS_SIDE + 4] = 0xc7;

    CHECK(gs_stunts_read(&t, file, sizeof file, &gs_st_report));
    CHECK(gs_st_report.ok);
    CHECK(gs_st_report.unknown_pieces == before + 2);

    // And they are road, not holes.
    CHECK(gs_track_surface(&t, GS_INT(3) + GS_HALF, GS_INT(3) + GS_HALF) == GS_SURF_PAVEMENT);
}

TEST(a_track_written_in_the_stunts_layout_reads_back_as_itself) {
    // The writer exists so CI has an input this repository made. It is only
    // worth having if the two agree, so this is the round trip: our track, out
    // through the donor's format, and back.
    static gs_track made;
    gs_track_init(&made, GS_STUNTS_SIDE, GS_STUNTS_SIDE, GS_SURF_GRASS);

    for (int x = 3; x < 27; x++) {
        gs_track_set_surface(&made, (uint8_t)x, 8, GS_SURF_PAVEMENT);
        gs_track_set_surface(&made, (uint8_t)x, 16, GS_SURF_DIRT);
        gs_track_set_surface(&made, (uint8_t)x, 22, GS_SURF_ICE);
    }
    for (int y = 2; y <= 6; y++) {
        for (int x = 20; x <= 26; x++) {
            gs_track_set_corner(&made, (uint8_t)x, (uint8_t)y, GS_RATIO(150, 100));
        }
    }

    static uint8_t file[GS_STUNTS_BYTES];
    CHECK(gs_stunts_write(&made, file, sizeof file, GS_STUNTS_CITY) == GS_STUNTS_BYTES);

    static gs_track back;
    CHECK(gs_stunts_read(&back, file, sizeof file, &gs_st_report));
    CHECK(gs_st_report.horizon == GS_STUNTS_CITY);

    // The surfaces survive the crossing exactly, because all three exist in both.
    for (int x = 3; x < 27; x++) {
        gs_fix cx = GS_INT(x) + GS_HALF;
        CHECK(gs_track_surface(&back, cx, GS_INT(8) + GS_HALF) == GS_SURF_PAVEMENT);
        CHECK(gs_track_surface(&back, cx, GS_INT(16) + GS_HALF) == GS_SURF_DIRT);
        CHECK(gs_track_surface(&back, cx, GS_INT(22) + GS_HALF) == GS_SURF_ICE);
        CHECK(gs_track_surface(&back, cx, GS_INT(4) + GS_HALF) == GS_SURF_GRASS);
    }

    // The elevation survives as elevation rather than exactly: the donor has two
    // levels and we have a continuous height, so what comes back is high where
    // it was high and flat where it was flat, and saying more than that would be
    // claiming a fidelity the format does not have.
    CHECK(gs_track_height(&back, GS_INT(23), GS_INT(4)) > GS_ONE);
    CHECK(gs_track_height(&back, GS_INT(5), GS_INT(20)) == 0);
}

// ---------------------------------------------------------------------------
// A store written by an older version
// ---------------------------------------------------------------------------

// **A version one writer, frozen.** This is what the previous release wrote, and
// it is spelled out here rather than produced by the current code precisely so
// that it cannot follow it: a migration test whose old format is generated by
// the new program tests nothing, because the two move together.
static void gs_put32_le(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xffu);
}
static void gs_put64_le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xffu);
}
// Reading one back, for checking what a saved file says its version is.
static uint32_t gs_get32_at(const uint8_t *p, size_t at) {
    return (uint32_t)p[at] | ((uint32_t)p[at + 1] << 8) |
           ((uint32_t)p[at + 2] << 16) | ((uint32_t)p[at + 3] << 24);
}

static void gs_put16_le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

TEST(records_written_by_the_previous_version_still_load) {
    // Two rows in the version one layout: no date on the end of either, because
    // dates did not exist.
    static uint8_t old_file[512];
    uint8_t *p = old_file;

    gs_put32_le(p, 0x43525347u); p += 4;    // "GSRC"
    gs_put32_le(p, 1u);          p += 4;    // version one
    gs_put32_le(p, 2u);          p += 4;    // two records

    struct { uint64_t track, cond; uint32_t lap, race; uint16_t laps;
             uint8_t veh, mode; const char *who; } rows[2] = {
        { 0xabc1ULL, 0x0f0fULL, 4211, 13000, 3, 1, 0, "ada" },
        { 0xabc2ULL, 0x0f0fULL, 5000, 16000, 3, 2, 0, "bez" },
    };

    for (int i = 0; i < 2; i++) {
        gs_put64_le(p, rows[i].track); p += 8;
        gs_put64_le(p, rows[i].cond);  p += 8;
        gs_put32_le(p, rows[i].lap);   p += 4;
        gs_put32_le(p, rows[i].race);  p += 4;
        gs_put16_le(p, rows[i].laps);  p += 2;
        *p++ = rows[i].veh;
        *p++ = rows[i].mode;
        memset(p, 0, GS_NAME_MAX);
        memcpy(p, rows[i].who, strlen(rows[i].who));
        p += GS_NAME_MAX;
    }
    size_t n = (size_t)(p - old_file);

    // **It loads, and everything that was in it is still in it.**
    static gs_records back;
    CHECK(gs_records_deserialize(&back, old_file, n));
    CHECK(back.count == 2);

    const gs_record *best = gs_records_best_lap(&back, 0xabc1ULL, 0x0f0fULL);
    CHECK(best != nullptr);
    if (best != nullptr) {
        CHECK(best->lap == 4211);
        CHECK(best->race == 13000);
        CHECK(best->vehicle == 1);
        CHECK(strcmp(best->who, "ada") == 0);

        // And the field that did not exist says it does not know, rather than
        // claiming the epoch.
        CHECK(best->when == 0);
    }

    // Saved again, it comes back as the current version with everything intact -
    // which is what makes this an upgrade rather than a read-only compatibility
    // shim.
    static uint8_t rewritten[4096];
    size_t m = gs_records_serialize(&back, rewritten, sizeof rewritten);
    CHECK(m > 0);
    CHECK(gs_get32_at(rewritten, 4) == GS_RECORDS_VERSION);

    static gs_records again;
    CHECK(gs_records_deserialize(&again, rewritten, m));
    CHECK(again.count == 2);

    // Guarded, because a test that crashes instead of failing reports nothing:
    // the first version of this dereferenced the result, and when the loader was
    // broken on purpose the suite died with a segmentation fault rather than
    // naming the fact that had stopped being true.
    const gs_record *reread = gs_records_best_lap(&again, 0xabc1ULL, 0x0f0fULL);
    CHECK(reread != nullptr);
    if (reread != nullptr) CHECK(reread->lap == 4211);
}

TEST(profiles_written_by_the_previous_version_still_load) {
    static uint8_t old_file[512];
    uint8_t *p = old_file;

    gs_put32_le(p, 0x50525347u); p += 4;    // "GSRP"
    gs_put32_le(p, 1u);          p += 4;
    gs_put32_le(p, 1u);          p += 4;    // one profile

    memset(p, 0, GS_PROFILE_NAME);
    memcpy(p, "ada", 3);
    p += GS_PROFILE_NAME;
    *p++ = 2;                    // colour
    *p++ = 1;                    // vehicle
    gs_put32_le(p, 12u); p += 4; // races
    gs_put32_le(p, 5u);  p += 4; // wins
    gs_put32_le(p, 9u);  p += 4; // podiums
    gs_put32_le(p, 1u);  p += 4; // wrecks
    gs_put64_le(p, 4321u); p += 8;  // tiles

    size_t n = (size_t)(p - old_file);

    static gs_profiles back;
    CHECK(gs_profiles_deserialize(&back, old_file, n));
    CHECK(back.count == 1);
    CHECK(strcmp(back.entry[0].name, "ada") == 0);
    CHECK(back.entry[0].colour == 2);
    CHECK(back.entry[0].vehicle == 1);
    CHECK(back.entry[0].races == 12);
    CHECK(back.entry[0].wins == 5);
    CHECK(back.entry[0].podiums == 9);
    CHECK(back.entry[0].wrecks == 1);
    CHECK(back.entry[0].tiles == 4321);
    CHECK(back.entry[0].last_raced == 0);
}

TEST(a_store_from_a_version_that_does_not_exist_is_refused) {
    // Tolerant of the past and not of the future: a file from a version this
    // build has never heard of cannot be read by guessing at it, and a table of
    // times read wrongly is worse than one that would not open.
    static uint8_t bad[64];
    gs_put32_le(bad, 0x43525347u);
    gs_put32_le(bad + 4, GS_RECORDS_VERSION + 1u);
    gs_put32_le(bad + 8, 0u);

    static gs_records back;
    CHECK(!gs_records_deserialize(&back, bad, 12));

    gs_put32_le(bad, 0x43525347u);
    gs_put32_le(bad + 4, 0u);              // and version zero never existed
    CHECK(!gs_records_deserialize(&back, bad, 12));

    // Nor is a different format read as this one.
    gs_put32_le(bad, 0x4b525447u);
    gs_put32_le(bad + 4, GS_RECORDS_VERSION);
    CHECK(!gs_records_deserialize(&back, bad, 12));
}

// ---------------------------------------------------------------------------
// Undo, and the route
// ---------------------------------------------------------------------------

TEST(placing_a_gate_can_be_undone_like_anything_else) {
    // **Undo used to cover everything except the one edit that decides what a
    // track is.** Terrain, surfaces and gravity were in the history and the
    // route was not, so "undo covers what you can change" had a footnote - and
    // the footnote was the part that changes a track's identity, which is what
    // every record and every shared code is keyed on.
    static uint8_t room[65536];
    gs_edit_log *log = (gs_edit_log *)room;
    gs_edit_log_init(log, 256);

    static gs_track t;
    gs_track_init(&t, 32, 16, GS_SURF_PAVEMENT);
    gs_track_add_gate(&t, GS_INT(4), GS_INT(8), 0, GS_INT(5));

    uint64_t before = gs_track_hash(&t);
    CHECK(t.gate_count == 1);

    CHECK(gs_edit_add_gate(log, &t, GS_INT(20), GS_INT(8), GS_DEG(90), GS_INT(6)) == 1);
    CHECK(t.gate_count == 2);
    CHECK(gs_track_hash(&t) != before);

    CHECK(gs_edit_undo(log, &t));
    CHECK(t.gate_count == 1);

    // **The hash, and not just the count.** A gate put back in the wrong place
    // or the wrong order leaves the same number of them and a different track.
    CHECK(gs_track_hash(&t) == before);

    // And forward again.
    CHECK(gs_edit_redo(log, &t));
    CHECK(t.gate_count == 2);
    CHECK(gs_track_hash(&t) != before);
}

TEST(removing_a_gate_puts_it_back_where_it_was_in_the_order) {
    // A route is ordered, so restoring a gate means restoring its place in the
    // order as well as its numbers. Taking one out of the middle is the case
    // that tells the two apart.
    static uint8_t room[65536];
    gs_edit_log *log = (gs_edit_log *)room;
    gs_edit_log_init(log, 256);

    static gs_track t;
    gs_track_init(&t, 48, 16, GS_SURF_PAVEMENT);
    gs_track_add_gate(&t, GS_INT(4), GS_INT(8), 0, GS_INT(5));
    gs_track_add_gate(&t, GS_INT(20), GS_INT(8), GS_DEG(90), GS_INT(6));
    gs_track_add_gate(&t, GS_INT(40), GS_INT(8), GS_DEG(180), GS_INT(7));

    uint64_t before = gs_track_hash(&t);

    CHECK(gs_edit_remove_gate(log, &t, 1));
    CHECK(t.gate_count == 2);
    // The one after it closed up, which is what makes this an ordered list.
    CHECK(t.gate[1].x == GS_INT(40));
    CHECK(gs_track_hash(&t) != before);

    CHECK(gs_edit_undo(log, &t));
    CHECK(t.gate_count == 3);
    CHECK(t.gate[1].x == GS_INT(20));
    CHECK(t.gate[1].heading == GS_DEG(90));
    CHECK(t.gate[1].half_width == GS_INT(6));
    CHECK(t.gate[2].x == GS_INT(40));
    CHECK(gs_track_hash(&t) == before);
}

TEST(the_route_and_the_ground_undo_in_one_history) {
    // Two histories would undo in two orders, and a player would find that the
    // last thing they did is not the first thing that comes back.
    static uint8_t room[65536];
    gs_edit_log *log = (gs_edit_log *)room;
    gs_edit_log_init(log, 256);

    static gs_track t;
    gs_track_init(&t, 32, 16, GS_SURF_PAVEMENT);
    uint64_t empty = gs_track_hash(&t);

    gs_edit_corner(log, &t, 5, 5, GS_INT(2));
    uint64_t after_ground = gs_track_hash(&t);

    gs_edit_add_gate(log, &t, GS_INT(8), GS_INT(8), 0, GS_INT(5));
    uint64_t after_gate = gs_track_hash(&t);

    gs_edit_corner(log, &t, 9, 9, GS_INT(1));

    // Backwards through the lot, in the order it was done.
    CHECK(gs_edit_undo(log, &t));
    CHECK(gs_track_hash(&t) == after_gate);
    CHECK(gs_edit_undo(log, &t));
    CHECK(gs_track_hash(&t) == after_ground);
    CHECK(gs_edit_undo(log, &t));
    CHECK(gs_track_hash(&t) == empty);
    CHECK(!gs_edit_can_undo(log));

    // And forwards through it again.
    CHECK(gs_edit_redo(log, &t));
    CHECK(gs_track_hash(&t) == after_ground);
    CHECK(gs_edit_redo(log, &t));
    CHECK(gs_track_hash(&t) == after_gate);
}

TEST(a_gate_placed_inside_a_stroke_undoes_with_the_stroke) {
    // Placing a gate and shaping the ground under it in one action comes back
    // as one action.
    static uint8_t room[65536];
    gs_edit_log *log = (gs_edit_log *)room;
    gs_edit_log_init(log, 256);

    static gs_track t;
    gs_track_init(&t, 32, 16, GS_SURF_PAVEMENT);
    uint64_t before = gs_track_hash(&t);

    gs_edit_begin(log);
    gs_edit_corner(log, &t, 6, 6, GS_INT(1));
    gs_edit_add_gate(log, &t, GS_INT(6), GS_INT(6), 0, GS_INT(4));
    gs_edit_corner(log, &t, 7, 6, GS_INT(1));
    gs_edit_end(log);

    CHECK(gs_track_hash(&t) != before);
    CHECK(gs_edit_undo(log, &t));
    CHECK(gs_track_hash(&t) == before);
    CHECK(t.gate_count == 0);
    CHECK(!gs_edit_can_undo(log));
}

// ---------------------------------------------------------------------------
// What surrounds a track
// ---------------------------------------------------------------------------

TEST(leaving_a_track_costs_time_before_it_costs_the_race) {
    // **The decision this pins.** Off the authored tiles there is a run-off and
    // then a drop: a mistake is expensive and recoverable, and only carrying on
    // outwards is fatal. The alternative that was there before was neither -
    // nothing was drawn outside the track and the physics clamped to the edge
    // tile, so a player saw a cliff and drove on an invisible plain for ever.
    static gs_track t;
    gs_track_init(&t, 40, 20, GS_SURF_PAVEMENT);

    // On the track: whatever it was painted with.
    CHECK(gs_track_surface(&t, GS_INT(20), GS_INT(10)) == GS_SURF_PAVEMENT);
    CHECK(gs_track_outside(&t, GS_INT(20), GS_INT(10)) == 0);

    // Just off it: run-off, and level with the edge it left. A step here would
    // launch a car that ran wide instead of slowing it.
    gs_fix edge = gs_track_height(&t, GS_INT(40), GS_INT(10));
    CHECK(gs_track_surface(&t, GS_INT(41), GS_INT(10)) == GS_RUNOFF_SURFACE);
    CHECK(gs_track_height(&t, GS_INT(41), GS_INT(10)) == edge);
    CHECK(gs_track_height(&t, GS_INT(40 + GS_RUNOFF_TILES), GS_INT(10)) == edge);

    // Past the shoulder: falling, and falling faster the further out.
    gs_fix near = gs_track_height(&t, GS_INT(40 + GS_RUNOFF_TILES + 2), GS_INT(10));
    gs_fix far = gs_track_height(&t, GS_INT(40 + GS_RUNOFF_TILES + 6), GS_INT(10));
    CHECK(near < edge);
    CHECK(far < near);

    // And the fall is steeper than anything can climb, so it is a drop and not
    // a hill somebody could drive back up.
    CHECK(GS_RUNOFF_FALL > GS_MAX_CLIMB);

    // The same on every side, so a track has a border and not a preference.
    CHECK(gs_track_surface(&t, -GS_ONE, GS_INT(10)) == GS_RUNOFF_SURFACE);
    CHECK(gs_track_surface(&t, GS_INT(20), -GS_ONE) == GS_RUNOFF_SURFACE);
    CHECK(gs_track_surface(&t, GS_INT(20), GS_INT(21)) == GS_RUNOFF_SURFACE);
}

TEST(the_run_off_is_a_thing_that_stops_you) {
    // A run-off works by drag, not by slipperiness. The first version used dust,
    // which is loose and has almost no rolling resistance - so a car that ran
    // wide kept every bit of its speed and sailed across to the drop. Sand is
    // the draggiest of the nine, and a car that brakes on reaching it stops
    // inside it.
    static gs_track t;
    gs_track_init(&t, 40, 20, GS_SURF_PAVEMENT);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(30), GS_INT(10), 0);

    // Up to speed on the road, then off the end with the brakes on.
    for (int i = 0; i < GS_TICK_HZ * 10 && w.car[0].x < GS_INT(39); i++) {
        gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, 0, 0, 0 };
        gs_world_step(&w, &t, in);
    }
    CHECK(gs_car_speed(&w.car[0]) > GS_INT(3));

    for (int i = 0; i < GS_TICK_HZ * 12; i++) {
        gs_input in[GS_MAX_CARS] = { GS_IN_BRAKE, 0, 0, 0 };
        gs_world_step(&w, &t, in);
        if (gs_car_speed(&w.car[0]) < GS_RATIO(20, 100)) break;
    }

    // Stopped, and stopped on the shoulder rather than over the edge of it.
    CHECK(gs_car_speed(&w.car[0]) < GS_HALF);
    CHECK(!w.car[0].wrecked);
    CHECK(gs_track_outside(&t, w.car[0].x, w.car[0].y) <
          GS_INT(GS_RUNOFF_TILES));
}

TEST(a_car_that_keeps_going_over_the_edge_is_finished) {
    // The other half of the bargain: the shoulder forgives a mistake and the
    // drop does not forgive persistence. Without an ending out there a car would
    // fall for ever, and "leaving the track has a consequence" would be a
    // sentence about an animation.
    static gs_track t;
    gs_track_init(&t, 40, 20, GS_SURF_PAVEMENT);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(30), GS_INT(10), 0);

    // When it goes over, and how far down it was when it did.
    int over_at = -1, wrecked_at = -1;
    gs_fix depth_at_wreck = 0;
    for (int i = 0; i < GS_TICK_HZ * 40 && wrecked_at < 0; i++) {
        gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, 0, 0, 0 };
        gs_world_step(&w, &t, in);

        if (over_at < 0 &&
            gs_track_outside(&t, w.car[0].x, w.car[0].y) > GS_INT(GS_RUNOFF_TILES)) {
            over_at = i;
        }
        if (w.car[0].wrecked) {
            wrecked_at = i;
            depth_at_wreck = gs_track_height(&t, GS_INT(40), GS_INT(10)) - w.car[0].z;
        }
    }

    CHECK(w.car[0].wrecked);
    CHECK(w.car[0].damage == 255);
    CHECK(over_at >= 0);

    // **Finished at the lip, not at the bottom of a long fall.** Without a rule
    // saying when a car has gone, it simply falls until the drop bottoms out and
    // is then wrecked by the landing - which looks the same from the outside and
    // is a car falling for several seconds first. Inside a second of going over,
    // and a few tiles down rather than dozens.
    CHECK(wrecked_at - over_at < GS_TICK_HZ);
    CHECK(depth_at_wreck < GS_INT(12));

    // It stops rather than falling for ever, and it stays stopped.
    gs_fix rest_x = w.car[0].x, rest_y = w.car[0].y, rest_z = w.car[0].z;
    for (int i = 0; i < GS_TICK_HZ * 5; i++) {
        gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, 0, 0, 0 };
        gs_world_step(&w, &t, in);
    }
    CHECK(w.car[0].x == rest_x);
    CHECK(w.car[0].y == rest_y);
    CHECK(w.car[0].z == rest_z);
}

TEST(the_ground_outside_a_track_is_a_number_and_not_an_overflow) {
    // A drop that kept falling with distance overflowed Q16.16 for a car thrown
    // a few thousand tiles off the map at low gravity, which is not a
    // hypothetical - it is what the roster sweep did the first time this
    // existed, and the ground came back *above* the car.
    static gs_track t;
    gs_track_init(&t, 40, 20, GS_SURF_PAVEMENT);

    gs_fix edge = gs_track_height(&t, GS_INT(40), GS_INT(10));

    static const gs_fix miles[] = {
        GS_INT(100), GS_INT(1000), GS_INT(10000), GS_INT(30000),
    };
    // Bounded against an absolute figure rather than against the constant that
    // does the bounding: a test written in terms of GS_RUNOFF_FLOOR passes no
    // matter what that is set to, including infinity, which is the case it
    // exists to catch. A thousand tiles is far deeper than any track is tall.
    for (size_t i = 0; i < sizeof miles / sizeof miles[0]; i++) {
        gs_fix z = gs_track_height(&t, miles[i], GS_INT(10));
        CHECK(z < edge);                     // still below, never wrapped above
        CHECK(z > edge - GS_INT(1000));      // and not somewhere absurd

        gs_fix back = gs_track_height(&t, -miles[i], GS_INT(10));
        CHECK(back < edge);
        CHECK(back > edge - GS_INT(1000));
    }
}

// ---------------------------------------------------------------------------
// The landing arc
// ---------------------------------------------------------------------------

// A ramp to fly off: flat, then a rise, then flat again a long way further on.
static void gs_ramp_track(gs_track *t) {
    gs_track_init(t, 64, 16, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= t->h; y++) {
        for (uint8_t x = 0; x <= t->w; x++) {
            gs_fix z = 0;
            if (x >= 14 && x < 20) z = (gs_fix)((int64_t)GS_INT(2) * (x - 14) / 6);
            else if (x >= 20) z = 0;
            gs_track_set_corner(t, x, y, z);
        }
    }
}

TEST(a_car_lands_where_the_arc_said_it_would) {
    // **The arc is a promise and it has to be kept**, or it is worse than no arc
    // because it is believed. It is computed by running the simulation rather
    // than by solving a parabola - an airborne car has drag on it and gravity is
    // sampled per tile - so this checks that the promise and the race are the
    // same code and not two descriptions of it.
    //
    // At three gravities, because a formula that quietly assumed Earth would
    // pass at one and fail at the others, and the whole dial is the game.
    static const gs_fix dial[3] = {
        GS_RATIO(17, 100), GS_ONE, GS_RATIO(253, 100),   // Moon, Earth, Jupiter
    };

    for (int g = 0; g < 3; g++) {
        static gs_track t;
        gs_ramp_track(&t);

        gs_world w;
        gs_world_init(&w, dial[g]);
        gs_world_add_car(&w, &t, GS_VEH_SPRINT_CAR, GS_INT(4), GS_INT(8), 0);

        // Flat out at the ramp until the wheels leave the ground.
        int flown = 0;
        for (int i = 0; i < GS_TICK_HZ * 40; i++) {
            gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, 0, 0, 0 };
            gs_world_step(&w, &t, in);
            if (!w.car[0].grounded) { flown = 1; break; }
        }
        CHECK(flown);
        if (!flown) continue;

        static gs_arc arc;
        uint8_t n = gs_world_arc(&w, &t, 0, &arc);
        CHECK(n > 1);
        if (n < 2) continue;

        // The car itself, flying the same flight it was just asked about.
        int landed = 0;
        for (int i = 0; i < GS_TICK_HZ * 40; i++) {
            gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, 0, 0, 0 };
            gs_world_step(&w, &t, in);
            if (w.car[0].grounded) { landed = 1; break; }
        }
        CHECK(landed);

        // **Exactly**, not nearly. Both are the same integer arithmetic on the
        // same state, so anything other than an exact match means the arc was
        // predicting a different world from the one the car is in.
        CHECK(w.car[0].x == arc.x[n - 1]);
        CHECK(w.car[0].y == arc.y[n - 1]);
        CHECK(w.car[0].z == arc.z[n - 1]);

        // And it is a flight rather than a full stop: the landing is somewhere
        // else, and further on.
        CHECK(arc.x[n - 1] > arc.x[0]);
        CHECK(arc.landed);
    }
}

TEST(a_long_flight_is_drawn_coarsely_rather_than_cut_off) {
    // **An arc that ran out of room used to stop half way**, and a path that
    // stops still ends somewhere - which is read as the landing. A jump longer
    // than the array is now sampled more widely instead, so the last point is
    // the touchdown however long the car is up there.
    static gs_track t;
    gs_track_init(&t, 64, 16, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= t.h; y++) {
        for (uint8_t x = 0; x <= t.w; x++) {
            gs_track_set_corner(&t, x, y, x < 8 ? GS_INT(30) : 0);
        }
    }
    gs_track_add_gate(&t, GS_INT(4), GS_INT(8), 0, GS_INT(5));

    // A long way up, at a sixth of a gravity: a flight of many seconds, far
    // beyond what the array holds one point per tick of.
    gs_world w;
    gs_world_init(&w, GS_RATIO(17, 100));
    gs_world_add_car(&w, &t, GS_VEH_SPRINT_CAR, GS_INT(4), GS_INT(8), 0);

    int flown = 0;
    for (int i = 0; i < GS_TICK_HZ * 40; i++) {
        gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, 0, 0, 0 };
        gs_world_step(&w, &t, in);
        if (!w.car[0].grounded) { flown = 1; break; }
    }
    CHECK(flown);
    if (!flown) return;

    static gs_arc arc;
    uint8_t n = gs_world_arc(&w, &t, 0, &arc);
    CHECK(n > 1);
    CHECK(n <= GS_ARC_MAX);
    CHECK(arc.landed);

    int landed = 0;
    for (int i = 0; i < GS_TICK_HZ * 40; i++) {
        gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, 0, 0, 0 };
        gs_world_step(&w, &t, in);
        if (w.car[0].grounded) { landed = 1; break; }
    }
    CHECK(landed);

    // The flight really was longer than the array, so this is the coarse path
    // and not the easy case wearing its name.
    CHECK(arc.x[n - 1] == w.car[0].x);
    CHECK(arc.y[n - 1] == w.car[0].y);
    CHECK(arc.z[n - 1] == w.car[0].z);
}

TEST(there_is_no_arc_for_a_car_on_the_ground) {
    // A car that is not going anywhere has nowhere predicted for it, and a
    // trajectory drawn from a parked car would be a line to where it is
    // standing.
    static gs_track t;
    gs_ramp_track(&t);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(4), GS_INT(8), 0);

    static gs_arc arc;
    CHECK(gs_world_arc(&w, &t, 0, &arc) == 0);
    CHECK(arc.count == 0);

    // Nor for a car that is not in the race at all.
    CHECK(gs_world_arc(&w, &t, 3, &arc) == 0);
}

TEST(asking_where_a_car_will_land_does_not_move_it) {
    // The arc runs the simulation forward. If it ran the *real* one, looking at
    // where you were going to land would send you there.
    static gs_track t;
    gs_ramp_track(&t);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_SPRINT_CAR, GS_INT(4), GS_INT(8), 0);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(4), GS_INT(11), 0);

    for (int i = 0; i < GS_TICK_HZ * 40 && w.car[0].grounded; i++) {
        gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, GS_IN_ACCEL, 0, 0 };
        gs_world_step(&w, &t, in);
    }
    CHECK(!w.car[0].grounded);

    uint64_t before = gs_world_hash(&w);

    static gs_arc arc;
    for (int k = 0; k < 5; k++) CHECK(gs_world_arc(&w, &t, 0, &arc) > 0);

    CHECK(gs_world_hash(&w) == before);

    // And the same question twice gets the same answer, which a design tool
    // that disagreed with itself between frames would not manage.
    static gs_arc again;
    CHECK(gs_world_arc(&w, &t, 0, &again) == arc.count);
    for (uint8_t i = 0; i < arc.count; i++) {
        CHECK(again.x[i] == arc.x[i]);
        CHECK(again.y[i] == arc.y[i]);
    }
}

// ---------------------------------------------------------------------------
// Wreckage
// ---------------------------------------------------------------------------

TEST(a_wreck_takes_up_more_room_than_the_car_it_used_to_be) {
    // Debris is a spread of parts, not a parked car. The two radii are the
    // reason a wreck is an obstacle rather than a parking space - and the
    // renderer draws the wreck at the size the physics uses, so what is in the
    // way looks the size of the thing that is in the way.
    CHECK(GS_WRECK_RADIUS > GS_CAR_RADIUS);

    // Half again, and no more: debris that swallowed a lane would decide races.
    CHECK(GS_WRECK_RADIUS < gs_fix_mul(GS_CAR_RADIUS, GS_INT(2)));

    // The size difference is real and not decorative: a car passing at an offset
    // that clears a live car does not clear a wreck.
    static gs_track t;
    gs_track_init(&t, 64, 24, GS_SURF_PAVEMENT);

    gs_fix gap = GS_CAR_RADIUS + GS_WRECK_RADIUS - GS_RATIO(5, 100);

    for (int wrecked = 0; wrecked < 2; wrecked++) {
        gs_world w;
        gs_world_init(&w, GS_ONE);
        gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(4), GS_INT(12), 0);
        gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(20), GS_INT(12) + gap, 0);
        if (wrecked) { w.car[1].damage = 255; w.car[1].wrecked = true; }

        for (int i = 0; i < GS_TICK_HZ * 8; i++) {
            gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, 0, 0, 0 };
            gs_world_step(&w, &t, in);
        }

        // At this offset a live car is missed and a wreck is not.
        if (wrecked) CHECK(w.car[0].y != GS_INT(12));
        else         CHECK(w.car[0].y == GS_INT(12));
    }
}

// ---------------------------------------------------------------------------
// The grounds
// ---------------------------------------------------------------------------

// What a surface is like to drive on, measured rather than asserted: how fast a
// car ends up going flat out, how long it takes to get to three tiles a second,
// how fast a full-lock circle settles at, and how much that circle changes once
// it has been driven into the ground.
typedef struct gs_feel {
    gs_fix top;
    int32_t to_speed;   // ticks
    gs_fix corner;
    int32_t wear;       // per cent change in the circle, once worn
} gs_feel;

static gs_track gs_ground;

static void gs_measure_ground(gs_surface s, gs_feel *out) {
    gs_track_init(&gs_ground, 64, 64, s);

    // Flat out until the speed settles or the far side arrives, whichever comes
    // first. **Stopping at the edge matters**: the ground outside a track is a
    // run-off and then a drop, so a car allowed to run on measures the sand it
    // ended up in rather than the surface it was asked about.
    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &gs_ground, GS_VEH_STOCK_CAR, GS_INT(2), GS_INT(6), 0);
    for (int i = 0; i < GS_TICK_HZ * 25; i++) {
        gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, 0, 0, 0 };
        gs_world_step(&w, &gs_ground, in);
        if (w.car[0].x > GS_INT(gs_ground.w) - GS_INT(4)) break;
    }
    out->top = gs_car_speed(&w.car[0]);

    gs_world a;
    gs_world_init(&a, GS_ONE);
    gs_world_add_car(&a, &gs_ground, GS_VEH_STOCK_CAR, GS_INT(2), GS_INT(6), 0);
    out->to_speed = GS_TICK_HZ * 30;
    for (int i = 0; i < GS_TICK_HZ * 30; i++) {
        gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, 0, 0, 0 };
        gs_world_step(&a, &gs_ground, in);
        if (gs_car_speed(&a.car[0]) >= GS_INT(3)) { out->to_speed = i; break; }
    }

    // A circle, held long enough to grind the tiles under it flat.
    gs_world c;
    gs_world_init(&c, GS_ONE);
    gs_world_add_car(&c, &gs_ground, GS_VEH_STOCK_CAR, GS_INT(32), GS_INT(32), 0);
    gs_fix early = 0;
    for (int i = 0; i < GS_TICK_HZ * 150; i++) {
        gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL | GS_IN_LEFT, 0, 0, 0 };
        gs_world_step(&c, &gs_ground, in);
        if (i == GS_TICK_HZ * 20) early = gs_car_speed(&c.car[0]);
    }
    out->corner = early;
    gs_fix late = gs_car_speed(&c.car[0]);
    out->wear = early > GS_RATIO(1, 100)
                    ? (int32_t)(((int64_t)(late - early) * 100) / early)
                    : 0;
}

// How far apart two of them are, as a percentage, on whichever of the four they
// differ by most.
static int32_t gs_ground_gap(const gs_feel *a, const gs_feel *b) {
    int32_t best = 0;
    int64_t pairs[3][2] = {
        { a->top, b->top }, { a->to_speed, b->to_speed }, { a->corner, b->corner },
    };
    for (int i = 0; i < 3; i++) {
        int64_t mean = (pairs[i][0] + pairs[i][1]) / 2;
        if (mean <= 0) continue;
        int64_t d = pairs[i][0] - pairs[i][1];
        if (d < 0) d = -d;
        int32_t pct = (int32_t)(d * 100 / mean);
        if (pct > best) best = pct;
    }
    int32_t dw = a->wear - b->wear;
    if (dw < 0) dw = -dw;
    if (dw > best) best = dw;
    return best;
}

TEST(every_ground_is_a_different_thing_to_drive_on) {
    // **The rule that makes a surface a surface and not a colour.** The gravity
    // dial names eight worlds; grounds for them are worth having only if the car
    // behaves differently on each, and a set where two of them drive the same is
    // a set with a spare entry in it.
    //
    // Measured on four counts, because two grounds can arrive at the same lap
    // time by being bad at different things - and because how a surface changes
    // under use is a difference a fresh-surface measurement cannot see at all.
    static gs_feel feel[GS_SURF_COUNT];
    for (uint8_t s = 0; s < GS_SURF_COUNT; s++) {
        gs_measure_ground((gs_surface)s, &feel[s]);

        // Every one of them is driveable: a ground nobody can move on is not a
        // ground, it is a wall painted on the floor.
        CHECK(feel[s].top > GS_INT(2));
        CHECK(feel[s].to_speed < GS_TICK_HZ * 20);
    }

    for (uint8_t i = 0; i < GS_SURF_COUNT; i++) {
        for (uint8_t j = (uint8_t)(i + 1); j < GS_SURF_COUNT; j++) {
            // A sixth apart on *something*. Two grounds closer than that are one
            // ground with two names.
            CHECK(gs_ground_gap(&feel[i], &feel[j]) >= 15);
        }
    }
}

TEST(a_ground_is_named_once_and_the_name_is_the_surface_table) {
    // Nine surfaces and a hard-coded list of three names was a combo box reading
    // past the end of its own array. Nothing may keep a second list.
    for (uint8_t s = 0; s < GS_SURF_COUNT; s++) {
        CHECK(gs_surfaces[s].name != nullptr);
        CHECK(gs_surfaces[s].name[0] != '\0');
        for (uint8_t o = (uint8_t)(s + 1); o < GS_SURF_COUNT; o++) {
            CHECK(strcmp(gs_surfaces[s].name, gs_surfaces[o].name) != 0);
        }
    }
}

TEST(a_saved_track_keeps_the_ground_it_was_painted_with) {
    // The stored value is the enum, so appending is safe and renumbering is not:
    // a surface that moved would silently change the ground under every track
    // anybody had built.
    static gs_track t;
    gs_track_init(&t, 16, 16, GS_SURF_PAVEMENT);
    for (uint8_t s = 0; s < GS_SURF_COUNT; s++) {
        gs_track_set_surface(&t, (uint8_t)s, 0, (gs_surface)s);
    }

    static uint8_t buf[GS_TRACK_TILES * 4 + 4096];
    size_t n = gs_track_serialize(&t, buf, sizeof buf);
    CHECK(n > 0);

    static gs_track back;
    CHECK(gs_track_deserialize(&back, buf, n));
    for (uint8_t s = 0; s < GS_SURF_COUNT; s++) {
        CHECK(gs_track_surface(&back, GS_INT(s) + GS_HALF, GS_HALF) == (gs_surface)s);
    }

    // And the first three are where they always were, because tracks in the
    // world were saved against those numbers.
    CHECK(GS_SURF_PAVEMENT == 0);
    CHECK(GS_SURF_DIRT == 1);
    CHECK(GS_SURF_ICE == 2);
}

// ---------------------------------------------------------------------------
// Where everybody is in the race
// ---------------------------------------------------------------------------

TEST(the_leader_is_whoever_is_furthest_round_the_route) {
    static gs_track t;
    gs_track_init(&t, 48, 16, GS_SURF_PAVEMENT);
    gs_track_add_gate(&t, GS_INT(4), GS_INT(8), 0, GS_INT(5));
    gs_track_add_gate(&t, GS_INT(40), GS_INT(8), 0, GS_INT(5));

    gs_world w;
    gs_world_init(&w, GS_ONE);
    for (int i = 0; i < 3; i++) {
        gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(4), GS_INT(6 + 2 * i), 0);
    }

    // All three on the first leg, spread along it. **Distance along the leg
    // decides it**, which is what makes a position change when the racing does
    // rather than twice a lap when somebody crosses a line.
    for (int i = 0; i < 3; i++) w.car[i].next_gate = 1;
    w.car[0].x = GS_INT(10);
    w.car[1].x = GS_INT(30);
    w.car[2].x = GS_INT(20);

    CHECK(gs_world_place(&w, &t, 1) == 1);
    CHECK(gs_world_place(&w, &t, 2) == 2);
    CHECK(gs_world_place(&w, &t, 0) == 3);

    // An overtake, and nothing else touched.
    w.car[0].x = GS_INT(36);
    CHECK(gs_world_place(&w, &t, 0) == 1);
    CHECK(gs_world_place(&w, &t, 1) == 2);

    // Round the far marker and heading back beats still crawling towards it,
    // however close to it the other car is. On an out-and-back the far gate is
    // where a lap is counted, so a car that has turned for home has one - which
    // is why this sets both fields and not just the gate: the pair is the state
    // the simulation produces, and testing one it never does proves nothing.
    w.car[2].laps = 1;
    w.car[2].next_gate = 0;
    w.car[2].x = GS_INT(39);
    CHECK(gs_world_place(&w, &t, 2) == 1);
    CHECK(gs_world_place(&w, &t, 0) == 2);

    // And a whole lap beats being most of the way round one.
    w.car[1].laps = 1;
    CHECK(gs_world_place(&w, &t, 1) == 1);
    CHECK(gs_world_place(&w, &t, 2) == 2);

    // **Level across the road is level.**
    //
    // A gate is a line and a car crosses it wherever it likes, so a straight
    // line to the gate's centre point is shorter for the car in the middle of
    // the road than for the one beside it on the outside - and that decided the
    // order. Four cars sitting level on a standing grid came out third, first,
    // second and fourth, which is what the HUD told the player on pole before
    // anybody had moved: **position 3 of 4**.
    //
    // Found by looking at a screenshot of the race, not by any test: every one
    // of them put its cars at different distances along the track.
    gs_world grid;
    gs_world_init(&grid, GS_ONE);
    for (uint8_t i = 0; i < GS_MAX_CARS; i++) {
        gs_fix sx = 0, sy = 0;
        gs_angle facing = 0;
        gs_track_grid(&t, i, &sx, &sy, &facing);
        gs_world_add_car(&grid, &t, (uint8_t)GS_VEH_STOCK_CAR, sx, sy, facing);
    }
    for (uint8_t i = 0; i < GS_MAX_CARS; i++) {
        const uint8_t place = gs_world_place(&grid, &t, i);
        if (place != i + 1u) {
            printf("  GRID car %u on the standing grid is %u of %u\n", i, place,
                   grid.car_count);
        }
        CHECK(place == i + 1u);
    }

    // **And level along a leg is level too**, which is the same claim where the
    // measurement is not saturated: four cars abreast, a third of the way to
    // the far gate.
    gs_world abreast;
    gs_world_init(&abreast, GS_ONE);
    for (uint8_t i = 0; i < GS_MAX_CARS; i++) {
        gs_world_add_car(&abreast, &t, (uint8_t)GS_VEH_STOCK_CAR,
                         GS_INT(14), GS_INT(4) + GS_INT(2) * i, 0);
        abreast.car[i].next_gate = 1;
    }
    for (uint8_t i = 0; i < GS_MAX_CARS; i++) {
        CHECK(gs_world_place(&abreast, &t, i) == i + 1u);
    }

    // Moving one *along* the road changes the order - so this measures
    // progress rather than merely ignoring where anybody is.
    abreast.car[GS_MAX_CARS - 1].x += GS_INT(3);
    CHECK(gs_world_place(&abreast, &t, GS_MAX_CARS - 1) == 1);

    // While moving one *across* the road changes nothing at all, which is the
    // fault this pins.
    gs_world beside = abreast;
    beside.car[0].y += GS_INT(3);
    beside.car[1].y -= GS_INT(1);
    for (uint8_t i = 0; i < GS_MAX_CARS; i++) {
        CHECK(gs_world_place(&beside, &t, i) == gs_world_place(&abreast, &t, i));
    }
}

TEST(every_car_has_a_place_and_no_two_share_one) {
    // Ties are the interesting case: three cars on the same spot still have a
    // first, a second and a third, because "joint second" is not something a
    // HUD can show and not something a race means.
    static gs_track t;
    gs_track_init(&t, 48, 16, GS_SURF_PAVEMENT);
    gs_track_add_gate(&t, GS_INT(4), GS_INT(8), 0, GS_INT(5));
    gs_track_add_gate(&t, GS_INT(40), GS_INT(8), 0, GS_INT(5));

    gs_world w;
    gs_world_init(&w, GS_ONE);
    for (int i = 0; i < 4; i++) {
        gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(20), GS_INT(8), 0);
    }

    bool seen[GS_MAX_CARS + 1] = { false };
    for (uint8_t i = 0; i < 4; i++) {
        uint8_t place = gs_world_place(&w, &t, i);
        CHECK(place >= 1 && place <= 4);
        CHECK(!seen[place]);
        seen[place] = true;
    }
}

TEST(a_car_that_has_finished_keeps_the_place_it_finished_in) {
    // Crossing the line is when a result stops moving. Without this, a winner
    // who parks is overtaken on the HUD by somebody still on their last lap,
    // which is not what happened.
    static gs_track t;
    gs_track_init(&t, 48, 16, GS_SURF_PAVEMENT);
    gs_track_add_gate(&t, GS_INT(4), GS_INT(8), 0, GS_INT(5));
    gs_track_add_gate(&t, GS_INT(40), GS_INT(8), 0, GS_INT(5));

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(4), GS_INT(8), 0);
    gs_world_add_car(&w, &t, GS_VEH_STOCK_CAR, GS_INT(4), GS_INT(10), 0);

    // One home, one still going and about to be further round than the winner
    // ever was.
    w.car[0].finish_tick = 1000;
    w.car[0].laps = 2;
    w.car[1].laps = 9;
    w.car[1].next_gate = 1;
    w.car[1].x = GS_INT(39);

    CHECK(gs_world_place(&w, &t, 0) == 1);
    CHECK(gs_world_place(&w, &t, 1) == 2);

    // And two finishers are ordered by the clock, not by where they parked.
    w.car[1].finish_tick = 900;
    CHECK(gs_world_place(&w, &t, 1) == 1);
    CHECK(gs_world_place(&w, &t, 0) == 2);
}

// ---------------------------------------------------------------------------
// The track generator
// ---------------------------------------------------------------------------

static gs_track gs_gen_a;
static gs_track gs_gen_b;
static gs_analysis gs_gen_look;
static gs_fix slot_x[GS_TRACK_GRID];
static gs_fix slot_y[GS_TRACK_GRID];

TEST(a_seed_always_generates_the_same_track) {
    // The seed is the track's name as far as sharing is concerned: two people
    // with the same number have to be driving the same ground, or a generated
    // track cannot be recommended, recorded against or raced on together.
    gs_generate(&gs_gen_a, 12345);
    gs_generate(&gs_gen_b, 12345);
    CHECK(gs_track_hash(&gs_gen_a) == gs_track_hash(&gs_gen_b));

    gs_generate(&gs_gen_b, 12346);
    CHECK(gs_track_hash(&gs_gen_a) != gs_track_hash(&gs_gen_b));
}

TEST(a_seed_always_generates_the_same_name) {
    char one[32], two[32];
    gs_generate_name(one, sizeof one, 999);
    gs_generate_name(two, sizeof two, 999);
    CHECK(strcmp(one, two) == 0);
    CHECK(strlen(one) > 2);

    gs_generate_name(two, sizeof two, 1000);
    CHECK(strcmp(one, two) != 0);
}

TEST(every_generated_track_has_terrain_on_it) {
    // A generator that rolled a zero height would produce a flat field, which
    // is completable, differently hashed from its neighbours and worthless.
    for (uint32_t seed = 1; seed <= 24; seed++) {
        gs_generate(&gs_gen_a, seed * 7919u);

        bool raised = false;
        for (uint8_t y = 0; y <= gs_gen_a.h && !raised; y++) {
            for (uint8_t x = 0; x <= gs_gen_a.w; x++) {
                if (gs_track_corner_at(&gs_gen_a, x, y) != 0) { raised = true; break; }
            }
        }
        CHECK(raised);
    }
}

TEST(no_generated_slope_is_steeper_than_a_car_can_climb) {
    // gs_sim.c stops a car on ground steeper than GS_MAX_CLIMB, so a generator
    // that picks a height and a ramp independently eventually builds a wall.
    // The ramps are derived from the heights precisely so this holds.
    for (uint32_t seed = 1; seed <= 24; seed++) {
        gs_generate(&gs_gen_a, seed * 7919u);

        for (uint8_t y = 0; y <= gs_gen_a.h; y++) {
            for (uint8_t x = 1; x <= gs_gen_a.w; x++) {
                gs_fix step = gs_track_corner_at(&gs_gen_a, x, y) -
                              gs_track_corner_at(&gs_gen_a, (uint8_t)(x - 1), y);
                CHECK(gs_fix_abs(step) < GS_MAX_CLIMB);
            }
        }
    }
}

TEST(a_generated_race_can_actually_be_finished) {
    // **The check that was missing, twice.** "Completable" was `laps > 0` after
    // an AI drive - and on a circuit that is true the instant the car leaves
    // the grid and crosses the start line, which is a few car lengths. So every
    // track reported completable however impossible the rest of it was, and two
    // rounds of unraceable tracks went out under a green tick that meant almost
    // nothing.
    //
    // What a player does is *finish*, so that is what is asked here: a whole
    // lap of a loop, or the arrival at the end of a path, with the race over
    // and the car timed.
    // **And from every slot on the grid**, because an opponent starts wherever
    // it is put. The slots are not the same place: they are staggered back from
    // the line and across it, so the car in the last one has a different corner
    // to make and different scenery to make it around. A driver that only
    // works from pole is a driver that works in a demo.
    int raced = 0;
    for (uint32_t seed = 1; seed <= 12; seed++) {
        gs_generate(&gs_gen_a, seed * 7919u);

        for (uint8_t slot = 0; slot < GS_MAX_CARS; slot++) {
            gs_world w;
            gs_world_init(&w, GS_ONE);
            gs_world_set_mode(&w, GS_MODE_RACE);
            gs_world_set_laps(&w, 1);

            gs_fix sx = 0, sy = 0;
            gs_angle facing = 0;
            gs_track_grid(&gs_gen_a, slot, &sx, &sy, &facing);
            gs_world_add_car(&w, &gs_gen_a, (uint8_t)GS_VEH_STOCK_CAR, sx, sy, facing);

            CHECK(gs_world_laps_needed(&w, &gs_gen_a) == 1);

            for (uint32_t i = 0; i < (uint32_t)GS_TICK_HZ * 180u; i++) {
                gs_input in[GS_MAX_CARS] = { gs_ai_drive(&w, &gs_gen_a, 0), 0, 0, 0 };
                gs_world_step(&w, &gs_gen_a, in);
                if (w.car[0].finish_tick != 0) break;
            }

            if (w.car[0].finish_tick == 0) {
                printf("  STUCK seed %u from slot %u: %d laps, at %.1f,%.1f\n",
                       seed, slot, gs_car_laps_done(&gs_gen_a, &w.car[0]),
                       (double)w.car[0].x / 65536.0,
                       (double)w.car[0].y / 65536.0);
            }
            CHECK(w.car[0].finish_tick != 0);
            CHECK(gs_car_laps_done(&gs_gen_a, &w.car[0]) >= 1);
            raced++;
        }
    }

    printf("  AI %d races finished: twelve tracks from every grid slot\n", raced);
    CHECK(raced == 12 * GS_MAX_CARS);
}

TEST(every_gate_is_wider_than_the_road_it_crosses) {
    // **"I drove across the finish line and the game did not recognise it."**
    //
    // A gate is finite across its line - that is what makes it a gate rather
    // than a tripwire across the world - and the generator was laying gates
    // three and four tiles either side of a road that is four. So a car keeping
    // to the outside of its own road went *past* a checkpoint without crossing
    // it, `next_gate` never advanced, and the finish line then did nothing when
    // it was reached, because gates count in order. The track was completable
    // and the analyser said so: the AI aims at gate centres, so the AI never
    // missed one and nothing noticed.
    for (uint32_t seed = 1; seed <= 40; seed++) {
        gs_generate(&gs_gen_a, seed * 7919u);
        CHECK(gs_gen_a.gate_count >= 2);

        for (uint8_t i = 0; i < gs_gen_a.gate_count; i++) {
            CHECK(gs_gen_a.gate[i].half_width >= GS_INT(GS_GEN_ROAD));
        }
    }
}

TEST(a_lap_of_a_loop_is_a_lap_and_arriving_ends_a_path) {
    // **What a lap means depends on the kind of route**, and getting it wrong
    // ends a three-lap race after two. On a loop the start line is the finish
    // line, so a car crosses it once on the way out of the grid - that crossing
    // is the run up to the line and not a lap anybody drove. On a path there
    // are no laps at all: there is a start at one end and a finish at the
    // other, and arriving is the whole race.
    static gs_track loop, path;

    // Two gates far apart, so a car driving straight crosses both.
    gs_track_init(&loop, 40, 12, GS_SURF_PAVEMENT);
    gs_track_add_gate(&loop, GS_INT(10), GS_INT(6), 0, GS_INT(5));
    gs_track_add_gate(&loop, GS_INT(30), GS_INT(6), 0, GS_INT(5));
    loop.route = (uint8_t)GS_ROUTE_CIRCUIT;

    path = loop;
    path.route = (uint8_t)GS_ROUTE_SPRINT;

    // A loop needs the laps it was asked for; a path needs one arrival however
    // many laps the setting says.
    CHECK(gs_track_finish_gate(&loop) == 0);
    CHECK(gs_track_finish_gate(&path) == 1);

    for (int which = 0; which < 2; which++) {
        const gs_track *t = which == 0 ? &loop : &path;

        gs_world w;
        gs_world_init(&w, GS_ONE);
        gs_world_set_mode(&w, GS_MODE_RACE);
        gs_world_set_laps(&w, 3);
        gs_world_add_car(&w, t, GS_VEH_STOCK_CAR, GS_INT(4), GS_INT(6), 0);

        CHECK(gs_world_laps_needed(&w, t) == (which == 0 ? 3 : 1));
        CHECK(gs_car_laps_done(t, &w.car[0]) == 0);

        // Straight down the middle and back, for as long as it takes.
        gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, 0, 0, 0 };
        for (int i = 0; i < GS_TICK_HZ * 90 && !w.over; i++) {
            // Turn round at each end, so a loop can be lapped on a strip.
            if (w.car[0].x > GS_INT(36)) in[0] = GS_IN_ACCEL | GS_IN_LEFT;
            else if (w.car[0].x < GS_INT(4)) in[0] = GS_IN_ACCEL | GS_IN_LEFT;
            else in[0] = GS_IN_ACCEL;
            gs_world_step(&w, t, in);
        }

        // A path is finished by arriving once. Whatever the loop managed on a
        // strip of ground, it is never finished by fewer laps than it asked
        // for - which is the fault this pins.
        if (which == 1) {
            CHECK(w.car[0].finish_tick != 0);
            CHECK(gs_car_laps_done(t, &w.car[0]) >= 1);
        } else if (w.car[0].finish_tick != 0) {
            CHECK(gs_car_laps_done(t, &w.car[0]) >= 3);
        }
    }
}

TEST(a_part_dropped_on_a_track_undoes_in_one_step) {
    // **A part is a way of editing, not a second track format.** Everything a
    // piece does is corner moves, surface changes and gate placements grouped
    // into one transaction - so a track built out of the parts box is the same
    // file as one built with brushes, and one undo takes a whole piece back.
    static gs_track t;
    gs_track_init(&t, 40, 40, GS_SURF_DIRT);

    static uint8_t buf[262144];
    gs_edit_log *log = (gs_edit_log *)buf;
    gs_edit_log_init(log, 8192);

    static gs_track before;
    before = t;

    gs_part road = gs_part_default(GS_PART_STRAIGHT);
    CHECK(gs_part_place(log, &t, &road, 6, 20));

    // It did something, and that something was one action.
    CHECK(gs_track_hash(&t) != gs_track_hash(&before));
    CHECK(gs_edit_undo_depth(log) == 1);

    CHECK(gs_edit_undo(log, &t));
    CHECK(gs_track_hash(&t) == gs_track_hash(&before));
    CHECK(gs_edit_undo_depth(log) == 0);

    // And forward again, to exactly where it was.
    static gs_track after;
    CHECK(gs_edit_redo(log, &t));
    after = t;
    CHECK(gs_edit_undo(log, &t));
    CHECK(gs_edit_redo(log, &t));
    CHECK(gs_track_hash(&t) == gs_track_hash(&after));
}

TEST(a_road_part_is_level_across_its_width) {
    // The rule the generator carves by and the reason a part exists at all: a
    // car is never tipped sideways by the road it is on. Laid across ground
    // that is not flat, so what is measured is the part and not the start.
    static gs_track t;
    gs_track_init(&t, 40, 40, GS_SURF_DIRT);
    for (uint8_t y = 0; y <= t.h; y++) {
        for (uint8_t x = 0; x <= t.w; x++) {
            gs_track_set_corner(&t, x, y, GS_INT(x) / 4);
        }
    }

    static uint8_t buf[262144];
    gs_edit_log *log = (gs_edit_log *)buf;
    gs_edit_log_init(log, 8192);

    gs_part road = gs_part_default(GS_PART_STRAIGHT);
    road.width = 8;
    road.length = 12;
    CHECK(gs_part_place(log, &t, &road, 8, 20));

    // Across the road, a third of the way along it, every corner is the same
    // height as the middle.
    gs_fix mid = gs_track_corner_at(&t, 12, 20);
    for (int8_t d = -3; d <= 3; d++) {
        gs_fix at = gs_track_corner_at(&t, 12, (uint8_t)(20 + d));
        CHECK(at == mid);
    }

    // And the road took the surface it was made of, while the ground beside it
    // did not.
    CHECK(gs_track_surface(&t, GS_INT(12), GS_INT(20)) == GS_SURF_PAVEMENT);
    CHECK(gs_track_surface(&t, GS_INT(12), GS_INT(33)) == GS_SURF_DIRT);
}

TEST(a_start_line_is_where_a_race_begins_however_late_it_was_dropped) {
    // **Gate zero is where a race begins**, so a start line dropped after the
    // rest of the route has to become gate zero rather than the last thing on
    // it - otherwise building a track in the order the pieces occur to somebody
    // gives a track that starts in the middle of itself.
    static gs_track t;
    gs_track_init(&t, 40, 40, GS_SURF_PAVEMENT);

    static uint8_t buf[262144];
    gs_edit_log *log = (gs_edit_log *)buf;
    gs_edit_log_init(log, 8192);

    gs_part check = gs_part_default(GS_PART_CHECKPOINT);
    CHECK(gs_part_place(log, &t, &check, 20, 10));
    CHECK(gs_part_place(log, &t, &check, 30, 20));
    CHECK(t.gate_count == 2);

    gs_part start = gs_part_default(GS_PART_START);
    CHECK(gs_part_place(log, &t, &start, 8, 20));

    // Three gates, and the one that begins the race is at the front.
    CHECK(t.gate_count == 3);
    CHECK(t.gate[0].x == GS_INT(8));
    CHECK(t.gate[0].y == GS_INT(20));

    // A start and a finish make a path, and the finish is its last gate.
    CHECK(!gs_track_is_circuit(&t));
    gs_part finish = gs_part_default(GS_PART_FINISH);
    CHECK(gs_part_place(log, &t, &finish, 34, 20));
    CHECK(gs_track_finish_gate(&t) == t.gate_count - 1);
    CHECK(t.gate[gs_track_finish_gate(&t)].x == GS_INT(34));

    // And taking the start line back leaves the route as it was.
    CHECK(gs_edit_undo(log, &t));      // the finish
    CHECK(gs_edit_undo(log, &t));      // the start
    CHECK(t.gate_count == 2);
    CHECK(t.gate[0].x == GS_INT(20));
}

TEST(a_combined_line_makes_the_track_a_loop) {
    // **The case a loop needs**: one line that is the start and the finish
    // both. Dropping it says the track is a circuit, which is what makes gate
    // zero the gate that ends a lap - and dropping a separate start or finish
    // says it is a path again.
    static gs_track t;
    gs_track_init(&t, 40, 40, GS_SURF_PAVEMENT);

    static uint8_t buf[262144];
    gs_edit_log *log = (gs_edit_log *)buf;
    gs_edit_log_init(log, 8192);

    CHECK(!gs_track_is_circuit(&t));

    gs_part both = gs_part_default(GS_PART_START_FINISH);
    CHECK(gs_part_place(log, &t, &both, 20, 20));

    CHECK(gs_track_is_circuit(&t));
    CHECK(t.gate_count == 1);

    // One line doing both jobs: the gate that ends a lap is the gate it starts
    // on, which is only true of a loop.
    CHECK(gs_track_finish_gate(&t) == 0);

    // Undo takes the kind back with it, because what a track *is* belongs in
    // the history like everything else.
    CHECK(gs_edit_undo(log, &t));
    CHECK(!gs_track_is_circuit(&t));
    CHECK(t.gate_count == 0);

    // And a plain start line turns a loop back into a path.
    CHECK(gs_edit_redo(log, &t));
    CHECK(gs_track_is_circuit(&t));
    gs_part start = gs_part_default(GS_PART_START);
    CHECK(gs_part_place(log, &t, &start, 8, 20));
    CHECK(!gs_track_is_circuit(&t));
}

TEST(a_part_that_will_not_fit_changes_nothing) {
    // An edit that cannot be made must not half-happen: the rule the whole edit
    // layer is built on, and a part is forty tiles of it at once.
    static gs_track t;
    gs_track_init(&t, 20, 20, GS_SURF_PAVEMENT);

    static uint8_t buf[262144];
    gs_edit_log *log = (gs_edit_log *)buf;
    gs_edit_log_init(log, 8192);

    static gs_track before;
    before = t;

    gs_part road = gs_part_default(GS_PART_STRAIGHT);
    road.length = 18;
    CHECK(!gs_part_place(log, &t, &road, 18, 10));   // runs off the far edge
    CHECK(gs_track_hash(&t) == gs_track_hash(&before));
    CHECK(gs_edit_undo_depth(log) == 0);
}

TEST(a_generated_track_leaves_clear_ground_to_get_up_to_speed_on) {
    // A car starts still. Ground that rises steeply between the grid and the
    // first gate is arrived at too slowly to climb, and the track is then
    // unfinishable for a reason that has nothing to do with how it was shaped.
    //
    // **Along the route rather than along +x.** This used to walk eight tiles
    // in the +x direction from gate zero and require the ground to be dead
    // level, which was right only because every generated route ran left to
    // right across the field. Routes are loops and bends now, so the run-up is
    // walked the way the car actually goes: from where it is gridded, through
    // the line, in the direction the line faces.
    for (uint32_t seed = 1; seed <= 24; seed++) {
        gs_generate(&gs_gen_a, seed * 7919u);
        CHECK(gs_gen_a.gate_count >= 2);
        if (gs_gen_a.gate_count < 2) continue;

        gs_fix gx = 0, gy = 0;
        gs_angle facing = 0;
        gs_track_grid(&gs_gen_a, 0, &gx, &gy, &facing);

        gs_fix fx = gs_cos(facing), fy = gs_sin(facing);

        // A standing start has to cover the run-up and the first tiles past the
        // line, which is where the car is still slow.
        gs_fix was = gs_track_height(&gs_gen_a, gx, gy);
        for (int step = 1; step <= 16; step++) {
            gs_fix at = (gs_fix)((int64_t)GS_ONE * step / 2);
            gs_fix x = gx + gs_fix_mul(fx, at);
            gs_fix y = gy + gs_fix_mul(fy, at);

            gs_fix now = gs_track_height(&gs_gen_a, x, y);

            // Half a tile of travel, so half the climb a full tile may have -
            // and a good margin under even that, because this is the one place
            // on the track where the car has no speed to help it.
            gs_fix rise = now - was;
            CHECK(rise < GS_MAX_CLIMB / 3);
            was = now;
        }
    }
}

TEST(every_generated_track_can_be_got_round) {
    // **The fact the generator exists to keep.** A track nobody can finish goes
    // into the library exactly like one they can, and somebody picks it. Checked
    // by racing them rather than by looking at them; `gearstick_cli generate 50`
    // is the same sweep over a wider net.
    for (uint32_t seed = 1; seed <= 12; seed++) {
        gs_generate(&gs_gen_a, seed * 7919u);
        CHECK(gs_track_validate(&gs_gen_a).problem == GS_TRACK_OK);
        gs_analyse(&gs_gen_a, gs_analyse_seconds(&gs_gen_a), &gs_gen_look);
        CHECK(gs_gen_look.completable);
    }
}

TEST(every_car_lines_up_behind_the_line_it_has_to_cross) {
    // The analyser used to put its car *on* the start line, which left it with
    // its own position to aim at and no reason to go anywhere; whether it
    // recovered depended on how much room it had to wander, so driveable tracks
    // came back impossible. A grid is behind the line, and driving forward off
    // it crosses the line - which is what starting a lap means.
    static gs_track t;
    static const gs_angle facings[4] = { 0, GS_DEG(90), GS_DEG(215), GS_DEG(300) };

    for (int f = 0; f < 4; f++) {
        gs_track_init(&t, 48, 32, GS_SURF_PAVEMENT);
        gs_track_add_gate(&t, GS_INT(24), GS_INT(16), facings[f], GS_INT(5));
        gs_track_add_gate(&t, GS_INT(40), GS_INT(28), facings[f], GS_INT(5));

        gs_fix fx = gs_cos(facings[f]), fy = gs_sin(facings[f]);

        for (uint8_t slot = 0; slot < GS_TRACK_GRID; slot++) {
            gs_fix x, y; gs_angle heading;
            gs_track_grid(&t, slot, &slot_x[slot], &slot_y[slot], &heading);
            x = slot_x[slot]; y = slot_y[slot];

            // Behind the line, facing the way the route runs through it.
            CHECK(heading == facings[f]);
            CHECK(gs_fix_mul(x - t.gate[0].x, fx) +
                  gs_fix_mul(y - t.gate[0].y, fy) < 0);

            // On the track, not off the edge of it.
            CHECK(x > 0 && x < GS_INT(t.w));
            CHECK(y > 0 && y < GS_INT(t.h));

            // **And driving straight ahead crosses the line**, which is the
            // whole point of being behind it.
            CHECK(gs_gate_crossed(&t.gate[0], x, y,
                                  x + gs_fix_mul(fx, GS_INT(6)),
                                  y + gs_fix_mul(fy, GS_INT(6))));
        }

        // Abreast rather than in a queue: no two cars share a place.
        for (uint8_t a = 0; a < GS_TRACK_GRID; a++) {
            for (uint8_t b = (uint8_t)(a + 1); b < GS_TRACK_GRID; b++) {
                CHECK(slot_x[a] != slot_x[b] || slot_y[a] != slot_y[b]);
            }
        }
    }
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
    run_a_race_is_identified_by_everything_that_decides_it();
    run_a_track_is_identified_by_everything_that_is_on_it();
    run_a_loop_and_a_path_over_the_same_ground_are_two_tracks();
    run_every_kind_of_edit_can_be_taken_back_and_put_back_again();
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
    run_nobody_drives_before_the_lights_go_green();
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
    run_a_tap_leaves_a_hazard_and_a_hold_changes_which_one();
    run_fire_burns_while_you_are_in_it_and_then_burns_out();
    run_smoke_hides_the_ground_and_does_nothing_to_the_car();
    run_oil_and_mines_stay_where_they_were_left();
    run_every_kind_of_hazard_can_be_carried_dropped_and_told_apart();
    run_a_car_carrying_nothing_leaves_nothing();
    run_a_weapon_runs_out_and_the_button_moves_on();
    run_one_a_second_however_fast_the_button_is_tapped();
    run_destruction_mode_ends_when_one_car_is_left_driving();
    run_everybody_going_at_once_is_a_draw_rather_than_a_win();
    run_a_race_does_not_end_just_because_somebody_was_wrecked();
    run_how_many_are_still_driving_is_one_number_with_one_definition();
    run_a_destruction_race_fought_out_between_two_cars_finishes_by_itself();
    run_a_wreck_changes_the_racing_line_for_the_rest_of_the_race();
    run_a_wreck_is_still_there_much_later_and_has_not_moved();
    run_the_ai_steers_both_ways();
    run_the_ai_gets_round_a_circuit_it_has_never_seen();
    run_the_skill_dial_changes_how_they_drive_and_not_only_how_fast();
    run_every_step_of_the_skill_dial_is_a_faster_lap_than_the_one_below_it();
    run_the_ai_gets_round_on_surfaces_and_vehicles_it_was_not_tuned_for();
    run_the_same_corner_is_braked_for_differently_under_different_gravity();
    run_the_same_corner_is_braked_for_differently_in_a_different_car();
    run_a_quicker_driver_carries_more_speed_through_the_same_corner();
    run_an_ai_race_is_deterministic_like_every_other_race();
    run_the_analyser_calls_a_jump_nobody_can_clear_impossible();
    run_the_analyser_says_so_when_a_track_cannot_be_got_round_at_all();
    run_a_longer_route_is_given_longer_to_get_round();
    run_the_heatmap_shows_where_everybody_actually_went();
    run_three_tracks_are_kept_and_all_three_survive_a_restart();
    run_editing_one_track_leaves_the_others_alone();
    run_a_track_that_came_with_the_game_is_not_yours_to_change();
    run_a_library_that_is_full_says_so_rather_than_losing_something();
    run_a_chunk_the_reassembler_refuses_does_not_poison_the_transfer();
    run_the_chunk_reader_refuses_a_datagram_that_does_not_add_up();
    run_an_honest_time_is_verified_and_a_doctored_one_is_not();
    run_a_time_from_a_different_race_is_not_this_record();
    run_a_time_survives_the_wire_and_is_still_verifiable();
    run_a_track_arrives_in_pieces_and_is_the_same_track();
    run_a_track_that_arrives_damaged_is_refused_rather_than_raced();
    run_a_record_belongs_to_a_track_and_the_conditions_it_was_set_under();
    run_beating_a_record_is_reported_and_not_beating_one_is_not();
    run_records_survive_being_written_and_read_back();
    run_a_profile_is_a_person_rather_than_a_settings_entry();
    run_a_roster_written_before_passwords_existed_still_loads();
    run_a_lock_survives_being_written_out_and_read_back();
    run_the_race_that_sets_a_record_is_the_race_that_reports_it();
    run_a_race_that_is_over_stays_over_until_a_new_one_replaces_it();
    run_a_race_ends_when_everybody_has_finished_and_the_first_one_wins();
    run_every_other_dial_on_the_setup_screen_reaches_the_race();
    run_every_lap_count_the_dial_offers_is_the_race_that_is_run();
    run_a_race_with_no_lap_target_never_ends();
    run_a_replay_carries_what_race_it_was();
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
    run_blake2s_produces_the_digests_the_rfc_and_an_independent_library_say();
    run_a_peer_that_reveals_an_input_it_did_not_commit_to_is_caught();
    run_a_peer_that_promises_two_different_things_for_one_tick_is_caught();
    run_a_promise_shown_in_the_same_breath_as_its_proof_buys_nothing();
    run_a_four_player_race_produces_a_log_that_re_races_to_the_ending_they_agreed_on();
    run_a_recording_carries_its_agreed_ending_across_a_serialise();
    run_a_machine_that_goes_quiet_stalls_the_race_rather_than_desyncing();
    run_a_race_with_nobody_else_in_it_never_stalls();
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
    run_a_replay_recorded_by_one_driver_cannot_be_claimed_by_another();
    run_a_recording_that_names_nobody_backs_nobodys_claim();
    run_the_driver_survives_the_wire_and_an_older_recording_still_loads();
    run_a_track_from_stunts_reads_as_a_track();
    run_an_imported_track_can_be_validated_and_driven();
    run_bytes_that_are_not_a_stunts_track_are_refused();
    run_a_road_piece_this_reader_does_not_know_is_counted_rather_than_hidden();
    run_a_track_written_in_the_stunts_layout_reads_back_as_itself();
    run_records_written_by_the_previous_version_still_load();
    run_profiles_written_by_the_previous_version_still_load();
    run_a_store_from_a_version_that_does_not_exist_is_refused();
    run_placing_a_gate_can_be_undone_like_anything_else();
    run_removing_a_gate_puts_it_back_where_it_was_in_the_order();
    run_the_route_and_the_ground_undo_in_one_history();
    run_a_gate_placed_inside_a_stroke_undoes_with_the_stroke();
    run_leaving_a_track_costs_time_before_it_costs_the_race();
    run_the_run_off_is_a_thing_that_stops_you();
    run_a_car_that_keeps_going_over_the_edge_is_finished();
    run_the_ground_outside_a_track_is_a_number_and_not_an_overflow();
    run_a_car_lands_where_the_arc_said_it_would();
    run_a_long_flight_is_drawn_coarsely_rather_than_cut_off();
    run_there_is_no_arc_for_a_car_on_the_ground();
    run_asking_where_a_car_will_land_does_not_move_it();
    run_a_wreck_takes_up_more_room_than_the_car_it_used_to_be();
    run_every_ground_is_a_different_thing_to_drive_on();
    run_a_ground_is_named_once_and_the_name_is_the_surface_table();
    run_a_saved_track_keeps_the_ground_it_was_painted_with();
    run_the_leader_is_whoever_is_furthest_round_the_route();
    run_every_car_has_a_place_and_no_two_share_one();
    run_a_car_that_has_finished_keeps_the_place_it_finished_in();
    run_a_seed_always_generates_the_same_track();
    run_a_seed_always_generates_the_same_name();
    run_every_generated_track_has_terrain_on_it();
    run_no_generated_slope_is_steeper_than_a_car_can_climb();
    run_a_generated_race_can_actually_be_finished();
    run_every_gate_is_wider_than_the_road_it_crosses();
    run_a_lap_of_a_loop_is_a_lap_and_arriving_ends_a_path();
    run_a_part_dropped_on_a_track_undoes_in_one_step();
    run_a_road_part_is_level_across_its_width();
    run_a_start_line_is_where_a_race_begins_however_late_it_was_dropped();
    run_a_combined_line_makes_the_track_a_loop();
    run_a_part_that_will_not_fit_changes_nothing();
    run_a_generated_track_leaves_clear_ground_to_get_up_to_speed_on();
    run_every_generated_track_can_be_got_round();
    run_every_car_lines_up_behind_the_line_it_has_to_cross();

    if (gs_failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d check%s failed\n", gs_failures, gs_failures == 1 ? "" : "s");
    return 1;
}

// main.c - the headless driver.
//
// **This program links gearstick_sim and nothing else.** No SDL, no window, no
// audio device. That is not a convenience: it is the standing proof that the
// simulation does not know it is being looked at, which is what lets the editor
// re-race a track in the background, lets CI notice a physics change, and will
// let rollback resimulate without a renderer attached.
//
// Run `gearstick_cli` with no arguments for the list.

#include <stdio.h>
#include <string.h>

#include "core/gs_replay.h"
#include "core/gs_sim.h"
#include "core/gs_track.h"
#include "core/gs_vehicle.h"

#include "golden.h"

static double as_double(gs_fix v) { return (double)v / (double)GS_ONE; }

// ---------------------------------------------------------------------------
// The selftest scenario
//
// Fixed, and fixed forever: a ramp to put both cars in the air, a stripe of ice
// to make the slip model matter, and a painted low-gravity pocket so the brush
// is exercised too. Two cars with different vehicles and different scripted
// inputs, long enough that any change to any part of the physics moves the
// final hash.
// ---------------------------------------------------------------------------

#define GS_SELFTEST_TICKS 900

static void selftest_track(gs_track *t) {
    gs_track_init(t, 32, 12, GS_SURF_PAVEMENT);

    // A ramp climbing one tile between x = 8 and x = 12, flat either side.
    for (uint8_t y = 0; y <= t->h; y++) {
        for (uint8_t x = 0; x <= t->w; x++) {
            gs_fix h;
            if (x <= 8) h = 0;
            else if (x >= 12) h = GS_INT(1);
            else h = (gs_fix)((int64_t)GS_INT(1) * (x - 8) / 4);
            gs_track_set_corner(t, x, y, h);
        }
    }

    for (uint8_t x = 16; x < 24; x++)
        for (uint8_t y = 0; y < t->h; y++)
            gs_track_set_surface(t, x, y, GS_SURF_ICE);

    for (uint8_t x = 13; x < 16; x++)
        for (uint8_t y = 0; y < t->h; y++)
            gs_track_set_gravity(t, x, y, GS_RATIO(35, 100));
}

static void selftest_inputs(uint32_t tick, gs_input *in) {
    in[0] = GS_IN_ACCEL;
    if ((tick / 40u) % 3u == 1u) in[0] |= GS_IN_LEFT;
    if ((tick / 55u) % 4u == 2u) in[0] |= GS_IN_RIGHT;

    in[1] = GS_IN_ACCEL;
    if ((tick / 33u) % 2u == 0u) in[1] |= GS_IN_RIGHT;
    if ((tick / 91u) % 3u == 0u) in[1] |= GS_IN_BRAKE;

    in[2] = 0;
    in[3] = 0;
}

static void selftest_world(gs_world *w, const gs_track *t) {
    gs_world_init(w, GS_ONE);
    gs_world_add_car(w, t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(2), GS_INT(4), 0);
    gs_world_add_car(w, t, (uint8_t)GS_VEH_MOTORCYCLE, GS_INT(2), GS_INT(8), 0);
}

static int cmd_selftest(bool verify) {
    static gs_track t;
    static gs_replay rec;
    gs_world w;

    selftest_track(&t);
    selftest_world(&w, &t);
    gs_replay_begin(&rec, &w, &t);

    for (uint32_t i = 0; i < GS_SELFTEST_TICKS; i++) {
        gs_input in[GS_MAX_CARS];
        selftest_inputs(i, in);
        gs_replay_record(&rec, in);
        gs_world_step(&w, &t, in);
    }

    uint64_t track_hash = gs_track_hash(&t);
    uint64_t world_hash = gs_world_hash(&w);

    printf("track  %u x %u, hash 0x%016llx\n", t.w, t.h,
           (unsigned long long)track_hash);
    printf("race   %u ticks at %d Hz (%.2f s)\n", GS_SELFTEST_TICKS, GS_TICK_HZ,
           (double)GS_SELFTEST_TICKS / GS_TICK_HZ);

    for (uint8_t i = 0; i < w.car_count; i++) {
        const gs_car *c = &w.car[i];
        printf("car %u  %-12s x %8.3f  y %8.3f  z %7.3f  speed %6.3f  "
               "damage %3u%s\n",
               i, gs_vehicle(c->vehicle)->name, as_double(c->x), as_double(c->y),
               as_double(c->z), as_double(gs_car_speed(c)), c->damage,
               c->wrecked ? "  WRECKED" : "");
    }
    printf("world  hash 0x%016llx\n", (unsigned long long)world_hash);

    // The replay has to re-race to the same place, or the recording is a lie.
    gs_world back;
    if (!gs_replay_restore(&rec, &back, &t)) {
        printf("FAIL   the replay refused its own track\n");
        return 1;
    }
    selftest_world(&back, &t);
    back.gravity = rec.meta.gravity;
    gs_replay_playback(&rec, &t, &back);

    if (gs_world_hash(&back) != world_hash) {
        printf("FAIL   the replay did not re-race to the same world\n");
        return 1;
    }
    printf("replay %u ticks, %zu bytes, re-races exactly\n",
           rec.meta.tick_count, gs_replay_size(&rec));

    if (!verify) return 0;

    if (GS_SELFTEST_WORLD_HASH == 0ULL) {
        printf("FAIL   src/frontend/cli/golden.h has no hash recorded yet\n");
        return 1;
    }
    if (track_hash != GS_SELFTEST_TRACK_HASH) {
        printf("FAIL   the selftest track is not the one the golden hash was "
               "taken from\n       want 0x%016llx\n       got  0x%016llx\n",
               (unsigned long long)GS_SELFTEST_TRACK_HASH,
               (unsigned long long)track_hash);
        return 1;
    }
    if (world_hash != GS_SELFTEST_WORLD_HASH) {
        printf("FAIL   the physics moved. Every ghost time and every shared\n"
               "       replay in existence is now wrong.\n"
               "       want 0x%016llx\n       got  0x%016llx\n"
               "       See src/frontend/cli/golden.h before changing this.\n",
               (unsigned long long)GS_SELFTEST_WORLD_HASH,
               (unsigned long long)world_hash);
        return 1;
    }

    printf("OK     the golden replay still lands where it did\n");
    return 0;
}

// Write the selftest track to a file, read it back, and check it is the same
// track. **The file I/O lives here and not in src/core/**: the simulation links
// nothing, so the buffer is core's business and the filesystem is the
// frontend's. The game does the same thing through SDL rather than stdio, over
// the identical core functions.
static int cmd_track(const char *path) {
    static gs_track built, loaded;
    static uint8_t buf[GS_TRACK_TILES * 4 + 4096];

    selftest_track(&built);

    size_t n = gs_track_serialize(&built, buf, sizeof buf);
    if (n == 0) {
        printf("FAIL   the track did not fit its own buffer\n");
        return 1;
    }

    FILE *f = fopen(path, "wb");
    if (f == nullptr) {
        printf("FAIL   could not open %s for writing\n", path);
        return 1;
    }
    size_t wrote = fwrite(buf, 1, n, f);
    fclose(f);
    if (wrote != n) {
        printf("FAIL   wrote %zu of %zu bytes\n", wrote, n);
        return 1;
    }

    f = fopen(path, "rb");
    if (f == nullptr) {
        printf("FAIL   could not open %s for reading\n", path);
        return 1;
    }
    static uint8_t back[sizeof buf];
    size_t got = fread(back, 1, sizeof back, f);
    fclose(f);

    if (got != n) {
        printf("FAIL   read %zu bytes, wrote %zu\n", got, n);
        return 1;
    }
    if (!gs_track_deserialize(&loaded, back, got)) {
        printf("FAIL   the file we just wrote was refused\n");
        return 1;
    }

    uint64_t a = gs_track_hash(&built), b = gs_track_hash(&loaded);
    printf("track  %u x %u, %zu bytes on disk\n", built.w, built.h, n);
    printf("wrote  0x%016llx\n", (unsigned long long)a);
    printf("read   0x%016llx\n", (unsigned long long)b);

    if (a != b) {
        printf("FAIL   the track that came back is not the track that went in\n");
        return 1;
    }
    printf("OK     it round-trips through the filesystem unchanged\n");
    return 0;
}

// Show that a sound route passes and that each way of breaking one is refused
// by name. Exits non-zero if a sound route is refused or a broken one accepted,
// so this is a check rather than a demonstration.
static int cmd_validate(void) {
    static gs_track t;
    int failures = 0;

    struct {
        const char *what;
        gs_track_problem want;
    } cases[] = {
        { "a sound route",             GS_TRACK_OK },
        { "no gates at all",           GS_TRACK_NO_START },
        { "a start and nowhere to go", GS_TRACK_TOO_FEW_GATES },
        { "a gate off the edge",       GS_TRACK_GATE_OFF_TRACK },
        { "a gate too narrow",         GS_TRACK_GATE_TOO_NARROW },
        { "two gates in one place",    GS_TRACK_GATES_COINCIDE },
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        gs_track_init(&t, 32, 24, GS_SURF_PAVEMENT);

        if (cases[i].want != GS_TRACK_NO_START) {
            gs_track_add_gate(&t, GS_INT(6), GS_INT(12), 0, GS_INT(4));
        }
        if (cases[i].want != GS_TRACK_NO_START &&
            cases[i].want != GS_TRACK_TOO_FEW_GATES) {
            gs_track_add_gate(&t, GS_INT(16), GS_INT(12), 0, GS_INT(4));
            gs_track_add_gate(&t, GS_INT(26), GS_INT(12), 0, GS_INT(4));
        }

        switch (cases[i].want) {
        case GS_TRACK_GATE_OFF_TRACK:  t.gate[1].y = GS_INT(2); break;
        case GS_TRACK_GATE_TOO_NARROW: t.gate[2].half_width = GS_ONE / 16; break;
        case GS_TRACK_GATES_COINCIDE:  t.gate[2].x = t.gate[0].x;
                                       t.gate[2].y = t.gate[0].y; break;
        default: break;
        }

        gs_track_issue got = gs_track_validate(&t);
        bool ok = got.problem == cases[i].want;
        if (!ok) failures++;

        printf("%-26s %-8s %s", cases[i].what, ok ? "ok" : "WRONG",
               gs_track_problem_text(got.problem));
        if (got.gate >= 0) printf(" (gate %d", got.gate);
        if (got.other >= 0) printf(" and %d", got.other);
        if (got.gate >= 0) printf(")");
        printf("\n");
    }

    if (failures != 0) {
        printf("\n%d case%s did not behave as stated\n", failures,
               failures == 1 ? "" : "s");
        return 1;
    }
    printf("\nOK     every broken route is refused, and named\n");
    return 0;
}

// --- the roster sweep -----------------------------------------------------
//
// Race every vehicle over a handful of deliberately different conditions and
// see who wins what. **The claim being tested is that nobody wins everything**:
// a roster where one machine is simply best is a roster with one vehicle in it
// and five decorations, and the whole premise of the game is that everything is
// a trade.
//
// This is the analyser in miniature. The real one sweeps gravity and vehicle
// across an authored track and draws the result over the editor; that is Phase
// 9. This asks the same question with fixed scenarios and no track files.

typedef struct gs_scenario {
    const char *name;
    gs_surface  surface;
    gs_fix      gravity;    // multiple of Earth
    bool        twisty;     // steer back and forth rather than run straight
    int32_t     drop;       // shelf height in tiles, 0 for flat ground
    gs_fix      bumps;      // washboard amplitude in tiles, 0 for smooth
} gs_scenario;

static void gs_scenario_track(gs_track *t, const gs_scenario *sc) {
    gs_track_init(t, 64, 24, sc->surface);

    for (uint8_t y = 0; y <= t->h; y++) {
        for (uint8_t x = 0; x <= t->w; x++) {
            gs_fix z = 0;

            // A shelf that ends, so the run is decided by what the landing does
            // rather than by how fast the car got there.
            if (sc->drop != 0 && x <= 10) z += GS_INT(sc->drop);

            // Washboard: ridges a car cannot avoid and has to survive. This is
            // where toughness stops being insurance against a mistake and
            // becomes the thing being measured.
            if (sc->bumps != 0 && x > 6 && (x % 3u) == 0u) z += sc->bumps;

            gs_track_set_corner(t, x, y, z);
        }
    }
}

// How far along the track a vehicle got in ten seconds, in tiles. A wreck
// scores whatever it managed before it stopped, which is the point of including
// a drop at all.
static double gs_run_scenario(const gs_scenario *sc, uint8_t vehicle) {
    static gs_track t;
    gs_scenario_track(&t, sc);

    gs_world w;
    gs_world_init(&w, sc->gravity);
    gs_world_add_car(&w, &t, vehicle, GS_INT(2), GS_INT(12), 0);

    for (uint32_t i = 0; i < GS_TICK_HZ * 10; i++) {
        gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, 0, 0, 0 };
        if (sc->twisty) {
            if ((i / 45u) % 2u == 0u) in[0] |= GS_IN_LEFT;
            else in[0] |= GS_IN_RIGHT;
        }
        gs_world_step(&w, &t, in);
    }
    return (double)(w.car[0].x - GS_INT(2)) / (double)GS_ONE;
}

static int cmd_roster(void) {
    // Chosen so that different things decide them: top speed, grip, survival,
    // and grip again where gravity has taken most of it away.
    const gs_scenario scenarios[] = {
        { "pavement sprint",  GS_SURF_PAVEMENT, GS_ONE,             false,  0, 0 },
        { "pavement, twisty", GS_SURF_PAVEMENT, GS_ONE,             true,   0, 0 },
        { "jupiter",          GS_SURF_PAVEMENT, GS_RATIO(253, 100), false,  0, 0 },
        { "ice",              GS_SURF_ICE,      GS_ONE,             false,  0, 0 },
        { "dirt, twisty",     GS_SURF_DIRT,     GS_ONE,             true,   0, 0 },
        { "the moon",         GS_SURF_PAVEMENT, GS_RATIO(17, 100),  true,   0, 0 },
        { "ceres",            GS_SURF_PAVEMENT, GS_RATIO(3, 100),   true,   0, 0 },
        { "off a shelf",      GS_SURF_DIRT,     GS_ONE,             false, 14, 0 },
        { "off a cliff",      GS_SURF_DIRT,     GS_ONE,             false, 30, 0 },
        { "washboard",        GS_SURF_DIRT,     GS_ONE,             false,  0,
          GS_RATIO(45, 100) },
        { "broken ground",    GS_SURF_DIRT,     GS_ONE,             false,  0,
          GS_RATIO(90, 100) },
    };
    const size_t count = sizeof scenarios / sizeof scenarios[0];

    printf("%-18s", "");
    for (uint8_t v = 0; v < GS_VEH_COUNT; v++) printf("%12s", gs_vehicle(v)->name);
    printf("\n");

    uint8_t wins[GS_VEH_COUNT] = { 0 };

    for (size_t i = 0; i < count; i++) {
        double best = -1e9;
        uint8_t winner = 0;
        double got[GS_VEH_COUNT];

        for (uint8_t v = 0; v < GS_VEH_COUNT; v++) {
            got[v] = gs_run_scenario(&scenarios[i], v);
            if (got[v] > best) { best = got[v]; winner = v; }
        }
        wins[winner]++;

        printf("%-18s", scenarios[i].name);
        for (uint8_t v = 0; v < GS_VEH_COUNT; v++) {
            printf("%11.1f%s", got[v], v == winner ? "*" : " ");
        }
        printf("\n");
    }

    printf("\n%-18s", "wins");
    for (uint8_t v = 0; v < GS_VEH_COUNT; v++) printf("%12u", wins[v]);
    printf("\n\n");

    uint8_t distinct = 0;
    for (uint8_t v = 0; v < GS_VEH_COUNT; v++) if (wins[v] > 0) distinct++;

    if (distinct < 2) {
        printf("FAIL   one vehicle wins everything: that is a roster of one and "
               "five decorations\n");
        return 1;
    }
    printf("OK     %u different vehicles win something across %zu conditions\n",
           distinct, count);
    return 0;
}

static int cmd_vehicles(void) {
    printf("%-13s %7s %7s %6s %6s %8s %6s\n",
           "vehicle", "power", "brake", "top", "grip", "steer", "tough");
    for (uint8_t i = 0; i < GS_VEH_COUNT; i++) {
        const gs_vehicle_def *v = gs_vehicle(i);
        printf("%-13s %7.2f %7.2f %6.2f %6.2f %8.0f %6.2f\n", v->name,
               as_double(v->power), as_double(v->brake), as_double(v->top),
               as_double(v->grip), as_double(v->steer), as_double(v->toughness));
    }
    printf("\npower and brake in tiles/s^2, top in tiles/s, steer in angle "
           "units/s.\nOne tile is %d metres.\n", GS_TILE_METRES);
    return 0;
}

static int cmd_gravity(void) {
    printf("%-9s %7s %10s\n", "preset", "x Earth", "tiles/s^2");
    for (int i = 0; i < GS_GRAVITY_PRESETS; i++) {
        const gs_gravity_preset *g = &gs_gravity_presets[i];
        printf("%-9s %7.2f %10.3f\n", g->name, as_double(g->scale),
               as_double(gs_fix_mul(GS_GRAVITY_EARTH, g->scale)));
    }
    printf("\nThe dial between them is continuous, and every tile carries its "
           "own\nmultiplier on top - see the gravity brush in "
           "docs/FEATURES.md.\n");
    return 0;
}

static int usage(void) {
    printf("gearstick_cli - the simulation, with nothing watching it\n\n"
           "  selftest [--verify]  race the fixed scenario and print its state "
           "hash\n"
           "  track FILE           write a track, read it back, check it survived\n"
           "  validate             show what the route checker accepts and refuses\n"
           "  roster               race every vehicle over every condition\n"
           "  vehicles             the roster and its numbers\n"
           "  gravity              the presets\n\n"
           "This program links the simulation and nothing else - no SDL, no "
           "window,\nno audio device. If it stops linking, the simulation has "
           "grown a\ndependency on being looked at.\n");
    return 2;
}

int main(int argc, char **argv) {
    if (argc < 2) return usage();

    if (strcmp(argv[1], "selftest") == 0) {
        bool verify = argc > 2 && strcmp(argv[2], "--verify") == 0;
        return cmd_selftest(verify);
    }
    if (strcmp(argv[1], "track") == 0 && argc > 2) return cmd_track(argv[2]);
    if (strcmp(argv[1], "validate") == 0) return cmd_validate();
    if (strcmp(argv[1], "roster") == 0) return cmd_roster();
    if (strcmp(argv[1], "vehicles") == 0) return cmd_vehicles();
    if (strcmp(argv[1], "gravity") == 0) return cmd_gravity();

    return usage();
}

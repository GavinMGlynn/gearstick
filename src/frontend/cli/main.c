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
#include <stdlib.h>
#include <string.h>

#include "core/gs_ai.h"
#include "core/gs_analyse.h"
#include "core/gs_generate.h"
#include "core/gs_ghost.h"
#include "core/gs_share.h"
#include "core/gs_stunts.h"
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

// ---------------------------------------------------------------------------
// The opponents scenario
//
// **A race with nobody at the keyboard.** Four cars on the grid, each driven by
// the game at a different point on the skill dial, on a circuit none of them
// has seen. The driver is a pure function of the world, so this has to re-race
// to the bit exactly as a recorded race does - and if it does not, every ghost
// and every shared replay of a race with opponents in it is wrong.
//
// Fixed forever, like the scenario above it.
// ---------------------------------------------------------------------------

#define GS_OPPONENTS_TICKS 12000

static void opponents_track(gs_track *t) {
    gs_track_init(t, 60, 60, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= t->h; y++) {
        for (uint8_t x = 0; x <= t->w; x++) gs_track_set_corner(t, x, y, 0);
    }
    gs_track_add_gate(t, GS_INT(45), GS_INT(15), 0, GS_INT(6));
    gs_track_add_gate(t, GS_INT(45), GS_INT(45), GS_QUARTER, GS_INT(6));
    gs_track_add_gate(t, GS_INT(15), GS_INT(45), (gs_angle)(GS_QUARTER * 2), GS_INT(6));
    gs_track_add_gate(t, GS_INT(15), GS_INT(15), (gs_angle)(GS_QUARTER * 3), GS_INT(6));
}

// One driver per grid slot, spread across the dial so that a change to any part
// of it moves this race.
static const int gs_opponent_skill[GS_MAX_CARS] = { 0, 7, 14, GS_AI_SKILL_STEPS };

static void opponents_world(gs_world *w, const gs_track *t) {
    static const uint8_t machines[GS_MAX_CARS] = {
        (uint8_t)GS_VEH_STOCK_CAR, (uint8_t)GS_VEH_DUNE_BUGGY,
        (uint8_t)GS_VEH_SPRINT_CAR, (uint8_t)GS_VEH_BAJA_BUG,
    };

    gs_world_init(w, GS_ONE);
    gs_world_set_mode(w, GS_MODE_RACE);
    gs_world_set_laps(w, 2);
    for (uint8_t i = 0; i < GS_MAX_CARS; i++) {
        gs_fix sx = 0, sy = 0;
        gs_angle facing = 0;
        gs_track_grid(t, i, &sx, &sy, &facing);
        gs_world_add_car(w, t, machines[i], sx, sy, facing);
    }
}

static void opponents_inputs(const gs_world *w, const gs_track *t, gs_input *in) {
    for (uint8_t i = 0; i < GS_MAX_CARS; i++) {
        in[i] = gs_ai_drive_style(w, t, i, gs_ai_skill_style(gs_opponent_skill[i]));
    }
}

static int cmd_opponents(bool verify) {
    static gs_track t;
    static gs_replay rec;
    gs_world w;

    opponents_track(&t);
    opponents_world(&w, &t);
    gs_replay_begin(&rec, &w, &t);

    for (uint32_t i = 0; i < GS_OPPONENTS_TICKS; i++) {
        gs_input in[GS_MAX_CARS];
        opponents_inputs(&w, &t, in);
        gs_replay_record(&rec, in);
        gs_world_step(&w, &t, in);
    }

    const uint64_t world_hash = gs_world_hash(&w);
    printf("race   %u ticks, four opponents at skill %d, %d, %d and %d\n",
           GS_OPPONENTS_TICKS, gs_opponent_skill[0], gs_opponent_skill[1],
           gs_opponent_skill[2], gs_opponent_skill[3]);
    for (uint8_t i = 0; i < w.car_count; i++) {
        const gs_car *c = &w.car[i];
        printf("car %u  %-12s laps %2u  best %6.2fs  %s\n", i,
               gs_vehicle(c->vehicle)->name, c->laps,
               c->best_lap > 0 ? (double)c->best_lap / GS_TICK_HZ : 0.0,
               c->finish_tick != 0 ? "finished" : "still going");
    }
    printf("world  hash 0x%016llx\n", (unsigned long long)world_hash);

    // **Driven again from the same start, and it is the same race.** The
    // driver reads the world and nothing else, so this is the claim that makes
    // an opponent's ghost worth anything.
    gs_world again;
    opponents_world(&again, &t);
    for (uint32_t i = 0; i < GS_OPPONENTS_TICKS; i++) {
        gs_input in[GS_MAX_CARS];
        opponents_inputs(&again, &t, in);
        gs_world_step(&again, &t, in);
    }
    if (gs_world_hash(&again) != world_hash) {
        printf("FAIL   the same race driven twice came out differently, so the\n"
               "       driver is reading something that is not the world\n");
        return 1;
    }
    printf("driven twice, identically\n");

    // And what was recorded of it re-races, which is what a shared replay is.
    gs_world back;
    if (!gs_replay_playback(&rec, &t, &back)) {
        printf("FAIL   the replay refused its own track\n");
        return 1;
    }
    if (gs_world_hash(&back) != world_hash) {
        printf("FAIL   the recorded race did not re-race to the same world\n");
        return 1;
    }
    printf("replay %u ticks, re-races exactly\n", rec.meta.tick_count);

    if (!verify) return 0;

    if (world_hash != GS_OPPONENTS_WORLD_HASH) {
        printf("FAIL   a race against opponents no longer ends where it did.\n"
               "       Either the physics moved or the driver did, and every\n"
               "       replay of a race with opponents in it is now wrong.\n"
               "       want 0x%016llx\n       got  0x%016llx\n",
               (unsigned long long)GS_OPPONENTS_WORLD_HASH,
               (unsigned long long)world_hash);
        return 1;
    }
    printf("OK     the opponents race is the one the golden hash was taken from\n");
    return 0;
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
    // Nothing is set up for it here on purpose: a recording that needs the
    // caller to remember where the cars stood is not a recording anybody can
    // send you.
    gs_world back;
    if (!gs_replay_playback(&rec, &t, &back)) {
        printf("FAIL   the replay refused its own track\n");
        return 1;
    }

    if (gs_world_hash(&back) != world_hash) {
        printf("FAIL   the replay did not re-race to the same world\n");
        return 1;
    }
    printf("replay %u ticks, %zu bytes, re-races exactly\n",
           rec.meta.tick_count, gs_replay_size(&rec));

    // --- And the same recording as a ghost, through the bytes.
    //
    // A ghost is the same simulation stepped in lockstep beside a live one, so
    // the claim worth checking is not that it ends in the same place. It is
    // that it is in the same place at *every* tick, having gone out to disk
    // format and back on the way - because a ghost that agrees only at the end
    // is a ghost you cannot race against.
    static uint8_t bytes[sizeof(gs_replay) + 4096];
    size_t rn = gs_replay_serialize(&rec, bytes, sizeof bytes);
    if (rn == 0) {
        printf("FAIL   the replay did not fit its own buffer\n");
        return 1;
    }

    static gs_ghost ghost;
    if (!gs_ghost_load(&ghost, &t, bytes, rn)) {
        printf("FAIL   the ghost refused the track it was recorded on\n");
        return 1;
    }

    gs_world beside;
    selftest_world(&beside, &t);
    for (uint32_t i = 0; i < GS_SELFTEST_TICKS; i++) {
        gs_input in[GS_MAX_CARS];
        selftest_inputs(i, in);
        gs_world_step(&beside, &t, in);
        gs_ghost_step(&ghost, &t);

        if (gs_world_hash(&ghost.world) != gs_world_hash(&beside)) {
            printf("FAIL   the ghost diverged from the race it recorded, at "
                   "tick %u of %u\n", i, GS_SELFTEST_TICKS);
            return 1;
        }
    }
    if (!ghost.finished) {
        printf("FAIL   the ghost still had recording left after %u ticks\n",
               GS_SELFTEST_TICKS);
        return 1;
    }
    printf("ghost  %u ticks, agrees at every one of them\n",
           gs_ghost_length(&ghost));

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

    // **And a race nobody was driving**, which is a different kind of replay:
    // the inputs are not recorded anywhere, they are worked out again from the
    // world each time. See cmd_opponents.
    if (cmd_opponents(verify) != 0) return 1;

    // The generator, folded over its first two hundred seeds. Same reason as
    // the two above and checked in the same place, so every platform in CI
    // pins it without a job of its own.
    static gs_track gen;
    uint64_t generated = 1469598103934665603ULL;
    for (int i = 0; i < 200; i++) {
        gs_generate(&gen, (uint32_t)(1 + i * 7919));
        generated = (generated ^ gs_track_hash(&gen)) * 1099511628211ULL;
    }
    if (generated != GS_SELFTEST_GENERATOR_HASH) {
        printf("FAIL   the track generator moved. Every seed anybody shared\n"
               "       now names a different track.\n"
               "       want 0x%016llx\n       got  0x%016llx\n"
               "       See src/frontend/cli/golden.h before changing this.\n",
               (unsigned long long)GS_SELFTEST_GENERATOR_HASH,
               (unsigned long long)generated);
        return 1;
    }
    printf("seeds  200 generated tracks, all the ones they always were\n");

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
        { "a gate across the route",   GS_TRACK_GATE_FACING },
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
        // Square across a route that runs east: the gate a car drives along
        // rather than through.
        case GS_TRACK_GATE_FACING:     t.gate[1].heading = GS_DEG(90); break;
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
    int32_t     stair;      // shelf every this many tiles, 0 for one shelf only
    uint32_t    seconds;    // how long the run lasts

    // Ten seconds is enough where speed decides. Where *survival* decides it is
    // not: a fast fragile car banks distance before it breaks, and over a short
    // run that is enough to win. Toughness only shows up when there is time for
    // the survivors to pull away from the wreckage.
} gs_scenario;

static void gs_scenario_track(gs_track *t, const gs_scenario *sc) {
    gs_track_init(t, 64, 24, sc->surface);

    for (uint8_t y = 0; y <= t->h; y++) {
        for (uint8_t x = 0; x <= t->w; x++) {
            gs_fix z = 0;

            // A shelf that ends, so the run is decided by what the landing does
            // rather than by how fast the car got there.
            if (sc->drop != 0 && sc->stair == 0 && x <= 10) z += GS_INT(sc->drop);

            // Or one shelf after another, all the way down. A single drop asks
            // whether a car survives a mistake; a staircase asks whether it
            // survives making the same one forty times, which is a different
            // question and the only one toughness alone answers.
            if (sc->stair != 0) z += GS_INT(40 - (int32_t)x / sc->stair * sc->drop);

            // Washboard: ridges a car cannot avoid and has to survive. This is
            // where toughness stops being insurance against a mistake and
            // becomes the thing being measured.
            //
            // Ridges, not spikes. A one-tile step of this height is a wall,
            // and since the physics started treating walls as walls a spiked
            // washboard stopped every machine dead at the first one and
            // measured nothing. Rising over two tiles and falling over two
            // keeps the amplitude - which is what the suspension feels - while
            // leaving a gradient a car can actually climb.
            if (sc->bumps != 0 && x > 6) {
                uint32_t phase = x % 4u;
                gs_fix up = phase == 1u || phase == 3u ? sc->bumps / 2
                          : phase == 2u                ? sc->bumps
                                                       : 0;
                z += up;
            }

            gs_track_set_corner(t, x, y, z);
        }
    }
}

// How far along the track a vehicle got, in tiles. A wreck scores whatever it
// managed before it stopped, which is the point of the rough scenarios: the
// clock keeps running and a broken car stops adding to its total.
static double gs_run_scenario(const gs_scenario *sc, uint8_t vehicle) {
    static gs_track t;
    gs_scenario_track(&t, sc);

    gs_world w;
    gs_world_init(&w, sc->gravity);
    gs_world_add_car(&w, &t, vehicle, GS_INT(2), GS_INT(12), 0);

    for (uint32_t i = 0; i < GS_TICK_HZ * sc->seconds; i++) {
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
    // Chosen so that different things decide them. Top speed on good ground;
    // grip where there is little of it; grip alone where gravity has taken
    // nearly all of it; and survival where the ground is trying to break the
    // car - which needs a long enough run that a wreck stops mattering less
    // than it costs.
    const gs_scenario scenarios[] = {
        { "pavement sprint",  GS_SURF_PAVEMENT, GS_ONE,             false,  0, 0, 0, 10 },
        { "pavement, twisty", GS_SURF_PAVEMENT, GS_ONE,             true,   0, 0, 0, 10 },
        { "jupiter",          GS_SURF_PAVEMENT, GS_RATIO(253, 100), false,  0, 0, 0, 10 },
        { "ice",              GS_SURF_ICE,      GS_ONE,             false,  0, 0, 0, 10 },
        { "dirt, twisty",     GS_SURF_DIRT,     GS_ONE,             true,   0, 0, 0, 10 },
        { "the moon",         GS_SURF_PAVEMENT, GS_RATIO(17, 100),  true,   0, 0, 0, 10 },
        { "ceres",            GS_SURF_PAVEMENT, GS_RATIO(3, 100),   true,   0, 0, 0, 10 },
        { "off a shelf",      GS_SURF_DIRT,     GS_ONE,             false, 14, 0, 0, 10 },
        { "rough and twisty", GS_SURF_DIRT,     GS_ONE,             true,   0,
          GS_RATIO(120, 100), 0, 25 },
        { "a staircase",      GS_SURF_DIRT,     GS_ONE,             false,  6,
          0, 6, 40 },
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

    // **Every vehicle has to win something.** "No vehicle wins everything" was
    // the first bar and it is too low: two winners and four also-rans is still
    // a roster of two with four decorations. If a machine is best at nothing,
    // there is no reason to ever pick it, and a choice nobody would make is not
    // a trade-off.
    uint8_t idle = 0;
    for (uint8_t v = 0; v < GS_VEH_COUNT; v++) {
        if (wins[v] == 0) {
            printf("FAIL   %s is best at nothing - so there is no reason to "
                   "choose it\n", gs_vehicle(v)->name);
            idle++;
        }
    }
    if (idle != 0) return 1;

    printf("OK     all %u vehicles win something across %zu conditions\n",
           (unsigned)GS_VEH_COUNT, count);
    return 0;
}

// --- the AI, over every surface and every machine ---------------------------
//
// The claim is that it *plans* rather than follows: no recorded line, no baked
// speed profile, nothing tuned per track. So the check is to hand it conditions
// nobody tuned it for and see whether it still gets round.

static void gs_ai_circuit(gs_track *t, gs_surface surface) {
    gs_track_init(t, 60, 60, surface);
    gs_track_add_gate(t, GS_INT(45), GS_INT(15), 0, GS_INT(6));
    gs_track_add_gate(t, GS_INT(45), GS_INT(45), GS_QUARTER, GS_INT(6));
    gs_track_add_gate(t, GS_INT(15), GS_INT(45), (gs_angle)(GS_QUARTER * 2), GS_INT(6));
    gs_track_add_gate(t, GS_INT(15), GS_INT(15), (gs_angle)(GS_QUARTER * 3), GS_INT(6));
}

static int cmd_ai(void) {
    static const struct { const char *name; gs_surface surface; gs_fix gravity; }
    conditions[] = {
        { "pavement, Earth",   GS_SURF_PAVEMENT, GS_ONE },
        { "dirt, Earth",       GS_SURF_DIRT,     GS_ONE },
        { "ice, Earth",        GS_SURF_ICE,      GS_ONE },
        { "pavement, Moon",    GS_SURF_PAVEMENT, GS_RATIO(17, 100) },
        { "pavement, Jupiter", GS_SURF_PAVEMENT, GS_RATIO(253, 100) },
        { "dirt, Mars",        GS_SURF_DIRT,     GS_RATIO(38, 100) },
    };
    const size_t count = sizeof conditions / sizeof conditions[0];
    const uint32_t seconds = 150;

    printf("%-20s", "");
    for (uint8_t v = 0; v < GS_VEH_COUNT; v++) printf("%12s", gs_vehicle(v)->name);
    printf("\n");

    int stuck = 0;
    for (size_t i = 0; i < count; i++) {
        static gs_track t;
        gs_ai_circuit(&t, conditions[i].surface);

        printf("%-20s", conditions[i].name);
        for (uint8_t v = 0; v < GS_VEH_COUNT; v++) {
            gs_world w;
            gs_world_init(&w, conditions[i].gravity);
            gs_world_add_car(&w, &t, v, GS_INT(20), GS_INT(15), 0);

            for (uint32_t k = 0; k < GS_TICK_HZ * seconds; k++) {
                gs_input in[GS_MAX_CARS] = { gs_ai_drive(&w, &t, 0), 0, 0, 0 };
                gs_world_step(&w, &t, in);
            }

            printf("%11u%s ", w.car[0].laps, w.car[0].wrecked ? "X" : " ");
            if (w.car[0].laps == 0) stuck++;
        }
        printf("\n");
    }

    printf("\n%u seconds each, no recorded line and nothing tuned per track.\n",
           seconds);
    if (stuck != 0) {
        printf("FAIL   %d of them could not get round at all\n", stuck);
        return 1;
    }
    printf("OK     every vehicle completes a lap in every condition\n");
    return 0;
}

// --- pace ------------------------------------------------------------------
//
// The item asks for lap times against a human baseline on the shipped tracks.
// There are no shipped tracks yet and nobody here can drive one, so this asks
// the same question with what exists: is the opponent's pace somewhere a person
// would find interesting?
//
// Difficulty is one number - how much of the available grip the driver is
// willing to use - so a cautious driver and a quick one are the same code at
// different settings. If the normal opponent sits strictly between them, it is
// beatable by driving better and not merely by driving longer, which is the
// property the item is really about.

static double gs_lap_time(const gs_track *t, uint8_t vehicle, gs_fix gravity,
                          gs_fix margin, uint32_t laps_wanted) {
    gs_world w;
    gs_world_init(&w, gravity);
    gs_world_add_car(&w, t, vehicle, GS_INT(20), GS_INT(15), 0);

    uint64_t started = 0;
    for (uint32_t i = 0; i < GS_TICK_HZ * 400; i++) {
        gs_input in[GS_MAX_CARS] = { gs_ai_drive_at(&w, t, 0, margin), 0, 0, 0 };
        gs_world_step(&w, t, in);

        // Timed from the first crossing of the start line, so the standing
        // start off the grid is not counted against the lap.
        if (started == 0 && w.car[0].laps == 1) started = w.tick;
        if (started != 0 && w.car[0].laps == 1 + laps_wanted) {
            return (double)(w.tick - started) / (double)GS_TICK_HZ / laps_wanted;
        }
    }
    return -1.0;
}

static int cmd_pace(void) {
    static const struct { const char *name; gs_surface surface; gs_fix gravity; }
    conditions[] = {
        { "pavement, Earth", GS_SURF_PAVEMENT, GS_ONE },
        { "dirt, Earth",     GS_SURF_DIRT,     GS_ONE },
        { "pavement, Moon",  GS_SURF_PAVEMENT, GS_RATIO(17, 100) },
    };
    // Three points on the dial rather than three kinds of driver: the bottom,
    // the middle and the top of the same continuous setting.
    static const struct { const char *name; int skill; } paces[] = {
        { "cautious", 0 },
        { "normal",   GS_AI_SKILL_DEFAULT },
        { "quick",    GS_AI_SKILL_STEPS },
    };

    printf("%-18s %10s %10s %10s\n", "", "cautious", "normal", "quick");

    int wrong = 0;
    for (size_t i = 0; i < sizeof conditions / sizeof conditions[0]; i++) {
        static gs_track t;
        gs_ai_circuit(&t, conditions[i].surface);

        double lap[3];
        printf("%-18s", conditions[i].name);
        for (int p = 0; p < 3; p++) {
            lap[p] = gs_lap_time(&t, (uint8_t)GS_VEH_STOCK_CAR,
                                 conditions[i].gravity,
                                 gs_ai_skill_margin(paces[p].skill), 3);
            if (lap[p] < 0) printf("%10s", "never");
            else printf("%9.2fs", lap[p]);
        }

        bool ordered = lap[0] > 0 && lap[1] > 0 && lap[2] > 0 &&
                       lap[0] > lap[1] && lap[1] > lap[2];
        printf("   %s\n", ordered ? "" : "  <- out of order");
        if (!ordered) wrong++;
    }

    printf("\nDifficulty is how much of the available grip the driver will use,\n"
           "and nothing else - no extra power, no cheating on the physics.\n");

    if (wrong != 0) {
        printf("FAIL   the normal opponent is not between the other two, so it is\n"
               "       either unbeatable or not worth racing\n");
        return 1;
    }
    printf("OK     driving better beats it and driving worse does not\n");
    return 0;
}

// --- the generator ----------------------------------------------------------

static gs_analysis gs_gen_report;

// Fifty tracks from fifty seeds, every one of them analysed. **This is the
// verification for the generator**, and it is a sweep rather than a look: a
// generated track that cannot be got round is worse than no generated track,
// because it goes in a library and somebody chooses it.
static int cmd_generate(int count) {
    static gs_track t;

    if (count <= 0) count = 50;

    printf("%-6s %-18s %-9s %5s %6s  %s\n", "seed", "name", "shape", "size",
           "gates", "completable");

    int bad = 0, flat = 0;
    uint64_t seen[256];
    int seen_count = 0;

    for (int i = 0; i < count; i++) {
        uint32_t seed = (uint32_t)(1 + i * 7919);
        gs_generate(&t, seed);

        char name[32];
        gs_generate_name(name, sizeof name, seed);

        gs_track_issue issue = gs_track_validate(&t);
        bool ok = issue.problem == GS_TRACK_OK;

        // Not flat: a generator that wrote nothing would produce forty
        // completable fields.
        bool raised = false;
        for (uint8_t y = 0; y <= t.h && !raised; y++) {
            for (uint8_t x = 0; x <= t.w; x++) {
                if (gs_track_corner_at(&t, x, y) != 0) { raised = true; break; }
            }
        }
        if (!raised) flat++;

        // And all different, which is the other way a generator fails
        // silently.
        uint64_t hash = gs_track_hash(&t);
        for (int k = 0; k < seen_count; k++) {
            if (seen[k] == hash) {
                printf("  seed %u repeats an earlier track\n", seed);
                bad++;
            }
        }
        if (seen_count < 256) seen[seen_count++] = hash;

        if (ok) {
            gs_analyse(&t, gs_analyse_seconds(&t), &gs_gen_report);
            ok = gs_gen_report.completable;
        }
        if (!ok) bad++;

        if (count <= 12 || !ok) {
            printf("%-6u %-18s %-9s %2ux%-2u %6u  %s\n", seed, name,
                   gs_shape_name(gs_generate_shape_for(seed)), t.w,
                   t.h, t.gate_count, ok ? "yes" : "NO");
        }
    }

    printf("\n%d track%s: %d not completable, %d flat\n", count,
           count == 1 ? "" : "s", bad, flat);

    if (bad > 0 || flat > 0) {
        printf("FAIL   a generated track nobody can drive is worse than none\n");
        return 1;
    }
    printf("OK     every generated track is driveable, and all of them differ\n");
    return 0;
}

// --- a track from somebody else's game ---------------------------------------
//
// **The verification for the importer, and it is exercised against a file this
// repository made.** The corpus is somebody else's and does not ship, so a check
// that needed a download would be a check that does not run - and one that only
// ever saw our own tracks would be testing the thing it was written from.
//
// Given a file, it converts that. Given none, it makes one, converts it, and
// checks the result is a track somebody could drive.
static gs_analysis gs_import_report;

static int cmd_import(const char *path) {
    static uint8_t bytes[GS_STUNTS_BYTES + 64];
    static gs_track t;
    size_t len = 0;

    if (path != nullptr) {
        FILE *f = fopen(path, "rb");
        if (f == nullptr) {
            printf("could not read %s\n", path);
            return 1;
        }
        len = fread(bytes, 1, sizeof bytes, f);
        fclose(f);
    } else {
        // A track of ours, written out in the donor's layout. The reader is
        // then being asked about a file it did not produce the contents of.
        static gs_track made;
        gs_track_init(&made, GS_STUNTS_SIDE, GS_STUNTS_SIDE, GS_SURF_GRASS);
        for (int x = 3; x < 27; x++) {
            gs_track_set_surface(&made, (uint8_t)x, 15, GS_SURF_PAVEMENT);
            gs_track_set_surface(&made, (uint8_t)x, 16, GS_SURF_PAVEMENT);
            gs_track_set_surface(&made, (uint8_t)x, 8, GS_SURF_DIRT);
        }
        for (int y = 2; y <= 6; y++) {
            for (int x = 18; x <= 26; x++) {
                gs_track_set_corner(&made, (uint8_t)x, (uint8_t)y, GS_RATIO(150, 100));
            }
        }
        len = gs_stunts_write(&made, bytes, sizeof bytes, (uint8_t)GS_STUNTS_ALPINE);
        printf("no file given, so one was written here: %zu bytes\n", len);
    }

    gs_stunts_report report;
    if (!gs_stunts_read(&t, bytes, len, &report)) {
        printf("FAIL   %zu bytes is not a Stunts track (they are %d)\n",
               len, GS_STUNTS_BYTES);
        return 1;
    }

    static const char *const skies[] = {
        "desert", "tropical", "alpine", "city", "country", "chaotic",
    };
    printf("%ux%u  horizon %s  %u road tiles  %u shaped  %u piece(s) unknown\n",
           t.w, t.h,
           report.horizon < 6 ? skies[report.horizon] : "?",
           report.road_tiles, report.raised_tiles, report.unknown_pieces);

    // A route, because the donor describes one with its road pieces and this
    // project describes one with gates. Across the middle, which is where the
    // generated file puts its road.
    gs_track_add_gate(&t, GS_INT(4), GS_INT(15) + GS_HALF, 0, GS_INT(4));
    gs_track_add_gate(&t, GS_INT(25), GS_INT(15) + GS_HALF, 0, GS_INT(4));

    gs_track_issue issue = gs_track_validate(&t);
    if (issue.problem != GS_TRACK_OK) {
        printf("FAIL   the converted track has a bad route: %s\n",
               gs_track_problem_text(issue.problem));
        return 1;
    }

    gs_analyse(&t, gs_analyse_seconds(&t), &gs_import_report);
    if (!gs_import_report.completable) {
        printf("FAIL   the converted track cannot be got round\n");
        return 1;
    }

    printf("OK     it converts, it validates, and somebody can drive it\n");
    return 0;
}

// --- sharing ----------------------------------------------------------------

static int cmd_code(const char *path, bool as_url) {
    static gs_track t;

    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        printf("could not open %s\n", path);
        return 1;
    }
    static uint8_t buf[GS_TRACK_TILES * 4 + 4096];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);

    if (!gs_track_deserialize(&t, buf, n)) {
        printf("%s is not a track\n", path);
        return 1;
    }

    static char code[GS_SHARE_MAX];
    size_t len = as_url ? gs_track_to_url(&t, code, sizeof code)
                        : gs_track_to_code(&t, code, sizeof code);
    if (len == 0) {
        printf("FAIL   the track did not fit a code\n");
        return 1;
    }

    printf("%s\n", code);
    fprintf(stderr, "%zu bytes on disk, %zu characters shared (%.0f%%)\n",
            n, len, 100.0 * (double)len / (double)n);
    return 0;
}

static int cmd_decode(const char *code, const char *path) {
    static gs_track t;

    if (!gs_track_from_code(&t, code)) {
        printf("FAIL   that is not a code for a track anybody built\n");
        return 1;
    }

    printf("track  %u x %u, %u gates, id %016llx\n", t.w, t.h, t.gate_count,
           (unsigned long long)gs_track_hash(&t));
    printf("route  %s\n", gs_track_problem_text(gs_track_validate(&t).problem));

    if (path == nullptr) return 0;

    static uint8_t buf[GS_TRACK_TILES * 4 + 4096];
    size_t n = gs_track_serialize(&t, buf, sizeof buf);
    FILE *f = fopen(path, "wb");
    if (f == nullptr || n == 0 || fwrite(buf, 1, n, f) != n) {
        printf("FAIL   could not write %s\n", path);
        if (f != nullptr) fclose(f);
        return 1;
    }
    fclose(f);
    printf("wrote  %s, %zu bytes\n", path, n);
    return 0;
}

// --- the analyser -----------------------------------------------------------

static gs_analysis gs_report;

static int cmd_analyse(const char *path) {
    static gs_track t;

    size_t n = 0;
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        printf("could not open %s\n", path);
        return 1;
    }
    static uint8_t buf[GS_TRACK_TILES * 4 + 4096];
    n = fread(buf, 1, sizeof buf, f);
    fclose(f);

    if (!gs_track_deserialize(&t, buf, n)) {
        printf("%s is not a track\n", path);
        return 1;
    }

    gs_track_issue issue = gs_track_validate(&t);
    printf("track  %u x %u, %u gates, id %016llx\n", t.w, t.h, t.gate_count,
           (unsigned long long)gs_track_hash(&t));
    printf("route  %s\n", gs_track_problem_text(issue.problem));
    if (issue.problem != GS_TRACK_OK) return 1;

    gs_analyse(&t, gs_analyse_seconds(&t), &gs_report);

    printf("\n%-14s %s\n", "gravity", "vehicles that got round");
    for (int i = 0; i < GS_ANALYSIS_STEPS; i++) {
        printf("%11.2fx  ", (double)gs_report.gravity[i] / (double)GS_ONE);
        for (uint8_t v = 0; v < gs_report.completed[i]; v++) printf("#");
        for (uint8_t v = gs_report.completed[i]; v < GS_VEH_COUNT; v++) printf(".");
        printf("  %u of %u\n", gs_report.completed[i], (unsigned)GS_VEH_COUNT);
    }

    if (!gs_report.completable) {
        printf("\nFAIL   nobody can get round this at any gravity\n");
        return 1;
    }
    printf("\nOK     completable between %.2fx and %.2fx Earth gravity\n",
           (double)gs_report.lightest / (double)GS_ONE,
           (double)gs_report.heaviest / (double)GS_ONE);
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
           "  opponents [--verify] race four opponents and print the state hash\n"
           "  track FILE           write a track, read it back, check it survived\n"
           "  validate             show what the route checker accepts and refuses\n"
           "  ai                   race the AI round a circuit in every condition\n"
           "  analyse FILE         what gravities and machines can get round a track\n"
           "  generate [N]         make N tracks from seeds and analyse every one\n"
           "  import [FILE]        read a Stunts .trk; with no file, make one first\n"
           "  code FILE            print a track as a code somebody can paste back\n"
           "  url FILE             the same code wrapped as a link\n"
           "  decode CODE [FILE]   read a code, and optionally write the track out\n"
           "  pace                 lap times for a cautious, normal and quick driver\n"
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
    if (strcmp(argv[1], "opponents") == 0) {
        bool verify = argc > 2 && strcmp(argv[2], "--verify") == 0;
        return cmd_opponents(verify);
    }
    if (strcmp(argv[1], "track") == 0 && argc > 2) return cmd_track(argv[2]);
    if (strcmp(argv[1], "validate") == 0) return cmd_validate();
    if (strcmp(argv[1], "ai") == 0) return cmd_ai();
    if (strcmp(argv[1], "pace") == 0) return cmd_pace();
    if (strcmp(argv[1], "analyse") == 0 && argc > 2) return cmd_analyse(argv[2]);
    if (strcmp(argv[1], "generate") == 0) {
        return cmd_generate(argc > 2 ? atoi(argv[2]) : 0);
    }
    if (strcmp(argv[1], "import") == 0) {
        return cmd_import(argc > 2 ? argv[2] : nullptr);
    }
    if (strcmp(argv[1], "code") == 0 && argc > 2) return cmd_code(argv[2], false);
    if (strcmp(argv[1], "url") == 0 && argc > 2) return cmd_code(argv[2], true);
    if (strcmp(argv[1], "decode") == 0 && argc > 2) {
        return cmd_decode(argv[2], argc > 3 ? argv[3] : nullptr);
    }
    if (strcmp(argv[1], "roster") == 0) return cmd_roster();
    if (strcmp(argv[1], "vehicles") == 0) return cmd_vehicles();
    if (strcmp(argv[1], "gravity") == 0) return cmd_gravity();

    return usage();
}

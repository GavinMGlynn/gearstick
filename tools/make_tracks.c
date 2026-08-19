// make_tracks - the stock tracks, written as data.
//
// **Content is data, not C.** A track compiled into the frontend is a track
// nobody can edit, share or replace, and it was a prototype from the day it was
// written. This produces the same shapes as files the game loads at run time,
// which the editor can then open, change and save like any other track.
//
// Four of them are written out deliberately, because a first track wants a shape
// somebody chose. The rest come from the seeded generator and are **picked by
// racing them**: every vehicle has to get round at Earth gravity, and no shape
// may supply more than its share, so the shipped set is a menu rather than
// twelve variations on one idea.
//
// Links the simulation and nothing else, like every other tool that touches the
// formats.
#include "core/gs_ai.h"
#include "core/gs_analyse.h"
#include "core/gs_generate.h"
#include "core/gs_track.h"

#include <stdio.h>
#include <string.h>

static gs_track gs_t;

static void gs_flat(uint8_t w, uint8_t h, gs_surface s) {
    gs_track_init(&gs_t, w, h, s);
    for (uint8_t y = 0; y <= h; y++) {
        for (uint8_t x = 0; x <= w; x++) gs_track_set_corner(&gs_t, x, y, 0);
    }
}

// A ridge running across the track between two x values, rising and falling
// over `ramp` tiles so it can be driven up rather than being a wall.
static void gs_ridge(uint8_t from, uint8_t to, uint8_t ramp, gs_fix height) {
    for (uint8_t y = 0; y <= gs_t.h; y++) {
        for (uint8_t x = 0; x <= gs_t.w; x++) {
            gs_fix h = 0;
            if (x >= from && x < from + ramp) {
                h = (gs_fix)((int64_t)height * (x - from) / ramp);
            } else if (x >= from + ramp && x < to - ramp) {
                h = height;
            } else if (x >= to - ramp && x < to) {
                h = (gs_fix)((int64_t)height * (to - x) / ramp);
            }
            if (h != 0) gs_track_set_corner(&gs_t, x, y, h);
        }
    }
}

static void gs_band(uint8_t from, uint8_t to, gs_surface s) {
    for (uint8_t x = from; x < to && x < gs_t.w; x++) {
        for (uint8_t y = 0; y < gs_t.h; y++) gs_track_set_surface(&gs_t, x, y, s);
    }
}

static void gs_gravity_patch(uint8_t from, uint8_t to, gs_fix multiplier) {
    for (uint8_t x = from; x < to && x < gs_t.w; x++) {
        for (uint8_t y = 0; y < gs_t.h; y++) {
            gs_track_set_gravity(&gs_t, x, y, multiplier);
        }
    }
}

static bool gs_write(const char *path) {
    static uint8_t buf[GS_TRACK_TILES * 4 + 4096];
    size_t n = gs_track_serialize(&gs_t, buf, sizeof buf);
    if (n == 0) return false;

    // Refuse to ship a track nobody can get round. The analyser is a heavier
    // check and lives elsewhere; this is the one that costs nothing and catches
    // a route somebody broke while editing the numbers above.
    if (gs_track_validate(&gs_t).problem != GS_TRACK_OK) {
        printf("  %s: the route is not sound\n", path);
        return false;
    }

    FILE *f = fopen(path, "wb");
    if (f == nullptr) return false;
    bool ok = fwrite(buf, 1, n, f) == n;
    fclose(f);

    printf("  %-28s %2u x %-3u %u gates  %5zu bytes  %016llx\n", path, gs_t.w,
           gs_t.h, gs_t.gate_count, n, (unsigned long long)gs_track_hash(&gs_t));
    return ok;
}

// --- choosing which generated tracks ship --------------------------------

// How many vehicles get round this track at Earth gravity, driven by the AI.
//
// Earth specifically, and not the analyser's whole range: the analyser answers
// "can this be got round *at all*", which is the right question when a designer
// is looking at their own work and the wrong one for a set that ships. Somebody
// starting the game picks a track and a car, and the dial is where it was left.
static int gs_finishers_at_earth(const gs_track *t) {
    uint32_t seconds = gs_analyse_seconds(t);
    int n = 0;

    for (uint8_t v = 0; v < GS_VEH_COUNT; v++) {
        gs_world w;
        gs_world_init(&w, GS_ONE);

        gs_fix x, y;
        gs_angle heading;
        gs_track_grid(t, 0, &x, &y, &heading);
        gs_world_add_car(&w, t, v, x, y, heading);

        for (uint32_t i = 0; i < GS_TICK_HZ * seconds; i++) {
            gs_input in[GS_MAX_CARS] = { gs_ai_drive(&w, t, 0), 0, 0, 0 };
            gs_world_step(&w, t, in);
            if (w.car[0].laps > 0) { n++; break; }
        }
    }
    return n;
}

#define GS_STOCK_GENERATED 12
#define GS_STOCK_PER_SHAPE (GS_STOCK_GENERATED / GS_SHAPE_COUNT)

static bool gs_write_generated(const char *dir) {
    uint8_t taken[GS_SHAPE_COUNT] = { 0 };
    int written = 0;

    // Seeds in order, so the set is reproducible and so adding a shape later
    // does not reshuffle the tracks anybody already has.
    for (uint32_t n = 1; written < GS_STOCK_GENERATED && n < 4000; n++) {
        uint32_t seed = n * 7919u;

        gs_track_shape shape = gs_generate_shape_for(seed);
        if (taken[shape] >= GS_STOCK_PER_SHAPE) continue;

        gs_generate(&gs_t, seed);
        if (gs_track_validate(&gs_t).problem != GS_TRACK_OK) continue;

        // **Every vehicle, or it does not ship.** A stock track that only the
        // sprint car can finish is a track that tells a new player their choice
        // of machine was wrong, which is the opposite of what a set of starting
        // tracks is for.
        if (gs_finishers_at_earth(&gs_t) < GS_VEH_COUNT) continue;

        char name[32];
        gs_generate_name(name, sizeof name, seed);
        for (char *c = name; *c != '\0'; c++) {
            if (*c == ' ') *c = '-';
        }

        char path[512];
        snprintf(path, sizeof path, "%s/%s.gstrack", dir, name);
        if (!gs_write(path)) return false;

        taken[shape]++;
        written++;
    }

    if (written < GS_STOCK_GENERATED) {
        printf("  only %d of %d generated tracks were good enough to ship\n",
               written, GS_STOCK_GENERATED);
        return false;
    }
    return true;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "assets/tracks";
    char path[512];

    // --- first light: the one a new player sees. A ramp, a landing, and
    // enough room either side to work out what the car does before it matters.
    gs_flat(40, 24, GS_SURF_PAVEMENT);
    gs_ridge(12, 26, 4, GS_INT(2));
    gs_band(0, 6, GS_SURF_DIRT);
    gs_band(30, 40, GS_SURF_DIRT);
    gs_track_add_gate(&gs_t, GS_INT(6), GS_INT(12), 0, GS_INT(6));
    gs_track_add_gate(&gs_t, GS_INT(34), GS_INT(12), 0, GS_INT(6));
    snprintf(path, sizeof path, "%s/first-light.gstrack", dir);
    if (!gs_write(path)) return 1;

    // --- the long drop: a shelf that ends. What the landing does decides the
    // run, which is the whole argument for building a downhill one.
    gs_flat(48, 20, GS_SURF_DIRT);
    for (uint8_t y = 0; y <= gs_t.h; y++) {
        for (uint8_t x = 0; x <= gs_t.w; x++) {
            gs_fix h = 0;
            if (x <= 18) h = GS_INT(6);
            else if (x < 24) h = (gs_fix)((int64_t)GS_INT(6) * (24 - x) / 6);
            gs_track_set_corner(&gs_t, x, y, h);
        }
    }
    gs_band(24, 34, GS_SURF_PAVEMENT);
    gs_track_add_gate(&gs_t, GS_INT(6), GS_INT(10), 0, GS_INT(6));
    gs_track_add_gate(&gs_t, GS_INT(42), GS_INT(10), 0, GS_INT(6));
    snprintf(path, sizeof path, "%s/the-long-drop.gstrack", dir);
    if (!gs_write(path)) return 1;

    // --- ice house: grip is the whole problem, and the machines sort
    // themselves out by it.
    gs_flat(44, 24, GS_SURF_ICE);
    gs_ridge(16, 28, 5, GS_INT(1));
    gs_band(0, 8, GS_SURF_PAVEMENT);
    gs_band(36, 44, GS_SURF_PAVEMENT);
    gs_track_add_gate(&gs_t, GS_INT(5), GS_INT(12), 0, GS_INT(7));
    gs_track_add_gate(&gs_t, GS_INT(38), GS_INT(12), 0, GS_INT(7));
    snprintf(path, sizeof path, "%s/ice-house.gstrack", dir);
    if (!gs_write(path)) return 1;

    // --- jupiter run: a painted low-gravity pocket over the jump, which is
    // the feature this game has and the original could not.
    gs_flat(48, 24, GS_SURF_PAVEMENT);
    gs_ridge(14, 24, 3, GS_INT(3));
    gs_gravity_patch(24, 32, GS_RATIO(30, 100));
    gs_gravity_patch(36, 44, GS_RATIO(180, 100));
    gs_band(32, 40, GS_SURF_DIRT);
    gs_track_add_gate(&gs_t, GS_INT(6), GS_INT(12), 0, GS_INT(6));
    gs_track_add_gate(&gs_t, GS_INT(42), GS_INT(12), 0, GS_INT(6));
    snprintf(path, sizeof path, "%s/jupiter-run.gstrack", dir);
    if (!gs_write(path)) return 1;

    // --- and a dozen from the generator, chosen by driving them.
    if (!gs_write_generated(dir)) return 1;

    return 0;
}

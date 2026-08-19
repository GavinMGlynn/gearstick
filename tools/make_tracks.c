// make_tracks - the stock tracks, written as data.
//
// **Content is data, not C.** A track compiled into the frontend is a track
// nobody can edit, share or replace, and it was a prototype from the day it was
// written. This produces the same shapes as files the game loads at run time,
// which the editor can then open, change and save like any other track.
//
// Each track here is written out deliberately rather than generated: the seeded
// generator is a later item and a different thing. These are the few that ship
// so that a fresh install has something to race on.
//
// Links the simulation and nothing else, like every other tool that touches the
// formats.
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

    return 0;
}

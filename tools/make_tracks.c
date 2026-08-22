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

    // Refuse to ship a track with a broken route. Cheap, and it catches a gate
    // somebody moved while editing the numbers above.
    if (gs_track_validate(&gs_t).problem != GS_TRACK_OK) {
        printf("  %s: the route is not sound\n", path);
        return false;
    }

    // **And refuse to ship one nobody can get round.** A designed track is
    // written out by hand, so nothing else checks it: the generated ones are
    // raced before they are chosen and these were not, which made hand-built the
    // *less* verified half of the set. Racing them here costs a few seconds at
    // build time and closes that.
    static gs_analysis look;
    gs_analyse(&gs_t, gs_analyse_seconds(&gs_t), &look);
    if (!look.completable) {
        printf("  %s: nobody can get round it\n", path);
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

// --- the ones somebody designed ---------------------------------------------
//
// **What the 1985 manual's section 7.0 turned out to be worth.** It lists all
// fifty of the original's tracks and says what each is *for*, and the categories
// are the useful part rather than the names:
//
//   - shapes, named for what they look like: a figure of eight, a clover, a
//     spiral, a letter "e";
//   - challenges, named for what happens on them: `jumps` ("big ones"),
//     `headon`, which "aims drivers directly at each other", and `whichway`,
//     which offers "seven different routes" of differing difficulty;
//   - test courses, after real ones: Fiorano, Weissach, an oval;
//   - and thirty-odd real circuits, all pavement, no jumps, Earth gravity, five
//     laps - which is a statement about what a *plain* track is for.
//
// The lesson is not the track list, which is theirs. It is that **every track
// existed for a reason somebody could say in one line**, and a set that cannot
// do that is a set of variations. Each of these says its reason, and the guide
// prints them.

// A ridge running along y instead of across it, for tracks whose interest is
// side to side rather than end to end.
static void gs_wall(uint8_t from, uint8_t to, uint8_t x0, uint8_t x1,
                    gs_fix height) {
    for (uint8_t y = from; y <= to && y <= gs_t.h; y++) {
        for (uint8_t x = x0; x <= x1 && x <= gs_t.w; x++) {
            gs_track_set_corner(&gs_t, x, y, height);
        }
    }
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

    // --- the crossing: a figure of eight, after the original's `dirt8`. The
    // route crosses itself, so the two halves of the field meet in the middle
    // going different ways. That is the whole point of the shape and the reason
    // it is the first one in their list.
    gs_flat(56, 40, GS_SURF_DIRT);
    gs_band(0, 6, GS_SURF_PAVEMENT);
    gs_track_add_gate(&gs_t, GS_INT(8), GS_INT(8), 0, GS_INT(6));
    gs_track_add_gate(&gs_t, GS_INT(48), GS_INT(8), 0, GS_INT(6));
    gs_track_add_gate(&gs_t, GS_INT(8), GS_INT(32), 0, GS_INT(6));
    gs_track_add_gate(&gs_t, GS_INT(48), GS_INT(32), 0, GS_INT(6));
    // A circuit: a figure of eight closes on itself, so the line it starts on is the line it ends on.
    gs_t.route = (uint8_t)GS_ROUTE_CIRCUIT;
    snprintf(path, sizeof path, "%s/the-crossing.gstrack", dir);
    if (!gs_write(path)) return 1;

    // --- head on: after `headon`, which "aims drivers directly at each other".
    // A corridor with the far gate at the end of it, so an out-and-back puts
    // everybody on the same line travelling opposite ways. Pavement, because
    // the interest is the meeting and not the grip.
    gs_flat(56, 14, GS_SURF_PAVEMENT);
    gs_wall(0, 3, 0, 56, GS_INT(3));
    gs_wall(11, 14, 0, 56, GS_INT(3));
    // As wide as the corridor between the walls, not narrower: a gate is finite
    // across its line, so one that does not span the road it crosses can be
    // driven past on the outside - and a checkpoint nobody crosses is a finish
    // line that never fires, because gates count in order.
    gs_track_add_gate(&gs_t, GS_INT(8), GS_INT(7), 0, GS_INT(5));
    gs_track_add_gate(&gs_t, GS_INT(48), GS_INT(7), 0, GS_INT(5));
    snprintf(path, sizeof path, "%s/head-on.gstrack", dir);
    if (!gs_write(path)) return 1;

    // --- which way: after `whichway`, "seven different routes". Three here, and
    // three is enough to make the question real: the short way is over a ramp,
    // the middle way is dirt and shorter than it looks, the long way is clean
    // pavement. Nobody can tell you which is quickest without driving all three.
    gs_flat(60, 36, GS_SURF_PAVEMENT);
    gs_band(0, 8, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= 12; y++) {
        for (uint8_t x = 20; x <= 40; x++) {
            gs_fix h = 0;
            if (x >= 24 && x <= 36) h = GS_INT(2);
            else if (x > 20 && x < 24) h = (gs_fix)((int64_t)GS_INT(2) * (x - 20) / 4);
            else if (x > 36 && x < 40) h = (gs_fix)((int64_t)GS_INT(2) * (40 - x) / 4);
            gs_track_set_corner(&gs_t, x, y, h);
        }
    }
    for (uint8_t x = 14; x < 46; x++) {
        for (uint8_t y = 14; y < 22; y++) gs_track_set_surface(&gs_t, x, y, GS_SURF_DIRT);
    }
    gs_track_add_gate(&gs_t, GS_INT(8), GS_INT(18), 0, GS_INT(12));
    gs_track_add_gate(&gs_t, GS_INT(52), GS_INT(18), 0, GS_INT(12));
    snprintf(path, sizeof path, "%s/which-way.gstrack", dir);
    if (!gs_write(path)) return 1;

    // --- the oval: after `indy`. The shortest thing that is still a race, and
    // the one to hand somebody who has never driven this. Four gates, no
    // scenery, nothing to learn but the car.
    // A gate's line runs *across* its heading, so a wide one near an edge hangs
    // off the track - which is what the validator caught the first time these
    // numbers were written. Six either side, well inside a sixty by forty.
    gs_flat(60, 40, GS_SURF_PAVEMENT);
    gs_track_add_gate(&gs_t, GS_INT(30), GS_INT(8), 0, GS_INT(6));
    gs_track_add_gate(&gs_t, GS_INT(50), GS_INT(20), GS_QUARTER, GS_INT(6));
    gs_track_add_gate(&gs_t, GS_INT(30), GS_INT(32), (gs_angle)(GS_QUARTER * 2), GS_INT(6));
    gs_track_add_gate(&gs_t, GS_INT(10), GS_INT(20), (gs_angle)(GS_QUARTER * 3), GS_INT(6));
    // A circuit: an oval is a loop and a loop has one line, crossed at the start of every lap and the end of every lap.
    gs_t.route = (uint8_t)GS_ROUTE_CIRCUIT;
    snprintf(path, sizeof path, "%s/the-oval.gstrack", dir);
    if (!gs_write(path)) return 1;

    // --- the big one: after `jumps`, described in the manual as "big ones". One
    // ramp, one landing, and nothing else to think about - so the only question
    // is how fast you arrive, which is the question a gravity dial makes
    // interesting.
    gs_flat(60, 20, GS_SURF_PAVEMENT);
    gs_ridge(16, 30, 5, GS_INT(4));
    gs_band(34, 46, GS_SURF_DIRT);
    gs_track_add_gate(&gs_t, GS_INT(8), GS_INT(10), 0, GS_INT(6));
    gs_track_add_gate(&gs_t, GS_INT(52), GS_INT(10), 0, GS_INT(6));
    snprintf(path, sizeof path, "%s/the-big-one.gstrack", dir);
    if (!gs_write(path)) return 1;

    // --- the long way round: after the Grand Prix circuits, which the manual
    // notes are all pavement, no jumps, Earth gravity, five laps. That is a
    // statement about what a plain track is for: when nothing is in the way, the
    // driving is the whole of it.
    gs_flat(60, 44, GS_SURF_PAVEMENT);
    gs_track_add_gate(&gs_t, GS_INT(30), GS_INT(10), 0, GS_INT(6));
    gs_track_add_gate(&gs_t, GS_INT(50), GS_INT(16), GS_QUARTER, GS_INT(6));
    gs_track_add_gate(&gs_t, GS_INT(40), GS_INT(34), (gs_angle)(GS_QUARTER * 2), GS_INT(6));
    gs_track_add_gate(&gs_t, GS_INT(16), GS_INT(34), (gs_angle)(GS_QUARTER * 2), GS_INT(6));
    gs_track_add_gate(&gs_t, GS_INT(10), GS_INT(16), (gs_angle)(GS_QUARTER * 3), GS_INT(6));
    // A circuit: five corners back to where you began, which is what makes it a lap rather than a journey.
    gs_t.route = (uint8_t)GS_ROUTE_CIRCUIT;
    snprintf(path, sizeof path, "%s/the-long-way-round.gstrack", dir);
    if (!gs_write(path)) return 1;

    // --- and a dozen from the generator, chosen by driving them.
    if (!gs_write_generated(dir)) return 1;

    return 0;
}

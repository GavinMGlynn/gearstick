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

// GS_STOCK_MIN_ROUTE - how long a default track has to be - lives in
// core/gs_track.h, next to the route length it is compared against, so the tool
// that writes the tracks and the suite that checks the written ones hold them to
// the same number rather than to two numbers that agree today.

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

// **The same feature, across a field big enough to hold several of it.** A
// track about a ramp used to have one ramp on it because the field was forty
// tiles wide; on a field of a hundred and eighty a single ramp is a thing you
// meet once in a twelve hundred tile route and forget. Repeating it is what
// makes the idea the track is about the thing you are actually driving.
static void gs_ridges_across(uint8_t first, uint8_t spacing, uint8_t width,
                             uint8_t ramp, gs_fix height) {
    for (uint8_t x = first; (uint32_t)x + width < gs_t.w; x = (uint8_t)(x + spacing)) {
        gs_ridge(x, (uint8_t)(x + width), ramp, height);
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

    // **And refuse to ship one that is over in thirty seconds.** Every track
    // that shipped in August 2026 was between twenty-eight and a hundred and
    // seventy-three tiles of route - twenty-seven seconds of driving on
    // pavement, which `gearstick_cli pace` had been printing all along. It was
    // asked for repeatedly that a default track be ten to twenty times that,
    // and the requirement kept being lost between one piece of work and the
    // next. It is not a thing to remember any more: a track under the floor is
    // not written, and the build fails.
    gs_fix route = gs_track_route_length(&gs_t);
    if (route < GS_INT(GS_STOCK_MIN_ROUTE)) {
        printf("  %s: %d tiles of route, and the floor is %d\n", path,
               (int)(route / GS_ONE), GS_STOCK_MIN_ROUTE);
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

    // **Every machine, from every slot on the grid.** This used to race each
    // vehicle from pole. A grid is staggered back from the line and across it,
    // so the car in the last slot has a different corner to make and different
    // ground to make it on - and on a route that folds back every thirty tiles
    // that is a different track. Opponents start in those slots, so a track
    // that ships has to be drivable from all of them.
    //
    // Pole first for each machine, because a track that fails there fails
    // cheaply and most candidates fail.
    for (uint8_t v = 0; v < GS_VEH_COUNT; v++) {
        bool everywhere = true;

        for (uint8_t slot = 0; slot < GS_MAX_CARS && everywhere; slot++) {
            gs_world w;
            gs_world_init(&w, GS_ONE);

            gs_fix x, y;
            gs_angle heading;
            gs_track_grid(t, slot, &x, &y, &heading);
            gs_world_add_car(&w, t, v, x, y, heading);

            bool round = false;
            for (uint32_t i = 0; i < GS_TICK_HZ * seconds; i++) {
                gs_input in[GS_MAX_CARS] = { gs_ai_drive(&w, t, 0), 0, 0, 0 };
                gs_world_step(&w, t, in);
                if (w.car[0].laps > 0) { round = true; break; }
            }
            everywhere = round;
        }

        if (everywhere) n++;
    }
    return n;
}

// **Six rather than twelve.** Every candidate is raced by every vehicle before
// it is allowed to ship, and a candidate is now a thirteen hundred tile route
// rather than a fifty tile one, so the choosing costs twenty-five times what it
// did. Six is what a scan of six thousand seeds finds in a couple of minutes:
// every shape is generated and every one of them is driveable, but not every
// shape produces two that *all six machines* can finish, which is the bar for
// shipping one. The ten written by hand make the set up to sixteen.
#define GS_STOCK_GENERATED 8
// **A ceiling per shape, not a share of the total.** This was the total divided
// by the number of shapes, which with six wanted and four shapes is one each -
// so one shape that cannot produce a track every machine can finish caps the
// whole set at three. Three each lets the shapes that do work cover for the one
// that does not, while still keeping any single shape from filling the library.
#define GS_STOCK_PER_SHAPE 3

// **A track written by hand has its gates faced along its own route.**
//
// Every one of them used to pass a heading of zero, which is east, whatever the
// route did - and `the crossing` is a figure of eight, so all four of its gates
// faced east and the one at the start was square across the way anybody drives
// through it. A gate is a plane whose normal is its heading, so that is a gate
// crossed sideways and an arrow pointing at nothing, not a cosmetic slip.
//
// Derived rather than typed, because typed is what produced ten tracks' worth
// of zeroes. The generated tracks already face their own centreline and are
// left alone: re-facing them would move hashes for nothing.
static bool gs_write_authored(const char *path) {
    gs_track_face_along_route(&gs_t);
    return gs_write(path);
}

static bool gs_write_generated(const char *dir) {
    uint8_t taken[GS_SHAPE_COUNT] = { 0 };
    int written = 0;

    // Seeds in order, so the set is reproducible and so adding a shape later
    // does not reshuffle the tracks anybody already has.
    for (uint32_t n = 1; written < GS_STOCK_GENERATED && n < 6000; n++) {
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

    // **The tracks written by hand, on the same big field as the generated
    // ones.** Each of these demonstrates one idea, and they used to do it on
    // forty by twenty-four with two gates: twenty-eight tiles of route, under
    // thirty seconds of driving, which is not a race whatever is painted on it.
    // They are laid on a field of the size the world now allows, their signature
    // feature is repeated across it, and the route is the same serpentine the
    // generator lays - so a track about a ramp puts a ramp on every pass rather
    // than one ramp in a field. A demonstration you meet five times is a better
    // demonstration than one you meet once.
    //
    // The seed after each is only there to shake the route's incidental choices
    // apart, so no two of these come out identically shaped.
    //
    // **Every feature starts well down the field.** The route begins at the
    // inset, so a ridge twenty tiles in is a wall six tiles in front of a
    // standing start - and `jupiter run` shipped with one, unfinishable from
    // two of the four grid slots. The generator keeps its own features clear of
    // the line with GS_GEN_RUNUP for the same reason.

    // --- first light: the one a new player sees. A ramp, a landing, and enough
    // room either side to work out what the car does before it matters.
    gs_flat(184, 176, GS_SURF_PAVEMENT);
    gs_ridges_across(44, 44, 14, 4, GS_INT(2));
    gs_band(0, 10, GS_SURF_DIRT);
    gs_band(174, 184, GS_SURF_DIRT);
    gs_generate_route(&gs_t, 101u, false);
    snprintf(path, sizeof path, "%s/first-light.gstrack", dir);
    if (!gs_write_authored(path)) return 1;

    // --- the long drop: shelves that end. What the landing does decides the
    // run, which is the whole argument for building a downhill one.
    gs_flat(184, 172, GS_SURF_DIRT);
    for (uint8_t y = 0; y <= gs_t.h; y++) {
        for (uint8_t x = 0; x <= gs_t.w; x++) {
            // Four terraces down the field, each with a lip to fall off.
            uint8_t step = (uint8_t)(x / 46);
            gs_fix h = GS_INT(6) - (gs_fix)((int64_t)GS_INT(2) * step);
            uint8_t into = (uint8_t)(x % 46);
            if (into > 40) h -= (gs_fix)((int64_t)GS_INT(2) * (into - 40) / 6);
            if (h < 0) h = 0;
            gs_track_set_corner(&gs_t, x, y, h);
        }
    }
    gs_band(46, 92, GS_SURF_PAVEMENT);
    gs_generate_route(&gs_t, 102u, false);
    snprintf(path, sizeof path, "%s/the-long-drop.gstrack", dir);
    if (!gs_write_authored(path)) return 1;

    // --- ice house: grip is the whole problem, and the machines sort
    // themselves out by it.
    gs_flat(180, 176, GS_SURF_ICE);
    gs_ridges_across(48, 50, 16, 5, GS_INT(1));
    gs_band(0, 12, GS_SURF_PAVEMENT);
    gs_band(168, 180, GS_SURF_PAVEMENT);
    gs_generate_route(&gs_t, 103u, false);
    snprintf(path, sizeof path, "%s/ice-house.gstrack", dir);
    if (!gs_write_authored(path)) return 1;

    // --- jupiter run: painted low-gravity pockets over the jumps, which is the
    // feature this game has and the original could not.
    gs_flat(184, 176, GS_SURF_PAVEMENT);
    // **A pocket, not a launchpad.** Three tenths of Earth over a three-tile
    // ramp threw a car high enough that it came down somewhere it could not
    // drive out of, and the track was unfinishable from every grid slot. Half
    // gravity over a two-tile ramp is the same idea at a size a car survives -
    // and the heavy patch after it is what brings the jump back down rather
    // than a second thing to be caught out by.
    gs_ridges_across(46, 46, 12, 3, GS_INT(2));
    // Light gravity only. The heavy patch on the far side was meant to bring
    // the jump back down and instead drove a car into the ground it had just
    // left, from every grid slot: painted gravity is a thing you fly through,
    // not a thing that lands you.
    // **Between the ramps, not off the end of them.** A pocket that begins
    // where a ramp ends turns every crossing into a launch, and a launch on a
    // route that turns thirty tiles later is a car in the scenery - it was
    // unfinishable from every grid slot twice over. Landing the pocket on the
    // flat between two ramps is the same feature to drive through and a
    // survivable one.
    for (uint8_t x = 70; x + 12 < gs_t.w; x = (uint8_t)(x + 46)) {
        gs_gravity_patch(x, (uint8_t)(x + 12), GS_RATIO(55, 100));
    }
    gs_generate_route(&gs_t, 104u, false);
    snprintf(path, sizeof path, "%s/jupiter-run.gstrack", dir);
    if (!gs_write_authored(path)) return 1;

    // --- the crossing: after the original's `dirt8`. Dirt the whole way, with
    // pavement down one side so the surface changes under you every pass.
    gs_flat(180, 180, GS_SURF_DIRT);
    gs_band(0, 12, GS_SURF_PAVEMENT);
    gs_band(88, 100, GS_SURF_PAVEMENT);
    gs_generate_route(&gs_t, 105u, true);
    snprintf(path, sizeof path, "%s/the-crossing.gstrack", dir);
    if (!gs_write_authored(path)) return 1;

    // --- head on: after `headon`, which "aims drivers directly at each other".
    // Walls down both sides, so every pass is a corridor and the cars meet on
    // it going opposite ways. Pavement, because the interest is the meeting and
    // not the grip.
    gs_flat(184, 172, GS_SURF_PAVEMENT);
    gs_wall(0, 3, 0, 184, GS_INT(3));
    gs_wall(169, 172, 0, 184, GS_INT(3));
    gs_generate_route(&gs_t, 106u, false);
    snprintf(path, sizeof path, "%s/head-on.gstrack", dir);
    if (!gs_write_authored(path)) return 1;

    // --- which way: after `whichway`, "seven different routes". The ground
    // offers three - a ramp, a dirt shortcut and clean pavement - and the route
    // crosses all of them, so the question is asked once a pass rather than
    // once a race.
    gs_flat(184, 176, GS_SURF_PAVEMENT);
    gs_ridges_across(50, 60, 16, 4, GS_INT(2));
    for (uint8_t x = 0; x < gs_t.w; x++) {
        for (uint8_t y = 60; y < 116; y++) gs_track_set_surface(&gs_t, x, y, GS_SURF_DIRT);
    }
    gs_generate_route(&gs_t, 107u, false);
    snprintf(path, sizeof path, "%s/which-way.gstrack", dir);
    if (!gs_write_authored(path)) return 1;

    // --- the oval: after `indy`. Nothing to learn but the car - no scenery, no
    // surprises, and a lap long enough to be a lap.
    gs_flat(180, 180, GS_SURF_PAVEMENT);
    gs_generate_route(&gs_t, 108u, true);
    snprintf(path, sizeof path, "%s/the-oval.gstrack", dir);
    if (!gs_write_authored(path)) return 1;

    // --- the big one: after `jumps`, described in the manual as "big ones".
    // Ramps and landings and nothing else to think about, so the only question
    // is how fast you arrive - which is the question a gravity dial makes
    // interesting.
    gs_flat(184, 172, GS_SURF_PAVEMENT);
    gs_ridges_across(48, 48, 18, 5, GS_INT(4));
    gs_band(96, 140, GS_SURF_DIRT);
    gs_generate_route(&gs_t, 109u, false);
    snprintf(path, sizeof path, "%s/the-big-one.gstrack", dir);
    if (!gs_write_authored(path)) return 1;

    // --- the long way round: after the Grand Prix circuits, which the manual
    // notes are all pavement, no jumps, Earth gravity, five laps. That is a
    // statement about what a plain track is for: when nothing is in the way,
    // the driving is the whole of it.
    gs_flat(184, 184, GS_SURF_PAVEMENT);
    gs_generate_route(&gs_t, 110u, true);
    snprintf(path, sizeof path, "%s/the-long-way-round.gstrack", dir);
    if (!gs_write_authored(path)) return 1;

    // --- and a dozen from the generator, chosen by driving them.
    if (!gs_write_generated(dir)) return 1;

    return 0;
}

// make_tracks - the stock tracks, written as data.
//
// **Content is data, not C.** A track compiled into the frontend is a track
// nobody can edit, share or replace, and it was a prototype from the day it
// was written. This produces the same files the game loads at run time, which
// the editor can open, change and save like any other track.
//
// **Thirty tracks, all of them draws from the generator's matrix, picked by
// racing them.** There used to be ten written out by hand and a handful from
// the generator; the hand-written ten were the *less* verified half of the set
// and two of them shipped losing a car off the world. Now every track that
// ships has cleared the same four bars - a sound route, a race long enough to
// be a race, every machine finishing from every grid slot, and nobody thrown
// off the field - and the spread of the set across the matrix is asserted
// below rather than hoped for.
//
// Links the simulation and nothing else, like every other tool that touches
// the formats.
#include "core/gs_ai.h"
#include "core/gs_analyse.h"
#include "core/gs_generate.h"
#include "core/gs_track.h"

#include <stdio.h>
#include <string.h>

static gs_track gs_t;

// GS_STOCK_MIN_ROUTE - how long a default track has to be - lives in
// core/gs_track.h, next to the race length it is compared against, so the tool
// that writes the tracks and the suite that checks the written ones hold them
// to the same number rather than to two numbers that agree today.

static bool gs_write(const char *path, const char *why) {
    static uint8_t buf[GS_TRACK_TILES * 4 + 4096];
    size_t n = gs_track_serialize(&gs_t, buf, sizeof buf);
    if (n == 0) return false;

    // Refuse to ship a track with a broken route. Cheap, and it catches a
    // gate somebody moved while editing the numbers above.
    if (gs_track_validate(&gs_t).problem != GS_TRACK_OK) {
        printf("  %s: the route is not sound\n", path);
        return false;
    }

    // **And refuse to ship one that is over in thirty seconds.** It was asked
    // for repeatedly that a default track be a real drive, and the requirement
    // kept being lost between one piece of work and the next. It is not a
    // thing to remember any more: a track under the floor is not written, and
    // the build fails. **How far it is raced, not how long the route is** - a
    // circuit's route is one lap of it; see gs_track_race_length.
    gs_fix raced = gs_track_race_length(&gs_t);
    if (raced < GS_INT(GS_STOCK_MIN_ROUTE)) {
        printf("  %s: %d tiles of racing, and the floor is %d\n", path,
               (int)(raced / GS_ONE), GS_STOCK_MIN_ROUTE);
        return false;
    }

    // **And refuse to ship one nobody can get round.**
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

    printf("  %-28s %3ux%-3u %2u gates %5d tiles raced  %s\n", path, gs_t.w,
           gs_t.h, gs_t.gate_count, (int)(raced / GS_ONE), why);
    return ok;
}

// --- choosing which tracks ship ----------------------------------------------

// **How long a pack stays a pack.** Cars leave the grid together and spread
// out over the opening; the shove that puts one off the world happens while
// they are still close. Measured on a set that shipped before this rule
// existed: the three cars lost over an edge went at 6.5, 8.3 and 25.4
// seconds, and the two lost to landings deep in a race went at 86 and 193. A
// minute covers the first kind with room and does not pay for the second,
// which this rule does not refuse a track for.
#define GS_STOCK_PACK_SECONDS 60u

// **Does a full grid stay on the world?** One car alone is never shoved; four
// abreast, the shove in the opening seconds is what loses cars over the drop.
// The bar is narrow and it is a property of the ground: **a track may not
// throw a car out of the world.** It cannot be asked statically - it has to be
// raced to be seen.
static bool gs_keeps_everybody_on_the_field(const gs_track *t) {
    gs_world w;
    gs_world_init(&w, GS_ONE);

    for (uint8_t slot = 0; slot < GS_MAX_CARS; slot++) {
        gs_fix x, y;
        gs_angle heading;
        gs_track_grid(t, slot, &x, &y, &heading);
        gs_world_add_car(&w, t, (uint8_t)(slot % GS_VEH_COUNT), x, y, heading);
    }

    for (uint32_t i = 0; i < GS_TICK_HZ * GS_STOCK_PACK_SECONDS; i++) {
        gs_input in[GS_MAX_CARS] = { 0 };
        for (uint8_t c = 0; c < w.car_count; c++) in[c] = gs_ai_drive(&w, t, c);
        gs_world_step(&w, t, in);

        for (uint8_t c = 0; c < w.car_count; c++) {
            // **Past the shoulder, not merely off the road.** The run-off is
            // there so a car that goes wide can come back; what is not
            // recoverable is the drop past it.
            const gs_fix over = GS_INT(GS_RUNOFF_TILES);
            if (w.car[c].x < -over || w.car[c].y < -over ||
                w.car[c].x > GS_INT(t->w) + over ||
                w.car[c].y > GS_INT(t->h) + over) {
                return false;
            }
        }
    }
    return true;
}

// **Every machine, from every slot on the grid, gets round** at Earth
// gravity, driven by the AI. A grid is staggered back from the line and
// across it, so the car in the last slot has a different corner to make and
// different ground to make it on. Opponents start in those slots, so a track
// that ships has to be drivable from all of them.
//
// **And it says no at the first car that does not.** This used to count
// finishers across all six machines and every slot, and a candidate the
// motorcycle wrecked on in its first minute went on to race the other five
// machines from four slots each for the full lap budget before it was
// refused. Nearly every candidate is refused, so that was nearly all of the
// tool's time - and the CI check that re-chooses the set had been timing out
// on it since vertical drama. **A car that has crossed no gate for two
// minutes is not going to**: the stuck cars in the refused candidates were
// wrecked and circling one gate from the first minute to the last, and the
// lap budget is up to a quarter of an hour. Both are what refuse a track
// now, so a refusal costs about one race instead of forty.
#define GS_STOCK_STUCK_SECONDS 120

static bool gs_everybody_finishes(const gs_track *t, gs_ai_style style) {
    uint32_t seconds = gs_analyse_seconds(t);

    for (uint8_t v = 0; v < GS_VEH_COUNT; v++) {
        for (uint8_t slot = 0; slot < GS_MAX_CARS; slot++) {
            gs_world w;
            gs_world_init(&w, GS_ONE);

            gs_fix x, y;
            gs_angle heading;
            gs_track_grid(t, slot, &x, &y, &heading);
            gs_world_add_car(&w, t, v, x, y, heading);

            bool round = false;
            uint8_t last_gate = w.car[0].next_gate;
            uint32_t since = 0;
            for (uint32_t i = 0; i < GS_TICK_HZ * seconds; i++) {
                gs_input in[GS_MAX_CARS] = { gs_ai_drive_style(&w, t, 0, style), 0, 0, 0 };
                gs_world_step(&w, t, in);
                // **Laps done, not laps counted.** A circuit's first crossing
                // of its start line is the run-up, three tiles from the grid,
                // and the raw count includes it - so this check passed every
                // circuit two seconds in and never raced one. Found the day a
                // crest stalled a car the gate had sworn could get round.
                if (gs_car_laps_done(t, &w.car[0]) > 0) { round = true; break; }
                if (w.car[0].next_gate != last_gate) {
                    last_gate = w.car[0].next_gate;
                    since = i;
                } else if (i - since > GS_TICK_HZ * GS_STOCK_STUCK_SECONDS) {
                    break;
                }
            }
            if (!round) return false;
        }
    }
    return true;
}

static bool gs_everybody_finishes_at_earth(const gs_track *t) {
    return gs_everybody_finishes(t, gs_ai_skill_style(GS_AI_SKILL_DEFAULT));
}

// **And the shortcut, if there is one, driven by every machine from every
// slot as well** - both ways, like everything else that ships.
static bool gs_shortcut_drivable(const gs_track *t) {
    if (!gs_track_has_shortcuts(t)) return true;
    return gs_everybody_finishes(t, gs_ai_shortcut_style(GS_AI_SKILL_DEFAULT));
}

// **Thirty tracks, spread across the matrix and asserted to be.**
//
// The quotas keep any one draw from filling the library - fifteen of each
// class, no more than six of a class at one length or one curviness - and the
// floors at the bottom of main() turn "the set is varied" from an impression
// into a build failure. A control added to the matrix next year and never
// shipped turns the tool red by itself.
#define GS_STOCK_COUNT 30
#define GS_STOCK_PER_CLASS 15
#define GS_STOCK_PER_CELL 6
// **No drama band takes more than its share.** The gate that races every
// candidate rejects the hard bands - bowls, gaps, crests - far more often
// than a level road, so a search that stops at thirty fills up with level
// and half-pipe roads first and never reaches them. Capped at six, the easy
// bands fill twenty-four of the thirty and the search keeps going until the
// rare ones supply the rest.
#define GS_STOCK_PER_DRAMA 6
// **The rare bands are chosen first.** A walk that takes the first thirty
// candidates to pass fills up with the common bands and never reaches the
// floors on the rare ones: the first regeneration after the routes dial
// shipped two tracks with a shortcut and two with small jumps, and capping
// the common bands did not help - once the caps bound, a candidate had to
// be rare in several ways at once, and two hundred thousand seeds held two.
// So the set is chosen in two passes over the same seed order: the first
// takes only candidates that raise an unmet floor on a rare band - a
// shortcut, small jumps, bowls, gaps, crests - and stops when those floors
// are met; the second fills the rest. The set is as reproducible as it was.
#define GS_STOCK_MIN_SHORTCUT 4
#define GS_STOCK_MIN_JUMPS    3
#define GS_STOCK_MIN_DRAMA    2

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "assets/tracks";
    char path[512];

    int written = 0;
    int by_class[GS_CLASS_COUNT] = { 0 };
    int by_len[GS_CLASS_COUNT][GS_LEN_COUNT] = { 0 };
    int by_curve[GS_CLASS_COUNT][GS_CURVE_COUNT] = { 0 };
    int len_total[GS_LEN_COUNT] = { 0 };
    int curve_total[GS_CURVE_COUNT] = { 0 };
    int jumps_total[GS_JUMPS_COUNT] = { 0 };
    int drama_total[GS_DRAMA_COUNT] = { 0 };
    int routes_total[GS_ROUTES_COUNT] = { 0 };

    // Two seeds can draw the same two-word name, and the same name is the
    // same file path - a silent overwrite, and a library of twenty-nine.
    char used[GS_STOCK_COUNT][32];

    // Seeds in order, so the set is reproducible and adding a dial later does
    // not reshuffle the tracks anybody already has.
    for (int pass = 0; pass < 2 && written < GS_STOCK_COUNT; pass++) {
    for (uint32_t n = 1; written < GS_STOCK_COUNT && n < 60000; n++) {
        if (pass == 0 && routes_total[GS_ROUTES_SHORTCUT] >= GS_STOCK_MIN_SHORTCUT &&
            jumps_total[GS_JUMPS_SMALL] >= GS_STOCK_MIN_JUMPS &&
            drama_total[GS_DRAMA_BOWLS] >= GS_STOCK_MIN_DRAMA &&
            drama_total[GS_DRAMA_GAPS] >= GS_STOCK_MIN_DRAMA &&
            drama_total[GS_DRAMA_CRESTS] >= GS_STOCK_MIN_DRAMA) {
            break;      // the floors are met; the second pass fills the rest
        }
        uint32_t seed = n * 7919u;

        gs_track_spec spec = gs_generate_spec_for(seed);
        if (by_class[spec.kind] >= GS_STOCK_PER_CLASS) continue;
        if (by_len[spec.kind][spec.length] >= GS_STOCK_PER_CELL) continue;
        if (by_curve[spec.kind][spec.curve] >= GS_STOCK_PER_CELL) continue;
        if (drama_total[spec.drama] >= GS_STOCK_PER_DRAMA) continue;
        // The first pass wants only the rare: a spec that can raise none of
        // the unmet floors is not generated, let alone raced.
        const bool rare_jumps = spec.jumps == GS_JUMPS_SMALL &&
                                jumps_total[GS_JUMPS_SMALL] < GS_STOCK_MIN_JUMPS;
        const bool rare_drama = (spec.drama == GS_DRAMA_BOWLS || spec.drama == GS_DRAMA_GAPS ||
                                 spec.drama == GS_DRAMA_CRESTS) &&
                                drama_total[spec.drama] < GS_STOCK_MIN_DRAMA;
        const bool maybe_cut = spec.routes == GS_ROUTES_SHORTCUT &&
                               routes_total[GS_ROUTES_SHORTCUT] < GS_STOCK_MIN_SHORTCUT;
        if (pass == 0 && !rare_jumps && !rare_drama && !maybe_cut) continue;

        char name[32];
        gs_generate_name(name, sizeof name, seed);
        bool taken = false;
        for (int i = 0; i < written && !taken; i++) {
            taken = strcmp(used[i], name) == 0;
        }
        if (taken) continue;

        gs_generate(&gs_t, seed);
        // A draw of "a shortcut" that found no pair of posts to cut between
        // is one way round, and raises no floor.
        if (pass == 0 && !rare_jumps && !rare_drama && !gs_track_has_shortcuts(&gs_t)) continue;
        if (gs_track_validate(&gs_t).problem != GS_TRACK_OK) continue;
        if (gs_track_race_length(&gs_t) < GS_INT(GS_STOCK_MIN_ROUTE)) continue;

        // **Every vehicle, or it does not ship.** A stock track only the
        // sprint car can finish tells a new player their choice of machine
        // was wrong, which is the opposite of what a starting set is for.
        if (!gs_everybody_finishes_at_earth(&gs_t)) continue;

        // **And it may not throw one of them off the world.** Raced after the
        // check above because that one is cheaper and most candidates fail it.
        if (!gs_keeps_everybody_on_the_field(&gs_t)) continue;
        if (!gs_shortcut_drivable(&gs_t)) continue;
        // **And the other way round.** Mirror mode races every shipped track
        // reversed, so a track ships only if its reverse passes the same
        // checks its forward route does: a route the validator accepts,
        // every machine round it from every grid slot, nobody off the field.
        static gs_track other;
        other = gs_t;
        gs_track_reverse(&other);
        if (gs_track_validate(&other).problem != GS_TRACK_OK) continue;
        if (!gs_everybody_finishes_at_earth(&other)) continue;
        if (!gs_keeps_everybody_on_the_field(&other)) continue;
        if (!gs_shortcut_drivable(&other)) continue;

        char why[160];
        gs_spec_line(&spec, &gs_t, why, sizeof why);

        snprintf(used[written], sizeof used[written], "%s", name);
        for (char *c = name; *c != '\0'; c++) {
            if (*c == ' ') *c = '-';
        }
        snprintf(path, sizeof path, "%s/%s.gstrack", dir, name);
        if (!gs_write(path, why)) return 1;

        by_class[spec.kind]++;
        by_len[spec.kind][spec.length]++;
        by_curve[spec.kind][spec.curve]++;
        len_total[spec.length]++;
        curve_total[spec.curve]++;
        jumps_total[spec.jumps]++;
        drama_total[spec.drama]++;
        routes_total[gs_track_has_shortcuts(&gs_t) ? GS_ROUTES_SHORTCUT : GS_ROUTES_ONE]++;
        written++;
    }
    }

    if (written < GS_STOCK_COUNT) {
        printf("  only %d of %d tracks were good enough to ship\n", written,
               GS_STOCK_COUNT);
        return 1;
    }

    // **The spread, asserted rather than believed.** The quotas above make
    // these reachable; a generator change that stops one band ever passing
    // the bars fails the build here, with the missing band named.
    printf("\n  the set: %d circuits, %d paths\n", by_class[GS_CLASS_CIRCUIT],
           by_class[GS_CLASS_PATH]);
    printf("  lengths standard/long/epic: %d/%d/%d\n", len_total[0],
           len_total[1], len_total[2]);
    printf("  curves flowing/winding/technical: %d/%d/%d\n", curve_total[0],
           curve_total[1], curve_total[2]);
    printf("  jumps none/small/big: %d/%d/%d\n", jumps_total[0],
           jumps_total[1], jumps_total[2]);

    bool spread = by_class[GS_CLASS_CIRCUIT] == GS_STOCK_PER_CLASS &&
                  by_class[GS_CLASS_PATH] == GS_STOCK_PER_CLASS;
    for (int i = 0; i < GS_LEN_COUNT; i++) {
        if (len_total[i] < 6) {
            printf("  too few tracks at length band %d: %d of at least 6\n",
                   i, len_total[i]);
            spread = false;
        }
    }
    for (int i = 0; i < GS_CURVE_COUNT; i++) {
        if (curve_total[i] < 6) {
            printf("  too few tracks at curve band %d: %d of at least 6\n",
                   i, curve_total[i]);
            spread = false;
        }
    }
    printf("  drama level/banked/bowls/pipes/gaps/crests: %d/%d/%d/%d/%d/%d\n",
           drama_total[0], drama_total[1], drama_total[2], drama_total[3],
           drama_total[4], drama_total[5]);
    printf("  routes one way/a shortcut: %d/%d\n", routes_total[0], routes_total[1]);
    if (routes_total[GS_ROUTES_SHORTCUT] < GS_STOCK_MIN_SHORTCUT) {
        printf("  too few tracks with a shortcut: %d of at least %d\n",
               routes_total[GS_ROUTES_SHORTCUT], GS_STOCK_MIN_SHORTCUT);
        spread = false;
    }
    for (int i = 0; i < GS_DRAMA_COUNT; i++) {
        if (drama_total[i] < GS_STOCK_MIN_DRAMA) {
            printf("  too few tracks at drama band %d: %d of at least %d\n",
                   i, drama_total[i], GS_STOCK_MIN_DRAMA);
            spread = false;
        }
    }
    for (int i = 0; i < GS_JUMPS_COUNT; i++) {
        if (jumps_total[i] < GS_STOCK_MIN_JUMPS) {
            printf("  too few tracks at jumps band %d: %d of at least %d\n",
                   i, jumps_total[i], GS_STOCK_MIN_JUMPS);
            spread = false;
        }
    }
    if (!spread) {
        printf("  the set does not cover the matrix\n");
        return 1;
    }

    return 0;
}

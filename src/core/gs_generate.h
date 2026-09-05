// gs_generate.h - tracks from a number.
//
// **A track is a draw from a matrix, not a variation on a template.** There
// were four hard-coded shapes and every one of them laid the same serpentine;
// the shapes chose the scenery and nothing chose the track. What replaced them
// is ten dials, each drawn from the seed: what class of track it is, how far it
// is raced, how it corners, how it straightens, how much air it is about, what
// the ground does, how hard the ground does it, what the weight is like, what
// it is dressed in and how wide the road is. Two of those draws contradicting
// each other is resolved at draw time, in gs_generate_spec_for, so the spec a
// caller sees is always the spec that was built.
//
// The route itself is *grown*, not laid: a randomised walk over a coarse cell
// grid, biased by the curviness and straightness draws, so the way a track
// folds is an outcome of the dice rather than a family it belongs to.
#ifndef GS_GENERATE_H
#define GS_GENERATE_H

#include <stddef.h>

#include "core/gs_track.h"

// What kind of race it is. A circuit closes and is driven GS_STOCK_LAPS times;
// a path runs end to end once, so all of its distance is in the route.
typedef enum gs_gen_class {
    GS_CLASS_CIRCUIT = 0,
    GS_CLASS_PATH,
    GS_CLASS_COUNT
} gs_gen_class;

// How far the race is, in tiles actually raced - a circuit's route is one lap
// of it. The epic ceiling is the field's: GS_TRACK_MAX bounds the world at 192
// tiles a side, and about twelve hundred tiles of race is what an organic route
// can spend inside that without folding into mush.
typedef enum gs_gen_length {
    GS_LEN_STANDARD = 0,   // about 630 to 840 tiles raced
    GS_LEN_LONG,           // about 780 to 1030
    GS_LEN_EPIC,           // about 950 to 1300; a path's tops out near 1100
    GS_LEN_COUNT
} gs_gen_length;

// How often the route corners, and how gently when it does. This biases the
// walk that grows the route - flowing hoards its straights and rounds what
// corners it must take; technical corners constantly and sharply.
typedef enum gs_gen_curve {
    GS_CURVE_FLOWING = 0,
    GS_CURVE_WINDING,
    GS_CURVE_TECHNICAL,
    GS_CURVE_COUNT
} gs_gen_curve;

// What the straights are like, independent of the cornering: a technical track
// with one huge straight is a real track, and this is the dial that builds it.
typedef enum gs_gen_straight {
    GS_STRAIGHT_BROKEN = 0,   // nothing long enough to rest on
    GS_STRAIGHT_BALANCED,     // straights, but none that decides the race
    GS_STRAIGHT_POWER,        // one long enough that top speed matters
    GS_STRAIGHT_COUNT
} gs_gen_straight;

// How much air the track is about. None still shapes the ground - a flat field
// is not a lesser track, it is a missing one - it just builds nothing meant to
// be flown off.
typedef enum gs_gen_jumps {
    GS_JUMPS_NONE = 0,
    GS_JUMPS_SMALL,   // bumps taken flat
    GS_JUMPS_BIG,     // ramps left at speed
    GS_JUMPS_COUNT
} gs_gen_jumps;

// The shape of the ground the route is cut through.
typedef enum gs_gen_relief {
    GS_RELIEF_FLAT = 0,
    GS_RELIEF_ROLLING,   // continuous hills
    GS_RELIEF_RIDGED,    // linear ridges at an angle to the world
    GS_RELIEF_BASIN,     // a dished field, high at the rim
    GS_RELIEF_COUNT
} gs_gen_relief;

// How hard the ground does it: the difference between the lowest and highest
// ground, in bands. Drawn separately from the shape, because a subtle basin
// and a severe basin are different tracks over the same idea.
typedef enum gs_gen_range {
    GS_RANGE_SUBTLE = 0,
    GS_RANGE_MODERATE,
    GS_RANGE_SEVERE,
    GS_RANGE_COUNT
} gs_gen_range;

// What the weight is like. Painted onto the tiles, not set on the race - see
// gs_track.h for why gravity is a field. Pockets are centred on the route so
// they are driven through rather than decorating a corner of the map.
typedef enum gs_gen_gravity {
    GS_GRAV_EARTH = 0,
    GS_GRAV_LIGHT,    // pockets below Earth
    GS_GRAV_HEAVY,    // pockets above it
    GS_GRAV_SPLIT,    // half the world one way, half the other
    GS_GRAV_COUNT
} gs_gen_gravity;

// What the ground is dressed in, over the base surface.
typedef enum gs_gen_dress {
    GS_DRESS_PLAIN = 0,   // one ground, one road
    GS_DRESS_BANDED,      // broad bands of a second ground
    GS_DRESS_PATCHWORK,   // patches of foreign ground crossing the route
    GS_DRESS_COUNT
} gs_gen_dress;

// How much room a mistake gets.
typedef enum gs_gen_width {
    GS_WIDTH_NARROW = 0,
    GS_WIDTH_STANDARD,
    GS_WIDTH_WIDE,
    GS_WIDTH_COUNT
} gs_gen_width;

// One track, as its dials. Every field is one of the enums above plus the base
// surface, and gs_generate_spec_for is the only place a spec is drawn - the
// vetoes live there, so a spec in hand is always a spec that can be built.
// **Vertical drama the ground can already hold.** What shape the road takes
// in its own cross-section and along its own length, over and above the
// relief of the field it is cut through. None of these need the world to be
// anything but a field of heights; the generator simply never asked for them.
typedef enum gs_gen_drama {
    GS_DRAMA_NONE = 0,   // a level road
    GS_DRAMA_BANKED,     // corners banked, the outside raised, harder corners more
    GS_DRAMA_BOWLS,      // banked and hollowed: a corner is a dish you drop into
    GS_DRAMA_PIPES,      // the road's edges curve up into quarter-pipe walls
    GS_DRAMA_GAPS,       // a trench after every ramp, with a wall to climb if short
    GS_DRAMA_CRESTS,     // ridges that drop away on the far side
    GS_DRAMA_COUNT
} gs_gen_drama;

// **One way round, or a shortcut.** The original's "whichway" offered
// seven routes; this generator offered one. A shortcut is a second road,
// narrower and rougher, carved straight between two checkpoints where the
// main road loops well away from the line between them - a decision every
// lap rather than a line to memorise. It needs no change to what a route
// is: the gates describe the main way, and a car on the shortcut misses
// waypoints it is forgiven for missing.
typedef enum gs_gen_routes {
    GS_ROUTES_ONE = 0,
    GS_ROUTES_SHORTCUT,
    GS_ROUTES_COUNT
} gs_gen_routes;

typedef struct gs_track_spec {
    gs_gen_class    kind;
    gs_gen_length   length;
    gs_gen_curve    curve;
    gs_gen_straight straight;
    gs_gen_jumps    jumps;
    gs_gen_relief   relief;
    gs_gen_range    range;
    gs_gen_gravity  gravity;
    gs_gen_dress    dress;
    gs_gen_width    width;
    gs_gen_drama    drama;
    gs_gen_routes   routes;
    gs_surface      base;
} gs_track_spec;

// The dials a seed draws, vetoes already applied. Deterministic: the same seed
// always answers the same, on every compiler and platform, like the physics.
gs_track_spec gs_generate_spec_for(uint32_t seed);

// The track a seed names: spec drawn, ground built, route grown, road carved,
// gates laid. Deterministic for the same reason the physics is.
void gs_generate(gs_track *t, uint32_t seed);

// The same, with the dials chosen by the caller rather than the seed - the
// seed still decides everything the spec does not. gs_generate is exactly
// gs_generate_from_spec(t, seed, gs_generate_spec_for(seed)).
void gs_generate_from_spec(gs_track *t, uint32_t seed, const gs_track_spec *spec);

// The road's half-width for this spec, in tiles. Public because a gate has to
// be wider than the road it crosses, and anything checking that needs the
// number the generator actually used.
uint8_t gs_spec_road(const gs_track_spec *spec);

// The one-line reason this track exists - "a winding epic circuit over rolling
// ice, small jumps, light pockets" - after the 1985 manual, where every track
// had a reason somebody could say in one line.
// With the track it built, when there is one: a shortcut the dial asked
// for that no pair of checkpoints could hold reads as "one way round",
// because that is the track. Null when there is no track yet.
void gs_spec_line(const gs_track_spec *spec, const gs_track *built, char *out,
                  size_t cap);

// Two words from the seed, the same two every time.
void gs_generate_name(char *out, size_t cap, uint32_t seed);

// **Today's track.** One seed a day, the same for everyone, so a track
// nobody built is a track everybody is on. A day is counted in UTC days
// since the first of January 2026, so two machines in two time zones agree
// on it for all but the hours around midnight. The seed is the first of a
// day's attempts whose track validates, is long enough to ship, and can be
// got round by the computer both ways - the same gate the shipped set
// passes, so that a day is never a dud - and every machine walks the same
// attempts in the same order and lands on the same one.
#define GS_DAILY_EPOCH_DAY 20454u          // 2026-01-01, in days since 1970
#define GS_DAILY_ATTEMPTS  24u
uint32_t gs_daily_seed(uint32_t day, uint32_t attempt);
bool     gs_daily_track(gs_track *t, uint32_t day, uint32_t *seed);
// The daily for `day` or the day before, if `hash` is one of them: how a
// server that was never sent today's track recognises a time set on it.
bool     gs_daily_track_for_hash(gs_track *t, uint32_t day, uint64_t hash);

#endif // GS_GENERATE_H

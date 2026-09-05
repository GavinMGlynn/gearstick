// gs_track.h - the ground: what shape it is, what it is made of, and how hard
// it pulls.
//
// **Everything downstream keys off this representation.** A track is a grid of
// tiles, but the shape is stored on the tile *corners* rather than the tiles,
// which is the decision the rest of the game rests on: ramps stitch
// continuously into their neighbours by construction, ground height at any
// point is a bilinear sample rather than a lookup, and a surface gradient falls
// out of the same four numbers for free. Nothing anywhere may assume a tile is
// flat.
//
// Three fields sit on the tiles themselves - surface, gravity and (at run time,
// in the world rather than here) wear.
//
// **Gravity is a field, not a setting.** The original's fourteen gravity steps
// were a pre-race dropdown; here every tile carries its own multiplier, so a
// low-gravity pocket at the top of a jump or a Jupiter zone that pins a car
// through a banked turn is something you paint. The physics asks the track what
// gravity is *here*, every tick. Anything that reads gravity once per race has
// broken the feature.
#ifndef GS_TRACK_H
#define GS_TRACK_H

#include "core/gs_fixed.h"

// A track is at most 64 tiles square. That is four times the original's usable
// area and keeps the whole thing - shape, surface and gravity - inside 22 KB,
// small enough to hash, copy and send without anyone thinking about it.
// **How big a track may be, in tiles.**
//
// This was sixty-four, and sixty-four is why every default track was a
// thirty-second drive. A route has to fit inside the field, and a field of
// 64x64 is 4096 tiles: with a road four tiles wide and a verge either side, the
// longest route that can be folded into it is about five hundred tiles, which
// at the speeds in gs_parts.c is well under a minute of driving. Every attempt
// to make the shipped tracks longer was clipped by this number, and nothing
// said so - the generator simply laid the longest arc the field allowed.
//
// A hundred and ninety-two is 36864 tiles, which holds a serpentine of thirteen
// hundred - ten to twenty times the old set, which is what a playable track
// needs. A hundred and twenty-eight was tried first and reached only ten times,
// at the bottom of the range. It costs nine times the memory per track: a track
// is about 148 KB rather than 17, and the library holds GS_LIBRARY_MAX of them.
#define GS_TRACK_MAX     192
#define GS_TRACK_TILES   (GS_TRACK_MAX * GS_TRACK_MAX)
#define GS_TRACK_CORNERS ((GS_TRACK_MAX + 1) * (GS_TRACK_MAX + 1))

// One world unit is one tile, and one tile is four metres. Pixels do not exist
// in this layer.
#define GS_TILE_METRES 4

// **A ground for every world on the gravity dial.**
//
// The dial names eight bodies and there were three surfaces, all of which could
// have been a car park. These are the grounds those worlds are actually made of
// - and each one earns its place in the numbers below rather than in a tint.
// **A surface that drives like another surface is a colour swatch**, and the
// rule that refuses a feature for adding fidelity without adding predictability
// refuses those too.
//
// Appended, never renumbered. The value is what a saved track stores, so moving
// one silently changes the ground under every track anybody has built.
typedef enum gs_surface {
    GS_SURF_PAVEMENT = 0,   // Earth, made
    GS_SURF_DIRT,           // Earth, not
    GS_SURF_ICE,            // Europa, and a hard winter

    GS_SURF_SAND,           // Mars, and every desert
    GS_SURF_GRAVEL,         // Ceres, and a rally stage
    GS_SURF_ROCK,           // Venus, Io - basalt, and nothing kind about it
    GS_SURF_DUST,           // the Moon: regolith, and nothing has ever swept it
    GS_SURF_SLUSH,          // Titan, and a thaw
    GS_SURF_GRASS,          // Earth again, and the one everybody underestimates

    GS_SURF_COUNT
} gs_surface;

typedef struct gs_surface_def {
    const char *name;
    gs_fix      grip;      // how much sideways force it will take before sliding
    gs_fix      rolling;   // rolling resistance, as a fraction of gravity
    gs_fix      drive;     // how much of the engine reaches the ground

    // How the surface changes under use. Wear lives in the world rather than
    // the track - it is what happened during *this* race, not what the track
    // is - and these say what a fully worn tile becomes.
    //
    // The point is that lap five is not lap one: the line everyone has been
    // taking stops being the fast one, and running second becomes a reason to
    // try somewhere else rather than a position to suffer.
    gs_fix      wear_rate;     // how quickly a working tyre marks it
    gs_fix      wear_grip;     // grip multiplier when fully worn
    gs_fix      wear_rolling;  // rolling-resistance multiplier when fully worn
} gs_surface_def;

extern const gs_surface_def gs_surfaces[GS_SURF_COUNT];

// --- what surrounds a track --------------------------------------------------
//
// **A shoulder, and then a drop.** Off the authored tiles the ground carries on
// level for a few tiles as loose run-off, and then it falls away. Leaving the
// track costs you time first and the race only if you keep going, which is the
// consequence a player should be able to see coming.
//
// The alternative that was there before was neither: nothing was drawn outside
// the track, so a player saw a cliff, and the physics clamped to the edge tile,
// so they drove on an infinite invisible plain. What you could see and what you
// could drive on disagreed, which is the one thing a surround must not do.
//
// No wall, anywhere. A car is never refused; it is charged.

// How far the level run-off reaches past the edge, in tiles.
//
// Wide enough to gather up an honest mistake, narrow enough that cutting a
// corner across it is never worth the time it costs. **Ten and not four**: four
// is eight tenths of a second at racing speed, which is not a run-off, it is a
// thinner cliff edge - a car that put a wheel over had no time to do anything
// about it, and "recoverable if you are quick" was not true of it.
#define GS_RUNOFF_TILES 10

// How steeply the ground falls once the run-off ends, in tiles per tile. Well
// past GS_MAX_CLIMB, so a car that has gone over cannot drive back up - the
// shoulder is the recovery, and the drop is not.
#define GS_RUNOFF_FALL GS_INT(3)

// **And how deep it goes, because forever does not fit in a number.** A drop
// that kept falling with distance overflowed Q16.16 for a car thrown a few
// thousand tiles off the map at low gravity - which is not a hypothetical, it is
// what the AI sweep did the first time this existed. The floor is far below
// anything a car can be recovered from, so bounding it changes nothing anybody
// can drive and stops the arithmetic wrapping into a ground that is suddenly
// above them.
#define GS_RUNOFF_FLOOR GS_INT(64)

// What the run-off is made of.
//
// **Sand, because a run-off is a thing that stops you.** The first version used
// dust, on the reasoning that it is loose - which it is, and it also has almost
// no rolling resistance, so a car that ran wide onto it kept every bit of its
// speed and sailed straight across to the drop. A gravel trap works by drag and
// not by slipperiness. Sand has the highest rolling resistance of the nine and
// enough grip left to steer with, so a car that brakes on entering it stops
// inside it, and one that does not, does not.
#define GS_RUNOFF_SURFACE GS_SURF_SAND

// Per-tile gravity, as a multiplier on the race setting. 64 is 1x, so the byte
// spans nothing at all up to just under 4x - Moon to well past Jupiter.
#define GS_GRAVITY_UNIT 64

// A gate is a line a car drives through, and a route is an ordered list of
// them. Gate zero is the start and the finish.
//
// **Gates rather than road tiles.** The terrain here is free-form - a bowl, a
// plateau, a jump to nowhere are all things a player can build - and a ribbon
// of road tiles would insist that the drivable part of a track is a ribbon.
// Gates leave the ground alone and say only which way round it goes, which is
// also the more predictable of the two: the order is authored rather than
// inferred from a shape.
// **How many checkpoints a route may have.** Thirty-two was plenty for a route
// fifty tiles long and is not for one of eight hundred. Ninety-six costs a
// kilobyte and a half in a track that is already a hundred and fifty. The
// generator lays one every four dozen tiles - it was one a dozen, which read
// as a fence of posts down every straight, and a player asked for them four
// times further apart - so a shipped track uses a quarter of this; the rest
// is room for a track built by hand with a gate wherever its author wants one.
#define GS_TRACK_MAX_GATES 96
// One bit per gate: which checkpoints a shortcut leaves. See gs_track.
#define GS_TRACK_SHORTCUT_BYTES (GS_TRACK_MAX_GATES / 8)

// **What kind of route this is, which is the difference between a track that
// goes somewhere and one that does not.**
//
// This was the missing distinction, and its absence is why the shipped tracks
// were not raceable. The generator laid two gates - one near the left edge, one
// near the right - and called it a route; the simulation counted a lap when the
// *last* gate was crossed. So a "lap" was a one-way trip, and lap two meant
// driving all the way back across the field with nothing marking the way, to a
// line that was where you had started. Every part of that worked exactly as
// written and the whole was not a race.
//
// A track now says which of the two things it is, and the two are raced
// differently:
//
// - **A circuit** is a closed loop. Gate zero is the start *and* the finish -
//   one line, crossed at the beginning of every lap and at the end of every
//   lap - and the gates after it run round the loop in order.
// - **A sprint** is a path from somewhere to somewhere else. Gate zero is the
//   start line, the last gate is the finish, and they are a long way apart.
//   There are no laps to count; arriving is the whole race.
typedef enum gs_route_kind {
    GS_ROUTE_SPRINT = 0,   // start at gate zero, finish at the last one
    GS_ROUTE_CIRCUIT       // a loop, gate zero being start and finish both
} gs_route_kind;

typedef struct gs_gate {
    gs_fix   x, y;          // centre, in tiles
    gs_fix   half_width;    // how far the gate reaches either side of centre
    gs_angle heading;       // the direction a car travels *through* it
    uint16_t pad;
} gs_gate;

typedef struct gs_track {
    uint8_t w, h;                        // in tiles; both in [1, GS_TRACK_MAX]

    // Corner heights in 1/256 of a tile, on a (w+1) x (h+1) lattice indexed
    // [y * (GS_TRACK_MAX + 1) + x]. The stride is the maximum rather than the
    // track's own width so that resizing a track in the editor never has to
    // move the data.
    int16_t corner[GS_TRACK_CORNERS];

    uint8_t surface[GS_TRACK_TILES];     // gs_surface, indexed [y * GS_TRACK_MAX + x]
    uint8_t gravity[GS_TRACK_TILES];     // multiples of 1/GS_GRAVITY_UNIT

    uint8_t route;                       // gs_route_kind
    // Every Nth gate is a checkpoint a car must cross in order; the rest are
    // waypoints that describe the route and may be missed. 1 means every
    // gate counts, which is what every track was before this existed.
    uint8_t checkpoint_every;
    // **Shortcuts.** Bit k set means a second road leaves checkpoint k
    // straight for the next checkpoint, narrower and rougher than the main
    // one - a decision every lap rather than a line to memorise. Nothing
    // else about the route changes: the gates describe the main way, the
    // waypoints between two checkpoints may be missed, and a car on the
    // shortcut simply misses them. All zero on every track from before.
    uint8_t shortcut[GS_TRACK_SHORTCUT_BYTES];
    uint8_t gate_count;
    gs_gate gate[GS_TRACK_MAX_GATES];    // gate[0] is where a race begins
} gs_track;

// Which gate ends a lap - or, on a sprint, ends the race. Gate zero on a
// circuit, because a loop's start line is its finish line; the last gate on a
// sprint, because a path's finish is at the far end of it.
uint8_t gs_track_finish_gate(const gs_track *t);

// **Whether gate `i` is one a car has to cross, or one it is merely shown.**
//
// A route is described by its gates - the driver steers by them, the length
// is measured through them, the line on the ground is drawn along them - and
// describing a winding road takes one every dozen tiles. A *checkpoint* is a
// different thing: a line a player must cross in order, or the lap does not
// count. Asking every gate to be both made a fence of posts down every
// straight, so a track says how many gates make a checkpoint, and gate zero
// and the finish always do whatever the count says.
bool gs_track_is_checkpoint(const gs_track *t, uint8_t i);

// A shortcut leaves gate `i` (a checkpoint) for the next checkpoint.
bool    gs_track_shortcut_from(const gs_track *t, uint8_t i);
void    gs_track_set_shortcut(gs_track *t, uint8_t i, bool on);
bool    gs_track_has_shortcuts(const gs_track *t);
// The next checkpoint after gate `i`, round a circuit; `i` itself on a
// path with no checkpoint after it.
uint8_t gs_track_next_checkpoint(const gs_track *t, uint8_t i);

// **The same track, raced the other way round.** Mirror mode. The gates are
// put in the opposite order and each is turned half a turn, so a car drives
// through every one of them the way it now travels; the ground is not
// touched. A circuit keeps its start line - it is the same line, crossed the
// other way - and a path starts where it used to finish. Reversing twice is
// the track it was, to the hash.
//
// A reversed track has a hash of its own, so its records and ghosts are its
// own too: a lap backwards is not a lap of the same thing.
void     gs_track_reverse(gs_track *t);
uint64_t gs_track_reversed_hash(const gs_track *t);

// Is this track a loop? Then one line does both jobs and the renderer draws one
// chequered line rather than a plain start and a chequered finish.
bool gs_track_is_circuit(const gs_track *t);

#define GS_CORNER_STRIDE (GS_TRACK_MAX + 1)
#define GS_TILE_INDEX(x, y) ((size_t)(y) * GS_TRACK_MAX + (size_t)(x))

// Heights are stored in 1/256 tile and used in Q16.16.
#define GS_HEIGHT_SHIFT 8

void gs_track_init(gs_track *t, uint8_t w, uint8_t h, gs_surface surface);

// Whether a point is over the authored track at all. Everything outside it is
// still drivable - the ground continues at the nearest edge's height and
// surface - so this answers "is this on the track", not "is this solid".
bool gs_track_contains(const gs_track *t, gs_fix x, gs_fix y);

// Ground height under a point, bilinear across the containing tile.
gs_fix gs_track_height(const gs_track *t, gs_fix x, gs_fix y);

// The gradient of the ground at a point: how much height changes per tile along
// each axis. The physics wants this directly; the renderer turns it into a
// normal. Either output may be null.
void gs_track_slope(const gs_track *t, gs_fix x, gs_fix y, gs_fix *dzdx, gs_fix *dzdy);

gs_surface gs_track_surface(const gs_track *t, gs_fix x, gs_fix y);

// How far outside the authored tiles a point is, in tiles. Zero on the track.
gs_fix gs_track_outside(const gs_track *t, gs_fix x, gs_fix y);

// The gravity multiplier at a point, in Q16.16 where GS_ONE is 1x. Sampled per
// tick, never cached - see the header comment.
gs_fix gs_track_gravity(const gs_track *t, gs_fix x, gs_fix y);

void gs_track_set_corner(gs_track *t, uint8_t x, uint8_t y, gs_fix height);

// What a corner is now. The lattice is public, but reaching into it to read one
// value means every caller repeats the stride and the units - and the units are
// the part that is easy to get wrong, because storage is 1/256 tile and
// everything else is Q16.16.
gs_fix gs_track_corner_at(const gs_track *t, uint8_t x, uint8_t y);
void gs_track_set_surface(gs_track *t, uint8_t x, uint8_t y, gs_surface s);
void gs_track_set_gravity(gs_track *t, uint8_t x, uint8_t y, gs_fix multiplier);

// How many cars the starting grid lines up. The same number as GS_MAX_CARS,
// stated here because the route belongs to the track and the track does not know
// about the simulation; gs_sim.h asserts the two agree.
#define GS_TRACK_GRID 4

// How far behind the start line the grid sits, in tiles. Public because the
// renderer draws the start line *where the cars actually are* rather than
// somewhere that looks about right - the same reason gs_track_grid is one
// definition shared by the race and the analyser. Two answers to "where does a
// race begin" is a start line painted somewhere nobody starts.
#define GS_GRID_BACK GS_INT(3)

// **And how much further back each slot after the first sits.**
//
// The grid used to be four cars abreast, all level with each other. Across a
// fourteen tile gate that is three and a half tiles apart, which sounds like
// room until every one of them steers for the same racing line: two seconds
// after the flag the field is converging, still level, and a car in the middle
// can be hit from both sides in the same tick. Two horizontal impulses partly
// cancel; the lift each one adds cannot, so the car in the middle is thrown
// nearly five tiles into the air and ten tiles backwards, off the top of the
// field, wrecked, on **six of the eighteen tracks that ship**. It only happens
// with all four slots filled, which is the only grid a player ever races.
//
// So the grid is an echelon, the way a starting grid has been since before any
// of us: each slot sits a stagger further back than the one beside it, so **no
// car is level with either of its neighbours**. A car with an empty diagonal to
// be shoved into is a car that gets shoved rather than squeezed, and one shove
// is a shove.
//
// **And the stagger is as small as it is** because depth is not free. A
// route that folds back every thirty tiles has the previous pass of itself
// close behind the line, so a grid that reaches nine tiles back puts a car on
// ground that belongs to a different part of the route - and it showed:
// stepping every slot back in turn put three of twelve generated seeds beyond
// being finished from every slot at all, which
// `a_generated_race_can_actually_be_finished` caught and its own comment had
// warned about. Alternating reaches five tiles rather than nine and separates
// exactly the cars that need it.
#define GS_GRID_STAGGER GS_RATIO(150, 100)

// Append a gate to the route. Returns its index, or -1 if the route is full.
int gs_track_add_gate(gs_track *t, gs_fix x, gs_fix y, gs_angle heading, gs_fix half_width);

// Remove a gate, closing the gap so the remaining order is unchanged.
bool gs_track_remove_gate(gs_track *t, uint8_t index);

// Did a car travelling from (px, py) to (nx, ny) pass through this gate?
//
// A gate is directional: driving through it backwards is not a crossing, which
// is what stops a player reversing over the finish line to score laps. And it
// is finite: passing the plane beyond the gate's width misses it, which is what
// makes a gate a gate rather than an infinite tripwire across the world.
bool gs_gate_crossed(const gs_gate *g, gs_fix px, gs_fix py, gs_fix nx, gs_fix ny);

// **And whether that step went past the gate instead of through it.**
//
// The same step, the same plane, the other answer: it reached the far side the
// way a car driving the route does, and did it outside the gate's width. That
// is a checkpoint missed, and it is worth being able to say so because the
// simulation only ever tests the gate a car is *expecting* - so one missed
// gate silently stops every later crossing counting, the finish included. A
// player who ran wide at a corner drove the rest of the lap, crossed the
// chequer, and was told nothing at all.
//
// Not the negation of gs_gate_crossed: a step that never reaches the plane is
// neither crossed nor missed, which is what a car still on its way to the gate
// is doing.
bool gs_gate_missed(const gs_gate *g, gs_fix px, gs_fix py, gs_fix nx, gs_fix ny);

// **Face every gate the way the route goes through it.**
//
// A gate is a plane whose normal is its heading - `gs_gate_crossed` is written
// against that, and so is the arrow drawn on the ground - so a gate facing
// somewhere the route does not go is a gate you have to cross sideways and an
// arrow pointing at nothing. Every gate on the tracks written by hand in
// tools/make_tracks.c was given a heading of zero, which is east; `the
// crossing` is a figure of eight and all four of its gates faced east, one of
// them ninety degrees off the way anybody drives through it. It shipped like
// that, because nothing had ever compared a gate's facing to its route.
//
// The way through a gate is the tangent of the route there, and the chord from
// the gate before it to the gate after it is that tangent to within a degree on
// anything anybody would call a track. A loop wraps; a path takes the first and
// last gates from the ends.
void gs_track_face_along_route(gs_track *t);

// **Where the route is, a fraction of the way along one leg of it.**
//
// `leg` is the gate the leg starts at and `s` runs from zero at that gate to
// GS_ONE at the next; a loop's last leg returns to gate zero. The curve is a
// Catmull-Rom through the gate positions, because a straight chord between
// gates cuts the corner - on a four gate loop it would run straight across the
// infield - and because the road a generated track carves is a curve of the
// same shape.
//
// **One definition, so everything that says where the route is agrees.** The
// line painted on the ground and the line drawn on the minimap are the same
// line; two of them computed separately would be two routes, and the one you
// could see from the car would not be the one you were steering by.
void gs_track_route_point(const gs_track *t, uint8_t leg, gs_fix s,
                          gs_fix *out_x, gs_fix *out_y);

// How many legs a route has: one fewer than its gates for a path, and one per
// gate for a loop, which comes back to where it started.
uint8_t gs_track_route_legs(const gs_track *t);

// **How long the route is, in tiles**, as the chords between its gates - a loop
// closing back to its first gate and a path stopping at its last.
//
// One definition, because the number decides whether a track is long enough to
// ship and that verdict cannot depend on who measured it. `tools/make_tracks.c`
// refuses to write a track under GS_STOCK_MIN_ROUTE and the suite holds the
// tracks already in `assets/tracks/` to the same floor.
gs_fix gs_track_route_length(const gs_track *t);

// **How long a track that ships has to be, in tiles of route.** Ten times the
// set of August 2026, which ran 28 to 173 tiles and averaged 63 - twenty-seven
// seconds of driving. A default track being a race rather than a demonstration
// has been asked for repeatedly and lost repeatedly between one piece of work
// and the next, so it is a number the tree is red without rather than a thing
// anybody is asked to remember.
#define GS_STOCK_MIN_ROUTE 630

// **How many times a stock circuit is driven.** A circuit's route is one lap of
// it, and a lap is not a race.
#define GS_STOCK_LAPS 3

// **How far a stock track is actually raced**, which is the number the floor is
// a floor on: a path once end to end, a circuit its route times the laps.
//
// The floor was written against the serpentine, when every route - loop and path
// alike - was one long fold driven once, and route length and race length were
// the same number. They are not the same number for a circuit that is a shape
// rather than a fold, and holding a shape to a fold's floor is what kept every
// track in the game looking identical: the longest closed curve that fits a
// field under two hundred tiles across is four hundred tiles, so six hundred and
// thirty could only ever be met by folding, and anything folded six times in a
// square field is a serpentine. A player said it plainly - "every track can't be
// the same shape".
//
// **What the floor is for is unchanged**: a default track being a race rather
// than a demonstration, asked for repeatedly and lost repeatedly. Three laps of
// a three hundred tile circuit is nine hundred tiles of driving, which is the
// thing that was being asked for; one lap of it is not, and this is why the laps
// are in the sum rather than left to whoever sets up the race.
//
// One definition, because the verdict decides what ships and cannot depend on
// who measured it: `tools/make_tracks.c` refuses to write a track under the
// floor and the suite holds the tracks already in `assets/tracks/` to it.
gs_fix gs_track_race_length(const gs_track *t);

// **Where a car waits for the flag.** Behind the start line, abreast across it,
// facing the way the route runs through it - so the first thing a car does is
// cross the line it has to cross, at whatever speed it managed on the way.
//
// One definition, used by the race and by the analyser both, because the two
// disagreeing is the analyser reporting on a race nobody drove. Starting *on*
// the line instead leaves a car with its own position to aim at and no reason to
// go anywhere, and whether it recovers depends on how much room it has to
// wander: driveable tracks then come back impossible.
//
// `slot` is the car's place on the grid, 0 to GS_MAX_CARS - 1, spread across the
// gate's width. Out-of-range slots are clamped, and a track with no route puts
// everybody in the middle of it.
void gs_track_grid(const gs_track *t, uint8_t slot,
                   gs_fix *x, gs_fix *y, gs_angle *heading);

// The same grid, centred on the line for the number of cars actually racing.
//
// gs_track_grid spreads slots across the gate as if all GS_TRACK_GRID of
// them were taken, so a two-car race put both cars on one side of the line -
// one in the middle, one at the edge - and a solo start was off-centre for
// no reason anybody chose. The slot *pitch* is the full grid's either way:
// that spacing is what the pack-shove measurements were taken at, and how
// far apart neighbours start should not depend on how many neighbours there
// are. What moves is only where the pack sits: symmetric about the gate's
// centre. A full grid is placed exactly where gs_track_grid places it.
void gs_track_grid_of(const gs_track *t, uint8_t slot, uint8_t racing,
                      gs_fix *x, gs_fix *y, gs_angle *heading);

// --- validation -----------------------------------------------------------
//
// What can be checked about a route without driving it. **Completability is
// not here**: "can a car actually get round this" needs something that drives,
// so it belongs with the analyser, and calling it validation would be claiming
// a check nothing performs.
//
// The problem is returned rather than a message, so `src/core/` needs no string
// formatting and whoever displays it decides how. `gs_track_problem_text` gives
// the plain English for callers that just want to say it.

typedef enum gs_track_problem {
    GS_TRACK_OK = 0,
    GS_TRACK_NO_START,        // no gates at all: nowhere to start, nothing to finish
    GS_TRACK_TOO_FEW_GATES,   // one gate is a line, not a route
    GS_TRACK_GATE_OFF_TRACK,  // a gate, or an end of one, hangs off the world
    GS_TRACK_GATE_TOO_NARROW, // a gate nothing can fit through
    GS_TRACK_GATES_COINCIDE,  // two gates in the same place: the order is ambiguous
    GS_TRACK_GATE_FACING      // a gate turned across the route rather than along it
} gs_track_problem;

typedef struct gs_track_issue {
    gs_track_problem problem;
    int gate;    // which gate is at fault, or -1 when it is the route as a whole
    int other;   // the second gate, for problems about a pair; otherwise -1
} gs_track_issue;

// **How far a gate may be turned from the route before it is a fault.**
//
// Ninety degrees is what the geometry forbids outright: at ninety a car driving
// the route travels along the gate's plane rather than through it, and past it
// the gate cannot be crossed in the direction of travel at all. Sixty is where
// the line is drawn, because a gate approached at sixty degrees has already
// lost half its width to the angle, and a track that only just works is one
// that will not survive being edited.
#define GS_GATE_FACING_MAX GS_DEG(60)

gs_track_issue gs_track_validate(const gs_track *t);
const char *gs_track_problem_text(gs_track_problem p);

// A track's identity is its content. Two players who built the same track
// independently have the same track, without a server deciding so, and a
// one-tile edit cleanly produces a different one - which is what lets ghosts
// and times aggregate by themselves. Only the used region is hashed, so a 16x16
// track's identity does not depend on GS_TRACK_MAX.
uint64_t gs_track_hash(const gs_track *t);

// The same, as it was answered before whether a track is a loop or a path
// became part of its identity. **Only for reading share codes issued then** -
// see the note in gs_track.c. Nothing new should be keyed on this.
uint64_t gs_track_hash_before_route_kind(const gs_track *t);

// --- the file format ------------------------------------------------------
//
// Explicit little-endian on the wire, like the replay format, so a track
// authored on one machine loads on another regardless of what the compiler
// chose to pad. Only the used region is written, so a 16x16 track is a small
// file rather than a mostly-empty 22 KB one.
//
// No allocation happens here: `src/core/` owns no allocator, so the caller
// provides the buffer and asks how big it needs to be.

#define GS_TRACK_MAGIC   0x4b525447u   // "GTRK"
// Version 3 carries the route kind. Version 2 files still load: they are read
// as sprints, which is what every one of them actually was - two gates, one at
// each end, raced as though it were a loop.
#define GS_TRACK_VERSION 5u

// Bytes `gs_track_serialize` will write for this track.
size_t gs_track_size(const gs_track *t);

// Returns the number of bytes written, or 0 if `cap` is too small.
size_t gs_track_serialize(const gs_track *t, uint8_t *buf, size_t cap);

// Returns false — and leaves `t` untouched — on a bad magic, an unknown
// version, impossible dimensions, or a buffer that ends early. A half-loaded
// track is worse than a refused one: it races, and it is not the track anybody
// built.
bool gs_track_deserialize(gs_track *t, const uint8_t *buf, size_t len);

#endif // GS_TRACK_H

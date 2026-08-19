// gs_sim.h - the world, and the one function that advances it.
//
// **The world contains no pointers.** Not to the track, not to anything. A
// snapshot is therefore a `memcpy` of this struct, which is what makes rollback
// netcode, replay scrubbing and the editor's background ghost cheap instead of
// a rewrite. The track is passed to the step function rather than held, because
// it does not change during a race and copying 22 KB per rollback frame for
// something immutable would be silly.
//
// **The step is fixed at 120 Hz.** Frames are drawn whenever the machine can
// and interpolate between the last two states. Physics that runs per frame is a
// desync with extra steps.
#ifndef GS_SIM_H
#define GS_SIM_H

#include "core/gs_fixed.h"
#include "core/gs_track.h"
#include "core/gs_vehicle.h"

#define GS_TICK_HZ  120
#define GS_DT       (GS_ONE / GS_TICK_HZ)

// Four, because four-player split-screen is on the plan and a state size that
// changes when it arrives would change every replay ever recorded.
#define GS_MAX_CARS 4
static_assert(GS_MAX_CARS == GS_TRACK_GRID,
              "the starting grid lines up one car per player");

// One byte of input per car per tick: eight directions and a button, as the
// original had. Small state is what makes rollback and replay sharing cheap,
// and a digital control is what makes the car predictable.
typedef uint8_t gs_input;

enum {
    GS_IN_ACCEL = 1u << 0,
    GS_IN_BRAKE = 1u << 1,
    GS_IN_LEFT  = 1u << 2,
    GS_IN_RIGHT = 1u << 3,
    GS_IN_FIRE  = 1u << 4,
};

// Earth gravity in tiles per second squared, a tile being four metres.
#define GS_GRAVITY_EARTH GS_RATIO(245166, 100000)

typedef struct gs_gravity_preset {
    const char *name;
    gs_fix      scale;   // multiple of Earth
} gs_gravity_preset;

#define GS_GRAVITY_PRESETS 8
extern const gs_gravity_preset gs_gravity_presets[GS_GRAVITY_PRESETS];

// What you leave behind you.
//
// The original's destruction toolkit, and the reason its second mode works:
// you cannot shoot forwards, so hurting somebody means getting in front of them
// first and then staying there. That is a race, and the weapon rewards driving.
#define GS_MAX_HAZARDS 32

typedef enum gs_hazard_kind {
    GS_HAZ_NONE = 0,
    GS_HAZ_OIL,     // takes the grip away and gives it back when you leave
    GS_HAZ_MINE,    // one use, and it hurts
    GS_HAZ_COUNT
} gs_hazard_kind;

typedef struct gs_hazard {
    gs_fix  x, y;
    uint8_t kind;
    uint8_t owner;   // the car that dropped it, which it never affects
    uint8_t spent;   // a mine that has gone off
    uint8_t pad;
} gs_hazard;

typedef struct gs_car {
    gs_fix   x, y, z;        // tiles; z is height above the datum, not above ground
    gs_fix   vx, vy, vz;     // tiles per second
    gs_angle heading;

    uint8_t  vehicle;        // gs_vehicle_id
    uint8_t  damage;         // 0 whole, 255 wrecked
    bool     grounded;
    bool     wrecked;
    bool     active;

    // Where it has got to round the route. Needed by anything that has to know
    // what "ahead" means: the AI aiming at something, a race deciding who won,
    // and eventually a ghost worth comparing against.
    uint8_t  next_gate;
    uint16_t laps;

    // The tick this car completed the last lap it had to, or zero for still
    // going. Times rather than positions, because a results screen that only
    // said who won would throw away the thing everybody actually argues about.
    uint32_t finish_tick;

    // The current lap's clock, and the quickest one so far. A best lap is what
    // a track is actually judged by - a race time is a race, but a lap time is
    // the track - so it is simulation state, hashed like everything else, and
    // not something the front end works out afterwards from things it watched.
    uint32_t lap_start;
    uint32_t best_lap;

    // Ticks until this car can drop another. Without it, holding the button
    // paves the track at a hundred and twenty hazards a second.
    uint16_t drop_cooldown;

    // How long since the wheels last touched. The renderer wants it for the
    // shadow, and the landing-prediction arc will want it later.
    uint32_t air_ticks;
} gs_car;

// Everything that changes during a race. No pointers - see the header comment.
// The two things a race can be, and the toggle between them is the whole of it -
// same track, same cars, same physics. That was true in 1985 and there is no
// reason for it not to be now.
typedef enum gs_mode {
    GS_MODE_RACE = 0,     // first past the flag
    GS_MODE_DESTRUCTION   // last one driving
} gs_mode;

#define GS_NO_WINNER 0xffu

typedef struct gs_world {
    uint64_t tick;

    // The dials, held here rather than read from a setting, so that a snapshot
    // restores the race it came from and a replay carries its own conditions.
    gs_fix gravity;          // tiles/s^2 before the track's per-tile multiplier
    gs_fix drag_scale;
    gs_fix friction_scale;
    gs_fix damage_scale;

    uint8_t car_count;
    gs_car  car[GS_MAX_CARS];

    uint8_t    hazard_count;
    gs_hazard  hazard[GS_MAX_HAZARDS];

    uint8_t  mode;      // gs_mode
    bool     over;      // the result is settled and will not change
    uint8_t  winner;    // car index, or GS_NO_WINNER for nobody

    // How many laps a race is. Zero means there is no finish line at all -
    // which is what a test drive is, and what every race was before there was
    // a front end to choose a number in.
    uint16_t laps_to_win;

    // What this race has done to the ground. In the world and not the track,
    // because it is not what the track *is* - reload the track and it is fresh
    // again, exactly as it should be. Eight kilobytes, so a snapshot is still a
    // memcpy and rollback is still cheap.
    //
    // Sixteen bits rather than eight, and that is not caution. A tyre marks a
    // tile by well under one part in 255 per tick, so in a byte every single
    // tick truncated to zero and the ground never wore at all - a feature that
    // did nothing, silently, because the arithmetic was too coarse to notice.
    uint16_t wear[GS_TRACK_TILES];
} gs_world;

void gs_world_init(gs_world *w, gs_fix gravity_scale);

// Which game is being played. Set it before the race starts; changing it
// half way through would be changing what everyone was doing.
void gs_world_set_mode(gs_world *w, gs_mode mode);

// How many laps to win. Set it before the race starts, for the same reason as
// the mode. Zero is a race with no end, which is what a test drive wants.
void gs_world_set_laps(gs_world *w, uint16_t laps);

// Put a car on the ground at (x, y), facing `heading`, and return its index.
// Returns -1 if the world is full.
int gs_world_add_car(gs_world *w, const gs_track *t,
                     uint8_t vehicle, gs_fix x, gs_fix y, gs_angle heading);

// Advance the world one tick. `in` holds one byte per car, in car order.
void gs_world_step(gs_world *w, const gs_track *t, const gs_input *in);

// A hash of everything that matters, field by field rather than over the
// struct's bytes - so compiler padding, which is not state, cannot change the
// answer. This is what the golden replay compares, and what a rollback session
// exchanges to prove two machines still agree.
uint64_t gs_world_hash(const gs_world *w);

// How big a car is when it hits something. A circle rather than the drawn box:
// two rectangles need a separating-axis test and a contact point, and all of
// that buys is corners catching corners - which is a source of surprise rather
// than of skill. A circle is predictable, and predictable is the whole ethic.
//
// **Matched to the drawn size and not to the metric one.** A car is drawn about
// 1.3 tiles long because an honest 2.7 m car reads as a speck; if collision used
// the honest figure, cars would pass through each other while visibly touching.
#define GS_CAR_RADIUS GS_RATIO(52, 100)

// **The steepest ground a car will drive up, as a gradient.** About fifty
// degrees: steeper than any road and shallower than anything anyone would call
// a wall. Ground beyond it stops a car rather than launching it. Out here in the
// header because the track generator has to build under the same limit, and a
// limit written down twice is a limit that will disagree with itself.
#define GS_MAX_CLIMB GS_RATIO(120, 100)

// Drop a hazard at a car's position, if it is allowed to. Returns whether one
// was left behind.
bool gs_world_drop(gs_world *w, uint8_t car, gs_hazard_kind kind);

// How worn the ground is at a point, 0 to GS_ONE. The renderer wants it, and so
// will the analyser when it starts asking where the line has moved to.
gs_fix gs_world_wear(const gs_world *w, gs_fix x, gs_fix y);

// Speed over the ground, tiles per second. Convenience for the HUD and the AI;
// the physics works in components.
gs_fix gs_car_speed(const gs_car *c);

#endif // GS_SIM_H

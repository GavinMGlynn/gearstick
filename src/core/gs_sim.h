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

typedef struct gs_car {
    gs_fix   x, y, z;        // tiles; z is height above the datum, not above ground
    gs_fix   vx, vy, vz;     // tiles per second
    gs_angle heading;

    uint8_t  vehicle;        // gs_vehicle_id
    uint8_t  damage;         // 0 whole, 255 wrecked
    bool     grounded;
    bool     wrecked;
    bool     active;

    // How long since the wheels last touched. The renderer wants it for the
    // shadow, and the landing-prediction arc will want it later.
    uint32_t air_ticks;
} gs_car;

// Everything that changes during a race. No pointers - see the header comment.
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

// How worn the ground is at a point, 0 to GS_ONE. The renderer wants it, and so
// will the analyser when it starts asking where the line has moved to.
gs_fix gs_world_wear(const gs_world *w, gs_fix x, gs_fix y);

// Speed over the ground, tiles per second. Convenience for the HUD and the AI;
// the physics works in components.
gs_fix gs_car_speed(const gs_car *c);

#endif // GS_SIM_H

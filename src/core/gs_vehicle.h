// gs_vehicle.h - the roster, as a table of trade-offs.
//
// Every vehicle is worse than every other at something. That is the whole
// design: the original handed you the entire garage on load and let you work
// out that the lunar rover is a joke on Earth and the only sane choice on the
// Moon. Nothing here is unlocked, earned or upgraded.
//
// The numbers are deliberately few and deliberately blunt. A player has to be
// able to hold "more power, less grip, heavier" in their head and predict what
// it does, which is the same reason there are no slip curves and no suspension.
#ifndef GS_VEHICLE_H
#define GS_VEHICLE_H

#include "core/gs_fixed.h"

typedef enum gs_vehicle_id {
    GS_VEH_STOCK_CAR = 0,
    GS_VEH_SPRINT_CAR,
    GS_VEH_DUNE_BUGGY,
    GS_VEH_BAJA_BUG,
    GS_VEH_MOTORCYCLE,
    GS_VEH_LUNAR_ROVER,
    GS_VEH_COUNT
} gs_vehicle_id;

typedef struct gs_vehicle_def {
    const char *name;

    gs_fix power;      // drive acceleration at rest, tiles/s^2
    gs_fix brake;      // braking acceleration, tiles/s^2
    gs_fix top;        // speed at which the engine has nothing left, tiles/s
    gs_fix grip;       // multiplier on whatever the surface offers - the tyres
    gs_fix steer;      // yaw rate at walking pace, angle units per second
    gs_fix drag;       // air resistance, per (tile/s)^2

    // How hard it is to break. A landing's damage is divided by this, so the
    // rover survives what folds the sprint car.
    gs_fix toughness;

    // **How many forward gears the box has.** Every machine has exactly one
    // reverse, which is the existing brake-at-a-standstill gear and is not
    // counted here. Gear g of G covers speeds up to top * g / G; the engine
    // pulls harder in a lower gear - power * G / g, capped at `power` so a
    // launch is exactly as traction-limited as it always was - and has
    // nothing left at the top of the gear, which is the limiter a manual
    // driver must shift off. See gs_gear_force in gs_sim.c.
    uint8_t gears;
} gs_vehicle_def;

extern const gs_vehicle_def gs_vehicles[GS_VEH_COUNT];

const gs_vehicle_def *gs_vehicle(uint8_t id);

#endif // GS_VEHICLE_H

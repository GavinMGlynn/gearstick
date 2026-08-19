// gs_vehicle.c - the roster. See gs_vehicle.h for why the numbers are blunt.

#include "core/gs_vehicle.h"

// Speeds are in tiles per second and a tile is four metres, so `top` of 6 is
// about 86 km/h. That is slower than it sounds: the tracks are 64 tiles across
// at most, and the original's charm was never top speed.
//
// Steering is the yaw rate at a crawl; it falls away with speed in the physics,
// which is what stops a car pirouetting at 80.
const gs_vehicle_def gs_vehicles[GS_VEH_COUNT] = {
    // Balanced, and the one to learn the track in.
    [GS_VEH_STOCK_CAR] = {
        "stock car",
        GS_RATIO(60, 10), GS_RATIO(90, 10), GS_INT(6),
        GS_ONE, GS_INT(9000), GS_RATIO(6, 1000), GS_ONE,
    },
    // Fastest on pavement, punished everywhere else, and fragile.
    [GS_VEH_SPRINT_CAR] = {
        "sprint car",
        GS_RATIO(82, 10), GS_RATIO(105, 10), GS_RATIO(72, 10),
        GS_RATIO(115, 100), GS_INT(7600), GS_RATIO(5, 1000), GS_RATIO(70, 100),
    },
    // Turns in on dirt, gives away a lot of straight-line speed.
    [GS_VEH_DUNE_BUGGY] = {
        "dune buggy",
        GS_RATIO(55, 10), GS_RATIO(80, 10), GS_RATIO(52, 10),
        GS_RATIO(105, 100), GS_INT(11500), GS_RATIO(9, 1000), GS_RATIO(120, 100),
    },
    // Slow, tough, lands well. The one to take somewhere silly.
    [GS_VEH_BAJA_BUG] = {
        "baja bug",
        GS_RATIO(48, 10), GS_RATIO(70, 10), GS_RATIO(48, 10),
        GS_RATIO(95, 100), GS_INT(10200), GS_RATIO(10, 1000), GS_RATIO(165, 100),
    },
    // Enormous grip and steering, no mass to speak of, folds on a bad landing.
    [GS_VEH_MOTORCYCLE] = {
        "motorcycle",
        GS_RATIO(95, 10), GS_RATIO(115, 10), GS_RATIO(68, 10),
        GS_RATIO(120, 100), GS_INT(14000), GS_RATIO(4, 1000), GS_RATIO(45, 100),
    },
    // A joke on Earth and the only sane choice on the Moon: no speed, no grip
    // to lose, and it will survive anything.
    [GS_VEH_LUNAR_ROVER] = {
        "lunar rover",
        GS_RATIO(30, 10), GS_RATIO(45, 10), GS_RATIO(32, 10),
        GS_RATIO(70, 100), GS_INT(8200), GS_RATIO(14, 1000), GS_RATIO(230, 100),
    },
};

const gs_vehicle_def *gs_vehicle(uint8_t id) {
    return &gs_vehicles[id < GS_VEH_COUNT ? id : (uint8_t)GS_VEH_STOCK_CAR];
}

// gs_vehicle.c - the roster. See gs_vehicle.h for why the numbers are blunt.

#include "core/gs_vehicle.h"

// Speeds are in tiles per second and a tile is four metres, so `top` of 6 is
// about 86 km/h. That is slower than it sounds: the tracks are 64 tiles across
// at most, and the original's charm was never top speed.
//
// Steering is the yaw rate at a crawl; it falls away with speed in the physics,
// which is what stops a car pirouetting at 80.
const gs_vehicle_def gs_vehicles[GS_VEH_COUNT] = {
    // **Every one of these is worst at something.** That is the whole roster
    // design, and it is checked rather than asserted: `gearstick_cli roster`
    // races all six over conditions chosen to reward different things and fails
    // if one machine wins everything. The first version of this table did - the
    // sprint car had the best top speed *and* good grip *and* high power, which
    // made it a roster of one vehicle and five decorations.
    //
    // Grip does double duty since the grip circle arrived: it limits cornering
    // *and* acceleration. So a low-grip car is not merely bad in corners, it is
    // slow off the line on anything loose - and a high-grip one keeps moving on
    // ice and on the Moon where everybody else is skating.

    // The baseline. Good at nothing in particular, bad at nothing in
    // particular, and the one to learn a track in.
    [GS_VEH_STOCK_CAR] = {
        "stock car",
        GS_RATIO(60, 10), GS_RATIO(90, 10), GS_INT(6),
        GS_ONE, GS_INT(9500), GS_RATIO(6, 1000), GS_RATIO(110, 100),
    },
    // The fastest thing here in a straight line on good ground, and it pays for
    // it everywhere else: least grip in the roster, so it slides in corners and
    // struggles to put its power down on anything but pavement, and it is
    // fragile.
    [GS_VEH_SPRINT_CAR] = {
        "sprint car",
        GS_RATIO(85, 10), GS_RATIO(105, 10), GS_RATIO(78, 10),
        GS_RATIO(78, 100), GS_INT(7000), GS_RATIO(5, 1000), GS_RATIO(55, 100),
    },
    // Turns in beautifully and shrugs off rough ground; gives away a lot of
    // straight-line speed for it.
    [GS_VEH_DUNE_BUGGY] = {
        "dune buggy",
        GS_RATIO(56, 10), GS_RATIO(85, 10), GS_RATIO(53, 10),
        GS_RATIO(128, 100), GS_INT(12500), GS_RATIO(9, 1000), GS_RATIO(105, 100),
    },
    // Slow, and it does not care what you drive it over or how you land it.
    [GS_VEH_BAJA_BUG] = {
        "baja bug",
        GS_RATIO(50, 10), GS_RATIO(75, 10), GS_INT(5),
        GS_RATIO(108, 100), GS_INT(10500), GS_RATIO(10, 1000), GS_RATIO(260, 100),
    },
    // Enormous grip and steering and the best acceleration in the game, and it
    // folds on a landing that the rest would shrug off.
    [GS_VEH_MOTORCYCLE] = {
        "motorcycle",
        GS_INT(9), GS_RATIO(115, 10), GS_RATIO(66, 10),
        GS_RATIO(130, 100), GS_INT(15000), GS_RATIO(4, 1000), GS_RATIO(35, 100),
    },
    // A joke on Earth and the only sane choice on the Moon. Huge tyres, so it
    // still has traction where gravity has taken everyone else's away - and no
    // speed whatsoever to use it with.
    [GS_VEH_LUNAR_ROVER] = {
        "lunar rover",
        GS_RATIO(34, 10), GS_INT(5), GS_RATIO(36, 10),
        GS_RATIO(155, 100), GS_INT(8500), GS_RATIO(14, 1000), GS_RATIO(260, 100),
    },
};

const gs_vehicle_def *gs_vehicle(uint8_t id) {
    return &gs_vehicles[id < GS_VEH_COUNT ? id : (uint8_t)GS_VEH_STOCK_CAR];
}

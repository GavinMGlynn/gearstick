// gs_vehicle.c - the roster. See gs_vehicle.h for why the numbers are blunt.

#include "core/gs_vehicle.h"

// Speeds are in tiles per second and a tile is four metres, so `top` of 6 is
// about 86 km/h. That is slower than it sounds: the tracks are 64 tiles across
// at most, and the original's charm was never top speed.
//
// Steering is the yaw rate at a crawl; it falls away with speed in the physics,
// which is what stops a car pirouetting at 80.
const gs_vehicle_def gs_vehicles[GS_VEH_COUNT] = {
    // **Half as much again of everything that decides pace.**
    //
    // Asked for after driving the shipped tracks: the game felt sluggish
    // against the one it is after. Power, top speed and grip are each
    // multiplied by 1.5 on every machine. Braking, steering and drag are
    // untouched.
    //
    // **All three together, because no one of them is the pace.** Measured on
    // `gearstick_cli pace`, against a lap of 27.27s: grip alone buys 5%,
    // because a car that can corner harder still runs out of engine on the
    // straight; power and top speed without grip buy 13%, because the corner
    // is where the time is; the two of them with grip buy 25%, and the lap
    // comes out at 20.43s.
    //
    // **And the same factor on all three on purpose**, because it keeps the
    // shape of the driving rather than flattening it. Cornering speed goes as
    // the square root of grip - up about 22% - while straight-line speed goes
    // up the full 50%, so a car now arrives at a corner *relatively* faster
    // than it used to and slides harder for it. Braking distance goes as v
    // squared over deceleration, which is half as long again. More speed
    // everywhere and more precision asked for at every corner, which is the
    // trade the original made too.
    //
    // **Toughness is scaled with them, and that is not a pace change.** A
    // landing's damage is divided by it, and damage arrives as the square of
    // the speed you land at - so half as much speed again into every jump is
    // more than twice the punishment out of it, on ground that was already the
    // limit. Left alone, four AI cars over the shipped set lost **14 of 72** to
    // landings that had been survivable the week before. The roster's ordering
    // is untouched by moving all six by one factor: the rover still shrugs off
    // what folds the sprint car.
    //
    // The rest of the roster is a set of trade-offs and this does not disturb
    // it either. All six move together, so which machine is best at what is
    // what it was, and `gearstick_cli roster` still finds every one of them
    // best at something - the same 1, 3, 1, 1, 3, 1 spread of wins it found
    // before. That check is on a knife edge - a uniform change to top speed
    // alone fails it at any size from a tenth upward - so it was measured
    // rather than assumed.

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
        GS_INT(9), GS_RATIO(90, 10), GS_INT(9),
        GS_RATIO(150, 100), GS_INT(9500), GS_RATIO(6, 1000), GS_RATIO(165, 100),
    },
    // The fastest thing here in a straight line on good ground, and it pays for
    // it everywhere else: least grip in the roster, so it slides in corners and
    // struggles to put its power down on anything but pavement, and it is
    // fragile.
    [GS_VEH_SPRINT_CAR] = {
        "sprint car",
        GS_RATIO(1275, 100), GS_RATIO(105, 10), GS_RATIO(117, 10),
        GS_RATIO(117, 100), GS_INT(7000), GS_RATIO(5, 1000), GS_RATIO(825, 1000),
    },
    // Turns in beautifully and shrugs off rough ground; gives away a lot of
    // straight-line speed for it.
    [GS_VEH_DUNE_BUGGY] = {
        "dune buggy",
        GS_RATIO(84, 10), GS_RATIO(85, 10), GS_RATIO(795, 100),
        GS_RATIO(192, 100), GS_INT(12500), GS_RATIO(9, 1000), GS_RATIO(210, 100),
    },
    // Slow, and it does not care what you drive it over or how you land it.
    [GS_VEH_BAJA_BUG] = {
        "baja bug",
        GS_RATIO(75, 10), GS_RATIO(75, 10), GS_RATIO(75, 10),
        GS_RATIO(162, 100), GS_INT(10500), GS_RATIO(10, 1000), GS_RATIO(390, 100),
    },
    // Enormous grip and steering and the best acceleration in the game, and it
    // folds on a landing that the rest would shrug off.
    [GS_VEH_MOTORCYCLE] = {
        "motorcycle",
        GS_RATIO(135, 10), GS_RATIO(115, 10), GS_RATIO(99, 10),
        GS_RATIO(195, 100), GS_INT(15000), GS_RATIO(4, 1000), GS_RATIO(525, 1000),
    },
    // A joke on Earth and the only sane choice on the Moon. Huge tyres, so it
    // still has traction where gravity has taken everyone else's away - and no
    // speed whatsoever to use it with.
    [GS_VEH_LUNAR_ROVER] = {
        "lunar rover",
        GS_RATIO(51, 10), GS_INT(5), GS_RATIO(54, 10),
        GS_RATIO(2325, 1000), GS_INT(8500), GS_RATIO(14, 1000), GS_RATIO(390, 100),
    },
};

const gs_vehicle_def *gs_vehicle(uint8_t id) {
    return &gs_vehicles[id < GS_VEH_COUNT ? id : (uint8_t)GS_VEH_STOCK_CAR];
}

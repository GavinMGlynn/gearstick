// gs_sim.c - the physics. Integers throughout; see gs_fixed.h for why.
//
// The model is deliberately shallow: drive along the heading, a grip limit that
// bleeds off sideways velocity, gravity down the slope, and a ballistic arc
// when the ground falls away faster than gravity can hold the car to it. No
// slip curves, no suspension, no weight transfer. A player has to be able to
// predict what the car will do, because predicting it is how you learn to
// exploit it, and exploiting it is the game.

#include "core/gs_sim.h"

// The planet names are kept because they were doing real work: "Jupiter" tells
// a player something a number does not. The values are the real ratios to Earth
// surface gravity, and the dial between them is continuous.
const gs_gravity_preset gs_gravity_presets[GS_GRAVITY_PRESETS] = {
    { "Ceres",   GS_RATIO( 3, 100) },
    { "Moon",    GS_RATIO(17, 100) },
    { "Mars",    GS_RATIO(38, 100) },
    { "Venus",   GS_RATIO(90, 100) },
    { "Earth",   GS_ONE },
    { "Saturn",  GS_RATIO(107, 100) },
    { "Neptune", GS_RATIO(114, 100) },
    { "Jupiter", GS_RATIO(253, 100) },
};

void gs_world_init(gs_world *w, gs_fix gravity_scale) {
    *w = (gs_world){ 0 };
    w->gravity        = gs_fix_mul(GS_GRAVITY_EARTH, gravity_scale);
    w->mode           = (uint8_t)GS_MODE_RACE;
    w->over           = false;
    w->winner         = GS_NO_WINNER;
    w->drag_scale     = GS_ONE;
    w->friction_scale = GS_ONE;
    w->damage_scale   = GS_ONE;
}

void gs_world_set_laps(gs_world *w, uint16_t laps) {
    w->laps_to_win = laps;
}

uint16_t gs_car_laps_done(const gs_track *t, const gs_car *c) {
    if (!gs_track_is_circuit(t)) return c->laps;
    return c->laps > 0 ? (uint16_t)(c->laps - 1) : 0;
}

uint16_t gs_world_laps_needed(const gs_world *w, const gs_track *t) {
    // Zero still means a race with no end at all, which is what a test drive
    // is. Everything else: a loop is raced for the number it was given, and a
    // path for exactly one, because a path has a start at one end and a finish
    // at the other and arriving is the whole race.
    if (w->laps_to_win == 0) return 0;
    return gs_track_is_circuit(t) ? w->laps_to_win : (uint16_t)1;
}

void gs_world_set_countdown(gs_world *w, uint32_t ticks) {
    w->green_tick = (uint32_t)w->tick + ticks;
}

bool gs_world_held(const gs_world *w) {
    return w->tick < w->green_tick;
}

uint32_t gs_world_countdown(const gs_world *w) {
    if (w->tick >= w->green_tick) return 0;
    return w->green_tick - (uint32_t)w->tick;
}

void gs_world_set_mode(gs_world *w, gs_mode mode) {
    w->mode = (uint8_t)mode;
    w->over = false;
    w->winner = GS_NO_WINNER;
}

int gs_world_add_car(gs_world *w, const gs_track *t,
                     uint8_t vehicle, gs_fix x, gs_fix y, gs_angle heading) {
    if (w->car_count >= GS_MAX_CARS) return -1;

    uint8_t i = w->car_count++;
    gs_car *c = &w->car[i];

    *c = (gs_car){ 0 };
    c->x        = x;
    c->y        = y;
    c->z        = gs_track_height(t, x, y);
    c->heading  = heading;
    c->vehicle  = vehicle < GS_VEH_COUNT ? vehicle : (uint8_t)GS_VEH_STOCK_CAR;
    c->grounded = true;
    c->active   = true;

    return (int)i;
}

// The tile a point sits in, clamped into the array. Wear is indexed the same
// way the track's own tile arrays are, so the two always agree about which tile
// is which.
static size_t gs_wear_index(gs_fix x, gs_fix y) {
    int32_t tx = GS_CLAMP(gs_fix_floor(x), 0, GS_TRACK_MAX - 1);
    int32_t ty = GS_CLAMP(gs_fix_floor(y), 0, GS_TRACK_MAX - 1);
    return GS_TILE_INDEX(tx, ty);
}

gs_fix gs_world_wear(const gs_world *w, gs_fix x, gs_fix y) {
    return (gs_fix)(((int64_t)w->wear[gs_wear_index(x, y)] * GS_ONE) / UINT16_MAX);
}

gs_fix gs_car_speed(const gs_car *c) {
    return gs_fix_len2(c->vx, c->vy);
}

// How much a car can steer at a given speed. Full authority at a crawl, falling
// away as it speeds up - which is what stops a car pirouetting at 80 and is
// also, deliberately, a rule simple enough to feel in the hands after two
// corners.
static gs_fix gs_steer_authority(const gs_vehicle_def *v, gs_fix speed) {
    gs_fix denom = GS_ONE + gs_fix_div(speed, v->top);
    return gs_fix_div(v->steer, denom);
}

// How far a hazard reaches, and what it does when it gets you.
#define GS_OIL_RADIUS   GS_RATIO(140, 100)
#define GS_OIL_GRIP     GS_RATIO(12, 100)
#define GS_MINE_RADIUS  GS_RATIO(90, 100)
#define GS_MINE_LIFT    GS_INT(4)
#define GS_MINE_HURT    GS_INT(9)

// One a second. Holding the button should leave a trail, not a carpet.
#define GS_DROP_COOLDOWN (GS_TICK_HZ)

bool gs_world_drop(gs_world *w, uint8_t car, gs_hazard_kind kind) {
    if (car >= w->car_count || kind == GS_HAZ_NONE || kind >= GS_HAZ_COUNT) return false;

    gs_car *c = &w->car[car];
    if (!c->active || c->wrecked || c->drop_cooldown != 0) return false;

    // A ring rather than a refusal when full. Running out of room to record
    // hazards should quietly forget the oldest one, not stop the newest from
    // existing - the player would have no idea why their button stopped
    // working.
    uint8_t at;
    if (w->hazard_count < GS_MAX_HAZARDS) {
        at = w->hazard_count++;
    } else {
        at = (uint8_t)(w->tick % GS_MAX_HAZARDS);
    }

    w->hazard[at] = (gs_hazard){ .x = c->x, .y = c->y, .kind = (uint8_t)kind,
                                 .owner = car, .spent = 0, .pad = 0 };
    c->drop_cooldown = GS_DROP_COOLDOWN;
    return true;
}

// How much grip is left where this car is standing, as a multiplier. One when
// there is no oil about.
static gs_fix gs_oil_here(const gs_world *w, uint8_t car, gs_fix x, gs_fix y) {
    gs_fix worst = GS_ONE;
    for (uint8_t i = 0; i < w->hazard_count; i++) {
        const gs_hazard *h = &w->hazard[i];
        if (h->kind != GS_HAZ_OIL || h->spent) continue;

        // **Never your own.** You dropped it; driving into it would make the
        // weapon a way of hurting yourself, and nobody would use it.
        if (h->owner == car) continue;

        if (gs_fix_len2(x - h->x, y - h->y) < GS_OIL_RADIUS && GS_OIL_GRIP < worst) {
            worst = GS_OIL_GRIP;
        }
    }
    return worst;
}

static void gs_car_step(gs_world *w, gs_car *c, const gs_track *t, gs_input in,
                        uint8_t index) {
    const gs_vehicle_def *v = gs_vehicle(c->vehicle);
    const gs_fix dt = GS_DT;

    // A wrecked car keeps its position and stops being simulated. It is still
    // there to be looked at, and later to be driven into.
    if (!c->active || c->wrecked) return;

    // Gravity is sampled where the car is, every tick. The per-tile multiplier
    // is the gravity brush; caching it for a race would break the feature.
    gs_fix g = gs_fix_mul(w->gravity, gs_track_gravity(t, c->x, c->y));

    if (c->drop_cooldown > 0) c->drop_cooldown--;
    if ((in & GS_IN_FIRE) != 0) gs_world_drop(w, index, GS_HAZ_OIL);

    gs_surface surf = gs_track_surface(t, c->x, c->y);
    const gs_surface_def *sd = &gs_surfaces[surf];

    // What previous laps did to this exact tile. Interpolating between the
    // fresh figures and the worn ones means a half-worn tile is half-changed,
    // rather than the surface flipping character at some threshold - which
    // would be a cliff a player could not read.
    gs_fix worn = gs_world_wear(w, c->x, c->y);
    gs_fix surf_grip = gs_lerp(sd->grip, gs_fix_mul(sd->grip, sd->wear_grip), worn);

    // Oil takes the grip away and gives it back the moment you are off it,
    // which is what makes it a thing to be driven through rather than a
    // punishment to be served.
    surf_grip = gs_fix_mul(surf_grip, gs_oil_here(w, index, c->x, c->y));
    gs_fix surf_rolling =
        gs_lerp(sd->rolling, gs_fix_mul(sd->rolling, sd->wear_rolling), worn);

    gs_fix cos_h = gs_cos(c->heading);
    gs_fix sin_h = gs_sin(c->heading);

    // Velocity split into "along the way the car points" and "sideways". The
    // whole traction model is about how much of the second one a surface will
    // tolerate.
    gs_fix vlong =  gs_fix_mul(c->vx, cos_h) + gs_fix_mul(c->vy, sin_h);
    gs_fix vlat  = -gs_fix_mul(c->vx, sin_h) + gs_fix_mul(c->vy, cos_h);

    if (c->grounded) {
        // --- Steering. Turning the car does not turn its velocity: the
        // difference between where it points and where it is going *is* the
        // slip, and the grip limit below decides how much of it survives.
        gs_fix speed = gs_car_speed(c);
        gs_fix rate = gs_steer_authority(v, speed);

        // Below a crawl the car pivots on the spot in the original; here it
        // simply stops steering, which is the same thing without the silliness.
        if (speed > GS_RATIO(15, 100)) {
            gs_fix turn = gs_fix_mul(rate, dt);
            if ((in & GS_IN_LEFT) != 0)  c->heading = (gs_angle)(c->heading - (uint16_t)(turn >> GS_FIX_SHIFT));
            if ((in & GS_IN_RIGHT) != 0) c->heading = (gs_angle)(c->heading + (uint16_t)(turn >> GS_FIX_SHIFT));

            // Re-split against the new heading, so the sideways component now
            // holds the slip the turn just created.
            cos_h = gs_cos(c->heading);
            sin_h = gs_sin(c->heading);
            vlong =  gs_fix_mul(c->vx, cos_h) + gs_fix_mul(c->vy, sin_h);
            vlat  = -gs_fix_mul(c->vx, sin_h) + gs_fix_mul(c->vy, cos_h);
        }

        // --- Drive and brake. Engine force falls to nothing at the vehicle's
        // top speed, and the surface decides how much of it reaches the ground.
        gs_fix accel = 0;
        if ((in & GS_IN_ACCEL) != 0) {
            gs_fix headroom = GS_ONE - gs_fix_div(vlong, v->top);
            if (headroom < 0) headroom = 0;
            accel += gs_fix_mul(gs_fix_mul(v->power, headroom), sd->drive);
        }
        if ((in & GS_IN_BRAKE) != 0) {
            // Brake going forwards, reverse from a standstill.
            if (vlong > GS_RATIO(5, 100)) accel -= gs_fix_mul(v->brake, sd->drive);
            else accel -= gs_fix_mul(gs_fix_mul(v->power, GS_HALF), sd->drive);
        }

        // --- The grip circle, in the one form simple enough to predict: you
        // have only so much traction, and putting the engine through it does
        // not create more. Without this, tyres matter in corners and nowhere
        // else, low gravity costs you nothing off the line, and ice is merely a
        // weaker engine for everybody - which makes the whole roster a contest
        // of top speed.
        //
        // With it, a grippy car gets off the line on ice where a heavy one
        // spins, and the Moon takes away your acceleration along with your
        // weight. Both are things a player can feel and predict.
        gs_fix traction = gs_fix_mul(gs_fix_mul(surf_grip, v->grip),
                                     gs_fix_mul(g, w->friction_scale));
        if (accel > traction) accel = traction;
        if (accel < -traction) accel = -traction;

        vlong += gs_fix_mul(accel, dt);

        // --- Rolling resistance and air drag. Drag matters only near the top
        // of the range, which is where it should.
        gs_fix roll = gs_fix_mul(gs_fix_mul(surf_rolling, g), w->friction_scale);
        vlong = gs_toward_zero(vlong, gs_fix_mul(roll, dt));

        gs_fix drag = gs_fix_mul(gs_fix_mul(v->drag, w->drag_scale),
                                 gs_fix_mul(vlong, gs_fix_abs(vlong)));
        vlong -= gs_fix_mul(drag, dt);

        // --- The grip limit. Sideways velocity is scrubbed off at whatever
        // acceleration the tyres and the surface can produce - and because that
        // is expressed as a multiple of gravity, a low-gravity pocket makes
        // every surface slippery. That interaction is the point of the brush.
        vlat = gs_toward_zero(vlat, gs_fix_mul(traction, dt));

        // --- Back to world axes.
        c->vx = gs_fix_mul(vlong, cos_h) - gs_fix_mul(vlat, sin_h);
        c->vy = gs_fix_mul(vlong, sin_h) + gs_fix_mul(vlat, cos_h);

        // --- The slope pulls. For a plane of gradient (a, b) the downhill
        // acceleration is -g times the gradient; at the angles a car can drive
        // on, the small-angle form is inside the noise and costs no square root.
        gs_fix dzdx, dzdy;
        gs_track_slope(t, c->x, c->y, &dzdx, &dzdy);
        c->vx -= gs_fix_mul(gs_fix_mul(g, dzdx), dt);
        c->vy -= gs_fix_mul(gs_fix_mul(g, dzdy), dt);
    } else {
        // Airborne: no engine, no steering, no grip. Deliberately - air control
        // would make the arc negotiable, and the arc being *not* negotiable is
        // what makes the take-off decision matter.
        gs_fix drag = gs_fix_mul(gs_fix_mul(v->drag, w->drag_scale),
                                 gs_fix_mul(vlong, gs_fix_abs(vlong)));
        vlong -= gs_fix_mul(drag, dt);
        c->vx = gs_fix_mul(vlong, cos_h) - gs_fix_mul(vlat, sin_h);
        c->vy = gs_fix_mul(vlong, sin_h) + gs_fix_mul(vlat, cos_h);
    }

    // --- Off the world.
    //
    // **Measured by how far out, not by falling.** The first version of this
    // waited for the car to be airborne and a few tiles below the ground - and a
    // car that goes over the lip does not fall, it *drives down* the slope,
    // because ground-following keeps up with any gradient going downhill. So it
    // sledged twenty tiles down a three-to-one face over seven seconds and was
    // eventually wrecked by arriving at the bottom, which is a long, silly way
    // to say the same thing.
    //
    // Past the shoulder and a few tiles beyond it, a car is gone: the slope back
    // is steeper than GS_MAX_CLIMB, so there is no way up and nothing left to
    // simulate. It stops where it is rather than at the bottom, because what a
    // player needs to see is where the mistake ended.
    if (gs_track_outside(t, c->x, c->y) > GS_INT(GS_RUNOFF_TILES) + GS_FALL_DEPTH) {
        c->damage = 255;
        c->wrecked = true;
        c->vx = 0;
        c->vy = 0;
        c->vz = 0;
        return;
    }

    // --- Mark the ground. A tyre wears a tile in proportion to how hard it is
    // working it: sliding sideways churns far more than rolling straight, which
    // is why the racing line goes off before the rest of the track does.
    if (c->grounded && sd->wear_rate != 0) {
        gs_fix working = gs_fix_abs(vlong) + gs_fix_mul(gs_fix_abs(vlat), GS_INT(3));
        gs_fix marked = gs_fix_mul(gs_fix_mul(working, sd->wear_rate), dt);

        size_t at = gs_wear_index(c->x, c->y);
        int64_t step = ((int64_t)marked * UINT16_MAX) >> GS_FIX_SHIFT;
        int64_t now_worn = (int64_t)w->wear[at] + step;
        w->wear[at] = (uint16_t)(now_worn > UINT16_MAX ? UINT16_MAX : now_worn);
    }

    // --- Move.
    gs_fix z_was = c->z;
    gs_fix was_x = c->x, was_y = c->y;
    c->x += gs_fix_mul(c->vx, dt);
    c->y += gs_fix_mul(c->vy, dt);

    gs_fix ground = gs_track_height(t, c->x, c->y);

    // **A slope steeper than a car can climb is a wall, not a ramp.**
    //
    // Without this the ground-following rate is whatever the terrain demands,
    // and a vertical face demands an enormous one: a car meeting a sixty-tile
    // wall was catapulted to six hundred tiles up, because the ground rose
    // faster in one tick than anything could and the physics obligingly kept
    // the wheels on it. The analyser found it by declaring an impassable track
    // completable, which is a better bug report than any crash.
    //
    // What happens instead is what happens in life: you stop.
    if (c->grounded) {
        gs_fix climbed = ground - z_was;
        gs_fix moved = gs_fix_len2(c->x - was_x, c->y - was_y);
        if (moved > 0 && climbed > gs_fix_mul(moved, GS_MAX_CLIMB)) {
            c->x = was_x;
            c->y = was_y;
            c->vx = 0;
            c->vy = 0;
            c->vz = 0;
            return;
        }
    }

    if (c->grounded) {
        // The rate the ground would demand if the car stayed glued to it.
        gs_fix follow = gs_fix_div(ground - z_was, dt);

        // Off the end of a ramp, the ground drops away faster than gravity can
        // pull the car down. That comparison - and not a "ramp" tile type - is
        // what launches a jump, which is why any shape a player builds can be
        // one.
        if (follow < c->vz - gs_fix_mul(g, dt)) {
            c->grounded = false;
            c->air_ticks = 0;
        } else {
            c->vz = follow;
            c->z  = ground;
        }
    }

    if (!c->grounded) {
        c->vz -= gs_fix_mul(g, dt);
        c->z  += gs_fix_mul(c->vz, dt);
        c->air_ticks++;


        if (c->z <= ground) {
            // --- Landing. What hurts is not falling speed but the *mismatch*
            // between how fast the car is coming down and how fast the ground
            // is falling away beneath it. Land on a downslope going downhill
            // and it barely registers; land the same jump on the flat and it
            // folds the sprint car. This is the whole reason to build a
            // downhill landing, and the reason a flat one is a mistake.
            gs_fix dzdx, dzdy;
            gs_track_slope(t, c->x, c->y, &dzdx, &dzdy);
            gs_fix ground_rate = gs_fix_mul(dzdx, c->vx) + gs_fix_mul(dzdy, c->vy);

            gs_fix impact = ground_rate - c->vz;
            if (impact < 0) impact = 0;

            gs_fix hurt = gs_fix_div(gs_fix_mul(impact, w->damage_scale), v->toughness);

            // Below a threshold a landing is free; above it, damage climbs with
            // the excess rather than with the whole impact, so an ordinary jump
            // never chips the car and a bad one is unmistakable.
            const gs_fix free_fall = GS_INT(3);
            if (hurt > free_fall) {
                int32_t d = (int32_t)c->damage +
                            (gs_fix_mul(hurt - free_fall, GS_INT(26)) >> GS_FIX_SHIFT);
                c->damage = (uint8_t)GS_CLAMP(d, 0, 255);
                if (c->damage >= 255) c->wrecked = true;

                // A hard landing also scrubs speed - the car is bouncing and
                // digging in, not carrying it all through.
                gs_fix keep = GS_ONE - gs_fix_div(hurt - free_fall, GS_INT(24));
                keep = GS_CLAMP(keep, GS_RATIO(35, 100), GS_ONE);
                c->vx = gs_fix_mul(c->vx, keep);
                c->vy = gs_fix_mul(c->vy, keep);
            }

            c->z = ground;
            c->vz = ground_rate;
            c->grounded = true;
            c->air_ticks = 0;
        }
    }

    // There is no wall at the edge. Off the track the ground continues at the
    // height and surface of the nearest edge tile, so a car that leaves simply
    // drives out onto the surrounding plain and can drive back - which is what
    // the original did, and is a great deal better than an invisible barrier
    // that a player cannot see to avoid.
    //
    // What that plain should *be* is undecided: at the moment it is a
    // continuation of whatever the edge happened to be, which means leaving the
    // track costs nothing. See the tails in docs/COMPLETION_PLAN.md.
}

// How much of the closing speed comes back out of a collision.
//
// Over one on purpose. Real cars absorb energy and stop; that is the correct
// physics and the wrong game - a hit that costs you two seconds and teaches
// nothing is the "collisions that punish rather than launch" the design
// deliberately refuses. Here a hit sends somebody somewhere funny, and the
// person it happened to can see exactly why.
#define GS_BOUNCE GS_RATIO(150, 100)

// And a little of it goes upward, because a car that leaves the ground is the
// difference between a shove and an event.
#define GS_BOUNCE_LIFT GS_RATIO(35, 100)

// Cars only touch if they are at roughly the same height: one flying over
// another has cleared it, and being swatted out of the air by something passing
// underneath would be the least readable thing in the game.
#define GS_CAR_HEIGHT GS_RATIO(60, 100)

static void gs_collide(gs_world *w, gs_car *a, gs_car *b) {
    if (!a->active || !b->active) return;

    gs_fix dx = b->x - a->x;
    gs_fix dy = b->y - a->y;
    gs_fix dist = gs_fix_len2(dx, dy);

    // Each brings its own size to the meeting: a wreck is bigger than the car it
    // used to be, so hitting one starts sooner than hitting a driver.
    gs_fix reach = (a->wrecked ? GS_WRECK_RADIUS : GS_CAR_RADIUS) +
                   (b->wrecked ? GS_WRECK_RADIUS : GS_CAR_RADIUS);
    if (dist >= reach) return;

    gs_fix dz = a->z - b->z;
    if (gs_fix_abs(dz) > GS_CAR_HEIGHT) return;

    // Exactly on top of one another: push along x rather than dividing by zero.
    gs_fix nx, ny;
    if (dist <= 0) {
        nx = GS_ONE;
        ny = 0;
        dist = 1;
    } else {
        nx = gs_fix_div(dx, dist);
        ny = gs_fix_div(dy, dist);
    }

    // A wreck is scenery: it does not get shoved, and everything that hits it
    // comes off worse. Being able to bounce a dead car around would make the
    // debris a toy rather than an obstacle.
    bool a_fixed = a->wrecked;
    bool b_fixed = b->wrecked;
    if (a_fixed && b_fixed) return;

    // Separate them first, so a pair that ends a tick overlapping does not sit
    // inside each other trading impulses for the rest of the race. All of the
    // push goes to whichever of them can still move.
    gs_fix overlap = reach - dist;
    gs_fix a_share = b_fixed ? overlap : (a_fixed ? 0 : overlap / 2);
    gs_fix b_share = a_fixed ? overlap : (b_fixed ? 0 : overlap / 2);

    a->x -= gs_fix_mul(nx, a_share);
    a->y -= gs_fix_mul(ny, a_share);
    b->x += gs_fix_mul(nx, b_share);
    b->y += gs_fix_mul(ny, b_share);

    // Only if they are coming together. Two cars sliding apart while still
    // overlapping have already had their collision.
    gs_fix closing = gs_fix_mul(b->vx - a->vx, nx) + gs_fix_mul(b->vy - a->vy, ny);
    if (closing >= 0) return;

    // Equal masses, so each takes half. Mass is not in the vehicle table and
    // does not need to be: a heavier car that shrugged off a hit would make the
    // choice of vehicle about winning collisions rather than about driving.
    // Against a wreck the live car takes all of it, since the wreck is not
    // going anywhere.
    gs_fix impulse = gs_fix_mul(-closing, GS_ONE + GS_BOUNCE);
    if (!a_fixed && !b_fixed) impulse /= 2;

    if (!a_fixed) {
        a->vx -= gs_fix_mul(nx, impulse);
        a->vy -= gs_fix_mul(ny, impulse);
    }
    if (!b_fixed) {
        b->vx += gs_fix_mul(nx, impulse);
        b->vy += gs_fix_mul(ny, impulse);
    }

    // A shove is not a launch. Any closing collision produces *some* lift, and
    // taking the wheels off the ground for all of them meant a gentle nudge put
    // a car in the air - which then landed, and the landing did the damage. The
    // hit looked harmless and the consequence arrived a moment later from
    // somewhere else entirely.
    gs_fix lift = gs_fix_mul(impulse, GS_BOUNCE_LIFT);
    // One tile a second upward is about a fifth of a tile of air: below that
    // there is nothing to see and no reason to take the wheels off the ground.
    if (lift > GS_ONE) {
        if (!a_fixed) { a->vz += lift; a->grounded = false; }
        if (!b_fixed) { b->vz += lift; b->grounded = false; }
    }

    // --- What it cost them.
    //
    // Same shape as a landing: below a threshold a knock is free, above it the
    // damage climbs with the excess. So trading paint is part of racing and a
    // proper hit is not, and the difference is legible from how hard it looked.
    gs_fix closing_speed = -closing;
    gs_car *pair[2] = { a, b };
    for (int side = 0; side < 2; side++) {
        gs_car *c = pair[side];
        if (c->wrecked) continue;

        const gs_vehicle_def *v = gs_vehicle(c->vehicle);
        gs_fix hurt = gs_fix_div(gs_fix_mul(closing_speed, w->damage_scale),
                                 v->toughness);

        const gs_fix free_knock = GS_INT(2);
        if (hurt <= free_knock) continue;

        int32_t d = (int32_t)c->damage +
                    (gs_fix_mul(hurt - free_knock, GS_INT(8)) >> GS_FIX_SHIFT);
        c->damage = (uint8_t)GS_CLAMP(d, 0, 255);
        if (c->damage >= 255) c->wrecked = true;
    }
}

// Mines, checked after everybody has moved so that two cars reaching one on the
// same tick get the same answer whichever order they were stepped in.
static void gs_mines(gs_world *w) {
    for (uint8_t i = 0; i < w->hazard_count; i++) {
        gs_hazard *h = &w->hazard[i];
        if (h->kind != GS_HAZ_MINE || h->spent) continue;

        for (uint8_t ci = 0; ci < w->car_count; ci++) {
            gs_car *c = &w->car[ci];
            if (!c->active || c->wrecked || h->owner == ci) continue;
            if (!c->grounded) continue;
            if (gs_fix_len2(c->x - h->x, c->y - h->y) >= GS_MINE_RADIUS) continue;

            c->vz += GS_MINE_LIFT;
            c->grounded = false;

            gs_fix hurt = gs_fix_div(gs_fix_mul(GS_MINE_HURT, w->damage_scale),
                                     gs_vehicle(c->vehicle)->toughness);
            int32_t d = (int32_t)c->damage + (gs_fix_mul(hurt, GS_INT(8)) >> GS_FIX_SHIFT);
            c->damage = (uint8_t)GS_CLAMP(d, 0, 255);
            if (c->damage >= 255) c->wrecked = true;

            h->spent = 1;
            break;
        }
    }
}

void gs_world_step(gs_world *w, const gs_track *t, const gs_input *in) {
    // Where everybody was, for the gate crossings below.
    struct { gs_fix x, y; } was[GS_MAX_CARS];
    for (uint8_t i = 0; i < w->car_count; i++) {
        was[i].x = w->car[i].x;
        was[i].y = w->car[i].y;
    }

    // **Held at the line until the lights go green.** The physics still runs,
    // so a car on a slope settles onto it and the grid is where it looks like
    // it is; it is only the driving that is held, which is what stops anybody
    // jumping the start.
    bool held = w->tick < w->green_tick;

    // **And no lap clock runs before the flag.** A lap begins at a crossing,
    // and until the first one `lap_start` is zero - so the HUD was showing time
    // already spent on a lap nobody had started, counting up while the cars sat
    // still. Pinned to the green while the hold lasts, so the clock reads zero
    // at the moment anybody can first move. Done here rather than when the
    // countdown is armed, so a car added after it is armed is right too.
    if (held) {
        for (uint8_t i = 0; i < w->car_count; i++) {
            w->car[i].lap_start = w->green_tick;
        }
    }

    for (uint8_t i = 0; i < w->car_count; i++) {
        gs_input got = (in != nullptr && !held) ? in[i] : (gs_input)0;
        gs_car_step(w, &w->car[i], t, got, i);
    }

    // Route progress. Checked against where each car was *before* this tick, so
    // a gate cannot be missed by a car fast enough to step clean over it.
    if (t->gate_count > 0) {
        for (uint8_t i = 0; i < w->car_count; i++) {
            gs_car *c = &w->car[i];
            if (!c->active) continue;

            const gs_gate *g = &t->gate[c->next_gate % t->gate_count];
            if (!gs_gate_crossed(g, was[i].x, was[i].y, c->x, c->y)) continue;

            uint8_t crossed = c->next_gate;
            c->next_gate = (uint8_t)((c->next_gate + 1) % t->gate_count);

            // **Which gate ends a lap depends on what kind of route this is**,
            // and getting that wrong is what made the shipped tracks
            // unraceable. A lap used to be counted whenever `next_gate` wrapped
            // to zero - that is, on crossing the *last* gate. On a loop that is
            // one gate early. On the two-gate sprints the generator was making
            // it meant a "lap" was a one-way trip, and lap two meant driving
            // all the way back across an open field to a line that was where
            // you started.
            //
            // A circuit's lap is bounded by gate zero, because a loop's start
            // line is its finish line. A sprint's race is ended by its last
            // gate, because a path finishes at the far end of it.
            if (crossed != gs_track_finish_gate(t)) continue;

            c->laps++;

            // On a circuit the first crossing is the start of lap one rather
            // than the end of a lap nobody drove: a car begins behind the line,
            // so its first "lap" is the run up to it and timing that would give
            // everybody a fictional best. A sprint has no such crossing - its
            // finish is a different gate from its start - so its one and only
            // crossing is a real time and is kept.
            uint32_t now = (uint32_t)w->tick;
            bool run_up = gs_track_is_circuit(t) && c->laps == 1;
            if (!run_up) {   // see gs_car_laps_done on why the first is not one
                uint32_t lap = now - c->lap_start;
                if (c->best_lap == 0 || lap < c->best_lap) c->best_lap = lap;
            }
            c->lap_start = now;
        }
    }

    // Finishing. A car that has done its laps is timed once and never again;
    // the first of them is the winner, settled the moment it happens, because
    // "first past the flag" cannot be revisited by anything that comes after.
    if (w->mode == (uint8_t)GS_MODE_RACE && gs_world_laps_needed(w, t) > 0) {
        for (uint8_t i = 0; i < w->car_count; i++) {
            gs_car *c = &w->car[i];
            if (c->finish_tick != 0) continue;
            if (gs_car_laps_done(t, c) < gs_world_laps_needed(w, t)) continue;

            c->finish_tick = (uint32_t)w->tick;
            if (w->winner == GS_NO_WINNER) w->winner = i;
        }

        // The race is over when there is nobody left who could still finish.
        // Not when the first car crosses: everybody's time is what a results
        // screen is for, and stopping the clock on the winner throws away every
        // other one.
        if (!w->over) {
            bool anybody_going = false;
            for (uint8_t i = 0; i < w->car_count; i++) {
                const gs_car *c = &w->car[i];
                if (c->active && !c->wrecked && c->finish_tick == 0) {
                    anybody_going = true;
                }
            }
            if (!anybody_going) w->over = true;
        }
    }

    gs_mines(w);

    // Destruction mode ends when there is nobody left to fight. Settled once
    // and never revisited: a winner who then drives off a cliff in the silence
    // afterwards has still won, and taking it back would be absurd.
    if (w->mode == (uint8_t)GS_MODE_DESTRUCTION && !w->over) {
        uint8_t last = GS_NO_WINNER;
        const uint8_t alive = gs_world_driving(w, &last);
        if (alive <= 1) {
            w->over = true;
            // `last` is only ever set for a car that is still driving, so it is
            // already nobody when nobody is - everybody going at once is a draw
            // without needing to be spelled out. An earlier version said
            // `alive == 1 ? last : GS_NO_WINNER`, which reads as though it is
            // deciding something and is not: no perturbation of it could change
            // the answer, which is how it was found.
            w->winner = last;
        }
    }

    // After everybody has moved, in a fixed order, so the result does not
    // depend on who was stepped first.
    for (uint8_t i = 0; i < w->car_count; i++) {
        for (uint8_t j = (uint8_t)(i + 1); j < w->car_count; j++) {
            gs_collide(w, &w->car[i], &w->car[j]);
        }
    }

    w->tick++;
}

static void gs_hash_u64(uint64_t *h, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        *h ^= (uint8_t)((v >> (i * 8)) & 0xffu);
        *h *= 0x00000100000001b3ULL;
    }
}

static void gs_hash_i32(uint64_t *h, int32_t v) {
    gs_hash_u64(h, (uint64_t)(uint32_t)v);
}

// How far round the route a car has got, as one number that only ever grows:
// laps, then gates within the lap, then the fraction of the way to the next one.
//
// The fraction is 1 minus (distance remaining / distance the leg started at),
// clamped - so a car that has covered nine tenths of a leg is ahead of one that
// has covered a tenth, whichever gate they are both aiming at.
static int64_t gs_progress(const gs_world *w, const gs_track *t, uint8_t i) {
    const gs_car *c = &w->car[i];
    if (t->gate_count == 0) return 0;

    int64_t whole = (int64_t)c->laps * t->gate_count + c->next_gate;

    const gs_gate *to = &t->gate[c->next_gate % t->gate_count];
    const gs_gate *from = &t->gate[(c->next_gate + t->gate_count - 1u) % t->gate_count];

    const gs_fix ax = to->x - from->x;
    const gs_fix ay = to->y - from->y;
    const gs_fix leg = gs_fix_len2(ax, ay);

    // **How far along the leg, not how far from the gate's middle.**
    //
    // A gate is a line across the road and a car crosses it wherever it likes,
    // so the straight line to the gate's centre point is shorter for the car in
    // the middle of the road than for the car level with it on the outside -
    // and it decided the order. Four cars sitting level on a standing grid came
    // out third, first, second and fourth, which is what the HUD said before
    // anybody had moved.
    //
    // What is wanted is the part of the trip that is left, so the distance
    // remaining is projected onto the leg. Two cars level across the road then
    // have identical progress and the tie is broken by index, which is stable.
    //
    // A zero-length leg is two gates in the same place, which the validator
    // refuses - but a track can be handed here without having been validated.
    gs_fix part = 0;
    if (leg > 0) {
        const gs_fix ux = gs_fix_div(ax, leg);
        const gs_fix uy = gs_fix_div(ay, leg);
        gs_fix left = gs_fix_mul(to->x - c->x, ux) + gs_fix_mul(to->y - c->y, uy);
        if (left < 0) left = 0;

        part = GS_ONE - gs_fix_div(left, leg);
        part = GS_CLAMP(part, 0, GS_ONE);
    }

    return whole * GS_ONE + part;
}

uint8_t gs_world_driving(const gs_world *w, uint8_t *last) {
    uint8_t driving = 0;
    if (last != nullptr) *last = GS_NO_WINNER;

    for (uint8_t i = 0; i < w->car_count; i++) {
        if (!w->car[i].active || w->car[i].wrecked) continue;
        driving++;
        if (last != nullptr) *last = i;
    }
    return driving;
}

uint8_t gs_world_place(const gs_world *w, const gs_track *t, uint8_t car) {
    if (car >= w->car_count || !w->car[car].active) return 0;

    // A finished car keeps the place it finished in, whatever anybody still
    // driving does afterwards: crossing the line is when a result stops moving.
    uint32_t mine_finish = w->car[car].finish_tick;
    int64_t mine = gs_progress(w, t, car);

    uint8_t ahead = 0;
    for (uint8_t i = 0; i < w->car_count; i++) {
        if (i == car || !w->car[i].active) continue;

        uint32_t theirs_finish = w->car[i].finish_tick;
        if (mine_finish != 0 || theirs_finish != 0) {
            if (theirs_finish != 0 && mine_finish == 0) { ahead++; continue; }
            if (theirs_finish != 0 && mine_finish != 0) {
                // Both home: the earlier tick, and the lower index if a tie -
                // so two cars finishing on the same tick still get two places.
                if (theirs_finish < mine_finish ||
                    (theirs_finish == mine_finish && i < car)) {
                    ahead++;
                }
            }
            continue;
        }

        int64_t theirs = gs_progress(w, t, i);
        if (theirs > mine || (theirs == mine && i < car)) ahead++;
    }

    return (uint8_t)(ahead + 1);
}

uint8_t gs_world_arc(const gs_world *w, const gs_track *t, uint8_t car,
                     gs_arc *out) {
    out->count = 0;
    out->landed = false;
    if (car >= w->car_count) return 0;
    if (!w->car[car].active || w->car[car].grounded || w->car[car].wrecked) return 0;

    // A copy, so nothing here can touch the race. The world holds no pointers,
    // which is what makes this one assignment rather than a walk.
    gs_world flight = *w;

    // Alone in it. A mid-air collision is not predictable, and an arc that
    // flinched at a car which might not be there would be answering a question
    // nobody asked.
    for (uint8_t i = 0; i < flight.car_count; i++) {
        if (i != car) flight.car[i].active = false;
    }

    // Twenty seconds. Long enough for any jump on any track at any gravity on
    // the dial, and short enough that a car which has left the world entirely
    // does not take the frame with it.
    const int32_t limit = GS_TICK_HZ * 20;

    int32_t every = 1;
    for (int32_t i = 0; i < limit; i++) {
        gs_world_step(&flight, t, nullptr);

        const gs_car *c = &flight.car[car];
        if (c->grounded) {
            // The touchdown is the point of the whole thing, so it is written
            // whatever the sampling was doing - and it is written last.
            if (out->count >= GS_ARC_MAX) out->count = GS_ARC_MAX - 1;
            out->x[out->count] = c->x;
            out->y[out->count] = c->y;
            out->z[out->count] = c->z;
            out->count++;
            out->landed = true;
            break;
        }

        if ((i % every) != 0) continue;

        // **Out of room is a reason to draw the same flight more coarsely, not
        // a reason to stop drawing it.** An arc cut off half way still ends
        // somewhere, and that somewhere is read as the landing. Keeping every
        // other point already taken and doubling the interval covers a flight of
        // any length in a fixed array, at whatever resolution it can afford.
        if (out->count >= GS_ARC_MAX) {
            for (uint8_t k = 0; k * 2 < out->count; k++) {
                out->x[k] = out->x[k * 2];
                out->y[k] = out->y[k * 2];
                out->z[k] = out->z[k * 2];
            }
            out->count = (uint8_t)((out->count + 1) / 2);
            every *= 2;
        }

        out->x[out->count] = c->x;
        out->y[out->count] = c->y;
        out->z[out->count] = c->z;
        out->count++;
    }

    return out->count;
}

uint64_t gs_world_hash(const gs_world *w) {
    uint64_t h = 0xcbf29ce484222325ULL;

    gs_hash_u64(&h, w->tick);
    gs_hash_i32(&h, w->gravity);
    gs_hash_i32(&h, w->drag_scale);
    gs_hash_i32(&h, w->friction_scale);
    gs_hash_i32(&h, w->damage_scale);
    gs_hash_u64(&h, w->car_count);
    gs_hash_u64(&h, w->mode);
    gs_hash_u64(&h, (uint64_t)w->over);
    gs_hash_u64(&h, w->winner);
    gs_hash_u64(&h, w->laps_to_win);

    // Wear is state that changes the race, so two machines disagreeing about it
    // is a desync exactly as much as a car in the wrong place would be.
    for (size_t i = 0; i < GS_TRACK_TILES; i++) {
        if (w->wear[i] != 0) {
            gs_hash_u64(&h, (uint64_t)i);
            gs_hash_u64(&h, w->wear[i]);
        }
    }

    for (uint8_t i = 0; i < w->car_count; i++) {
        const gs_car *c = &w->car[i];
        gs_hash_i32(&h, c->x);  gs_hash_i32(&h, c->y);  gs_hash_i32(&h, c->z);
        gs_hash_i32(&h, c->vx); gs_hash_i32(&h, c->vy); gs_hash_i32(&h, c->vz);
        gs_hash_u64(&h, c->heading);
        gs_hash_u64(&h, c->vehicle);
        gs_hash_u64(&h, c->damage);
        gs_hash_u64(&h, (uint64_t)c->grounded);
        gs_hash_u64(&h, (uint64_t)c->wrecked);
        gs_hash_u64(&h, (uint64_t)c->active);
        gs_hash_u64(&h, c->air_ticks);
        gs_hash_u64(&h, c->drop_cooldown);
        gs_hash_u64(&h, c->next_gate);
        gs_hash_u64(&h, c->laps);
        gs_hash_u64(&h, c->finish_tick);
        gs_hash_u64(&h, c->lap_start);
        gs_hash_u64(&h, c->best_lap);
    }

    // Hazards are state that changes the race, so they are hashed like
    // everything else that does.
    gs_hash_u64(&h, w->hazard_count);
    for (uint8_t i = 0; i < w->hazard_count; i++) {
        const gs_hazard *z = &w->hazard[i];
        gs_hash_i32(&h, z->x);
        gs_hash_i32(&h, z->y);
        gs_hash_u64(&h, z->kind);
        gs_hash_u64(&h, z->owner);
        gs_hash_u64(&h, z->spent);
    }
    return h;
}

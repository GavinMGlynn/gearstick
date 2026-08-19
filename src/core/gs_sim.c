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
    w->drag_scale     = GS_ONE;
    w->friction_scale = GS_ONE;
    w->damage_scale   = GS_ONE;
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

static void gs_car_step(gs_world *w, gs_car *c, const gs_track *t, gs_input in) {
    const gs_vehicle_def *v = gs_vehicle(c->vehicle);
    const gs_fix dt = GS_DT;

    // A wrecked car keeps its position and stops being simulated. It is still
    // there to be looked at, and later to be driven into.
    if (!c->active || c->wrecked) return;

    // Gravity is sampled where the car is, every tick. The per-tile multiplier
    // is the gravity brush; caching it for a race would break the feature.
    gs_fix g = gs_fix_mul(w->gravity, gs_track_gravity(t, c->x, c->y));

    gs_surface surf = gs_track_surface(t, c->x, c->y);
    const gs_surface_def *sd = &gs_surfaces[surf];

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

        vlong += gs_fix_mul(accel, dt);

        // --- Rolling resistance and air drag. Drag matters only near the top
        // of the range, which is where it should.
        gs_fix roll = gs_fix_mul(gs_fix_mul(sd->rolling, g), w->friction_scale);
        vlong = gs_toward_zero(vlong, gs_fix_mul(roll, dt));

        gs_fix drag = gs_fix_mul(gs_fix_mul(v->drag, w->drag_scale),
                                 gs_fix_mul(vlong, gs_fix_abs(vlong)));
        vlong -= gs_fix_mul(drag, dt);

        // --- The grip limit. Sideways velocity is scrubbed off at whatever
        // acceleration the tyres and the surface can produce - and because that
        // is expressed as a multiple of gravity, a low-gravity pocket makes
        // every surface slippery. That interaction is the point of the brush.
        gs_fix grip = gs_fix_mul(gs_fix_mul(sd->grip, v->grip),
                                 gs_fix_mul(g, w->friction_scale));
        vlat = gs_toward_zero(vlat, gs_fix_mul(grip, dt));

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

    // --- Move.
    gs_fix z_was = c->z;
    c->x += gs_fix_mul(c->vx, dt);
    c->y += gs_fix_mul(c->vy, dt);

    gs_fix ground = gs_track_height(t, c->x, c->y);

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

void gs_world_step(gs_world *w, const gs_track *t, const gs_input *in) {
    for (uint8_t i = 0; i < w->car_count; i++) {
        gs_car_step(w, &w->car[i], t, in != nullptr ? in[i] : (gs_input)0);
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

uint64_t gs_world_hash(const gs_world *w) {
    uint64_t h = 0xcbf29ce484222325ULL;

    gs_hash_u64(&h, w->tick);
    gs_hash_i32(&h, w->gravity);
    gs_hash_i32(&h, w->drag_scale);
    gs_hash_i32(&h, w->friction_scale);
    gs_hash_i32(&h, w->damage_scale);
    gs_hash_u64(&h, w->car_count);

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
    }
    return h;
}

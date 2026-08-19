// gs_ai.c - see gs_ai.h.

#include "core/gs_ai.h"

// Below this the steering does nothing useful and the car weaves; above it, it
// turns. A dead band is the difference between driving and hunting.
#define GS_AI_DEADBAND 900

gs_input gs_ai_drive(const gs_world *w, const gs_track *t, uint8_t car) {
    return gs_ai_drive_at(w, t, car, GS_AI_NORMAL);
}

gs_input gs_ai_drive_at(const gs_world *w, const gs_track *t, uint8_t car,
                        gs_fix margin) {
    if (car >= w->car_count || t->gate_count == 0) return 0;

    const gs_car *c = &w->car[car];
    if (!c->active || c->wrecked) return 0;

    const gs_gate *target = &t->gate[c->next_gate % t->gate_count];

    gs_fix dx = target->x - c->x;
    gs_fix dy = target->y - c->y;
    gs_fix distance = gs_fix_len2(dx, dy);

    gs_angle want = gs_atan2(dy, dx);
    int32_t turn = gs_angle_delta(c->heading, want);

    gs_input in = 0;
    if (turn < -GS_AI_DEADBAND) in |= GS_IN_LEFT;
    else if (turn > GS_AI_DEADBAND) in |= GS_IN_RIGHT;

    // --- The corner at the next gate, and whether there is room left to slow
    // down for it.
    //
    // The first version of this looked only at the angle between where the car
    // points and where the gate is. That angle is small until you are almost on
    // top of the gate, so it braked a fifth of a tile before it - which got
    // round, and was not planning anything. A driver that only reacts when the
    // corner is already happening is a driver with no plan.
    //
    // What matters is the angle the *route* turns through at that gate, which is
    // knowable from a long way back.
    const gs_gate *after = &t->gate[(c->next_gate + 1u) % t->gate_count];

    gs_angle coming = gs_atan2(dy, dx);
    gs_angle going = gs_atan2(after->y - target->y, after->x - target->x);
    int32_t corner = gs_angle_delta(coming, going);
    if (corner < 0) corner = -corner;

    // Everything in this can change during a race: the surface underfoot, the
    // wear in it, the gravity painted on this tile, the dials. Which is the
    // whole point - a speed profile computed when the track was authored would
    // be wrong the moment somebody moved the gravity dial, and wrong in the
    // worst way available, because it would look right.
    gs_fix g = gs_fix_mul(w->gravity, gs_track_gravity(t, c->x, c->y));
    const gs_surface_def *sd = &gs_surfaces[gs_track_surface(t, c->x, c->y)];
    gs_fix worn = gs_world_wear(w, c->x, c->y);
    gs_fix surf_grip = gs_lerp(sd->grip, gs_fix_mul(sd->grip, sd->wear_grip), worn);

    gs_fix traction = gs_fix_mul(gs_fix_mul(surf_grip, gs_vehicle(c->vehicle)->grip),
                                 gs_fix_mul(g, w->friction_scale));

    // How fast the corner itself can be taken. The radius of a turn of angle
    // `corner` with legs of length `leg` is leg / (2 tan(corner/2)) - a hairpin
    // has almost none and a kink has a great deal.
    gs_fix leg = gs_fix_len2(after->x - target->x, after->y - target->y);
    if (distance < leg) leg = distance;

    gs_angle half_corner = (gs_angle)(corner / 2);
    gs_fix sin_half = gs_fix_abs(gs_sin(half_corner));
    gs_fix cos_half = gs_cos(half_corner);

    gs_fix corner_speed;
    if (sin_half < GS_RATIO(3, 100) || cos_half <= 0) {
        corner_speed = INT32_MAX;      // straight on, or as near as makes no odds
    } else {
        gs_fix radius = gs_fix_div(gs_fix_mul(leg, cos_half),
                                   gs_fix_mul(sin_half, GS_INT(2)));
        corner_speed = gs_fix_mul(gs_fix_sqrt(gs_fix_mul(traction, radius)),
                                  margin);
    }

    // And the turn it needs *right now* to stay pointed at the gate. Planning
    // the corner ahead is not enough on its own: a car already carrying more
    // speed than the present curve will bear runs wide, and running wide is how
    // the sprint car - lowest grip, highest top speed - spent a whole race
    // circling outside a corner it could never have made.
    gs_fix here_limit = INT32_MAX;
    {
        gs_fix sine = gs_fix_abs(gs_sin((gs_angle)(turn < 0 ? -turn : turn)));
        if (sine >= GS_RATIO(4, 100) && distance > 0) {
            gs_fix radius = gs_fix_div(distance, gs_fix_mul(sine, GS_INT(2)));
            here_limit = gs_fix_mul(gs_fix_sqrt(gs_fix_mul(traction, radius)),
                                    margin);
        }
    }

    gs_fix speed = gs_car_speed(c);

    // v^2 = u^2 + 2as, rearranged: how much road it needs to get down to the
    // corner speed, braking at the traction available. Slowing down early is
    // free and slowing down late is not, so the comparison is against the road
    // remaining rather than against a fixed distance.
    bool must_brake = false;
    if (corner_speed != INT32_MAX && speed > corner_speed) {
        gs_fix excess = gs_fix_mul(speed, speed) - gs_fix_mul(corner_speed, corner_speed);
        gs_fix needed = gs_fix_div(excess, gs_fix_mul(traction, GS_INT(2)));
        if (needed >= distance) must_brake = true;
    }

    if (speed > here_limit) must_brake = true;

    if (must_brake) in |= GS_IN_BRAKE;
    else in |= GS_IN_ACCEL;

    return in;
}

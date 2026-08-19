// gs_ai.c - see gs_ai.h.

#include "core/gs_ai.h"

// Below this the steering does nothing useful and the car weaves; above it, it
// turns. A dead band is the difference between driving and hunting.
#define GS_AI_DEADBAND 900

// How much of the theoretical cornering speed the AI actually trusts. Under one
// because the estimate below is a chord approximation and because a driver who
// is exactly at the limit is a driver who is about to be over it.
#define GS_AI_MARGIN GS_RATIO(82, 100)

gs_input gs_ai_drive(const gs_world *w, const gs_track *t, uint8_t car) {
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

    // --- How fast this corner can be taken, worked out now rather than looked
    // up. Everything in it can change during a race: the surface underfoot, the
    // wear in it, the gravity painted on this tile, the dials.
    gs_fix g = gs_fix_mul(w->gravity, gs_track_gravity(t, c->x, c->y));
    const gs_surface_def *sd = &gs_surfaces[gs_track_surface(t, c->x, c->y)];
    gs_fix worn = gs_world_wear(w, c->x, c->y);
    gs_fix surf_grip = gs_lerp(sd->grip, gs_fix_mul(sd->grip, sd->wear_grip), worn);

    gs_fix traction = gs_fix_mul(gs_fix_mul(surf_grip, gs_vehicle(c->vehicle)->grip),
                                 gs_fix_mul(g, w->friction_scale));

    // The turn ahead as a radius: a chord of `distance` subtending twice the
    // angle we must come round is an arc of radius d / (2 sin(turn)). Rough,
    // and rough in a knowable direction - it overestimates on very tight turns,
    // which is what the margin above is for.
    gs_fix sine = gs_fix_abs(gs_sin((gs_angle)(turn < 0 ? -turn : turn)));

    gs_fix limit;
    if (sine < GS_RATIO(4, 100) || distance <= 0) {
        // Near enough straight ahead: no cornering limit worth applying.
        limit = INT32_MAX;
    } else {
        gs_fix radius = gs_fix_div(distance, gs_fix_mul(sine, GS_INT(2)));
        // v = sqrt(a r), the oldest result in the book and still the one that
        // decides whether a corner is takeable.
        limit = gs_fix_mul(gs_fix_sqrt(gs_fix_mul(traction, radius)), GS_AI_MARGIN);
    }

    gs_fix speed = gs_car_speed(c);
    if (speed > limit) in |= GS_IN_BRAKE;
    else in |= GS_IN_ACCEL;

    return in;
}

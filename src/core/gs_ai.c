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

    // --- Approach a gate from behind it, not from in front.
    //
    // **A gate is directional**, so a car that has gone past one has to come
    // round and take it the right way - and aiming at the gate's centre from the
    // wrong side means driving at it, arriving from the front, and not crossing.
    // The car then sits beside it turning circles.
    //
    // This never showed because the ground used to go on forever: a car that
    // overshot wandered about on an infinite plain and eventually blundered back
    // through from the correct side. With an edge to the world, that same
    // wandering goes over it. Aiming at a point *behind* the gate turns an
    // overshoot into a deliberate loop.
    {
        gs_fix fx = gs_cos(target->heading);
        gs_fix fy = gs_sin(target->heading);

        // How far in front of the gate the car is: positive means past it.
        gs_fix ahead = gs_fix_mul(c->x - target->x, fx) +
                       gs_fix_mul(c->y - target->y, fy);

        if (ahead > 0) {
            // A fixed run-up on the near side, and **not one that grows with the
            // overshoot**: scaling it meant a car thirty-five tiles past a gate
            // aimed twenty-five tiles off the map, and drove there. Four tiles is
            // enough to straighten up in and is always somewhere real.
            //
            // Kept on the track, because the point of coming back is to be on
            // it: a gate near an edge would otherwise put the run-up outside,
            // and a car would drive off the world in order to line up correctly.
            gs_fix ax = target->x - gs_fix_mul(fx, GS_INT(4));
            gs_fix ay = target->y - gs_fix_mul(fy, GS_INT(4));
            ax = GS_CLAMP(ax, GS_HALF, GS_INT(t->w) - GS_HALF);
            ay = GS_CLAMP(ay, GS_HALF, GS_INT(t->h) - GS_HALF);

            dx = ax - c->x;
            dy = ay - c->y;
        }
    }

    gs_fix distance = gs_fix_len2(dx, dy);

    gs_angle want = 0;
    int32_t  turn = 0;
    gs_input in = 0;

    // --- Stay on the track.
    //
    // **The shoulder is a warning and the drop past it is not.** A driver aiming
    // only at the next gate will run wide on the way to it, and running wide
    // used to cost nothing at all because the ground went on forever. It does
    // not any more: past the run-off the world falls away, and a car that goes
    // over is finished.
    //
    // The correction is **per axis, and only towards the edge being approached**.
    // Pulling the aim point towards the middle of the track instead looks
    // simpler and is wrong: on a track narrower than the lookahead - which is
    // most of them at speed - every point is near an edge, so the car drives to
    // the centre for ever and never reaches a gate. Nudging only the axis that
    // is in trouble leaves the route intact along the other one.
    {
        // How far ahead the edge starts mattering is a *time*, not a distance.
        // Four tiles is plenty at walking pace and half a second at Jupiter,
        // where a sprint car does seven tiles a second - and half a second is
        // not enough to turn in.
        // **No steering bias while still on the track.** An earlier version
        // nudged the aim point away from whichever edge was close, and every
        // shape of that fought the route it was supposed to be following: too
        // wide and the car drove to the middle of a narrow track for ever, too
        // narrow and it never turned in time. What keeps a car on is the sand -
        // it is a run-off, and a run-off stops you - together with braking for
        // the edge the way the corner planner brakes for a gate. Steering is
        // left to the route.

        // Off the track already: forget the gate and get back on. Nothing else
        // matters until it is, and the gate is very likely behind the car by
        // then anyway.
        gs_fix out_now = gs_track_outside(t, c->x, c->y);
        if (out_now > 0) {
            dx = GS_CLAMP(c->x, 0, GS_INT(t->w)) - c->x;
            dy = GS_CLAMP(c->y, 0, GS_INT(t->h)) - c->y;
            if (dx == 0 && dy == 0) dx = GS_ONE;
        }

        distance = gs_fix_len2(dx, dy);
        want = gs_atan2(dy, dx);
        turn = gs_angle_delta(c->heading, want);

        in = 0;
        if (turn < -GS_AI_DEADBAND) in |= GS_IN_LEFT;
        else if (turn > GS_AI_DEADBAND) in |= GS_IN_RIGHT;
    }

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

    // --- And the edge of the world, which is a corner like any other.
    //
    // **Steering away from the boundary is not enough on its own.** A car
    // arriving at the edge faster than it can turn runs out of track whatever it
    // does with the wheel - which is exactly how a sprint car at Jupiter left a
    // sixty-tile circuit and kept going. So the same braking rule that plans for
    // a gate plans for the edge: how much room is left, how fast it is closing,
    // and whether there is space to shed the difference.
    //
    // Only when actually heading at it. A car running parallel to the boundary
    // is not approaching it, and braking for scenery it is driving alongside
    // would make every lap of a narrow track a crawl.
    {
        gs_fix room = INT32_MAX;
        gs_fix closing = 0;

        if (c->vx < 0)      { room = c->x;                 closing = -c->vx; }
        else if (c->vx > 0) { room = GS_INT(t->w) - c->x;   closing = c->vx; }
        if (c->vy < 0 && c->y < room)                { room = c->y;               closing = -c->vy; }
        else if (c->vy > 0 && GS_INT(t->h) - c->y < room) { room = GS_INT(t->h) - c->y; closing = c->vy; }

        // The shoulder counts as room, because arriving on it slowly is a
        // mistake and not an ending. Only when there is a room to add it to: a
        // car heading at no edge at all has room INT32_MAX, and adding ten tiles
        // to that is an overflow rather than a longer straight.
        if (closing > 0) room += GS_INT(GS_RUNOFF_TILES);

        // **Braking for the edge happens on the run-off, not on the track.**
        // Working this out with the grip underfoot says a car doing seven tiles
        // a second on pavement can stop in two - and then it crosses onto loose
        // ground with a third of the grip, where it cannot, and the sum was
        // answering a question about ground it had already left.
        const gs_surface_def *loose = &gs_surfaces[GS_RUNOFF_SURFACE];
        gs_fix edge_traction =
            gs_fix_mul(gs_fix_mul(loose->grip, gs_vehicle(c->vehicle)->grip),
                       gs_fix_mul(g, w->friction_scale));

        if (closing > 0 && room > 0 && edge_traction > 0) {
            gs_fix stopping = gs_fix_div(gs_fix_mul(closing, closing),
                                         gs_fix_mul(edge_traction, GS_INT(2)));
            if (stopping >= room) must_brake = true;
        }

        // **Already off it: slow down until pointed back.** A car on the
        // shoulder still carrying racing speed cannot turn on ground that has no
        // grip, and every tile it covers is a tile further from the track. Speed
        // is what it has to give up first; the wheel does nothing until it has.
        //
        // The turn here is towards the middle rather than towards the gate,
        // because the aim point was replaced above for exactly this case.
        //
        // **Only while it is still moving.** The brake is also reverse, so a car
        // that has come to rest on the shoulder facing the wrong way is told to
        // brake, reverses, and backs over the edge it just stopped short of -
        // which is how this rule failed the first time it existed.
        if (gs_track_outside(t, c->x, c->y) > 0 && speed > GS_ONE) {
            int32_t away = turn < 0 ? -turn : turn;
            if (away > GS_QUARTER / 2) must_brake = true;
        }
    }

    if (must_brake) in |= GS_IN_BRAKE;
    else in |= GS_IN_ACCEL;

    return in;
}

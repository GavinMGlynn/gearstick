// gs_ai.c - see gs_ai.h.

#include "core/gs_ai.h"

// Below this the steering does nothing useful and the car weaves; above it, it
// turns. A dead band is the difference between driving and hunting - and how
// wide it is, is one of the things the dial moves.
#define GS_AI_DEADBAND 900

gs_ai_style gs_ai_skill_style(int skill) {
    if (skill < 0) skill = 0;
    if (skill > GS_AI_SKILL_STEPS) skill = GS_AI_SKILL_STEPS;

    gs_ai_style st;

    // 0.600 at the bottom to 0.960 at the top, in twenty steps of 0.018. Fine
    // enough that no two settings drive the same and coarse enough that every
    // one of them is a different lap time.
    st.margin = GS_RATIO(600 + 18 * skill, 1000);

    // A third early at the bottom, on the sum at the top. This is what stops
    // the dial being a speed handicap: two drivers can lap within a tenth of
    // each other and still be visibly doing different things at the corner,
    // because one of them has finished braking before the other has started.
    st.brake_early = GS_RATIO(1200 - 10 * skill, 1000);

    // Twice the base dead band at the bottom, half of it at the top. A driver
    // who corrects late wanders on the straights and arrives at a jump pointing
    // slightly wrong, which is where the lines diverge.
    st.deadband = (gs_angle)(GS_AI_DEADBAND * 2 - (GS_AI_DEADBAND * 3 / 2) *
                             skill / GS_AI_SKILL_STEPS);
    return st;
}

gs_fix gs_ai_skill_margin(int skill) {
    return gs_ai_skill_style(skill).margin;
}

gs_input gs_ai_drive(const gs_world *w, const gs_track *t, uint8_t car) {
    return gs_ai_drive_style(w, t, car, gs_ai_skill_style(GS_AI_SKILL_DEFAULT));
}

gs_input gs_ai_drive_at(const gs_world *w, const gs_track *t, uint8_t car,
                        gs_fix margin) {
    gs_ai_style st = gs_ai_skill_style(GS_AI_SKILL_DEFAULT);
    st.margin = margin;
    return gs_ai_drive_style(w, t, car, st);
}

// **How close somebody has to be behind for it to be worth leaving something.**
// Seven tiles: near enough that they will drive into it before they can see it
// coming, far enough that it is not the same as being rammed.
#define GS_AI_DROP_RANGE GS_INT(7)

// One tick in forty, so the press is a *tap*. The button drops on release and
// changes the selection when held, so a driver that simply holds it down while
// somebody is behind would cycle through its weapons and never leave one -
// which is the shape of the control, not a detail of it.
#define GS_AI_DROP_EVERY 40u

// **Is anybody close behind?** You cannot shoot forwards, so hurting somebody
// means getting in front of them and staying there. That is a race, and it is
// the whole reason the weapon is worth having.
static bool gs_ai_hunted(const gs_world *w, uint8_t car) {
    const gs_car *me = &w->car[car];
    const gs_fix ch = gs_cos(me->heading);
    const gs_fix sh = gs_sin(me->heading);

    for (uint8_t i = 0; i < w->car_count; i++) {
        if (i == car) continue;
        const gs_car *o = &w->car[i];
        if (!o->active || o->wrecked) continue;

        const gs_fix dx = o->x - me->x, dy = o->y - me->y;
        if (gs_fix_len2(dx, dy) > GS_AI_DROP_RANGE) continue;

        // Behind: the direction they lie in is against the way this car faces.
        if (gs_fix_mul(dx, ch) + gs_fix_mul(dy, sh) < 0) return true;
    }
    return false;
}

gs_input gs_ai_drive_style(const gs_world *w, const gs_track *t, uint8_t car,
                           gs_ai_style style) {
    const gs_fix margin = style.margin;
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
    bool     blocked = false;      // against something it cannot climb

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

        // **A step too steep to climb is a wall, and a driver goes round it.**
        //
        // The ground is authored per corner and nothing stops a track having a
        // whole tile of rise in one tile of ground - the parts box drops pieces
        // with sides like that, the generator makes them, and two of the tracks
        // in the box have one across the way to the first gate. A driver aiming
        // only at the gate drives into it, stops dead, and sits there with the
        // throttle open for the rest of the race: full power, wheel hard over,
        // nought point nought one tiles a second, for three minutes.
        //
        // **Asked of the way to the gate rather than of the way the car is
        // pointing**, which is the difference between going round it and
        // bouncing off it. A test on the heading changes its mind every time
        // the car turns, so the car turns back, and the two argue until the
        // race ends. From here to there is a fact about the map: it holds while
        // the car crawls along the wall, and stops holding when the wall is
        // behind it.
        {
            const gs_fix reach = gs_fix_len2(dx, dy);
            if (reach > 0) {
                const gs_fix ux = gs_fix_div(dx, reach);
                const gs_fix uy = gs_fix_div(dy, reach);
                const gs_fix look = GS_ONE + GS_HALF;

                const gs_fix here_h = gs_track_height(t, c->x, c->y);
                const gs_fix ahead_h =
                    gs_track_height(t, c->x + gs_fix_mul(ux, look),
                                       c->y + gs_fix_mul(uy, look));

                // What momentum will carry: a car at speed rides over a step
                // that stops a car at rest, which is why a run-up works and why
                // this is not a fixed height.
                const gs_fix speed_now = gs_car_speed(c);
                const gs_fix climbable =
                    GS_HALF + gs_fix_mul(speed_now, GS_RATIO(15, 100));

                if (ahead_h - here_h > climbable) {
                    // **Turned away from it a little at a time, and the first
                    // way that is not a wall wins.** Half a turn either side in
                    // eighths, nearest first, so the car gives up as little of
                    // the direction it wanted as the ground allows - which is
                    // what going round something means. Trying only ninety
                    // degrees either way finds the way round a wall and drives
                    // along the side of a hill that a smaller turn would have
                    // cleared.
                    static const int32_t sweep[] = {
                        4096, -4096, 8192, -8192, 12288, -12288, 16384, -16384,
                    };

                    const gs_angle facing_now = gs_atan2(dy, dx);
                    gs_fix best_h = ahead_h;
                    gs_angle best_way = facing_now;
                    bool clear = false;

                    for (size_t k = 0; k < sizeof sweep / sizeof sweep[0]; k++) {
                        const gs_angle way =
                            (gs_angle)((int32_t)facing_now + sweep[k]);
                        const gs_fix tx = c->x + gs_fix_mul(gs_cos(way), look);
                        const gs_fix ty = c->y + gs_fix_mul(gs_sin(way), look);
                        const gs_fix h = gs_track_height(t, tx, ty);

                        if (h - here_h <= climbable) {
                            best_way = way;
                            clear = true;
                            break;
                        }
                        // Nothing clear yet: remember the least bad, so a car
                        // in a bowl still climbs out of the shallowest side
                        // rather than sitting in the bottom of it.
                        if (h < best_h) { best_h = h; best_way = way; }
                    }
                    (void)clear;

                    dx = gs_fix_mul(gs_cos(best_way), reach);
                    dy = gs_fix_mul(gs_sin(best_way), reach);

                }

                // **And whether it is against one right now**, which is a
                // different question from whether the way to the gate crosses
                // one. A car can be aiming somewhere perfectly clear and be
                // pinned by a cliff it is *pointing* at - it cannot steer,
                // because steering is something a moving car does, and full
                // power holds it there for the rest of the race. Two of the
                // tracks in the box have a three-tile face, and a car that
                // arrived at it sideways sat under it at nought tiles a second
                // with the throttle open.
                //
                // So this one asks about the heading, and the answer is to back
                // off until there is room to turn.
                if (speed_now < GS_RATIO(30, 100)) {
                    const gs_fix nx = c->x + gs_fix_mul(gs_cos(c->heading), look);
                    const gs_fix ny = c->y + gs_fix_mul(gs_sin(c->heading), look);
                    if (gs_track_height(t, nx, ny) - here_h > climbable) {
                        blocked = true;
                    }
                }
            }
        }

        distance = gs_fix_len2(dx, dy);
        want = gs_atan2(dy, dx);
        turn = gs_angle_delta(c->heading, want);

        in = 0;
        if (turn < -(int32_t)style.deadband) in |= GS_IN_LEFT;
        else if (turn > (int32_t)style.deadband) in |= GS_IN_RIGHT;
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
    if (sin_half < GS_RATIO(3, 100)) {
        corner_speed = INT32_MAX;      // straight on, or as near as makes no odds
    } else {
        // **A hairpin is the tightest corner there is, and this read it as the
        // straightest.**
        //
        // The radius of a turn of angle `corner` is leg*cos(half)/(2 sin(half)),
        // which goes to nothing as the corner approaches a full reversal -
        // and cos(half) reaches exactly zero at a hundred and eighty degrees.
        // That case was lumped in with "straight on", so a driver arriving at a
        // hairpin planned no braking at all, went straight on at the gate, and
        // then spent the rest of the race looping back for it. **Every track
        // with two gates on it is that corner**, which is why a bare rectangle
        // with a start and a finish was where it showed.
        //
        // The floor is a turning circle rather than nothing. A radius of zero
        // says come to a complete stop, which no car has to do for a hairpin,
        // and it makes the last degree before the reversal behave nothing like
        // the one before it.
        gs_fix radius = cos_half > 0
                            ? gs_fix_div(gs_fix_mul(leg, cos_half),
                                         gs_fix_mul(sin_half, GS_INT(2)))
                            : 0;
        if (radius < GS_HALF) radius = GS_HALF;
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
        needed = gs_fix_mul(needed, style.brake_early);
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

    if (blocked) {
        // **Backing away from a wall turns the nose the other way.** The rule
        // above picked the side to go round by; reversing with the wheel that
        // way swings the front the wrong way and the car comes back off the
        // wall pointing at it again, which is an oscillation rather than a
        // recovery. So while it is backing off, the wheel goes the other way -
        // and it arrives with its nose already pointed along the wall.
        const bool left = (in & GS_IN_LEFT) != 0;
        const bool right = (in & GS_IN_RIGHT) != 0;
        // Complemented inside the width it is going to be stored in. A `~` on
        // a promoted int makes a constant with the top twenty-four bits set,
        // and casting that back down to the byte `gs_input` is truncates it -
        // which is exactly what was meant and is also, to MSVC, warning C4310,
        // and warnings are errors here. **Every Windows build failed to
        // compile for four commits on that one cast**, and nothing on this
        // machine could see it: gcc and clang say nothing at all.
        in &= (gs_input)(0xFFu & ~(unsigned)(GS_IN_LEFT | GS_IN_RIGHT));
        if (left) in |= GS_IN_RIGHT;
        else if (right) in |= GS_IN_LEFT;
    }

    if (must_brake || blocked) in |= GS_IN_BRAKE;
    else in |= GS_IN_ACCEL;

    // **And leave something behind, if there is anybody to leave it for.**
    // Nothing carried is nothing pressed, so a race with the weapons off is
    // exactly the race it was before opponents could use them - the button is
    // never touched and the world never differs by a bit.
    if (gs_car_selected(c) != GS_HAZ_NONE &&
        (w->tick % GS_AI_DROP_EVERY) == 0 && gs_ai_hunted(w, car)) {
        in |= GS_IN_FIRE;
    }

    return in;
}

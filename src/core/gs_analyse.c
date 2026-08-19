// gs_analyse.c - see gs_analyse.h.

#include "core/gs_analyse.h"

// A pace slow enough that a car failing to keep it has been stopped by the
// track rather than merely slowed by it. Cars cruise at four or five tiles a
// second on the flat; two is what is left after the climbs, the lightest gravity
// on the dial and an AI that is not trying to be quick.
#define GS_ANALYSIS_PACE GS_INT(2)

// A floor and a ceiling. The floor gives a very short track a fair go at the
// heavy end of the gravity dial, where everything is slow; the ceiling stops one
// enormous route from turning a fifty-track sweep into a coffee break.
#define GS_ANALYSIS_MIN_SECONDS 20u
#define GS_ANALYSIS_MAX_SECONDS 90u

uint32_t gs_analyse_seconds(const gs_track *t) {
    if (t->gate_count == 0) return GS_ANALYSIS_MIN_SECONDS;

    // The whole lap, closing back to the first gate, plus the run from the grid
    // up to the line.
    gs_fix len = GS_INT(3);
    for (uint8_t i = 0; i < t->gate_count; i++) {
        const gs_gate *a = &t->gate[i];
        const gs_gate *b = &t->gate[(i + 1u) % t->gate_count];
        len += gs_fix_len2(b->x - a->x, b->y - a->y);
    }

    uint32_t seconds = (uint32_t)(gs_fix_div(len, GS_ANALYSIS_PACE) / GS_ONE);
    if (seconds < GS_ANALYSIS_MIN_SECONDS) seconds = GS_ANALYSIS_MIN_SECONDS;
    if (seconds > GS_ANALYSIS_MAX_SECONDS) seconds = GS_ANALYSIS_MAX_SECONDS;
    return seconds;
}

void gs_analyse(const gs_track *t, uint32_t seconds, gs_analysis *out) {
    for (size_t i = 0; i < GS_TRACK_TILES; i++) out->visits[i] = 0;
    out->completable = false;
    out->lightest = 0;
    out->heaviest = 0;
    out->busiest = 0;

    for (int step = 0; step < GS_ANALYSIS_STEPS; step++) {
        // 0.15x Earth up to 2.55x, which spans the Moon to past Jupiter.
        gs_fix gravity = GS_RATIO(15 + 30 * step, 100);
        out->gravity[step] = gravity;
        out->completed[step] = 0;

        for (uint8_t v = 0; v < GS_VEH_COUNT; v++) {
            gs_world w;
            gs_world_init(&w, gravity);
            if (t->gate_count == 0) continue;

            gs_fix sx, sy;
            gs_angle facing;
            gs_track_grid(t, 0, &sx, &sy, &facing);
            gs_world_add_car(&w, t, v, sx, sy, facing);

            for (uint32_t i = 0; i < GS_TICK_HZ * seconds; i++) {
                gs_input in[GS_MAX_CARS] = { gs_ai_drive(&w, t, 0), 0, 0, 0 };
                gs_world_step(&w, t, in);

                // Sampled rather than counted every tick: a car sitting still
                // would otherwise light up a tile brighter than the whole
                // racing line.
                if ((i % 8u) == 0u && gs_car_speed(&w.car[0]) > GS_HALF) {
                    int32_t tx = GS_CLAMP(gs_fix_floor(w.car[0].x), 0, (int32_t)t->w - 1);
                    int32_t ty = GS_CLAMP(gs_fix_floor(w.car[0].y), 0, (int32_t)t->h - 1);
                    size_t at = GS_TILE_INDEX(tx, ty);
                    if (out->visits[at] < UINT16_MAX) out->visits[at]++;
                    if (out->visits[at] > out->busiest) out->busiest = out->visits[at];
                }

                // Done at the flag, not at the end of the time allowed. A car
                // that has finished its lap and is still driving is milling
                // about, and the tile it happens to mill on collects more
                // visits than any part of the route - which paints the busiest
                // corner bright and the line everybody drove invisible.
                if (w.car[0].laps > 0) break;
            }

            if (w.car[0].laps > 0) out->completed[step]++;
        }

        if (out->completed[step] > 0) {
            if (!out->completable) {
                out->completable = true;
                out->lightest = gravity;
            }
            out->heaviest = gravity;
        }
    }
}

gs_fix gs_analysis_heat(const gs_analysis *a, uint8_t x, uint8_t y) {
    if (a->busiest == 0) return 0;
    size_t at = GS_TILE_INDEX(x, y);
    return (gs_fix)(((int64_t)a->visits[at] * GS_ONE) / a->busiest);
}

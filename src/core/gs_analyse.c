// gs_analyse.c - see gs_analyse.h.

#include "core/gs_analyse.h"

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
            gs_world_add_car(&w, t, v, t->gate[0].x, t->gate[0].y, t->gate[0].heading);

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

#include "core/gs_target.h"

#include "core/gs_ai.h"
#include "core/gs_analyse.h"
#include "core/gs_vehicle.h"

uint32_t gs_target_lap(const gs_track *t, gs_fix gravity, int skill,
                       uint8_t *vehicle) {
    if (vehicle != nullptr) *vehicle = 0;
    if (t->gate_count == 0) return 0;

    const gs_ai_style style = gs_ai_skill_style(skill);
    const uint32_t budget = gs_analyse_seconds(t) * (uint32_t)GS_TICK_HZ;
    uint32_t best = 0;

    for (uint8_t v = 0; v < GS_VEH_COUNT; v++) {
        gs_world w;
        gs_world_init(&w, gravity);
        gs_world_set_mode(&w, GS_MODE_RACE);
        gs_world_set_laps(&w, 1);

        gs_fix sx, sy;
        gs_angle facing;
        gs_track_grid(t, 0, &sx, &sy, &facing);
        gs_world_add_car(&w, t, v, sx, sy, facing);

        for (uint32_t i = 0; i < budget; i++) {
            gs_input in[GS_MAX_CARS] = { gs_ai_drive_style(&w, t, 0, style), 0, 0, 0 };
            gs_world_step(&w, t, in);
            if (w.car[0].finish_tick != 0) break;
        }
        // A circuit's best lap is its one real lap, the run-up excluded; a
        // path's is the whole run, start to finish. Both are what a person
        // would be timed on.
        const gs_car *c = &w.car[0];
        if (c->finish_tick == 0 || c->best_lap == 0) continue;
        if (best == 0 || c->best_lap < best) {
            best = c->best_lap;
            if (vehicle != nullptr) *vehicle = v;
        }
    }
    return best;
}

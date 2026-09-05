#include "core/gs_ghost.h"

#include <string.h>

void gs_ghost_clear(gs_ghost *g) {
    memset(g, 0, sizeof *g);
}

// Common tail of load and take: set the world up as the recording describes and
// decide whether this ghost can run here at all.
int32_t gs_split_index(const gs_track *t, const gs_car *c) {
    const uint8_t n = t->gate_count;
    if (n == 0) return -1;
    if (c->laps == 0 && c->next_gate == 0) return -1;      // crossed nothing yet
    const uint8_t crossed = (uint8_t)((c->next_gate + n - 1u) % n);
    // A circuit's laps count its first start-line crossing, which is the run
    // up rather than a lap; a path has one pass and one lap index.
    uint32_t lap = 0;
    if (gs_track_is_circuit(t)) lap = c->laps > 0 ? (uint32_t)c->laps - 1u : 0u;
    const uint32_t index = lap * n + crossed;
    return index < GS_GHOST_SPLITS ? (int32_t)index : -1;
}

bool gs_ghost_split(const gs_ghost *g, int32_t index, uint32_t *tick) {
    if (index < 0 || index >= GS_GHOST_SPLITS || g->split[index] == 0) return false;
    if (tick != nullptr) *tick = g->split[index] - 1u;
    return true;
}

// One headless playback of the recording, noting the tick of every gate
// crossing the followed car makes. Cheap - a race is a few hundred thousand
// steps at most - and done once, when the ghost is armed, so a live race
// never pays for it.
static gs_world gs_survey_world;

static void gs_ghost_survey(gs_ghost *g, const gs_track *t) {
    for (size_t i = 0; i < GS_GHOST_SPLITS; i++) g->split[i] = 0;
    if (!g->ready || g->car >= g->world.car_count) return;
    gs_survey_world = g->world;
    for (uint32_t tick = 0; tick < g->replay.meta.tick_count; tick++) {
        const gs_car *c = &gs_survey_world.car[g->car];
        const uint8_t was_gate = c->next_gate;
        const uint16_t was_laps = c->laps;
        gs_world_step(&gs_survey_world, t, gs_replay_at(&g->replay, tick));
        if (c->next_gate == was_gate && c->laps == was_laps) continue;
        // The world's tick after this step is tick + 1; stored one higher
        // again so that zero can mean "never crossed".
        const int32_t index = gs_split_index(t, c);
        if (index >= 0 && g->split[index] == 0) g->split[index] = tick + 2u;
    }
}

static void gs_ghost_arm(gs_ghost *g, const gs_track *t) {
    g->loaded = true;
    g->car = 0;
    g->ready = gs_replay_restore(&g->replay, &g->world, t);
    g->prev = g->world;
    g->tick = 0;
    g->finished = !g->ready || g->replay.meta.tick_count == 0;
    gs_ghost_survey(g, t);
}

bool gs_ghost_load(gs_ghost *g, const gs_track *t, const uint8_t *buf, size_t len) {
    gs_ghost_clear(g);
    if (!gs_replay_deserialize(&g->replay, buf, len)) return false;
    gs_ghost_arm(g, t);
    return g->ready;
}

bool gs_ghost_take(gs_ghost *g, const gs_replay *r, const gs_track *t) {
    // Deliberately a copy. The ghost outlives the race that produced it - it is
    // usually taken from the recording of the run you have just finished, which
    // is about to be started over on top of.
    gs_ghost_clear(g);
    g->replay = *r;
    gs_ghost_arm(g, t);
    return g->ready;
}

void gs_ghost_reset(gs_ghost *g, const gs_track *t) {
    if (!g->loaded) return;
    gs_ghost_arm(g, t);
}

void gs_ghost_step(gs_ghost *g, const gs_track *t) {
    if (!g->ready || g->finished) return;

    g->prev = g->world;
    gs_world_step(&g->world, t, gs_replay_at(&g->replay, g->tick));
    g->tick++;

    if (g->tick >= g->replay.meta.tick_count) g->finished = true;
}

const gs_car *gs_ghost_car(const gs_ghost *g) {
    if (!g->ready || g->car >= g->world.car_count) return nullptr;
    return &g->world.car[g->car];
}

const gs_car *gs_ghost_prev_car(const gs_ghost *g) {
    if (!g->ready || g->car >= g->prev.car_count) return nullptr;
    return &g->prev.car[g->car];
}

uint32_t gs_ghost_length(const gs_ghost *g) {
    return g->loaded ? g->replay.meta.tick_count : 0;
}

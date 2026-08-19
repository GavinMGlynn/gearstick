#include "core/gs_ghost.h"

#include <string.h>

void gs_ghost_clear(gs_ghost *g) {
    memset(g, 0, sizeof *g);
}

// Common tail of load and take: set the world up as the recording describes and
// decide whether this ghost can run here at all.
static void gs_ghost_arm(gs_ghost *g, const gs_track *t) {
    g->loaded = true;
    g->car = 0;
    g->ready = gs_replay_restore(&g->replay, &g->world, t);
    g->prev = g->world;
    g->tick = 0;
    g->finished = !g->ready || g->replay.meta.tick_count == 0;
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

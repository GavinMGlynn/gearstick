// gs_input.h - turning a keyboard and up to four pads into one byte per car.
//
// One byte, because that is what the simulation takes and what a replay stores.
// Everything a player does has to fit through here, which is a constraint worth
// keeping: it is what makes rollback cheap and a shared race small.
#ifndef GS_INPUT_H
#define GS_INPUT_H

#include <SDL3/SDL.h>

#include "core/gs_sim.h"

typedef struct gs_input_state {
    SDL_Gamepad *pad[GS_MAX_CARS];
    int          pads;
} gs_input_state;

void gs_input_init(gs_input_state *s);
void gs_input_quit(gs_input_state *s);

// Reacts to pads arriving and leaving. Safe to call with any event.
void gs_input_event(gs_input_state *s, const SDL_Event *e);

// Fill `out` with one byte per car. Pad N drives car N; the keyboard drives
// cars 0 and 1 as well, so one person can test two cars without two pads.
void gs_input_poll(const gs_input_state *s, gs_input *out, uint8_t cars);

#endif // GS_INPUT_H

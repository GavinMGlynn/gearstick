// gs_input.h - turning a keyboard and up to four pads into one byte per car.
//
// One byte, because that is what the simulation takes and what a replay stores.
// Everything a player does has to fit through here, which is a constraint worth
// keeping: it is what makes rollback cheap and a shared race small.
#ifndef GS_INPUT_H
#define GS_INPUT_H

#include <SDL3/SDL.h>

#include "core/gs_sim.h"
#include "platform/gs_bind.h"

typedef struct gs_input_state {
    SDL_Gamepad *pad[GS_MAX_CARS];
    int          pads;

    // Last frame's buttons on pad zero, so the editor can tell a press from a
    // hold. The race does not care - a held accelerator is a held accelerator -
    // but undo repeating sixty times a second would be a disaster.
    uint32_t     was_down;

    // What each control does. Loaded from the player's own file if they have
    // one, defaults otherwise.
    gs_bindings  bind;
} gs_input_state;

// The editor's view of pad zero: sticks as floats, buttons as edges. Built here
// rather than in the editor so that the editor can be driven by a test without
// a pad, and so SDL's specifics stay in the platform layer.
typedef struct gs_pad_edit {
    bool  present;
    float x, y;            // left stick, deadzoned, -1 to 1
    float zoom;            // right stick vertical, for zooming
    bool  paint;           // held: apply the brush
    bool  undo, redo;      // pressed this frame
    bool  next_brush;      // pressed this frame: cycle the brush
    bool  drive;           // pressed this frame: take a test drive
} gs_pad_edit;

void gs_input_editor_pad(gs_input_state *s, gs_pad_edit *out);

// Who drives what. **Pad N drives car N**, and the keyboard drives cars zero
// and one as well, so one person can try two cars without owning two pads.
//
// Split out from the SDL reading so the rule can be tested without hardware:
// "the second pad drives the second car" is the whole of two-player on one
// machine, and it should not be a claim that rests on reading the code.
void gs_input_combine(const gs_input *from_pads, int pads,
                      const gs_input *from_keys, int keys,
                      gs_input *out, uint8_t cars);

void gs_input_init(gs_input_state *s);

// Read and write the player's bindings. Failure to load is not an error - it
// means they have never changed anything - and leaves the defaults in place.
bool gs_input_load_bindings(gs_input_state *s);
bool gs_input_save_bindings(const gs_input_state *s);
void gs_input_quit(gs_input_state *s);

// Reacts to pads arriving and leaving. Safe to call with any event.
void gs_input_event(gs_input_state *s, const SDL_Event *e);

// Fill `out` with one byte per car. Pad N drives car N; the keyboard drives
// cars 0 and 1 as well, so one person can test two cars without two pads.
void gs_input_poll(const gs_input_state *s, gs_input *out, uint8_t cars);

// **Back, said two ways.** Escape is a key and a pad has none of those - so
// somebody on a pad could only leave a screen by walking to the button that
// says so, which on the tracks screen means stepping down through every track
// they own. The pad's cancel button is back as well.
//
// **Except while a race is on**, where that same button is the brake. Backing
// out of a corner is not what anybody means by it.
bool gs_input_is_back(const SDL_Event *e, bool racing);

#endif // GS_INPUT_H

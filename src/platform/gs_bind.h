// gs_bind.h - which button does what, and the ability to change it.
//
// **Every control is remappable**, which is not a luxury feature here. Four
// people on one sofa is the shape of this game, and a pad that has its buttons
// somewhere else, a left-handed player, or somebody who simply cannot reach the
// default keys are all ordinary rather than exotic.
//
// The resolution is a pure function of "which keys are down" and "which pad
// buttons are down", so it can be tested without a keyboard or a pad. The SDL
// reading lives in gs_input.c and hands this the answers.
#ifndef GS_BIND_H
#define GS_BIND_H

#include <SDL3/SDL.h>

#include "core/gs_sim.h"

typedef enum gs_action {
    GS_ACT_ACCEL = 0,
    GS_ACT_BRAKE,
    GS_ACT_LEFT,
    GS_ACT_RIGHT,
    GS_ACT_FIRE,
    GS_ACT_RESCUE,
    GS_ACT_COUNT
} gs_action;

// A binding may be unset, which is what makes "clear this control" possible
// rather than only "change it to something else".
#define GS_KEY_NONE    SDL_SCANCODE_UNKNOWN
#define GS_BUTTON_NONE ((int16_t)-1)

typedef struct gs_bindings {
    SDL_Scancode key[GS_MAX_CARS][GS_ACT_COUNT];
    int16_t      button[GS_MAX_CARS][GS_ACT_COUNT];   // SDL_GamepadButton, or none
} gs_bindings;

// Arrows and the right shift for player one, WASD and the left shift for two,
// and every player gets the same pad layout - because a pad is per player and
// there is no reason for the second one to be arranged differently.
void gs_bind_defaults(gs_bindings *b);

const char *gs_action_name(gs_action a);

// Which actions a player is asking for, given what is held down. `keys` is
// SDL's keyboard array (or null), `buttons` a bitmask of SDL_GamepadButton.
//
// Keyboard and pad are *both* consulted and combined, so a player can use
// either at any moment without a mode to switch.
gs_input gs_bind_resolve(const gs_bindings *b, uint8_t player,
                         const bool *keys, int key_count, uint32_t buttons);

// **What a rebind capture should do with whatever is held down right now.**
//
// Split out of the editor for the same reason `gs_bind_resolve` is: it is the
// rule, it is a pure function of "what is down", and where it lived it could
// not be tested without a keyboard and a pad in somebody's hands. A third of
// its lines had never run.
//
// `armed` is the part that is not obvious and is the whole of the fault this
// was written for. A capture begins the instant a control is pressed - and the
// control was pressed *with something*: Space or Enter if the player walked to
// it with the keyboard, the pad's bottom button if they walked to it with a
// pad. That key is still down on the very next frame, so the capture bound the
// action to it immediately, and a player rebinding their controls from the
// keyboard could only ever bind Space. **So a capture waits for everything to
// be let go before it accepts anything.** Point `armed` at a bool the caller
// keeps for the duration of the capture, initialised to false.
typedef enum gs_rebind_what {
    GS_REBIND_WAIT = 0,   // nothing yet - still held, or nothing pressed
    GS_REBIND_CANCEL,     // Escape: leave the binding alone
    GS_REBIND_KEY,        // `which` is an SDL_Scancode
    GS_REBIND_BUTTON      // `which` is an SDL_GamepadButton
} gs_rebind_what;

typedef struct gs_rebind_pick {
    gs_rebind_what what;
    int            which;
} gs_rebind_pick;

gs_rebind_pick gs_bind_pick(bool *armed, const bool *keys, int key_count,
                            uint32_t buttons);

// Point an action at a new key or button. Rebinding to something another action
// on the same player already uses clears it there first, because two actions on
// one button is a control scheme nobody meant to make.
void gs_bind_set_key(gs_bindings *b, uint8_t player, gs_action a, SDL_Scancode key);
void gs_bind_set_button(gs_bindings *b, uint8_t player, gs_action a, int16_t button);

// Little-endian and explicit, like everything else that goes to a file.
size_t gs_bind_size(void);
size_t gs_bind_serialize(const gs_bindings *b, uint8_t *buf, size_t cap);
bool   gs_bind_deserialize(gs_bindings *b, const uint8_t *buf, size_t len);

#endif // GS_BIND_H

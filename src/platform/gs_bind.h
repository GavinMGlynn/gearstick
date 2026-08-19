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

// gs_input.c - see gs_input.h.

#include "platform/gs_input.h"

void gs_input_init(gs_input_state *s) {
    *s = (gs_input_state){ 0 };

    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    if (ids == nullptr) return;

    for (int i = 0; i < count && s->pads < GS_MAX_CARS; i++) {
        SDL_Gamepad *g = SDL_OpenGamepad(ids[i]);
        if (g != nullptr) s->pad[s->pads++] = g;
    }
    SDL_free(ids);
}

void gs_input_quit(gs_input_state *s) {
    for (int i = 0; i < s->pads; i++) {
        if (s->pad[i] != nullptr) SDL_CloseGamepad(s->pad[i]);
    }
    *s = (gs_input_state){ 0 };
}

void gs_input_event(gs_input_state *s, const SDL_Event *e) {
    if (e->type == SDL_EVENT_GAMEPAD_ADDED) {
        if (s->pads >= GS_MAX_CARS) return;
        SDL_Gamepad *g = SDL_OpenGamepad(e->gdevice.which);
        if (g != nullptr) s->pad[s->pads++] = g;
    } else if (e->type == SDL_EVENT_GAMEPAD_REMOVED) {
        for (int i = 0; i < s->pads; i++) {
            if (s->pad[i] != nullptr &&
                SDL_GetGamepadID(s->pad[i]) == e->gdevice.which) {
                SDL_CloseGamepad(s->pad[i]);
                // Close the gap rather than leaving a hole, so pad N still
                // means car N after somebody trips over a cable.
                for (int j = i; j + 1 < s->pads; j++) s->pad[j] = s->pad[j + 1];
                s->pad[--s->pads] = nullptr;
                break;
            }
        }
    }
}

// The deadzone is generous because the control is digital anyway: the question
// is only "is the stick pushed", and a stick that rests at 3000 should not
// steer.
#define GS_STICK_ON 12000

static gs_input gs_from_pad(SDL_Gamepad *g) {
    gs_input in = 0;
    if (g == nullptr) return in;

    if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000 ||
        SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_SOUTH)) in |= GS_IN_ACCEL;
    if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8000 ||
        SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_EAST)) in |= GS_IN_BRAKE;

    Sint16 lx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
    if (lx < -GS_STICK_ON || SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_LEFT))
        in |= GS_IN_LEFT;
    if (lx > GS_STICK_ON || SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_RIGHT))
        in |= GS_IN_RIGHT;

    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_WEST)) in |= GS_IN_FIRE;
    return in;
}

void gs_input_poll(const gs_input_state *s, gs_input *out, uint8_t cars) {
    const bool *key = SDL_GetKeyboardState(nullptr);

    for (uint8_t i = 0; i < GS_MAX_CARS; i++) out[i] = 0;

    for (uint8_t i = 0; i < cars && i < GS_MAX_CARS; i++) {
        if ((int)i < s->pads) out[i] = gs_from_pad(s->pad[i]);
    }

    if (key == nullptr) return;

    if (cars > 0) {
        gs_input in = 0;
        if (key[SDL_SCANCODE_UP])    in |= GS_IN_ACCEL;
        if (key[SDL_SCANCODE_DOWN])  in |= GS_IN_BRAKE;
        if (key[SDL_SCANCODE_LEFT])  in |= GS_IN_LEFT;
        if (key[SDL_SCANCODE_RIGHT]) in |= GS_IN_RIGHT;
        if (key[SDL_SCANCODE_RSHIFT]) in |= GS_IN_FIRE;
        out[0] |= in;
    }
    if (cars > 1) {
        gs_input in = 0;
        if (key[SDL_SCANCODE_W]) in |= GS_IN_ACCEL;
        if (key[SDL_SCANCODE_S]) in |= GS_IN_BRAKE;
        if (key[SDL_SCANCODE_A]) in |= GS_IN_LEFT;
        if (key[SDL_SCANCODE_D]) in |= GS_IN_RIGHT;
        if (key[SDL_SCANCODE_LSHIFT]) in |= GS_IN_FIRE;
        out[1] |= in;
    }
}

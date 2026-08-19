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

void gs_input_combine(const gs_input *from_pads, int pads,
                      const gs_input *from_keys, int keys,
                      gs_input *out, uint8_t cars) {
    for (uint8_t i = 0; i < GS_MAX_CARS; i++) out[i] = 0;

    // Pad N drives car N. Nothing cleverer: a player who picks up the second
    // pad expects to be driving the second car, and any scheme that decides
    // otherwise has to be explained to them.
    for (uint8_t i = 0; i < cars && i < GS_MAX_CARS; i++) {
        if ((int)i < pads) out[i] = from_pads[i];
    }

    // The keyboard is *added* rather than substituted, so a pad and the arrow
    // keys can drive the same car and neither disables the other.
    for (uint8_t i = 0; i < cars && (int)i < keys && i < GS_MAX_CARS; i++) {
        out[i] |= from_keys[i];
    }
}

void gs_input_poll(const gs_input_state *s, gs_input *out, uint8_t cars) {
    gs_input pads[GS_MAX_CARS] = { 0 };
    for (int i = 0; i < s->pads && i < GS_MAX_CARS; i++) {
        pads[i] = gs_from_pad(s->pad[i]);
    }

    // Two sets of keys, so one person at one keyboard can drive two cars -
    // which is how most of this game gets tested, and how a lot of it will be
    // played.
    gs_input keys[2] = { 0, 0 };
    const bool *key = SDL_GetKeyboardState(nullptr);
    if (key != nullptr) {
        if (key[SDL_SCANCODE_UP])     keys[0] |= GS_IN_ACCEL;
        if (key[SDL_SCANCODE_DOWN])   keys[0] |= GS_IN_BRAKE;
        if (key[SDL_SCANCODE_LEFT])   keys[0] |= GS_IN_LEFT;
        if (key[SDL_SCANCODE_RIGHT])  keys[0] |= GS_IN_RIGHT;
        if (key[SDL_SCANCODE_RSHIFT]) keys[0] |= GS_IN_FIRE;

        if (key[SDL_SCANCODE_W])      keys[1] |= GS_IN_ACCEL;
        if (key[SDL_SCANCODE_S])      keys[1] |= GS_IN_BRAKE;
        if (key[SDL_SCANCODE_A])      keys[1] |= GS_IN_LEFT;
        if (key[SDL_SCANCODE_D])      keys[1] |= GS_IN_RIGHT;
        if (key[SDL_SCANCODE_LSHIFT]) keys[1] |= GS_IN_FIRE;
    }

    gs_input_combine(pads, s->pads, keys, 2, out, cars);
}

// A stick reading, deadzoned and normalised. The deadzone is generous because
// this steers a cursor over a grid rather than a car: drift that would be
// invisible while driving would leave a brush creeping across the track.
static float gs_axis(SDL_Gamepad *g, SDL_GamepadAxis axis) {
    float v = (float)SDL_GetGamepadAxis(g, axis) / 32767.0f;
    if (v > -0.25f && v < 0.25f) return 0.0f;
    return v;
}

void gs_input_editor_pad(gs_input_state *s, gs_pad_edit *out) {
    *out = (gs_pad_edit){ 0 };
    if (s->pads < 1 || s->pad[0] == nullptr) {
        s->was_down = 0;
        return;
    }

    SDL_Gamepad *g = s->pad[0];
    out->present = true;
    out->x = gs_axis(g, SDL_GAMEPAD_AXIS_LEFTX);
    out->y = gs_axis(g, SDL_GAMEPAD_AXIS_LEFTY);
    out->zoom = gs_axis(g, SDL_GAMEPAD_AXIS_RIGHTY);
    out->paint = SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_SOUTH);

    // Edges, not levels. A held undo button that fired every frame would walk
    // back through an afternoon's work in under a second.
    uint32_t down = 0;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))  down |= 1u << 0;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) down |= 1u << 1;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_WEST))           down |= 1u << 2;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_START))          down |= 1u << 3;

    uint32_t pressed = down & ~s->was_down;
    s->was_down = down;

    out->undo = (pressed & (1u << 0)) != 0;
    out->redo = (pressed & (1u << 1)) != 0;
    out->next_brush = (pressed & (1u << 2)) != 0;
    out->drive = (pressed & (1u << 3)) != 0;
}

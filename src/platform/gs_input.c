// gs_input.c - see gs_input.h.

#include "platform/gs_input.h"
#include "platform/gs_paths.h"

void gs_input_init(gs_input_state *s) {
    *s = (gs_input_state){ 0 };
    gs_bind_defaults(&s->bind);
    gs_input_load_bindings(s);

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
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        if (e->key.scancode < SDL_SCANCODE_COUNT) {
            s->held[e->key.scancode] = true;
        }
        return;
    case SDL_EVENT_KEY_UP:
        // A release always releases - the shield exists to keep keys that
        // were never let go of, not to argue with a driver's own fingers.
        if (e->key.scancode < SDL_SCANCODE_COUNT) {
            s->held[e->key.scancode] = false;
        }
        s->shielding = false;
        return;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        // The event's own clock, not SDL_GetTicks: a real event carries the
        // moment it happened, and a test can carry any moment it needs.
        s->lost_at = e->common.timestamp / 1000000u;
        if (s->lost_at == 0) s->lost_at = 1;
        return;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        if (s->lost_at != 0 &&
            e->common.timestamp / 1000000u - s->lost_at <=
                (uint64_t)GS_INPUT_BLIP_MS) {
            // A blip. SDL has already forgotten every held key; what the
            // driver never released carries on from here.
            s->shielding = true;
        } else {
            // Long enough gone that a key held on the way out may have been
            // released anywhere. Forget everything rather than guess.
            SDL_memset(s->held, 0, sizeof s->held);
            s->shielding = false;
        }
        s->lost_at = 0;
        return;
    default:
        break;
    }

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

// Which of a pad's buttons are down, as a bitmask the bindings can be resolved
// against. The sticks and triggers are folded in as though they were the
// buttons they stand in for, so a player who steers with the stick and one who
// steers with the d-pad are asking for the same thing.
static uint32_t gs_pad_buttons(SDL_Gamepad *g) {
    if (g == nullptr) return 0;

    uint32_t down = 0;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 32; b++) {
        if (SDL_GetGamepadButton(g, (SDL_GamepadButton)b)) down |= 1u << b;
    }

    if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000) {
        down |= 1u << SDL_GAMEPAD_BUTTON_SOUTH;
    }
    if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8000) {
        down |= 1u << SDL_GAMEPAD_BUTTON_EAST;
    }

    Sint16 lx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
    if (lx < -GS_STICK_ON) down |= 1u << SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    if (lx > GS_STICK_ON)  down |= 1u << SDL_GAMEPAD_BUTTON_DPAD_RIGHT;

    return down;
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

bool gs_input_shielded(const gs_input_state *s, SDL_Scancode key) {
    return s->shielding && key < SDL_SCANCODE_COUNT && s->held[key];
}

void gs_input_poll(const gs_input_state *s, gs_input *out, uint8_t cars) {
    int key_count = 0;
    const bool *keys = SDL_GetKeyboardState(&key_count);

    // The keys SDL believes in, plus the ones the blip shield is carrying
    // over a focus loss it knows the driver never noticed. The shield stands
    // down the moment any key is genuinely released, so while everything is
    // ordinary this copies SDL's answer exactly.
    static bool merged[SDL_SCANCODE_COUNT];
    const bool *effective = keys;
    if (s->shielding && keys != nullptr) {
        int n = key_count < SDL_SCANCODE_COUNT ? key_count : SDL_SCANCODE_COUNT;
        for (int i = 0; i < n; i++) merged[i] = keys[i] || s->held[i];
        effective = merged;
    }

    gs_input from_pads[GS_MAX_CARS] = { 0 };
    gs_input from_keys[GS_MAX_CARS] = { 0 };

    for (uint8_t i = 0; i < GS_MAX_CARS; i++) {
        uint32_t buttons = (int)i < s->pads ? gs_pad_buttons(s->pad[i]) : 0;
        from_pads[i] = gs_bind_resolve(&s->bind, i, nullptr, 0, buttons);
        from_keys[i] = gs_bind_resolve(&s->bind, i, effective, key_count, 0);
    }

    gs_input_combine(from_pads, s->pads, from_keys, GS_MAX_CARS, out, cars);
}

static bool gs_bind_path(char *out, size_t cap) {
    const char *dir = gs_pref_dir();
    if (dir == nullptr) return false;
    SDL_snprintf(out, cap, "%scontrols.gsbind", dir);
    return true;
}

bool gs_input_load_bindings(gs_input_state *s) {
    char path[1024];
    if (!gs_bind_path(path, sizeof path)) return false;

    size_t n = 0;
    void *data = SDL_LoadFile(path, &n);
    if (data == nullptr) return false;      // never changed anything: fine

    bool ok = gs_bind_deserialize(&s->bind, (const uint8_t *)data, n);
    SDL_free(data);
    return ok;
}

bool gs_input_save_bindings(const gs_input_state *s) {
    char path[1024];
    if (!gs_bind_path(path, sizeof path)) return false;

    uint8_t buf[512];
    size_t n = gs_bind_serialize(&s->bind, buf, sizeof buf);
    return n != 0 && SDL_SaveFile(path, buf, n);
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

bool gs_input_is_back(const SDL_Event *e, bool racing) {
    if (e == nullptr) return false;

    if (e->type == SDL_EVENT_KEY_DOWN) return e->key.key == SDLK_ESCAPE;

    // The cancel button, in the position every pad calls B - which is also the
    // brake, so it only means back when nobody is driving.
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        return !racing && e->gbutton.button == SDL_GAMEPAD_BUTTON_EAST;
    }
    return false;
}

bool gs_input_back_may_quit(const SDL_Event *e) {
    return e != nullptr && e->type == SDL_EVENT_KEY_DOWN;
}

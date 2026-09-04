// gs_bind.c - see gs_bind.h.

#include "platform/gs_bind.h"

#define GS_BIND_MAGIC   0x444e4247u   // "GBND"
// Two: version one had five actions and no tow. A version-one file is still
// read - its five actions verbatim, the tow at its default - because "your
// controls survived the update" is not a feature anybody should lose to an
// enum growing.
#define GS_BIND_VERSION 2u
#define GS_BIND_V1_ACTS 5
#define GS_BIND_V1_BYTES (4 + 4 + GS_MAX_CARS * GS_BIND_V1_ACTS * 6)

static const gs_input gs_action_bit[GS_ACT_COUNT] = {
    [GS_ACT_ACCEL] = GS_IN_ACCEL,
    [GS_ACT_BRAKE] = GS_IN_BRAKE,
    [GS_ACT_LEFT]  = GS_IN_LEFT,
    [GS_ACT_RIGHT] = GS_IN_RIGHT,
    [GS_ACT_FIRE]  = GS_IN_FIRE,
};

const char *gs_action_name(gs_action a) {
    switch (a) {
    case GS_ACT_ACCEL: return "accelerate";
    case GS_ACT_BRAKE: return "brake";
    case GS_ACT_LEFT:  return "left";
    case GS_ACT_RIGHT: return "right";
    case GS_ACT_FIRE:  return "drop";
    case GS_ACT_RESCUE: return "tow";
    case GS_ACT_COUNT: break;
    }
    return "?";
}

void gs_bind_defaults(gs_bindings *b) {
    for (uint8_t p = 0; p < GS_MAX_CARS; p++) {
        for (int a = 0; a < GS_ACT_COUNT; a++) {
            b->key[p][a] = GS_KEY_NONE;
            // Every player gets the same pad layout. A pad belongs to one
            // person, so there is no reason for the second one to be arranged
            // differently from the first.
            b->button[p][a] = GS_BUTTON_NONE;
        }
        b->button[p][GS_ACT_ACCEL] = (int16_t)SDL_GAMEPAD_BUTTON_SOUTH;
        b->button[p][GS_ACT_BRAKE] = (int16_t)SDL_GAMEPAD_BUTTON_EAST;
        b->button[p][GS_ACT_LEFT]  = (int16_t)SDL_GAMEPAD_BUTTON_DPAD_LEFT;
        b->button[p][GS_ACT_RIGHT] = (int16_t)SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
        b->button[p][GS_ACT_FIRE]  = (int16_t)SDL_GAMEPAD_BUTTON_WEST;
        b->button[p][GS_ACT_RESCUE] = (int16_t)SDL_GAMEPAD_BUTTON_NORTH;
    }

    // Two people at one keyboard, which is how most of this gets played before
    // anybody finds a second pad.
    b->key[0][GS_ACT_ACCEL] = SDL_SCANCODE_UP;
    b->key[0][GS_ACT_BRAKE] = SDL_SCANCODE_DOWN;
    b->key[0][GS_ACT_LEFT]  = SDL_SCANCODE_LEFT;
    b->key[0][GS_ACT_RIGHT] = SDL_SCANCODE_RIGHT;
    b->key[0][GS_ACT_FIRE]  = SDL_SCANCODE_RSHIFT;
    b->key[0][GS_ACT_RESCUE] = SDL_SCANCODE_BACKSPACE;

    b->key[1][GS_ACT_ACCEL] = SDL_SCANCODE_W;
    b->key[1][GS_ACT_BRAKE] = SDL_SCANCODE_S;
    b->key[1][GS_ACT_LEFT]  = SDL_SCANCODE_A;
    b->key[1][GS_ACT_RIGHT] = SDL_SCANCODE_D;
    b->key[1][GS_ACT_FIRE]  = SDL_SCANCODE_LSHIFT;
    b->key[1][GS_ACT_RESCUE] = SDL_SCANCODE_Q;
}

gs_input gs_bind_resolve(const gs_bindings *b, uint8_t player,
                         const bool *keys, int key_count, uint32_t buttons) {
    if (player >= GS_MAX_CARS) return 0;

    gs_input in = 0;
    for (int a = 0; a < GS_ACT_COUNT; a++) {
        SDL_Scancode k = b->key[player][a];
        if (keys != nullptr && k != GS_KEY_NONE && (int)k < key_count && keys[k]) {
            in |= gs_action_bit[a];
        }

        int16_t btn = b->button[player][a];
        if (btn != GS_BUTTON_NONE && btn < 32 && (buttons & (1u << btn)) != 0) {
            in |= gs_action_bit[a];
        }
    }
    return in;
}

gs_rebind_pick gs_bind_pick(bool *armed, const bool *keys, int key_count,
                            uint32_t buttons) {
    gs_rebind_pick pick = { GS_REBIND_WAIT, 0 };

    // What is held, whatever it is. Escape is not special here: a capture that
    // began with Escape still down should not read it as "leave it alone".
    bool anything = buttons != 0;
    if (keys != nullptr) {
        for (int k = 0; k < key_count && !anything; k++) {
            if (keys[k]) anything = true;
        }
    }

    // **Let go first.** See the note in gs_bind.h: the control that started
    // this capture was pressed with something, and that something is still
    // down now.
    if (armed != nullptr && !*armed) {
        if (anything) return pick;
        *armed = true;
        return pick;
    }
    if (!anything) return pick;

    if (keys != nullptr && key_count > (int)SDL_SCANCODE_ESCAPE &&
        keys[SDL_SCANCODE_ESCAPE]) {
        pick.what = GS_REBIND_CANCEL;
        return pick;
    }

    // The keyboard first, and the lowest scancode held. A player pressing one
    // key is the case; a player mashing three gets the first of them, which is
    // arbitrary but is at least the same arbitrary answer every time.
    if (keys != nullptr) {
        for (int k = 0; k < key_count; k++) {
            if (!keys[k]) continue;
            pick.what = GS_REBIND_KEY;
            pick.which = k;
            return pick;
        }
    }

    for (int b = 0; b < 32; b++) {
        if ((buttons & (1u << b)) == 0) continue;
        pick.what = GS_REBIND_BUTTON;
        pick.which = b;
        return pick;
    }
    return pick;
}

void gs_bind_set_key(gs_bindings *b, uint8_t player, gs_action a, SDL_Scancode key) {
    if (player >= GS_MAX_CARS || a >= GS_ACT_COUNT) return;

    // Whoever else on this player had it loses it. Two actions on one key is a
    // control scheme nobody set out to make, and the player would discover it
    // mid-corner.
    if (key != GS_KEY_NONE) {
        for (int other = 0; other < GS_ACT_COUNT; other++) {
            if (other != (int)a && b->key[player][other] == key) {
                b->key[player][other] = GS_KEY_NONE;
            }
        }
    }
    b->key[player][a] = key;
}

void gs_bind_set_button(gs_bindings *b, uint8_t player, gs_action a, int16_t button) {
    if (player >= GS_MAX_CARS || a >= GS_ACT_COUNT) return;

    if (button != GS_BUTTON_NONE) {
        for (int other = 0; other < GS_ACT_COUNT; other++) {
            if (other != (int)a && b->button[player][other] == button) {
                b->button[player][other] = GS_BUTTON_NONE;
            }
        }
    }
    b->button[player][a] = button;
}

// --- the file --------------------------------------------------------------

static void gs_put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static uint32_t gs_get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#define GS_BIND_BYTES (4 + 4 + GS_MAX_CARS * GS_ACT_COUNT * 6)

size_t gs_bind_size(void) { return GS_BIND_BYTES; }

size_t gs_bind_serialize(const gs_bindings *b, uint8_t *buf, size_t cap) {
    if (cap < GS_BIND_BYTES) return 0;

    uint8_t *p = buf;
    gs_put_u32(p, GS_BIND_MAGIC);   p += 4;
    gs_put_u32(p, GS_BIND_VERSION); p += 4;

    for (uint8_t pl = 0; pl < GS_MAX_CARS; pl++) {
        for (int a = 0; a < GS_ACT_COUNT; a++) {
            gs_put_u32(p, (uint32_t)b->key[pl][a]); p += 4;
            uint16_t btn = (uint16_t)b->button[pl][a];
            *p++ = (uint8_t)(btn & 0xffu);
            *p++ = (uint8_t)((btn >> 8) & 0xffu);
        }
    }
    return GS_BIND_BYTES;
}

bool gs_bind_deserialize(gs_bindings *b, const uint8_t *buf, size_t len) {
    if (len < GS_BIND_V1_BYTES) return false;

    const uint8_t *p = buf;
    if (gs_get_u32(p) != GS_BIND_MAGIC) return false;
    p += 4;
    const uint32_t version = gs_get_u32(p);
    if (version != GS_BIND_VERSION && version != 1u) return false;
    p += 4;
    const int acts = version == 1u ? GS_BIND_V1_ACTS : (int)GS_ACT_COUNT;
    if (len < (size_t)(8 + GS_MAX_CARS * acts * 6)) return false;

    // Checked before anything is written, so a refused file leaves the player
    // with the controls they had rather than with half of somebody else's.
    //
    // Zeroed rather than left to the loops below, which do fill every field:
    // MSVC cannot see that they do and refuses to build it, and a struct that
    // gains a field later would silently start carrying stack rubbish into a
    // player's controls. Both reasons point the same way.
    // Started from the defaults rather than from zero, so an action a file
    // is too old to know about arrives set to something rather than to
    // nothing - a version-one player gets the tow on its default key.
    gs_bindings loaded;
    gs_bind_defaults(&loaded);
    for (uint8_t pl = 0; pl < GS_MAX_CARS; pl++) {
        for (int a = 0; a < acts; a++) {
            loaded.key[pl][a] = (SDL_Scancode)gs_get_u32(p);
            p += 4;
            uint16_t btn = (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
            loaded.button[pl][a] = (int16_t)btn;
            p += 2;
        }
    }
    *b = loaded;
    return true;
}

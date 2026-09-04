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

    // **The blip shield: a focus loss shorter than a second never releases
    // the keys a driver is holding.**
    //
    // SDL clears its whole keyboard state the moment the window loses focus,
    // and under WSLg the state it reads back on regaining it has been seen
    // empty - so a compositor blip mid-race left a physically held
    // accelerator dead until it was released and pressed again, reported as
    // "the acceleration does not increase until I lift the key". These keys
    // are tracked from the events themselves and survive a blip; a loss
    // longer than GS_INPUT_BLIP_MS is somebody actually leaving, and
    // everything is dropped, because a key held on the way out may have been
    // released anywhere at all while the game could not hear it.
    bool         held[SDL_SCANCODE_COUNT];
    uint64_t     lost_at;          // SDL_GetTicks() at focus loss, 0 focused
    bool         shielding;        // a blip survived; merge held into polls

    // **The debounce: a release only counts after the key has stayed up for
    // GS_INPUT_GRACE_MS.** Under WSLg a *held* key arrives as a thirty-hertz
    // stream of press/release pairs - the down phase lasting nothing, the up
    // phase a frame - so the poll nearly always found the accelerator "up"
    // and a held throttle read dead until re-pressed. Real events, repeat
    // flag clear, straight off the wire: no game state was ever wrong, the
    // key honestly flapped. Bridging releases shorter than the grace turns
    // the flapping back into the hold the finger is actually performing,
    // while a deliberate human tap - sixty milliseconds and up between
    // presses - still releases.
    uint64_t     up_at[SDL_SCANCODE_COUNT];   // ms of the last release, 0 none

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

#define GS_INPUT_BLIP_MS 1000u
#define GS_INPUT_GRACE_MS 50u

// What the shield believes about one key, for the suite: true while the key
// is held and shielded through a blip. Tests feed gs_input_event synthetic
// events and ask this, so the shield's rules are pinned without a keyboard.
bool gs_input_shielded(const gs_input_state *s, SDL_Scancode key);

// Whether the debounce is bridging this key's release at `now_ms`: true when
// the key's last release is younger than the grace and no newer press has
// claimed it. Takes the clock as an argument so the suite can walk the rule
// without waiting for one.
bool gs_input_graced(const gs_input_state *s, SDL_Scancode key,
                     uint64_t now_ms);

// Read and write the player's bindings. Failure to load is not an error - it
// means they have never changed anything - and leaves the defaults in place.
bool gs_input_load_bindings(gs_input_state *s);
bool gs_input_save_bindings(const gs_input_state *s);
void gs_input_quit(gs_input_state *s);

// Reacts to pads arriving and leaving, and keeps the blip shield fed with
// key and focus events. Safe to call with any event; the game calls it with
// every one.
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

// **And only a key can ask to quit.** Backing out of the title screen means
// leaving the game, which the title screen says in as many words - "Escape
// quit". A pad's cancel is the button everybody presses to go back one step,
// reflexively, and having it close the game from the title is not something
// anybody asked for. Where there is nothing behind the screen, a pad's cancel
// does nothing at all.
bool gs_input_back_may_quit(const SDL_Event *e);

#endif // GS_INPUT_H

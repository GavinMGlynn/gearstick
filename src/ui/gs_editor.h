// gs_editor.h - the construction set.
//
// **The editor is not a mode bolted onto the game; the game is what the editor
// is for.** It runs inside the same executable and over the same track the race
// uses, because the thing that mattered most about the original was the loop
// between having an idea and driving it, and a separate editor binary puts a
// reload in the middle of that loop.
//
// The interface here is thin on purpose. All the *state* — what a track is,
// what an edit is, how to take one back — lives in `src/core/` where it links
// nothing and can be tested without a window. What is left over is this: where
// the pointer is, which brush is selected, and drawing the panel.
#ifndef GS_EDITOR_H
#define GS_EDITOR_H

#include <SDL3/SDL.h>

#include "core/gs_edit.h"
#include "core/gs_track.h"
#include "gfx/gs_render.h"
#include "platform/gs_input.h"

typedef enum gs_brush {
    GS_BRUSH_RAISE = 0,
    GS_BRUSH_LOWER,
    GS_BRUSH_SURFACE,
    GS_BRUSH_GRAVITY,
    GS_BRUSH_GATE,
    GS_BRUSH_COUNT
} gs_brush;

typedef struct gs_editor {
    bool active;

    int   brush;        // gs_brush, an int because that is what ImGui edits
    int   surface;      // gs_surface
    float gravity;      // multiplier to paint, 1.0 being normal
    int   radius;       // in tiles, 0 being a single tile or corner
    float step;         // how far one application raises or lowers, in tiles

    float gate_heading; // degrees, the way a car drives through a placed gate
    float gate_width;   // half width, in tiles

    // The editor's own camera. It does not follow a car, because the thing you
    // are looking at while building is the part of the track you are building.
    float cam_x, cam_y, zoom;

    float hover_x, hover_y;
    bool  hover_on;
    bool  stroke;       // inside a click-drag, which is one undo step
    float last_mouse_x, last_mouse_y;

    bool  placed;       // the editor camera has been positioned at least once

    // The dials, live. Everything is a dial and nothing is locked - so they sit
    // in the editor beside the brushes rather than behind a race-setup screen,
    // and the ghost re-races under them the moment one moves.
    float dial_gravity;    // multiple of Earth
    float dial_drag;
    float dial_friction;
    float dial_damage;

    // The live ghost: a car re-racing the design in the background, so that
    // changing a ramp shows you what it does to a landing a second later
    // instead of when you next go and drive it. Editing stops being blind
    // construction and becomes a feedback loop.
    bool     ghost_on;
    gs_world ghost;
    uint64_t ghost_track;   // the track hash it is racing; a change restarts it
    uint32_t ghost_ticks;

    // Rebinding: which control is waiting to be told what it should be. -1
    // when nothing is.
    int rebind_player;
    int rebind_action;
    bool show_controls;

    gs_edit_log *log;
    char status[192];
} gs_editor;

bool gs_editor_init(gs_editor *e, uint32_t history);
void gs_editor_quit(gs_editor *e);

// Enter or leave, taking the camera from wherever the race view was so the
// world does not jump underneath the player.
void gs_editor_toggle(gs_editor *e, const gs_view *view);

// Apply the brush and draw the palette. Call once a frame while active, after
// ImGui's new frame and before its render.
void gs_editor_frame(gs_editor *e, gs_track *t, const gs_view *view,
                     gs_input_state *input);

// Where a test drive should begin, and facing which way.
//
// The cursor if it is over the track, because "drive from here" is the whole
// point of a test drive - you are asking about the corner you are looking at,
// not about the track from the beginning. The start gate otherwise, which is
// what you want when the pointer is off in the margins.
//
// Returns false if there is nowhere sensible at all: no cursor and no route.
bool gs_editor_drive_start(const gs_editor *e, const gs_track *t,
                           gs_fix *x, gs_fix *y, gs_angle *heading);

// One frame of pad input. **Everything the mouse can do, a pad can do**, which
// is not a courtesy: this game is two people on a sofa, and an editor only one
// of them can drive is half a construction set.
//
// Separate from gs_editor_frame so it can be driven by a test with no window
// and no pad plugged in. Returns true if the player asked for a test drive.
bool gs_editor_pad_input(gs_editor *e, gs_track *t, const gs_pad_edit *pad, float dt);

// Apply the current brush once, at a world position. The pointer path calls
// this; so can a test or a script, which is the point of it being here rather
// than buried in the mouse handling. Does not open a transaction - the caller
// decides what counts as one action.
void gs_editor_paint(gs_editor *e, gs_track *t, float wx, float wy);

// Set a world up under the editor's current dials. Both the ghost and a test
// drive go through here, so what you tuned is what you drive.
void gs_editor_apply_dials(const gs_editor *e, gs_world *w);

// Advance the ghost. **It notices for itself that the track changed** - by its
// hash - and starts again, which is what makes it live rather than a thing you
// have to remember to re-run. Cheap because the simulation links nothing and
// runs headless; this is the same code the game races.
void gs_editor_ghost_step(gs_editor *e, const gs_track *t, uint32_t ticks);

// The ghost's car, or null when it has nowhere to run.
const gs_car *gs_editor_ghost_car(const gs_editor *e);

// The cursor, drawn over the world in the view's own projection.
void gs_editor_draw_cursor(const gs_editor *e, SDL_Renderer *ren,
                           const gs_track *t, const gs_view *view);

// Where the editor puts a track. One file for now; the editor has no browser
// yet and a fixed path is better than a dialog that does not exist.
bool gs_editor_save(gs_editor *e, const gs_track *t);
bool gs_editor_load(gs_editor *e, gs_track *t);

#endif // GS_EDITOR_H

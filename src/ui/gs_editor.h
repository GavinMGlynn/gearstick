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

typedef enum gs_brush {
    GS_BRUSH_RAISE = 0,
    GS_BRUSH_LOWER,
    GS_BRUSH_SURFACE,
    GS_BRUSH_GRAVITY,
    GS_BRUSH_COUNT
} gs_brush;

typedef struct gs_editor {
    bool active;

    int   brush;        // gs_brush, an int because that is what ImGui edits
    int   surface;      // gs_surface
    float gravity;      // multiplier to paint, 1.0 being normal
    int   radius;       // in tiles, 0 being a single tile or corner
    float step;         // how far one application raises or lowers, in tiles

    // The editor's own camera. It does not follow a car, because the thing you
    // are looking at while building is the part of the track you are building.
    float cam_x, cam_y, zoom;

    float hover_x, hover_y;
    bool  hover_on;
    bool  stroke;       // inside a click-drag, which is one undo step

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
void gs_editor_frame(gs_editor *e, gs_track *t, const gs_view *view);

// Apply the current brush once, at a world position. The pointer path calls
// this; so can a test or a script, which is the point of it being here rather
// than buried in the mouse handling. Does not open a transaction - the caller
// decides what counts as one action.
void gs_editor_paint(gs_editor *e, gs_track *t, float wx, float wy);

// The cursor, drawn over the world in the view's own projection.
void gs_editor_draw_cursor(const gs_editor *e, SDL_Renderer *ren,
                           const gs_track *t, const gs_view *view);

// Where the editor puts a track. One file for now; the editor has no browser
// yet and a fixed path is better than a dialog that does not exist.
bool gs_editor_save(gs_editor *e, const gs_track *t);
bool gs_editor_load(gs_editor *e, gs_track *t);

#endif // GS_EDITOR_H

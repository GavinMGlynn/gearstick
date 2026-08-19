// gs_render.h - drawing a race.
//
// **Terrain is emitted as shaded geometry, never assembled from a tile atlas.**
// Four corner heights per tile means arbitrary elevation joins, and no
// hand-authored tile set can stitch those cleanly - there is always an edge or
// a hole baked into the wrong pick. Two triangles tinted by surface and by
// slope are correct by construction, and they make the two things that will
// arrive later - painted gravity and progressive surface wear - another input
// to the tint rather than a new art requirement.
#ifndef GS_RENDER_H
#define GS_RENDER_H

#include <SDL3/SDL.h>

#include "core/gs_sim.h"
#include "core/gs_track.h"
#include "core/gs_analyse.h"
#include "gfx/gs_iso.h"

typedef struct gs_view {
    gs_camera cam;
    SDL_Rect  rect;      // where on the window this view lives
    uint8_t   car;       // whose view it is
    bool      show_gravity;

    // The analyser's heatmap, or null for none. Borrowed, not owned: the view
    // paints whatever the editor last worked out and never runs the sweep
    // itself, because a sweep is thirty seconds of simulation and a frame is
    // eight milliseconds.
    const gs_analysis *heat;
} gs_view;

// Draw one view of the world. `alpha` in [0,1] interpolates between the
// previous simulation state and the current one, so motion is smooth at frame
// rates that have nothing to do with 120 Hz.
void gs_render_view(SDL_Renderer *ren, const gs_track *t, const gs_world *prev,
                    const gs_world *now, float alpha, const gs_view *view);

// The split screen, and its willingness to stop being one.
//
// **Cars that are close together share a view.** Two panes showing almost the
// same thing is a waste of a screen and, worse, it is hard to read: a collision
// is legible when both cars are in one picture, and legibility is the whole
// argument for this camera.
//
// The difficulty is the transition. A hard switch from one pane to two is a
// jump, and a jump in a racing game is the thing players notice above all else.
// So the panes' cameras converge on the shared one *before* the divider goes,
// and diverge after it appears - at the moment of the switch every pane is
// already showing what the single view showed, and only the divider changes.
typedef struct gs_split {
    float     merge;     // 1 sharing one view, 0 fully split, moving between
    gs_camera shared;    // where that one view looks
} gs_split;

void gs_split_init(gs_split *s);

// Advance the merge state by `dt` seconds. Hysteresis is deliberate: cars
// hovering at the threshold must not flicker the screen in half.
void gs_split_update(gs_split *s, const gs_world *w, int win_w, int win_h, float dt);

// Fill in the views to draw. Returns how many - one while merged, one per car
// otherwise. Cameras are blended by the merge factor, which is what makes the
// switch continuous rather than sudden.
uint8_t gs_split_views(const gs_split *s, const gs_world *w,
                       int win_w, int win_h, gs_view *out);

// Where each view goes, for one to four of them, in a window `w` by `h`.
//
// One fills the window. Two split it left and right, because a racing track is
// wider than it is tall and two tall thin views waste the shape of it. Three and
// four both take a two-by-two grid: three players leave a cell empty rather than
// get a different layout each time somebody joins, which is worth more than the
// tidiness of using every pixel.
//
// Returns how many rectangles it wrote.
uint8_t gs_render_layout(uint8_t views, int w, int h, SDL_Rect *out);

// What the last stretch of drawing actually submitted.
//
// Here so that "four quarter-sized views cost about one window" can be checked
// as a fact rather than as a stopwatch reading. The first version of that test
// timed the two and failed one run in ten on a busy machine - which is worse
// than no test, because a green tick that means "the machine was quiet" is not
// evidence of anything.
typedef struct gs_render_stats {
    uint32_t tiles;   // tiles whose geometry was built and submitted
    uint32_t cars;
} gs_render_stats;

void gs_render_reset_stats(void);
gs_render_stats gs_render_stats_now(void);

// Draw a single car ghosted - translucent, and without the nose flash, so it
// reads as a prediction rather than as a competitor.
void gs_render_ghost(SDL_Renderer *ren, const gs_track *t, const gs_car *c,
                     const gs_view *view);

// Put the camera where it should be for the car it follows.
void gs_render_track_camera(gs_view *view, const gs_world *prev,
                            const gs_world *now, float alpha);

#endif // GS_RENDER_H

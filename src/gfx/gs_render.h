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
#include "gfx/gs_iso.h"

typedef struct gs_view {
    gs_camera cam;
    SDL_Rect  rect;      // where on the window this view lives
    uint8_t   car;       // whose view it is
    bool      show_gravity;
} gs_view;

// Draw one view of the world. `alpha` in [0,1] interpolates between the
// previous simulation state and the current one, so motion is smooth at frame
// rates that have nothing to do with 120 Hz.
void gs_render_view(SDL_Renderer *ren, const gs_track *t, const gs_world *prev,
                    const gs_world *now, float alpha, const gs_view *view);

// Put the camera where it should be for the car it follows.
void gs_render_track_camera(gs_view *view, const gs_world *prev,
                            const gs_world *now, float alpha);

#endif // GS_RENDER_H

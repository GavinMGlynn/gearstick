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

// Draw a single car ghosted - translucent, and without the nose flash, so it
// reads as a prediction rather than as a competitor.
void gs_render_ghost(SDL_Renderer *ren, const gs_track *t, const gs_car *c,
                     const gs_view *view);

// Put the camera where it should be for the car it follows.
void gs_render_track_camera(gs_view *view, const gs_world *prev,
                            const gs_world *now, float alpha);

#endif // GS_RENDER_H

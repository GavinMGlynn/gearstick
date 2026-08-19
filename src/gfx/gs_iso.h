// gs_iso.h - where a point in the world lands on the screen.
//
// The projection is 2:1 isometric, which is the angle the original used and the
// angle that makes a two-car collision legible at a glance. It is the reason
// there is no chase camera and no free camera: those make the *other* car a
// surprise, and being able to read both at once is the whole point.
//
// This layer, and only this layer, thinks in pixels. Everything in src/core/ is
// in tiles.
#ifndef GS_ISO_H
#define GS_ISO_H

#include "core/gs_fixed.h"

// A tile is 64 px across and 32 px deep on screen - the 2:1 diamond. One tile
// of *height* is 32 px, so a one-tile ramp rises exactly one tile-depth, which
// makes elevation read at the same scale as distance.
#define GS_ISO_TILE_W 64.0f
#define GS_ISO_TILE_H 32.0f
#define GS_ISO_TILE_Z 32.0f

// How far back the camera sits by default. One tile to 64 pixels at 1.0, so a
// split-screen pane shows ten tiles - forty metres - of track.
//
// Arrived at from the other end than expected. The cars first read as specks,
// and the reflex was to move the camera in; that was wrong, because zoom scales
// the car and the ground together and leaves the car exactly as many tiles long
// as it was. What was wrong was the *ratio*: an honest 2.7 m car is two thirds
// of a tile, and the original's were never honest - they were chunky relative
// to the view, because a two-car collision has to be legible at a glance.
//
// So the cars grew to about 1.3 tiles (see gs_render.c) and the camera went
// back out, which is more track in view *and* a car you can see. Moving the
// camera alone would have bought the second at the cost of the first.
#define GS_ISO_DEFAULT_ZOOM 1.0f

typedef struct gs_camera {
    float cx, cy, cz;   // the world point held at the centre of the viewport
    float zoom;
    float vw, vh;       // viewport size, pixels
} gs_camera;

// World tiles to viewport pixels.
void gs_iso_project(const gs_camera *cam, float wx, float wy, float wz,
                    float *sx, float *sy);

// Depth key for the painter's algorithm: bigger is nearer the viewer.
static inline float gs_iso_depth(float wx, float wy) { return wx + wy; }

// Convenience for the fixed-point side of the fence.
static inline float gs_to_f(gs_fix v) { return (float)v / (float)GS_ONE; }

// The inverse: which point on the ground is under this pixel.
//
// It is not a straight inversion, because the projection throws a dimension
// away — a screen position maps to a *line* through the world, and which point
// on that line you meant depends on how high the ground is there, which is what
// you are trying to find out. So it iterates: assume the ground is at the
// camera's height, solve, sample the terrain where that lands, solve again.
// Four passes is plenty at any slope a car can drive on.
//
// Returns false if the answer is off the authored track, having still written
// the position — the editor wants to know where the pointer is even when it is
// out of bounds.
struct gs_track;
bool gs_iso_pick(const gs_camera *cam, const struct gs_track *t,
                 float sx, float sy, float *wx, float *wy);

#endif // GS_ISO_H

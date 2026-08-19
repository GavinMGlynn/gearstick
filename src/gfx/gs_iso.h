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

#endif // GS_ISO_H

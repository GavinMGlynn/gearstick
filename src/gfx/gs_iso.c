// gs_iso.c - see gs_iso.h.

#include "gfx/gs_iso.h"

void gs_iso_project(const gs_camera *cam, float wx, float wy, float wz,
                    float *sx, float *sy) {
    float dx = wx - cam->cx;
    float dy = wy - cam->cy;
    float dz = wz - cam->cz;

    // The diamond. Height comes straight off the vertical, which is what makes
    // the gap between a car and its shadow read as altitude rather than as
    // distance up the screen.
    *sx = cam->vw * 0.5f + (dx - dy) * (GS_ISO_TILE_W * 0.5f) * cam->zoom;
    *sy = cam->vh * 0.5f + ((dx + dy) * (GS_ISO_TILE_H * 0.5f) - dz * GS_ISO_TILE_Z) * cam->zoom;
}

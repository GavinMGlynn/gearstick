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

#include "core/gs_track.h"

bool gs_iso_pick(const gs_camera *cam, const struct gs_track *t,
                 float sx, float sy, float *wx, float *wy) {
    // From the projection in gs_iso_project:
    //     sx = vw/2 + (dx - dy) * (TILE_W/2) * zoom
    //     sy = vh/2 + ((dx + dy) * (TILE_H/2) - dz * TILE_Z) * zoom
    // so with dz known, (dx - dy) and (dx + dy) both fall out and the pair
    // separates.
    float u = (sx - cam->vw * 0.5f) / ((GS_ISO_TILE_W * 0.5f) * cam->zoom);

    float x = cam->cx, y = cam->cy;
    float dz = 0.0f;

    // Iterate to a standstill rather than a fixed count. Each pass contracts
    // the error by roughly the slope under the pointer, so gentle ground
    // converges in three or four and a steep ramp wants a dozen. The cap is
    // there because the editor can build ground steeper than the projection
    // ray, where the iteration oscillates instead of converging — a wall, seen
    // edge on. Stopping early on a wall gives a slightly wrong tile; not
    // stopping gives a hang.
    for (int pass = 0; pass < 24; pass++) {
        float v = ((sy - cam->vh * 0.5f) / cam->zoom + dz * GS_ISO_TILE_Z) /
                  (GS_ISO_TILE_H * 0.5f);

        x = cam->cx + (u + v) * 0.5f;
        y = cam->cy + (v - u) * 0.5f;

        // Clamped before it becomes fixed point: a pointer dragged far off the
        // window produces coordinates that would overflow Q16.16, and a wrapped
        // coordinate would sample the terrain somewhere absurd.
        float qx = GS_CLAMP(x, -4096.0f, 4096.0f);
        float qy = GS_CLAMP(y, -4096.0f, 4096.0f);
        gs_fix h = gs_track_height(t, (gs_fix)(qx * (float)GS_ONE),
                                   (gs_fix)(qy * (float)GS_ONE));

        float next = gs_to_f(h) - cam->cz;
        float moved = next - dz;
        dz = next;

        // A four-thousandth of a tile is far below what a cursor can mean.
        if (moved > -0.00025f && moved < 0.00025f) break;
    }

    *wx = x;
    *wy = y;
    return x >= 0.0f && y >= 0.0f && x < (float)t->w && y < (float)t->h;
}

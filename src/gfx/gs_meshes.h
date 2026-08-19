// gs_meshes.h - the vehicles, as geometry.
//
// **Generated, not drawn.** Every mesh here comes out of tools/make_meshes.py,
// which builds each vehicle from a handful of boxes described in tiles. Nothing
// is modelled by hand and nothing is downloaded, so there is no third-party art
// in the game and no licence condition to satisfy - see assets/ATTRIBUTION.md,
// which that same script writes in the same run.
//
// Colours are not baked in. A triangle carries a *role*, and the renderer
// decides what a role looks like: that is what lets four players share one mesh
// and still be told apart by their paint, and what lets a wreck darken without
// a second set of geometry.
#ifndef GS_MESHES_H
#define GS_MESHES_H

#include <stdint.h>

typedef enum gs_paint {
    GS_PAINT_BODY = 0,   // the player's colour
    GS_PAINT_TRIM,       // seats, cockpit surrounds, bumpers
    GS_PAINT_GLASS,
    GS_PAINT_TYRE,
    GS_PAINT_METAL,      // roll cages, wings, exposed engines
    GS_PAINT_LIGHT,
    GS_PAINT_COUNT
} gs_paint;

// Model space in tiles: +x forward, +y left, +z up, origin on the ground
// between the wheels.
typedef struct gs_mesh_vertex { float x, y, z; } gs_mesh_vertex;

typedef struct gs_mesh_tri {
    uint16_t a, b, c;
    uint8_t  paint;      // gs_paint
} gs_mesh_tri;

typedef struct gs_mesh {
    const char           *name;
    const gs_mesh_vertex *vertex;
    uint16_t              vertex_count;
    const gs_mesh_tri    *tri;
    uint16_t              tri_count;
} gs_mesh;

// The mesh for a gs_vehicle_id. Out of range gives the first one rather than
// null: a car with no geometry is invisible, which is worse than a car that
// looks like the wrong thing.
const gs_mesh *gs_mesh_for(uint8_t vehicle);

#endif // GS_MESHES_H

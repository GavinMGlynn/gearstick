// gs_track.h - the ground: what shape it is, what it is made of, and how hard
// it pulls.
//
// **Everything downstream keys off this representation.** A track is a grid of
// tiles, but the shape is stored on the tile *corners* rather than the tiles,
// which is the decision the rest of the game rests on: ramps stitch
// continuously into their neighbours by construction, ground height at any
// point is a bilinear sample rather than a lookup, and a surface gradient falls
// out of the same four numbers for free. Nothing anywhere may assume a tile is
// flat.
//
// Three fields sit on the tiles themselves - surface, gravity and (at run time,
// in the world rather than here) wear.
//
// **Gravity is a field, not a setting.** The original's fourteen gravity steps
// were a pre-race dropdown; here every tile carries its own multiplier, so a
// low-gravity pocket at the top of a jump or a Jupiter zone that pins a car
// through a banked turn is something you paint. The physics asks the track what
// gravity is *here*, every tick. Anything that reads gravity once per race has
// broken the feature.
#ifndef GS_TRACK_H
#define GS_TRACK_H

#include "core/gs_fixed.h"

// A track is at most 64 tiles square. That is four times the original's usable
// area and keeps the whole thing - shape, surface and gravity - inside 22 KB,
// small enough to hash, copy and send without anyone thinking about it.
#define GS_TRACK_MAX     64
#define GS_TRACK_TILES   (GS_TRACK_MAX * GS_TRACK_MAX)
#define GS_TRACK_CORNERS ((GS_TRACK_MAX + 1) * (GS_TRACK_MAX + 1))

// One world unit is one tile, and one tile is four metres. Pixels do not exist
// in this layer.
#define GS_TILE_METRES 4

typedef enum gs_surface {
    GS_SURF_PAVEMENT = 0,
    GS_SURF_DIRT,
    GS_SURF_ICE,
    GS_SURF_COUNT
} gs_surface;

typedef struct gs_surface_def {
    const char *name;
    gs_fix      grip;      // how much sideways force it will take before sliding
    gs_fix      rolling;   // rolling resistance, as a fraction of gravity
    gs_fix      drive;     // how much of the engine reaches the ground
} gs_surface_def;

extern const gs_surface_def gs_surfaces[GS_SURF_COUNT];

// Per-tile gravity, as a multiplier on the race setting. 64 is 1x, so the byte
// spans nothing at all up to just under 4x - Moon to well past Jupiter.
#define GS_GRAVITY_UNIT 64

typedef struct gs_track {
    uint8_t w, h;                        // in tiles; both in [1, GS_TRACK_MAX]

    // Corner heights in 1/256 of a tile, on a (w+1) x (h+1) lattice indexed
    // [y * (GS_TRACK_MAX + 1) + x]. The stride is the maximum rather than the
    // track's own width so that resizing a track in the editor never has to
    // move the data.
    int16_t corner[GS_TRACK_CORNERS];

    uint8_t surface[GS_TRACK_TILES];     // gs_surface, indexed [y * GS_TRACK_MAX + x]
    uint8_t gravity[GS_TRACK_TILES];     // multiples of 1/GS_GRAVITY_UNIT
} gs_track;

#define GS_CORNER_STRIDE (GS_TRACK_MAX + 1)
#define GS_TILE_INDEX(x, y) ((size_t)(y) * GS_TRACK_MAX + (size_t)(x))

// Heights are stored in 1/256 tile and used in Q16.16.
#define GS_HEIGHT_SHIFT 8

void gs_track_init(gs_track *t, uint8_t w, uint8_t h, gs_surface surface);

// Whether a point is over the authored track at all. Everything outside it is
// still drivable - the ground continues at the nearest edge's height and
// surface - so this answers "is this on the track", not "is this solid".
bool gs_track_contains(const gs_track *t, gs_fix x, gs_fix y);

// Ground height under a point, bilinear across the containing tile.
gs_fix gs_track_height(const gs_track *t, gs_fix x, gs_fix y);

// The gradient of the ground at a point: how much height changes per tile along
// each axis. The physics wants this directly; the renderer turns it into a
// normal. Either output may be null.
void gs_track_slope(const gs_track *t, gs_fix x, gs_fix y, gs_fix *dzdx, gs_fix *dzdy);

gs_surface gs_track_surface(const gs_track *t, gs_fix x, gs_fix y);

// The gravity multiplier at a point, in Q16.16 where GS_ONE is 1x. Sampled per
// tick, never cached - see the header comment.
gs_fix gs_track_gravity(const gs_track *t, gs_fix x, gs_fix y);

void gs_track_set_corner(gs_track *t, uint8_t x, uint8_t y, gs_fix height);
void gs_track_set_surface(gs_track *t, uint8_t x, uint8_t y, gs_surface s);
void gs_track_set_gravity(gs_track *t, uint8_t x, uint8_t y, gs_fix multiplier);

// A track's identity is its content. Two players who built the same track
// independently have the same track, without a server deciding so, and a
// one-tile edit cleanly produces a different one - which is what lets ghosts
// and times aggregate by themselves. Only the used region is hashed, so a 16x16
// track's identity does not depend on GS_TRACK_MAX.
uint64_t gs_track_hash(const gs_track *t);

#endif // GS_TRACK_H

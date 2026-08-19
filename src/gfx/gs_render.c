// gs_render.c - see gs_render.h for why the ground is geometry and not art.

#include "gfx/gs_render.h"

#include "gfx/gs_meshes.h"
#include "core/gs_profile.h"

typedef struct gs_rgb { float r, g, b; } gs_rgb;

// Counting what was drawn, so the cost of a split screen can be asserted rather
// than timed. Not thread safe and does not need to be: one renderer, one thread.
static gs_render_stats gs_stats;

void gs_render_reset_stats(void) { gs_stats = (gs_render_stats){ 0 }; }
gs_render_stats gs_render_stats_now(void) { return gs_stats; }

// Far apart on purpose: a player has to be able to tell what they are about to
// drive onto from a glance at a 64-pixel diamond.
static const gs_rgb gs_surface_colour[GS_SURF_COUNT] = {
    [GS_SURF_PAVEMENT] = { 0.42f, 0.44f, 0.47f },
    [GS_SURF_DIRT]     = { 0.55f, 0.40f, 0.24f },
    [GS_SURF_ICE]      = { 0.68f, 0.82f, 0.92f },
};

// Fixed light, up and to the left. Nothing here casts a real shadow except the
// cars; this is only here to make the shape of the ground readable.
static float gs_light(gs_fix dzdx, gs_fix dzdy) {
    float nx = -gs_to_f(dzdx);
    float ny = -gs_to_f(dzdy);
    float nz = 1.0f;
    float len = SDL_sqrtf(nx * nx + ny * ny + nz * nz);

    static const float lx = -0.42f, ly = -0.57f, lz = 0.71f;
    float d = (nx * lx + ny * ly + nz * lz) / len;

    // Never fully dark: an unlit face still has to show its surface colour.
    return SDL_clamp(0.55f + d * 0.45f, 0.40f, 1.15f);
}

static SDL_FColor gs_tile_colour(const gs_track *t, uint8_t tx, uint8_t ty,
                                 bool show_gravity, const gs_analysis *heat) {
    gs_fix cx = GS_INT(tx) + GS_HALF;
    gs_fix cy = GS_INT(ty) + GS_HALF;

    gs_surface s = gs_track_surface(t, cx, cy);
    gs_rgb base = gs_surface_colour[s];

    gs_fix dzdx, dzdy;
    gs_track_slope(t, cx, cy, &dzdx, &dzdy);
    float shade = gs_light(dzdx, dzdy);

    float r = base.r * shade, g = base.g * shade, b = base.b * shade;

    if (heat != nullptr) {
        // Where everybody actually went, over every gravity and every machine
        // the analyser tried. The line a track *has* is rarely the line its
        // author drew, and this is the only way to be shown the difference.
        // Cold ground is left as it is; the used line runs up through green
        // into a hot white centre.
        float k = gs_to_f(gs_analysis_heat(heat, tx, ty));
        if (k > 0.02f) {
            float rr = k < 0.5f ? k * 0.6f : 0.3f + (k - 0.5f) * 1.4f;
            float gg = k < 0.5f ? 0.35f + k * 0.9f : 0.8f + (k - 0.5f) * 0.4f;
            float bb = k < 0.5f ? k * 0.3f : 0.15f + (k - 0.5f) * 1.7f;
            float m = SDL_clamp(0.25f + k * 0.65f, 0.0f, 0.9f);
            r += (rr - r) * m; g += (gg - g) * m; b += (bb - b) * m;
        }
    }

    if (show_gravity) {
        // Painted gravity has to be visible while you paint it, or the brush is
        // a dial you cannot see. Violet where the ground pulls less than
        // normal, amber where it pulls more; untouched tiles are left alone.
        float mul = gs_to_f(gs_track_gravity(t, cx, cy));
        float d = mul - 1.0f;
        if (d < -0.02f) {
            float k = SDL_clamp(-d, 0.0f, 1.0f) * 0.55f;
            r += (0.55f - r) * k; b += (0.95f - b) * k;
        } else if (d > 0.02f) {
            float k = SDL_clamp(d * 0.5f, 0.0f, 1.0f) * 0.55f;
            r += (1.00f - r) * k; g += (0.55f - g) * k;
        }
    }

    return (SDL_FColor){ SDL_clamp(r, 0.0f, 1.0f), SDL_clamp(g, 0.0f, 1.0f),
                         SDL_clamp(b, 0.0f, 1.0f), 1.0f };
}

// A quad as two triangles, in the order SDL_RenderGeometry wants them.
static void gs_quad(SDL_Renderer *ren, const SDL_FPoint p[4], SDL_FColor c) {
    SDL_Vertex v[4];
    for (int i = 0; i < 4; i++) {
        v[i].position = p[i];
        v[i].color = c;
        v[i].tex_coord = (SDL_FPoint){ 0.0f, 0.0f };
    }
    static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
    SDL_RenderGeometry(ren, nullptr, v, 4, idx, 6);
}

// Is this tile anywhere near the viewport?
//
// Clipping happens in the rasteriser, which saves the pixels and not the work
// of getting there - four views each submitting the whole track is four times
// the geometry however small each one is drawn. That is what made four-player
// cost four times one-player rather than the one window's worth of pixels it
// actually paints.
//
// The margin is generous: a tile's corners can sit well outside its centre once
// the ground is steep, and dropping a tile that should have been drawn leaves a
// hole in the world. Cheap and slightly wrong in the safe direction.
static bool gs_tile_in_view(const gs_camera *cam, const gs_track *t,
                            uint8_t tx, uint8_t ty) {
    gs_fix cx = GS_INT(tx) + GS_HALF;
    gs_fix cy = GS_INT(ty) + GS_HALF;
    gs_fix cz = gs_track_height(t, cx, cy);

    float sx = 0, sy = 0;
    gs_iso_project(cam, gs_to_f(cx), gs_to_f(cy), gs_to_f(cz), &sx, &sy);

    float margin = (GS_ISO_TILE_W + GS_ISO_TILE_Z * 4.0f) * cam->zoom;
    return sx >= -margin && sy >= -margin &&
           sx <= cam->vw + margin && sy <= cam->vh + margin;
}

static void gs_draw_tile(SDL_Renderer *ren, const gs_camera *cam,
                         const gs_track *t, uint8_t tx, uint8_t ty,
                         bool show_gravity, const gs_analysis *heat) {
    // The four corners, each at its own height - which is exactly why this is
    // geometry and not a sprite.
    static const int ox[4] = { 0, 1, 1, 0 };
    static const int oy[4] = { 0, 0, 1, 1 };

    SDL_FPoint p[4];
    for (int i = 0; i < 4; i++) {
        // Sampling exactly on a corner lands on that corner: the bilinear
        // weights are 0 and 1, so no interpolation happens and adjacent tiles
        // necessarily agree about the vertex they share. That is what "stitches
        // by construction" means, and it is why there is no seam to hide.
        gs_fix wx = GS_INT(tx + ox[i]);
        gs_fix wy = GS_INT(ty + oy[i]);
        gs_fix wz = gs_track_height(t, wx, wy);
        gs_iso_project(cam, gs_to_f(wx), gs_to_f(wy), gs_to_f(wz),
                       &p[i].x, &p[i].y);
    }

    gs_stats.tiles++;
    gs_quad(ren, p, gs_tile_colour(t, tx, ty, show_gravity, heat));
}

// The car is a box: a footprint rotated to its heading, lifted by its ride
// height. Crude, and it reads correctly - which at this stage is the whole
// requirement. Pre-rendered sprites replace it in Phase 10.
// The paint. Eight colours rather than four, so four players can all be
// different and still argue about who gets red - and indexed by *choice* rather
// than by car number, because which car you are is the simulation's business
// and what colour it is is yours.
//
// Nothing here is in src/core/: a colour cannot change where a car ends up, so
// it is not part of the state two machines have to agree about, and a player
// can repaint without invalidating a single replay or record.
static const SDL_FColor gs_paint_palette[GS_COLOUR_COUNT] = {
    [GS_COLOUR_RED]    = { 0.86f, 0.16f, 0.14f, 1.0f },
    [GS_COLOUR_BLUE]   = { 0.16f, 0.44f, 0.88f, 1.0f },
    [GS_COLOUR_GREEN]  = { 0.20f, 0.72f, 0.28f, 1.0f },
    [GS_COLOUR_YELLOW] = { 0.94f, 0.80f, 0.12f, 1.0f },
    [GS_COLOUR_ORANGE] = { 0.94f, 0.47f, 0.10f, 1.0f },
    [GS_COLOUR_PURPLE] = { 0.60f, 0.28f, 0.80f, 1.0f },
    [GS_COLOUR_WHITE]  = { 0.90f, 0.90f, 0.92f, 1.0f },
    [GS_COLOUR_BLACK]  = { 0.16f, 0.16f, 0.18f, 1.0f },
};

// Which car wears which colour. Defaulted so that a race started without any
// profiles still has four cars nobody confuses.
static uint8_t gs_car_paint[GS_MAX_CARS] = {
    GS_COLOUR_RED, GS_COLOUR_BLUE, GS_COLOUR_YELLOW, GS_COLOUR_GREEN,
};

void gs_render_set_car_paint(uint8_t car, uint8_t colour) {
    if (car >= GS_MAX_CARS || colour >= GS_COLOUR_COUNT) return;
    gs_car_paint[car] = colour;
}

uint8_t gs_render_car_paint(uint8_t car) {
    return car < GS_MAX_CARS ? gs_car_paint[car] : 0;
}

SDL_FColor gs_render_paint_colour(uint8_t colour) {
    return colour < GS_COLOUR_COUNT ? gs_paint_palette[colour]
                                    : gs_paint_palette[GS_COLOUR_RED];
}


static void gs_car_footprint(const gs_car *c, float half_len, float half_wid,
                             float out_x[4], float out_y[4]) {
    float ch = gs_to_f(gs_cos(c->heading));
    float sh = gs_to_f(gs_sin(c->heading));
    static const float lx[4] = { 1.0f, 1.0f, -1.0f, -1.0f };
    static const float ly[4] = { -1.0f, 1.0f, 1.0f, -1.0f };

    for (int i = 0; i < 4; i++) {
        float ax = lx[i] * half_len;
        float ay = ly[i] * half_wid;
        out_x[i] = gs_to_f(c->x) + ax * ch - ay * sh;
        out_y[i] = gs_to_f(c->y) + ax * sh + ay * ch;
    }
}

// What each role looks like. Only the body takes the player's colour; the rest
// is shared, which is the whole reason a triangle carries a role rather than a
// colour - four players, six vehicles, one set of geometry.
static SDL_FColor gs_role_colour(uint8_t paint, SDL_FColor body) {
    switch (paint) {
    case GS_PAINT_TRIM:  return (SDL_FColor){ 0.22f, 0.23f, 0.27f, body.a };
    case GS_PAINT_GLASS: return (SDL_FColor){ 0.42f, 0.55f, 0.66f, body.a };
    case GS_PAINT_TYRE:  return (SDL_FColor){ 0.12f, 0.12f, 0.14f, body.a };
    case GS_PAINT_METAL: return (SDL_FColor){ 0.62f, 0.64f, 0.68f, body.a };
    case GS_PAINT_LIGHT: return (SDL_FColor){ 0.98f, 0.95f, 0.72f, body.a };
    default:             return body;
    }
}

// Depth along the view axis, for sorting a car's own triangles. Points that
// land on the same pixel differ by (1, 1, GS_ISO_TILE_H / GS_ISO_TILE_Z), so
// that direction is the axis and this is the distance along it.
static inline float gs_mesh_depth(float x, float y, float z) {
    return x + y + z * (GS_ISO_TILE_H / GS_ISO_TILE_Z);
}

#define GS_MESH_MAX_TRIS 256

typedef struct gs_sorted_tri {
    float     depth;
    SDL_FPoint p[3];
    SDL_FColor colour;
} gs_sorted_tri;

static int gs_tri_compare(const void *a, const void *b) {
    const gs_sorted_tri *x = (const gs_sorted_tri *)a;
    const gs_sorted_tri *y = (const gs_sorted_tri *)b;
    // Far first: the painter's algorithm, same as the terrain uses.
    if (x->depth < y->depth) return -1;
    if (x->depth > y->depth) return 1;
    return 0;
}

static void gs_tri(SDL_Renderer *ren, const SDL_FPoint p[3], SDL_FColor c) {
    SDL_Vertex v[3];
    for (int i = 0; i < 3; i++) {
        v[i].position = p[i];
        v[i].color = c;
        v[i].tex_coord = (SDL_FPoint){ 0.0f, 0.0f };
    }
    SDL_RenderGeometry(ren, nullptr, v, 3, nullptr, 0);
}

static void gs_draw_car(SDL_Renderer *ren, const gs_camera *cam,
                        const gs_track *t, const gs_car *c, uint8_t index,
                        float alpha) {
    if (!c->active) return;
    gs_stats.cars++;

    // **Deliberately not to scale.** A real car is 2.7 m and a tile is four, so
    // an honest one is two thirds of a tile and reads as a speck against the
    // ground it is driving on. The meshes are about 1.3 tiles long - roughly the
    // proportion the original used, and for the same reason: a two-car
    // collision has to be legible at a glance, and legibility is the entire
    // argument for this camera.
    //
    // Nothing in src/core/ knows about these numbers today. When collision
    // arrives it must use *these* rather than the metric truth, or a car is hit
    // by something the player cannot see.
    const float half_len = 0.65f, half_wid = 0.38f;

    float fx[4], fy[4];
    gs_car_footprint(c, half_len, half_wid, fx, fy);

    // --- The shadow, on the ground directly under the car. This is the single
    // most important thing in the frame: the gap between car and shadow is the
    // only cue that says how high it is, and it was the same on a C64.
    SDL_FPoint sp[4];
    for (int i = 0; i < 4; i++) {
        gs_fix gz = gs_track_height(t, (gs_fix)(fx[i] * GS_ONE), (gs_fix)(fy[i] * GS_ONE));
        gs_iso_project(cam, fx[i], fy[i], gs_to_f(gz) + 0.01f, &sp[i].x, &sp[i].y);
    }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    gs_quad(ren, sp, (SDL_FColor){ 0.0f, 0.0f, 0.0f, 0.35f * alpha });

    // --- The car itself, from the generated mesh.
    //
    // A basis rather than a heading alone: forward and left are tilted to
    // follow the ground the car is standing on, so a car on a ramp points up
    // the ramp instead of hovering flat above it. The simulation has no pitch
    // or roll to give - it does not need any - so this is read straight off the
    // terrain, which is where the answer was all along.
    float ch = gs_to_f(gs_cos(c->heading));
    float sh = gs_to_f(gs_sin(c->heading));

    float fz = 0.0f, lz = 0.0f;
    if (c->grounded) {
        gs_fix dzdx, dzdy;
        gs_track_slope(t, c->x, c->y, &dzdx, &dzdy);
        float gx = gs_to_f(dzdx), gy = gs_to_f(dzdy);
        fz = gx * ch + gy * sh;
        lz = -gx * sh + gy * ch;

        // A cliff would stand a car on its nose. Clamped to something a car
        // could plausibly be sitting on, which is also all the terrain now
        // allows it to climb - see GS_MAX_CLIMB in gs_sim.c.
        fz = SDL_clamp(fz, -1.2f, 1.2f);
        lz = SDL_clamp(lz, -1.2f, 1.2f);
    }

    // Forward, left, and up as their cross product, so the car is planted on
    // the ground rather than intersecting it.
    float f[3] = { ch, sh, fz };
    float l[3] = { -sh, ch, lz };
    float u[3] = { f[1] * l[2] - f[2] * l[1],
                   f[2] * l[0] - f[0] * l[2],
                   f[0] * l[1] - f[1] * l[0] };
    float ulen = SDL_sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (ulen > 0.0001f) { u[0] /= ulen; u[1] /= ulen; u[2] /= ulen; }

    float ox = gs_to_f(c->x), oy = gs_to_f(c->y), oz = gs_to_f(c->z);

    SDL_FColor body = gs_paint_palette[gs_car_paint[index & (GS_MAX_CARS - 1)]];
    if (c->wrecked) { body.r *= 0.35f; body.g *= 0.35f; body.b *= 0.35f; }
    body.a = alpha;

    const gs_mesh *m = gs_mesh_for(c->vehicle);

    static gs_sorted_tri sorted[GS_MESH_MAX_TRIS];
    int count = 0;

    for (uint16_t i = 0; i < m->tri_count && count < GS_MESH_MAX_TRIS; i++) {
        const gs_mesh_tri *tri = &m->tri[i];
        const uint16_t idx[3] = { tri->a, tri->b, tri->c };

        float wx[3], wy[3], wz[3];
        SDL_FPoint p[3];
        for (int k = 0; k < 3; k++) {
            const gs_mesh_vertex *v = &m->vertex[idx[k]];
            wx[k] = ox + v->x * f[0] + v->y * l[0] + v->z * u[0];
            wy[k] = oy + v->x * f[1] + v->y * l[1] + v->z * u[1];
            wz[k] = oz + v->x * f[2] + v->y * l[2] + v->z * u[2];
            gs_iso_project(cam, wx[k], wy[k], wz[k], &p[k].x, &p[k].y);
        }

        // Back-face culling in screen space, which needs no normals and cannot
        // disagree with what is actually drawn. Two thirds of a closed mesh
        // face away at any moment, and not drawing them is both faster and the
        // reason the far side of a roll cage does not show through the near one.
        float cross = (p[1].x - p[0].x) * (p[2].y - p[0].y) -
                      (p[2].x - p[0].x) * (p[1].y - p[0].y);
        if (cross <= 0.0f) continue;

        // Flat shading from the world-space normal, so a roof catches the light
        // and a flank does not. The same light direction as the terrain, or the
        // car would look pasted onto it.
        float ax = wx[1] - wx[0], ay = wy[1] - wy[0], az = wz[1] - wz[0];
        float bx = wx[2] - wx[0], by = wy[2] - wy[0], bz = wz[2] - wz[0];
        float nx = ay * bz - az * by;
        float ny = az * bx - ax * bz;
        float nz = ax * by - ay * bx;
        float nlen = SDL_sqrtf(nx * nx + ny * ny + nz * nz);
        float shade = 1.0f;
        if (nlen > 0.0001f) {
            static const float lxd = -0.42f, lyd = -0.57f, lzd = 0.71f;
            float d = (nx * lxd + ny * lyd + nz * lzd) / nlen;
            shade = SDL_clamp(0.62f + d * 0.38f, 0.45f, 1.15f);
        }

        SDL_FColor col = gs_role_colour(tri->paint, body);
        col.r = SDL_clamp(col.r * shade, 0.0f, 1.0f);
        col.g = SDL_clamp(col.g * shade, 0.0f, 1.0f);
        col.b = SDL_clamp(col.b * shade, 0.0f, 1.0f);

        sorted[count].depth = (gs_mesh_depth(wx[0], wy[0], wz[0]) +
                               gs_mesh_depth(wx[1], wy[1], wz[1]) +
                               gs_mesh_depth(wx[2], wy[2], wz[2])) / 3.0f;
        sorted[count].p[0] = p[0];
        sorted[count].p[1] = p[1];
        sorted[count].p[2] = p[2];
        sorted[count].colour = col;
        count++;
    }

    // A car is a handful of boxes and boxes are not convex together, so culling
    // alone leaves a wheel drawn over the body that hides it. Sorted far to
    // near, which for this many triangles costs nothing worth measuring.
    SDL_qsort(sorted, (size_t)count, sizeof sorted[0], gs_tri_compare);

    for (int i = 0; i < count; i++) {
        gs_tri(ren, sorted[i].p, sorted[i].colour);
        gs_stats.tris++;
    }
}

// Two thresholds rather than one, in tiles: cars must come this close to merge
// and go this far to split again. Anything else flickers the screen in half
// while two cars trade places at the boundary.
#define GS_MERGE_CLOSE 11.0f
#define GS_MERGE_APART 16.0f

// How fast the transition runs, in merge units per second. Slow enough to read
// as a camera move, fast enough not to be waited on.
#define GS_MERGE_RATE 1.6f

void gs_split_init(gs_split *s) {
    *s = (gs_split){ 0 };
    s->merge = 1.0f;      // start together, because a race starts on a grid
    s->shared.zoom = GS_ISO_DEFAULT_ZOOM;
}

// The smallest box holding every active car, and its middle.
static void gs_car_extent(const gs_world *w, float *cx, float *cy, float *spread) {
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    int seen = 0;

    for (uint8_t i = 0; i < w->car_count; i++) {
        if (!w->car[i].active) continue;
        float x = gs_to_f(w->car[i].x), y = gs_to_f(w->car[i].y);
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
        seen++;
    }
    if (seen == 0) {
        *cx = 0; *cy = 0; *spread = 0;
        return;
    }
    *cx = (minx + maxx) * 0.5f;
    *cy = (miny + maxy) * 0.5f;

    float dx = maxx - minx, dy = maxy - miny;
    *spread = dx > dy ? dx : dy;
}

void gs_split_update(gs_split *s, const gs_world *w, int win_w, int win_h, float dt) {
    float cx, cy, spread;
    gs_car_extent(w, &cx, &cy, &spread);

    // Hysteresis: only the crossing of a threshold changes the answer, and the
    // two thresholds are apart, so a pair of cars swapping places at the
    // boundary cannot make the screen split and re-join every frame.
    float wanted = s->merge > 0.5f ? (spread < GS_MERGE_APART ? 1.0f : 0.0f)
                                   : (spread < GS_MERGE_CLOSE ? 1.0f : 0.0f);
    if (w->car_count <= 1) wanted = 1.0f;

    float step = GS_MERGE_RATE * dt;
    if (s->merge < wanted) s->merge = s->merge + step > wanted ? wanted : s->merge + step;
    else if (s->merge > wanted) s->merge = s->merge - step < wanted ? wanted : s->merge - step;

    // The shared view holds everybody, pulling back as they spread so that the
    // last frame before a split already shows what the split panes will.
    float fit = GS_ISO_DEFAULT_ZOOM;
    if (spread > 0.0f) {
        float want = (float)win_h / ((spread + 6.0f) * GS_ISO_TILE_H);
        fit = want < GS_ISO_DEFAULT_ZOOM ? want : GS_ISO_DEFAULT_ZOOM;
        if (fit < 0.35f) fit = 0.35f;
    }

    s->shared.cx = cx;
    s->shared.cy = cy;
    s->shared.cz = 0.0f;
    s->shared.zoom = fit;
    s->shared.vw = (float)win_w;
    s->shared.vh = (float)win_h;
}

uint8_t gs_split_views(const gs_split *s, const gs_world *w,
                       int win_w, int win_h, gs_view *out) {
    if (s->merge >= 1.0f || w->car_count <= 1) {
        out[0] = (gs_view){ 0 };
        out[0].car = 0;
        out[0].cam = s->shared;
        out[0].rect = (SDL_Rect){ 0, 0, win_w, win_h };
        return 1;
    }

    SDL_Rect rects[GS_MAX_CARS];
    uint8_t n = gs_render_layout(w->car_count, win_w, win_h, rects);

    // Eased rather than linear. A linear blend has the camera at full speed on
    // the very first frame of a transition and stopped on the very last, so it
    // snaps into motion and snaps out of it - which reads as two small jumps
    // bracketing a smooth move, and is exactly what "no visible seam" is asking
    // about. Smoothstep is zero-velocity at both ends.
    float m = s->merge;
    float eased = m * m * (3.0f - 2.0f * m);

    for (uint8_t i = 0; i < n; i++) {
        out[i] = (gs_view){ 0 };
        out[i].car = i;
        out[i].rect = rects[i];

        // Its own car when fully split, the shared view as the merge closes -
        // so at the instant the divider appears or goes, every pane is already
        // showing what the other arrangement showed.
        float ox = gs_to_f(w->car[i].x), oy = gs_to_f(w->car[i].y);
        out[i].cam.cx = ox + (s->shared.cx - ox) * eased;
        out[i].cam.cy = oy + (s->shared.cy - oy) * eased;
        out[i].cam.cz = 0.0f;
        out[i].cam.zoom = GS_ISO_DEFAULT_ZOOM +
                          (s->shared.zoom - GS_ISO_DEFAULT_ZOOM) * eased;
        out[i].cam.vw = (float)rects[i].w;
        out[i].cam.vh = (float)rects[i].h;
    }
    return n;
}

uint8_t gs_render_layout(uint8_t views, int w, int h, SDL_Rect *out) {
    // A gap, so two views read as two views rather than as one confusing one.
    const int gap = 2;

    if (views <= 1) {
        out[0] = (SDL_Rect){ 0, 0, w, h };
        return 1;
    }
    if (views == 2) {
        int half = w / 2;
        out[0] = (SDL_Rect){ 0, 0, half - gap, h };
        out[1] = (SDL_Rect){ half + gap, 0, w - half - gap, h };
        return 2;
    }

    int hw = w / 2, hh = h / 2;
    const int cx[4] = { 0, 1, 0, 1 };
    const int cy[4] = { 0, 0, 1, 1 };

    uint8_t n = views > 4 ? 4 : views;
    for (uint8_t i = 0; i < n; i++) {
        int x = cx[i] == 0 ? 0 : hw + gap;
        int y = cy[i] == 0 ? 0 : hh + gap;
        int cw = cx[i] == 0 ? hw - gap : w - hw - gap;
        int ch = cy[i] == 0 ? hh - gap : h - hh - gap;
        out[i] = (SDL_Rect){ x, y, cw, ch };
    }
    return n;
}

void gs_render_ghost(SDL_Renderer *ren, const gs_track *t, const gs_car *c,
                     const gs_view *view) {
    if (!c->active) return;

    SDL_SetRenderViewport(ren, &view->rect);
    SDL_SetRenderClipRect(ren, &(SDL_Rect){ 0, 0, view->rect.w, view->rect.h });

    gs_camera cam = view->cam;
    cam.vw = (float)view->rect.w;
    cam.vh = (float)view->rect.h;

    // Drawn last and translucent rather than sorted into the terrain: a ghost
    // that disappears behind a rise is a ghost you cannot follow, and the point
    // of it is to be watched.
    gs_draw_car(ren, &cam, t, c, 3, 0.45f);

    SDL_SetRenderClipRect(ren, nullptr);
    SDL_SetRenderViewport(ren, nullptr);
}

// Interpolate a car between the last two simulation states. The world advances
// 120 times a second; frames do not, and without this the difference shows.
static gs_car gs_car_lerp(const gs_car *a, const gs_car *b, float alpha) {
    gs_car out = *b;
    out.x = a->x + (gs_fix)((float)(b->x - a->x) * alpha);
    out.y = a->y + (gs_fix)((float)(b->y - a->y) * alpha);
    out.z = a->z + (gs_fix)((float)(b->z - a->z) * alpha);

    // Headings wrap, so interpolate the short way round rather than through a
    // full turn - otherwise a car crossing north spins on the spot for a frame.
    int32_t d = gs_angle_delta(a->heading, b->heading);
    out.heading = (gs_angle)(a->heading + (int32_t)((float)d * alpha));
    return out;
}

void gs_render_track_camera(gs_view *view, const gs_world *prev,
                            const gs_world *now, float alpha) {
    if (view->car >= now->car_count) return;
    gs_car c = gs_car_lerp(&prev->car[view->car], &now->car[view->car], alpha);

    view->cam.cx = gs_to_f(c.x);
    view->cam.cy = gs_to_f(c.y);
    // The camera follows height only partly, so a jump moves the car up the
    // screen instead of moving the world down it. Losing that entirely makes a
    // jump invisible; following it entirely makes the ground lurch.
    view->cam.cz = gs_to_f(c.z) * 0.35f;
}

void gs_render_view(SDL_Renderer *ren, const gs_track *t, const gs_world *prev,
                    const gs_world *now, float alpha, const gs_view *view) {
    SDL_SetRenderViewport(ren, &view->rect);
    SDL_SetRenderClipRect(ren, &(SDL_Rect){ 0, 0, view->rect.w, view->rect.h });

    gs_camera cam = view->cam;
    cam.vw = (float)view->rect.w;
    cam.vh = (float)view->rect.h;

    // Painter's algorithm along the screen's depth axis. In a 2:1 diamond that
    // axis is x + y, so sweeping the diagonals draws back to front - and
    // drawing each car as the sweep reaches its tile is what puts it behind the
    // rise it is behind.
    int diagonals = (int)t->w + (int)t->h - 1;
    for (int d = 0; d < diagonals; d++) {
        for (int x = 0; x <= d; x++) {
            int y = d - x;
            if (x >= (int)t->w || y >= (int)t->h) continue;
            if (!gs_tile_in_view(&cam, t, (uint8_t)x, (uint8_t)y)) continue;
            gs_draw_tile(ren, &cam, t, (uint8_t)x, (uint8_t)y, view->show_gravity,
                         view->heat);
        }
        for (uint8_t i = 0; i < now->car_count; i++) {
            gs_car c = gs_car_lerp(&prev->car[i], &now->car[i], alpha);
            int cd = gs_fix_floor(c.x) + gs_fix_floor(c.y);
            if (cd == d) gs_draw_car(ren, &cam, t, &c, i, 1.0f);
        }
    }

    // Hazards, drawn on the ground under everything that moves. Flat, dark and
    // slightly translucent for oil, so it reads as something on the road rather
    // than something standing on it; a mine is small and bright, because the
    // one thing a player must be able to do is see it in time.
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    for (uint8_t i = 0; i < now->hazard_count; i++) {
        const gs_hazard *h = &now->hazard[i];
        if (h->kind == GS_HAZ_NONE || h->spent) continue;

        float hx = gs_to_f(h->x), hy = gs_to_f(h->y);
        float r = h->kind == GS_HAZ_OIL ? 1.3f : 0.45f;

        static const float ox[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
        static const float oy[4] = { -1.0f, -1.0f, 1.0f, 1.0f };
        SDL_FPoint p[4];
        for (int k = 0; k < 4; k++) {
            float px = hx + ox[k] * r, py = hy + oy[k] * r;
            gs_fix pz = gs_track_height(t, (gs_fix)(px * (float)GS_ONE),
                                        (gs_fix)(py * (float)GS_ONE));
            gs_iso_project(&cam, px, py, gs_to_f(pz) + 0.02f, &p[k].x, &p[k].y);
        }

        SDL_FColor col = h->kind == GS_HAZ_OIL
                             ? (SDL_FColor){ 0.05f, 0.04f, 0.10f, 0.72f }
                             : (SDL_FColor){ 1.0f, 0.45f, 0.1f, 0.95f };
        gs_quad(ren, p, col);
    }

    // Cars that have driven off the authored track are past the last diagonal,
    // so they are drawn after everything - which is where they are.
    for (uint8_t i = 0; i < now->car_count; i++) {
        gs_car c = gs_car_lerp(&prev->car[i], &now->car[i], alpha);
        int cd = gs_fix_floor(c.x) + gs_fix_floor(c.y);
        if (cd >= diagonals || cd < 0) gs_draw_car(ren, &cam, t, &c, i, 1.0f);
    }

    SDL_SetRenderClipRect(ren, nullptr);
    SDL_SetRenderViewport(ren, nullptr);
}

void gs_render_ghost_lerp(SDL_Renderer *ren, const gs_track *t,
                          const gs_car *prev, const gs_car *now, float alpha,
                          const gs_view *view) {
    gs_car c = gs_car_lerp(prev, now, alpha);
    gs_render_ghost(ren, t, &c, view);
}

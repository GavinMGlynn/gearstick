// gs_render.c - see gs_render.h for why the ground is geometry and not art.

#include "gfx/gs_render.h"

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
                                 bool show_gravity) {
    gs_fix cx = GS_INT(tx) + GS_HALF;
    gs_fix cy = GS_INT(ty) + GS_HALF;

    gs_surface s = gs_track_surface(t, cx, cy);
    gs_rgb base = gs_surface_colour[s];

    gs_fix dzdx, dzdy;
    gs_track_slope(t, cx, cy, &dzdx, &dzdy);
    float shade = gs_light(dzdx, dzdy);

    float r = base.r * shade, g = base.g * shade, b = base.b * shade;

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
                         bool show_gravity) {
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
    gs_quad(ren, p, gs_tile_colour(t, tx, ty, show_gravity));
}

// The car is a box: a footprint rotated to its heading, lifted by its ride
// height. Crude, and it reads correctly - which at this stage is the whole
// requirement. Pre-rendered sprites replace it in Phase 10.
static const SDL_FColor gs_car_colour[GS_MAX_CARS] = {
    { 0.90f, 0.25f, 0.20f, 1.0f },
    { 0.25f, 0.55f, 0.95f, 1.0f },
    { 0.95f, 0.80f, 0.20f, 1.0f },
    { 0.35f, 0.80f, 0.40f, 1.0f },
};

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

static void gs_draw_car(SDL_Renderer *ren, const gs_camera *cam,
                        const gs_track *t, const gs_car *c, uint8_t index,
                        float alpha) {
    if (!c->active) return;
    gs_stats.cars++;

    // **Deliberately not to scale.** A real car is 2.7 m and a tile is four, so
    // an honest one is two thirds of a tile and reads as a speck against the
    // ground it is driving on. These are about 1.3 tiles - roughly the
    // proportion the original used, and for the same reason: a two-car
    // collision has to be legible at a glance, and legibility is the entire
    // argument for this camera.
    //
    // Nothing in src/core/ knows about these numbers today. When collision
    // arrives it must use *these* rather than the metric truth, or a car is hit
    // by something the player cannot see.
    const float half_len = 0.65f, half_wid = 0.38f, body = 0.32f;

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

    // --- The body: a base quad, a roof quad, and the sides between them.
    float z = gs_to_f(c->z);
    SDL_FColor col = gs_car_colour[index & 3];
    if (c->wrecked) { col.r *= 0.35f; col.g *= 0.35f; col.b *= 0.35f; }
    col.a = alpha;

    SDL_FPoint lo[4], hi[4];
    for (int i = 0; i < 4; i++) {
        gs_iso_project(cam, fx[i], fy[i], z + 0.04f, &lo[i].x, &lo[i].y);
        gs_iso_project(cam, fx[i], fy[i], z + 0.04f + body, &hi[i].x, &hi[i].y);
    }

    SDL_FColor side = { col.r * 0.66f, col.g * 0.66f, col.b * 0.66f, alpha };
    for (int i = 0; i < 4; i++) {
        int j = (i + 1) & 3;
        SDL_FPoint face[4] = { lo[i], lo[j], hi[j], hi[i] };
        gs_quad(ren, face, side);
    }
    gs_quad(ren, hi, col);

    // A nose flash, so which way the car is pointing is readable when it is
    // sliding sideways - which, on ice, is most of the time.
    SDL_FPoint nose[4] = { hi[0], hi[1],
                           { (hi[1].x + hi[2].x) * 0.5f, (hi[1].y + hi[2].y) * 0.5f },
                           { (hi[0].x + hi[3].x) * 0.5f, (hi[0].y + hi[3].y) * 0.5f } };
    gs_quad(ren, nose, (SDL_FColor){ 1.0f, 1.0f, 1.0f, 0.85f * alpha });
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
            gs_draw_tile(ren, &cam, t, (uint8_t)x, (uint8_t)y, view->show_gravity);
        }
        for (uint8_t i = 0; i < now->car_count; i++) {
            gs_car c = gs_car_lerp(&prev->car[i], &now->car[i], alpha);
            int cd = gs_fix_floor(c.x) + gs_fix_floor(c.y);
            if (cd == d) gs_draw_car(ren, &cam, t, &c, i, 1.0f);
        }
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

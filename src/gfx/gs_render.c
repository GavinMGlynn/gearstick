// gs_render.c - see gs_render.h for why the ground is geometry and not art.

#include "gfx/gs_render.h"

typedef struct gs_rgb { float r, g, b; } gs_rgb;

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
                        const gs_track *t, const gs_car *c, uint8_t index) {
    if (!c->active) return;

    const float half_len = 0.34f, half_wid = 0.20f, body = 0.16f;

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
    gs_quad(ren, sp, (SDL_FColor){ 0.0f, 0.0f, 0.0f, 0.35f });

    // --- The body: a base quad, a roof quad, and the sides between them.
    float z = gs_to_f(c->z);
    SDL_FColor col = gs_car_colour[index & 3];
    if (c->wrecked) { col.r *= 0.35f; col.g *= 0.35f; col.b *= 0.35f; }

    SDL_FPoint lo[4], hi[4];
    for (int i = 0; i < 4; i++) {
        gs_iso_project(cam, fx[i], fy[i], z + 0.04f, &lo[i].x, &lo[i].y);
        gs_iso_project(cam, fx[i], fy[i], z + 0.04f + body, &hi[i].x, &hi[i].y);
    }

    SDL_FColor side = { col.r * 0.66f, col.g * 0.66f, col.b * 0.66f, 1.0f };
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
    gs_quad(ren, nose, (SDL_FColor){ 1.0f, 1.0f, 1.0f, 0.85f });
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
            gs_draw_tile(ren, &cam, t, (uint8_t)x, (uint8_t)y, view->show_gravity);
        }
        for (uint8_t i = 0; i < now->car_count; i++) {
            gs_car c = gs_car_lerp(&prev->car[i], &now->car[i], alpha);
            int cd = gs_fix_floor(c.x) + gs_fix_floor(c.y);
            if (cd == d) gs_draw_car(ren, &cam, t, &c, i);
        }
    }

    // Cars that have driven off the authored track are past the last diagonal,
    // so they are drawn after everything - which is where they are.
    for (uint8_t i = 0; i < now->car_count; i++) {
        gs_car c = gs_car_lerp(&prev->car[i], &now->car[i], alpha);
        int cd = gs_fix_floor(c.x) + gs_fix_floor(c.y);
        if (cd >= diagonals || cd < 0) gs_draw_car(ren, &cam, t, &c, i);
    }

    SDL_SetRenderClipRect(ren, nullptr);
    SDL_SetRenderViewport(ren, nullptr);
}

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
    [GS_SURF_PAVEMENT] = { 0.42f, 0.44f, 0.48f },
    [GS_SURF_DIRT]     = { 0.56f, 0.38f, 0.22f },
    [GS_SURF_ICE]      = { 0.70f, 0.84f, 0.94f },

    // **No two of these are near each other**, and the first attempt had three
    // that were: gravel, dust and rock came out as the same grey at three
    // brightnesses. That is fine on a flat plane and useless the moment the
    // ground tilts, because shading changes a tile's brightness by more than
    // those differed by - so two surfaces separated only by how pale they are
    // become one surface on a hillside. A test measures the whole palette.
    [GS_SURF_SAND]     = { 0.82f, 0.70f, 0.38f },
    [GS_SURF_GRAVEL]   = { 0.50f, 0.58f, 0.46f },
    [GS_SURF_ROCK]     = { 0.38f, 0.29f, 0.33f },
    [GS_SURF_DUST]     = { 0.72f, 0.62f, 0.50f },
    [GS_SURF_SLUSH]    = { 0.48f, 0.62f, 0.74f },
    [GS_SURF_GRASS]    = { 0.26f, 0.50f, 0.22f },
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

static SDL_FColor gs_tile_colour(const gs_track *t, int32_t tx, int32_t ty,
                                 bool show_gravity, const gs_analysis *heat) {
    gs_fix cx = GS_INT(tx) + GS_HALF;
    gs_fix cy = GS_INT(ty) + GS_HALF;

    gs_surface s = gs_track_surface(t, cx, cy);
    gs_rgb base = gs_surface_colour[s];

    gs_fix dzdx, dzdy;
    gs_track_slope(t, cx, cy, &dzdx, &dzdy);
    float shade = gs_light(dzdx, dzdy);

    float r = base.r * shade, g = base.g * shade, b = base.b * shade;

    // **Off the track reads as off the track.** The run-off is ground a car can
    // be on and has to be drawn, and it is also not where the racing is - a
    // shoulder rendered at the same weight as the circuit makes a thin ribbon of
    // track in a field of sand, and the eye has to hunt for the part that
    // matters. Darkened rather than recoloured, so it is plainly the same
    // material and plainly not the road.
    if (gs_track_outside(t, cx, cy) > 0) {
        r *= 0.55f; g *= 0.55f; b *= 0.55f;
    }

    if (heat != nullptr) {
        // Where everybody actually went, over every gravity and every machine
        // the analyser tried. The line a track *has* is rarely the line its
        // author drew, and this is the only way to be shown the difference.
        // Cold ground is left as it is; the used line runs up through green
        // into a hot white centre.
        float k = (tx >= 0 && ty >= 0 && tx < t->w && ty < t->h)
                      ? gs_to_f(gs_analysis_heat(heat, (uint8_t)tx, (uint8_t)ty))
                      : 0.0f;
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

// A quad lying on the ground, its corners sampled from the terrain so it
// follows whatever it is painted on rather than floating over a slope. Given in
// tiles; `lift` is how far above the ground to draw, which stops it fighting
// the surface for the same pixels.
static void gs_ground_quad(SDL_Renderer *ren, const gs_camera *cam,
                           const gs_track *t, const float x[4], const float y[4],
                           float lift, SDL_FColor c) {
    SDL_FPoint p[4];
    for (int i = 0; i < 4; i++) {
        gs_fix fx = (gs_fix)(x[i] * (float)GS_ONE);
        gs_fix fy = (gs_fix)(y[i] * (float)GS_ONE);
        float z = gs_to_f(gs_track_height(t, fx, fy)) + lift;
        gs_iso_project(cam, x[i], y[i], z, &p[i].x, &p[i].y);
    }
    gs_quad(ren, p, c);
}

// **The edge of the road, said in kerb.** The authored track ends and the
// run-off begins, and until now the only thing that said so was a change of
// shade - which a player driving flat out at the end of a shelf does not see in
// time. This is the red and white a racing driver has been reading since before
// any of us: alternating blocks along the boundary, on the ground, one to a
// tile. The original said it with a fence and there is no argument about which
// is clearer; what matters is that the edge is a thing you can see coming.
#define GS_KERB_DEPTH 0.32f

static void gs_draw_kerb(SDL_Renderer *ren, const gs_camera *cam,
                         const gs_track *t, int tx, int ty) {
    if (tx < 0 || ty < 0 || tx >= (int)t->w || ty >= (int)t->h) return;

    // Which sides of this tile face off the track. A corner tile has two.
    const bool side[4] = {
        ty == 0,                    // north, towards -y
        tx == (int)t->w - 1,        // east
        ty == (int)t->h - 1,        // south
        tx == 0,                    // west
    };

    static const SDL_FColor pale = { 0.92f, 0.92f, 0.90f, 1.0f };
    static const SDL_FColor red  = { 0.78f, 0.16f, 0.14f, 1.0f };

    float x0 = (float)tx, y0 = (float)ty, d = GS_KERB_DEPTH;

    for (int e = 0; e < 4; e++) {
        if (!side[e]) continue;

        // Alternating along the boundary, so the blocks read as a kerb rather
        // than as a stripe. Keyed to the tile the block is on, which keeps
        // neighbouring tiles in step.
        bool even = (((e == 0 || e == 2) ? tx : ty) & 1) == 0;
        SDL_FColor c = even ? pale : red;

        float x[4], y[4];
        switch (e) {
        case 0:  // north edge, drawn just inside it
            x[0] = x0;       y[0] = y0;
            x[1] = x0 + 1.f; y[1] = y0;
            x[2] = x0 + 1.f; y[2] = y0 + d;
            x[3] = x0;       y[3] = y0 + d;
            break;
        case 1:  // east
            x[0] = x0 + 1.f - d; y[0] = y0;
            x[1] = x0 + 1.f;     y[1] = y0;
            x[2] = x0 + 1.f;     y[2] = y0 + 1.f;
            x[3] = x0 + 1.f - d; y[3] = y0 + 1.f;
            break;
        case 2:  // south
            x[0] = x0;       y[0] = y0 + 1.f - d;
            x[1] = x0 + 1.f; y[1] = y0 + 1.f - d;
            x[2] = x0 + 1.f; y[2] = y0 + 1.f;
            x[3] = x0;       y[3] = y0 + 1.f;
            break;
        default: // west
            x[0] = x0;     y[0] = y0;
            x[1] = x0 + d; y[1] = y0;
            x[2] = x0 + d; y[2] = y0 + 1.f;
            x[3] = x0;     y[3] = y0 + 1.f;
            break;
        }
        gs_ground_quad(ren, cam, t, x, y, 0.03f, c);
    }
}

// How far a start-line flag stands above the ground it is planted in, and how
// deep the chequered line itself is. Both in tiles, because everything here is.
#define GS_FLAG_POLE_H    2.30f
#define GS_LINE_HALF_DEPTH 0.55f

// **A flag is what says "line" from the other end of a straight.** Chequer
// painted on the ground is only legible once it is nearly under the car, which
// is too late to be told where the lap ends; a pair of flags stands up out of
// the scene and is readable from a long way back. Racing has marked its line
// this way for as long as there has been a line.
//
// Drawn facing the screen rather than lying in the world. A panel standing in a
// plane of the world collapses to a line at the two headings where that plane
// is edge on to an isometric camera - the projection maps both the panel's
// width and its height onto the screen's vertical there - and a flag that
// disappears on some tracks and not others is worse than no flag at all. Sizes
// are still given in tiles and turned into pixels by the same numbers the
// projection uses, so a flag keeps its size against the world at every zoom.
static void gs_draw_flag(SDL_Renderer *ren, const gs_camera *cam,
                         const gs_track *t, float bx, float by,
                         float mid_x, float mid_y) {
    float bz = gs_to_f(gs_track_height(t, (gs_fix)(bx * (float)GS_ONE),
                                       (gs_fix)(by * (float)GS_ONE)));
    float px = 0.0f, py = 0.0f;
    gs_iso_project(cam, bx, by, bz, &px, &py);

    // Which way the flag flies: away from the middle of the line, so the two of
    // them open outwards and neither one hangs over the road.
    float mz = gs_to_f(gs_track_height(t, (gs_fix)(mid_x * (float)GS_ONE),
                                       (gs_fix)(mid_y * (float)GS_ONE)));
    float mx = 0.0f, my = 0.0f;
    gs_iso_project(cam, mid_x, mid_y, mz, &mx, &my);
    (void)my;
    float away = (px >= mx) ? 1.0f : -1.0f;

    float pole_h = GS_FLAG_POLE_H * GS_ISO_TILE_Z * cam->zoom;
    float pole_w = 0.07f * GS_ISO_TILE_W * cam->zoom;
    if (pole_w < 1.0f) pole_w = 1.0f;

    float flag_w = 0.95f * GS_ISO_TILE_W * 0.5f * cam->zoom;
    float flag_h = 0.62f * GS_ISO_TILE_Z * cam->zoom;

    float top = py - pole_h;

    static const SDL_FColor shaft = { 0.80f, 0.80f, 0.83f, 1.0f };
    const SDL_FPoint pole[4] = {
        { px - pole_w * 0.5f, py },
        { px + pole_w * 0.5f, py },
        { px + pole_w * 0.5f, top },
        { px - pole_w * 0.5f, top },
    };
    gs_quad(ren, pole, shaft);

    // Four squares across and three down: enough to read as a chequered flag,
    // few enough that each square is still a square and not a pixel.
    const int across = 4, down = 3;
    for (int i = 0; i < across; i++) {
        for (int j = 0; j < down; j++) {
            float x0 = px + away * flag_w * (float)i / (float)across;
            float x1 = px + away * flag_w * (float)(i + 1) / (float)across;
            float y0 = top + flag_h * (float)j / (float)down;
            float y1 = top + flag_h * (float)(j + 1) / (float)down;

            SDL_FColor c = ((i + j) & 1)
                               ? (SDL_FColor){ 0.06f, 0.06f, 0.07f, 1.0f }
                               : (SDL_FColor){ 0.97f, 0.97f, 0.97f, 1.0f };
            const SDL_FPoint q[4] = {
                { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 },
            };
            gs_quad(ren, q, c);
        }
    }
}

// **The route, drawn on the ground.** Gates were in the simulation and in the
// editor's white line and nowhere at all in a race, so a player arriving at a
// track had nothing to say which way round it went - "the direction was sort of
// obvious" in the game this one is descended from, and it was obvious because
// the track said so. A band across the gate, and an arrow through it pointing
// the way a car is meant to pass.
// **A post, for a gate that is only a waypoint.** These used to be a solid
// band right across the road, which reads as a line you cross to finish
// something - a player looking at one asked whether it was the end of the
// track. A gate that is not the finish should say "through here" without
// claiming to be a finish, so it is marked at its edges and left open in the
// middle, the way a rally stage is.
//
// Screen-facing for the same reason the flag is: a panel standing in a plane of
// the world collapses to a line at the two headings where that plane is edge on
// to an isometric camera.
static void gs_draw_post(SDL_Renderer *ren, const gs_camera *cam,
                         const gs_track *t, float bx, float by) {
    float bz = gs_to_f(gs_track_height(t, (gs_fix)(bx * (float)GS_ONE),
                                       (gs_fix)(by * (float)GS_ONE)));
    float px = 0.0f, py = 0.0f;
    gs_iso_project(cam, bx, by, bz, &px, &py);

    float h = 1.15f * GS_ISO_TILE_Z * cam->zoom;
    float w = 0.13f * GS_ISO_TILE_W * cam->zoom;
    if (w < 1.0f) w = 1.0f;

    // A pale post with a blue head: the blue is what the route used to be
    // painted in, kept so a player who learned that colour keeps it, and the
    // head is at the top where it is not hidden by the car in front.
    static const SDL_FColor shaft = { 0.88f, 0.88f, 0.86f, 1.0f };
    static const SDL_FColor head  = { 0.30f, 0.65f, 0.95f, 1.0f };

    float split = py - h * 0.62f;
    const SDL_FPoint lower[4] = {
        { px - w * 0.5f, py }, { px + w * 0.5f, py },
        { px + w * 0.5f, split }, { px - w * 0.5f, split },
    };
    gs_quad(ren, lower, shaft);

    const SDL_FPoint upper[4] = {
        { px - w * 0.5f, split }, { px + w * 0.5f, split },
        { px + w * 0.5f, py - h }, { px - w * 0.5f, py - h },
    };
    gs_quad(ren, upper, head);
}

// **The light tree: a race that begins when everybody is ready.** Until now a
// race simply was, from tick zero, so arriving at a track meant already being
// late. Three lamps come on a second apart and all of them go green together,
// which is the moment the simulation stops holding the cars - see
// `gs_world_held`. Standing beside the grid rather than over it, because a
// gantry across the road would be the one thing between the camera and the cars
// at the only moment nobody may miss.
//
// Where it stands: beside the start line, one gate width plus a margin out, so
// it is clear of the widest grid and never over the road.
static void gs_start_lights_at(const gs_track *t, float *x, float *y) {
    const gs_gate *g = &t->gate[0];
    float gx = gs_to_f(g->x), gy = gs_to_f(g->y);
    float fx = gs_to_f(gs_cos(g->heading)), fy = gs_to_f(gs_sin(g->heading));
    float sx = -fy, sy = fx;

    float back = gs_to_f(GS_GRID_BACK) - 0.95f;
    float out = gs_to_f(g->half_width) + 1.7f;

    *x = gx - fx * back + sx * out;
    *y = gy - fy * back + sy * out;
}

static void gs_draw_start_lights(SDL_Renderer *ren, const gs_camera *cam,
                                 const gs_track *t, const gs_world *w,
                                 float bx, float by) {
    float bz = gs_to_f(gs_track_height(t, (gs_fix)(bx * (float)GS_ONE),
                                       (gs_fix)(by * (float)GS_ONE)));
    float px = 0.0f, py = 0.0f;
    gs_iso_project(cam, bx, by, bz, &px, &py);

    float mast = 2.85f * GS_ISO_TILE_Z * cam->zoom;
    float post_w = 0.10f * GS_ISO_TILE_W * cam->zoom;
    if (post_w < 1.0f) post_w = 1.0f;

    static const SDL_FColor steel = { 0.72f, 0.72f, 0.76f, 1.0f };
    const SDL_FPoint post[4] = {
        { px - post_w * 0.5f, py }, { px + post_w * 0.5f, py },
        { px + post_w * 0.5f, py - mast }, { px - post_w * 0.5f, py - mast },
    };
    gs_quad(ren, post, steel);

    // How many lamps are lit, and what colour. One a second, counting down, and
    // all of them green together at the off.
    uint32_t left = gs_world_countdown(w);
    bool green = left == 0 && w->green_tick > 0 &&
                 w->tick < (uint64_t)w->green_tick + GS_GREEN_TICKS;

    int lit = 0;
    if (left > 0) {
        lit = GS_COUNTDOWN_LAMPS - (int)((left - 1u) / (uint32_t)GS_TICK_HZ);
        if (lit < 0) lit = 0;
        if (lit > GS_COUNTDOWN_LAMPS) lit = GS_COUNTDOWN_LAMPS;
    }

    // The housing, and the lamps down it: the first to light is the top one, so
    // the tree fills downwards the way every start light a driver has seen does.
    float lamp = 0.42f * GS_ISO_TILE_Z * cam->zoom;
    float pad = lamp * 0.22f;
    float box_w = lamp + pad * 2.0f;
    float top = py - mast;

    static const SDL_FColor housing = { 0.10f, 0.10f, 0.12f, 1.0f };
    float box_h = (lamp + pad) * (float)GS_COUNTDOWN_LAMPS + pad;
    const SDL_FPoint box[4] = {
        { px - box_w * 0.5f, top }, { px + box_w * 0.5f, top },
        { px + box_w * 0.5f, top + box_h }, { px - box_w * 0.5f, top + box_h },
    };
    gs_quad(ren, box, housing);

    for (int i = 0; i < GS_COUNTDOWN_LAMPS; i++) {
        float y0 = top + pad + (lamp + pad) * (float)i;

        SDL_FColor c;
        if (green) {
            c = (SDL_FColor){ 0.20f, 0.92f, 0.32f, 1.0f };
        } else if (i < lit) {
            c = (SDL_FColor){ 0.96f, 0.16f, 0.12f, 1.0f };
        } else {
            c = (SDL_FColor){ 0.22f, 0.09f, 0.09f, 1.0f };
        }

        const SDL_FPoint q[4] = {
            { px - lamp * 0.5f, y0 }, { px + lamp * 0.5f, y0 },
            { px + lamp * 0.5f, y0 + lamp }, { px - lamp * 0.5f, y0 + lamp },
        };
        gs_quad(ren, q, c);
    }
}

// The diagonal a flat mark on the ground sorts on: the furthest into the sweep
// that any corner of it reaches, so it is drawn after every tile it lies on.
static int gs_ground_quad_diagonal(const float x[4], const float y[4]) {
    int d = (int)SDL_floorf(x[0]) + (int)SDL_floorf(y[0]);
    for (int i = 1; i < 4; i++) {
        int e = (int)SDL_floorf(x[i]) + (int)SDL_floorf(y[i]);
        if (e > d) d = e;
    }
    return d;
}

// **A gate is drawn a piece at a time, each piece where it lies.** The band
// reaches right across the track, so it covers a wide span of diagonals while
// the gate has only one - and drawn all at once at the gate's own diagonal, the
// far half of the line is painted over every car that is nearer than the gate
// centre. A player saw exactly that: "the car just went behind the finish
// line". Each block and each piece of the arrow now sorts on the ground it
// actually lies on, so a car on this side of the line is drawn in front of it
// and a car beyond it is drawn behind.
static void gs_draw_gate(SDL_Renderer *ren, const gs_camera *cam,
                         const gs_track *t, const gs_gate *g, bool start,
                         int world_d) {
    float gx = gs_to_f(g->x), gy = gs_to_f(g->y);
    float hw = gs_to_f(g->half_width);

    // The way through, and the line across it.
    float fx = gs_to_f(gs_cos(g->heading)), fy = gs_to_f(gs_sin(g->heading));
    float sx = -fy, sy = fx;

    // **The chequer belongs to the finish and to nothing else.** A waypoint
    // gets posts at its edges instead - see gs_draw_post - so that the one
    // line a player has to cross to finish is the only line drawn across the
    // road. Two rows deep, so the squares alternate across the line as well as
    // along it: one row of alternating blocks is a dashed line rather than a
    // chequered one, which is what this was and what a picture of it showed.
    float half_depth = GS_LINE_HALF_DEPTH;
    int rows = 2;
    float row = (2.0f * half_depth) / (float)rows;

    // Square blocks: how many fit across follows the depth of a row and not the
    // width of the gate, so the chequer does not stretch on a wide one.
    int blocks = (int)((2.0f * hw) / row + 0.5f);
    if (blocks < 2) blocks = 2;
    if (blocks > 24) blocks = 24;

    for (int r = 0; start && r < rows; r++) {
        float d0 = -half_depth + row * (float)r, d1 = d0 + row;

        for (int i = 0; i < blocks; i++) {
            float a = -hw + (2.0f * hw) * (float)i / (float)blocks;
            float b = -hw + (2.0f * hw) * (float)(i + 1) / (float)blocks;

            float x[4] = { gx + sx * a + fx * d0, gx + sx * b + fx * d0,
                           gx + sx * b + fx * d1, gx + sx * a + fx * d1 };
            float y[4] = { gy + sy * a + fy * d0, gy + sy * b + fy * d0,
                           gy + sy * b + fy * d1, gy + sy * a + fy * d1 };

            if (gs_ground_quad_diagonal(x, y) != world_d) continue;

            SDL_FColor c = ((i + r) & 1)
                               ? (SDL_FColor){ 0.08f, 0.08f, 0.09f, 1.0f }
                               : (SDL_FColor){ 0.95f, 0.95f, 0.95f, 1.0f };
            gs_ground_quad(ren, cam, t, x, y, 0.04f, c);
        }
    }

    // And the arrow through it: a shaft along the way through, and two barbs.
    // Drawn short of the line rather than on it, so it reads as "this way to
    // there" rather than as part of the gate.
    SDL_FColor tip = { 0.95f, 0.85f, 0.25f, 0.9f };
    float base = -2.4f, len = 1.6f, half = 0.20f;

    float ax[4] = { gx + fx * base + sx * half, gx + fx * (base + len) + sx * half,
                    gx + fx * (base + len) - sx * half, gx + fx * base - sx * half };
    float ay[4] = { gy + fy * base + sy * half, gy + fy * (base + len) + sy * half,
                    gy + fy * (base + len) - sy * half, gy + fy * base - sy * half };
    if (gs_ground_quad_diagonal(ax, ay) == world_d) {
        gs_ground_quad(ren, cam, t, ax, ay, 0.05f, tip);
    }

    // The head, as one triangle: a quad with two corners in the same place is
    // a triangle, and gs_quad draws the degenerate half for nothing.
    float head = 0.85f, wide = 0.62f;
    float hx[4] = {
        gx + fx * (base + len + head),
        gx + fx * (base + len) + sx * wide,
        gx + fx * (base + len) - sx * wide,
        gx + fx * (base + len + head),
    };
    float hy[4] = {
        gy + fy * (base + len + head),
        gy + fy * (base + len) + sy * wide,
        gy + fy * (base + len) - sy * wide,
        gy + fy * (base + len + head),
    };
    if (gs_ground_quad_diagonal(hx, hy) == world_d) {
        gs_ground_quad(ren, cam, t, hx, hy, 0.05f, tip);
    }

    // **Where the race begins, as opposed to where it ends.** The grid sits
    // GS_GRID_BACK behind the line a lap is measured on, so a player looking at
    // a chequered line under their own front wheels cannot tell whether it is
    // the start or the finish - and one of them said exactly that. A plain
    // white line is painted across the grid instead: that is the start, the
    // chequer ahead of it is the finish, and the difference between them is now
    // something you can see rather than something you have to be told.
    if (start) {
        // Just in front of the grid rather than under it, so the cars line up
        // behind their line the way a grid does. Clear of the longest car.
        float back = gs_to_f(GS_GRID_BACK) - 0.95f;
        float depth = 0.16f;

        float lx[4] = { gx - fx * back + sx * hw, gx - fx * back - sx * hw,
                        gx - fx * (back - depth) - sx * hw,
                        gx - fx * (back - depth) + sx * hw };
        float ly[4] = { gy - fy * back + sy * hw, gy - fy * back - sy * hw,
                        gy - fy * (back - depth) - sy * hw,
                        gy - fy * (back - depth) + sy * hw };

        // Split across its length so it sorts a piece at a time, for the same
        // reason the chequer does: it reaches right across the road.
        const int pieces = 12;
        for (int i = 0; i < pieces; i++) {
            float a = (float)i / (float)pieces, b = (float)(i + 1) / (float)pieces;
            float qx[4] = { lx[0] + (lx[1] - lx[0]) * a, lx[0] + (lx[1] - lx[0]) * b,
                            lx[3] + (lx[2] - lx[3]) * b, lx[3] + (lx[2] - lx[3]) * a };
            float qy[4] = { ly[0] + (ly[1] - ly[0]) * a, ly[0] + (ly[1] - ly[0]) * b,
                            ly[3] + (ly[2] - ly[3]) * b, ly[3] + (ly[2] - ly[3]) * a };
            if (gs_ground_quad_diagonal(qx, qy) != world_d) continue;
            gs_ground_quad(ren, cam, t, qx, qy, 0.04f,
                           (SDL_FColor){ 0.95f, 0.95f, 0.95f, 1.0f });
        }
    } else {
        // A waypoint is marked at its edges and left open in the middle.
        for (int k = 0; k < 2; k++) {
            float side = k == 0 ? 1.0f : -1.0f;
            float bx = gx + sx * hw * side, by = gy + sy * hw * side;
            if ((int)SDL_floorf(bx) + (int)SDL_floorf(by) == world_d) {
                gs_draw_post(ren, cam, t, bx, by);
            }
        }
    }
}

// Where the start line's two flags stand: at its ends and a little outside
// them, so a car crossing on the extreme edge still passes between the flags
// rather than through one.
static void gs_flag_feet(const gs_gate *g, float out[2][2]) {
    float gx = gs_to_f(g->x), gy = gs_to_f(g->y);
    float fx = gs_to_f(gs_cos(g->heading)), fy = gs_to_f(gs_sin(g->heading));
    float sx = -fy, sy = fx;
    float off = gs_to_f(g->half_width) + 0.40f;

    out[0][0] = gx + sx * off;  out[0][1] = gy + sy * off;
    out[1][0] = gx - sx * off;  out[1][1] = gy - sy * off;
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
                            int32_t tx, int32_t ty) {
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
                         const gs_track *t, int32_t tx, int32_t ty,
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

SDL_FColor gs_render_surface_colour(gs_surface surface) {
    const gs_rgb *c = &gs_surface_colour[surface < GS_SURF_COUNT ? surface
                                                                 : GS_SURF_PAVEMENT];
    return (SDL_FColor){ c->r, c->g, c->b, 1.0f };
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

// Room for the largest vehicle's triangles with plenty over. Sized from the
// meshes rather than guessed: the loop below stops filling when it runs out,
// so a mesh larger than this loses its tail silently - a car with a piece
// missing and nothing said about it. tools/make_meshes.py prints every
// vehicle's count, and the largest today is the lunar rover at 268.
#define GS_MESH_MAX_TRIS 512

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

// How a wreck is drawn against how the car was: wider on the ground and lower to
// it. The spread matches GS_WRECK_RADIUS over GS_CAR_RADIUS, so the picture and
// the collision are the same size.
// How big a car is drawn, in tiles. See gs_draw_car on why this is deliberately
// not to scale.
#define GS_CAR_HALF_LEN 0.65f
#define GS_CAR_HALF_WID 0.38f

#define GS_WRECK_SPREAD 1.5f
#define GS_WRECK_SQUASH 0.55f

// The predicted flight, as a dotted line to the touchdown with a marker on the
// spot. Dotted rather than solid because it is a guess about the future and has
// to look like one - a solid line through the world reads as scenery.
static void gs_draw_arc(SDL_Renderer *ren, const gs_camera *cam,
                        const gs_track *t, const gs_world *w, uint8_t car) {
    static gs_arc arc;
    uint8_t n = gs_world_arc(w, t, car, &arc);
    if (n < 2) return;

    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

    // Every other point, so it reads as dots rather than as a rope.
    for (uint8_t i = 0; i < n; i += 2) {
        float sx, sy;
        gs_iso_project(cam, gs_to_f(arc.x[i]), gs_to_f(arc.y[i]),
                       gs_to_f(arc.z[i]), &sx, &sy);

        // Fading along its length: the near end is where you are and the far end
        // is a prediction, and it should not claim equal confidence in both.
        float along = (float)i / (float)(n - 1);
        SDL_SetRenderDrawColorFloat(ren, 1.0f, 0.85f, 0.35f, 0.65f - along * 0.3f);
        SDL_FRect dot = { sx - 1.5f, sy - 1.5f, 3.0f, 3.0f };
        SDL_RenderFillRect(ren, &dot);
    }

    // Where it comes down, on the ground rather than in the air: a ring at the
    // touchdown is the thing a player is actually reading, and the height of the
    // last dot is not where the wheels arrive.
    if (!arc.landed) return;

    gs_fix gz = gs_track_height(t, arc.x[n - 1], arc.y[n - 1]);
    float cx = gs_to_f(arc.x[n - 1]), cy = gs_to_f(arc.y[n - 1]);

    SDL_SetRenderDrawColorFloat(ren, 1.0f, 0.85f, 0.35f, 0.75f);
    SDL_FPoint ring[13];
    for (int i = 0; i < 13; i++) {
        float a = (float)i * 6.2831853f / 12.0f;
        gs_iso_project(cam, cx + SDL_cosf(a) * 0.5f, cy + SDL_sinf(a) * 0.5f,
                       gs_to_f(gz) + 0.02f, &ring[i].x, &ring[i].y);
    }
    SDL_RenderLines(ren, ring, 13);
}

// **The diagonal the sweep must have reached before a car may be drawn - which
// is not the car's centre tile.** A car is about 1.3 tiles long, so its body
// reaches into the tile in front of the one it is standing on, and that tile
// is on the *next* diagonal, which the sweep draws afterwards. Sorted by its
// centre, a car therefore has the ground it is standing on painted over its
// bonnet; and because the centre crosses into a new tile every car length or
// so, the overpaint arrives and leaves as it drives. That is the "background
// comes over the bonnet every few seconds" a player reported.
//
// Taken over the whole footprint instead, so a car is drawn only once every
// tile it covers has been. Ground genuinely in front of it is on a diagonal
// beyond the footprint and still draws over it, which is what makes a car
// behind a rise disappear behind the rise.
static int gs_car_diagonal(const gs_car *c) {
    float half_len = GS_CAR_HALF_LEN, half_wid = GS_CAR_HALF_WID;
    if (c->wrecked) {
        half_len *= GS_WRECK_SPREAD;
        half_wid *= GS_WRECK_SPREAD;
    }

    float fx[4], fy[4];
    gs_car_footprint(c, half_len, half_wid, fx, fy);

    int d = (int)SDL_floorf(fx[0]) + (int)SDL_floorf(fy[0]);
    for (int i = 1; i < 4; i++) {
        int e = (int)SDL_floorf(fx[i]) + (int)SDL_floorf(fy[i]);
        if (e > d) d = e;
    }
    return d;
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
    float half_len = GS_CAR_HALF_LEN, half_wid = GS_CAR_HALF_WID;
    if (c->wrecked) {
        half_len *= GS_WRECK_SPREAD;
        half_wid *= GS_WRECK_SPREAD;
    }

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

            // **A wreck is spread out and flattened.** The same mesh, pushed
            // outwards and squashed down: a car that has stopped being a car
            // reads instantly, and the wider footprint is the one the physics
            // is already using - GS_WRECK_RADIUS - so what a player sees in the
            // way is the size of the thing that is in the way. A wreck drawn
            // car-sized over a wreck-sized obstacle would be the worst of both.
            float vx = v->x, vy = v->y, vz = v->z;
            if (c->wrecked) {
                vx *= GS_WRECK_SPREAD;
                vy *= GS_WRECK_SPREAD;
                vz *= GS_WRECK_SQUASH;
            }

            wx[k] = ox + vx * f[0] + vy * l[0] + vz * u[0];
            wy[k] = oy + vx * f[1] + vy * l[1] + vz * u[1];
            wz[k] = oz + vx * f[2] + vy * l[2] + vz * u[2];
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

// **How much of a car's height the camera takes on.** Not all of it: a jump has
// to move the car up the screen rather than move the world down it, or nobody
// can see they are airborne - and that gap between car and shadow is the single
// most readable thing in the frame. Not none of it either, which is what the
// race camera used to do: on ground that is not at height zero, "none of it"
// draws the car as far above the middle of the screen as the ground is high.
#define GS_CAM_FOLLOW_Z 0.35f

void gs_split_init(gs_split *s) {
    *s = (gs_split){ 0 };
    s->merge = 1.0f;      // start together, because a race starts on a grid
    s->shared.zoom = GS_ISO_DEFAULT_ZOOM;
}

// The smallest box holding every active car, and its middle.
// **How high the camera rides for one car.** The ground it is over, plus a
// fraction of however far above that ground it has got. Following the ground
// fully is what keeps a car centred on a track built up in the air; following
// the air only partly is what makes a jump read as a jump, by letting the car
// climb the screen away from its own shadow.
static float gs_cam_height(const gs_track *t, const gs_car *c) {
    float ground = gs_to_f(gs_track_height(t, c->x, c->y));
    float z = gs_to_f(c->z);
    return ground + (z - ground) * GS_CAM_FOLLOW_Z;
}

// **Partly, but never so partly that the car leaves the pane.** 0.35 of the air
// is what makes a jump read: the car climbs the screen away from its shadow.
// It is a rule for a car that is briefly airborne, and a car can be
// permanently airborne - wrecked over the drop, where the simulation stops it
// where it is rather than at the bottom - at which point "partly" means "gone".
// So the follow is capped by the pane it has to stay inside.
static float gs_cam_hold(float cam_z, float car_z, float zoom, float vh) {
    if (zoom <= 0.0f || vh <= 0.0f) return cam_z;

    // A third of the pane, in tiles, which leaves the car well inside it with
    // its shadow and its own height still on screen.
    float most = (vh * 0.33f) / (GS_ISO_TILE_Z * zoom);
    if (car_z - cam_z > most) return car_z - most;
    if (cam_z - car_z > most) return car_z + most;
    return cam_z;
}

// Where a car is *this frame*, which is between two ticks rather than on one.
// The renderer draws cars interpolated; a camera that reads the settled state
// instead is pointed a fraction of a tick away from what is drawn, and the
// fraction changes every frame - which is a car that judders in a smooth world,
// and is what a recording of one showed.
static gs_car gs_car_lerp(const gs_car *a, const gs_car *b, float alpha);

static gs_car gs_car_now(const gs_world *prev, const gs_world *now, uint8_t i,
                         float alpha) {
    if (prev == nullptr || i >= prev->car_count) return now->car[i];
    return gs_car_lerp(&prev->car[i], &now->car[i], alpha);
}

static void gs_car_extent(const gs_track *t, const gs_world *prev,
                          const gs_world *w, float alpha,
                          float *cx, float *cy, float *cz, float *spread) {
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    float minz = 1e9f, maxz = -1e9f;
    float sumz = 0.0f;
    int seen = 0;

    for (uint8_t i = 0; i < w->car_count; i++) {
        if (!w->car[i].active) continue;
        gs_car c = gs_car_now(prev, w, i, alpha);
        float x = gs_to_f(c.x), y = gs_to_f(c.y);
        float h = gs_cam_height(t, &c);
        float z = gs_to_f(c.z);
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
        // **How far apart in height is asked of where the cars are, not of
        // where the camera would like to ride.** The damped height is what the
        // camera rides at; the raw one is what the screen has to hold, and
        // holding both is what the answer here decides.
        if (z < minz) minz = z;
        if (z > maxz) maxz = z;
        sumz += h;
        seen++;
    }
    if (seen == 0) {
        *cx = 0; *cy = 0; *cz = 0; *spread = 0;
        return;
    }
    *cx = (minx + maxx) * 0.5f;
    *cy = (miny + maxy) * 0.5f;
    *cz = sumz / (float)seen;

    // **How far apart they are, height included.** Measuring the ground plane
    // alone says two cars are together when one of them is twenty tiles down a
    // drop - and a shared view of that pair is a view of the empty air between
    // them, with a car above the top edge and a car below the bottom one. Which
    // is what a player got, with one car wrecked in the run-off and the other
    // still racing.
    float dx = maxx - minx, dy = maxy - miny, dz = maxz - minz;
    float widest = dx > dy ? dx : dy;
    *spread = dz > widest ? dz : widest;
}

void gs_split_update(gs_split *s, const gs_track *t, const gs_world *prev,
                     const gs_world *w, float alpha, int win_w, int win_h,
                     float dt) {
    float cx, cy, cz, spread;
    gs_car_extent(t, prev, w, alpha, &cx, &cy, &cz, &spread);

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

    // **The camera follows height, or the race happens off the top of the
    // screen.** This was zero, which is right only on ground that happens to
    // sit at height zero - and the ground under a start line usually does not.
    // A car eight tiles up is drawn eight tiles up: three hundred and eighty
    // pixels above the middle of a 720-pixel window, which is a race with no
    // car in it and a camera that is, technically, exactly where the car is.
    // Found by a player who raced a served track and saw nothing at all.
    //
    // The same partial follow the start-line camera uses, and for the same
    // reason: losing height entirely makes a jump invisible, following it
    // entirely makes the ground lurch.
    s->shared.cz = cz;

    // Held inside the pane for every car it is meant to be showing. With the
    // spread above including height, a pair too far apart in the air to hold
    // has already split the screen; this is what keeps each of them on their
    // own pane once it has.
    for (uint8_t i = 0; i < w->car_count; i++) {
        if (!w->car[i].active) continue;
        gs_car held = gs_car_now(prev, w, i, alpha);
        s->shared.cz = gs_cam_hold(s->shared.cz, gs_to_f(held.z), fit,
                                   (float)win_h);
    }
    s->shared.zoom = fit;
    s->shared.vw = (float)win_w;
    s->shared.vh = (float)win_h;
}

uint8_t gs_split_views(const gs_split *s, const gs_track *t,
                       const gs_world *prev, const gs_world *w, float alpha,
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
        gs_car c = gs_car_now(prev, w, i, alpha);
        float ox = gs_to_f(c.x), oy = gs_to_f(c.y);
        float oz = gs_cam_height(t, &c);
        out[i].cam.cx = ox + (s->shared.cx - ox) * eased;
        out[i].cam.cy = oy + (s->shared.cy - oy) * eased;
        out[i].cam.cz = oz + (s->shared.cz - oz) * eased;
        out[i].cam.zoom = GS_ISO_DEFAULT_ZOOM +
                          (s->shared.zoom - GS_ISO_DEFAULT_ZOOM) * eased;
        out[i].cam.vw = (float)rects[i].w;
        out[i].cam.vh = (float)rects[i].h;

        // Held inside this pane, once the pane and its zoom are known - a car
        // stuck high above the ground is otherwise followed only partly and
        // stays above the top edge for the rest of the race.
        out[i].cam.cz = gs_cam_hold(out[i].cam.cz, gs_to_f(c.z),
                                    out[i].cam.zoom, (float)rects[i].h);
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

void gs_render_track_camera(gs_view *view, const gs_track *t,
                            const gs_world *prev, const gs_world *now,
                            float alpha) {
    if (view->car >= now->car_count) return;
    gs_car c = gs_car_lerp(&prev->car[view->car], &now->car[view->car], alpha);

    view->cam.cx = gs_to_f(c.x);
    view->cam.cy = gs_to_f(c.y);
    view->cam.cz = gs_cam_height(t, &c);
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
    // **The surround is drawn too, because it is ground a car can be on.**
    // Before this, nothing outside the authored tiles was drawn and the physics
    // clamped to the edge - so a player saw the track end in blackness and then
    // kept driving on an invisible plain. What you can see and what you can
    // drive on now agree: the shoulder is there, and so is the lip it falls
    // away over.
    //
    // Far enough out to show the drop starting. Beyond that the ground is a long
    // way below and there is nothing to learn from more of it.
    const int fringe = GS_RUNOFF_TILES + 6;

    int diagonals = (int)t->w + (int)t->h - 1 + fringe * 2;
    for (int d = 0; d < diagonals; d++) {
        for (int i = 0; i <= d; i++) {
            int x = i - fringe;
            int y = d - i - fringe;
            if (x >= (int)t->w + fringe || y >= (int)t->h + fringe) continue;
            if (x < -fringe || y < -fringe) continue;
            if (!gs_tile_in_view(&cam, t, x, y)) continue;
            gs_draw_tile(ren, &cam, t, x, y, view->show_gravity, view->heat);
            gs_draw_kerb(ren, &cam, t, x, y);
        }

        // The route's marks go down with the ground they are painted on, so a
        // gate beyond a rise is hidden by the rise rather than floating over
        // it. Same trick as the cars below: drawn when the sweep reaches the
        // diagonal the gate is on.
        for (uint8_t gi = 0; gi < t->gate_count; gi++) {
            gs_draw_gate(ren, &cam, t, &t->gate[gi], gi == 0, d - fringe * 2);
        }

        // The flags stand up out of the world rather than being painted on it,
        // so each one sorts on the tile it is planted in and not on the gate's
        // - exactly as a car does. Drawn at the gate's own diagonal instead,
        // the flag at the near end of the line is painted over by every tile
        // the sweep reaches afterwards, and a line that should have a flag at
        // both ends is drawn with one.
        if (t->gate_count > 0) {
            float lx = 0.0f, ly = 0.0f;
            gs_start_lights_at(t, &lx, &ly);
            if ((int)SDL_floorf(lx) + (int)SDL_floorf(ly) == d - fringe * 2) {
                gs_draw_start_lights(ren, &cam, t, now, lx, ly);
            }

            float feet[2][2];
            gs_flag_feet(&t->gate[0], feet);
            for (int k = 0; k < 2; k++) {
                int fd = (int)SDL_floorf(feet[k][0]) + (int)SDL_floorf(feet[k][1]);
                if (fd == d - fringe * 2) {
                    gs_draw_flag(ren, &cam, t, feet[k][0], feet[k][1],
                                 gs_to_f(t->gate[0].x), gs_to_f(t->gate[0].y));
                }
            }
        }
        // The sweep counts from the fringe, so the world diagonal this pass is
        // drawing is `d` shifted back by the two tiles of margin it starts
        // outside on each axis. Comparing a car's own diagonal against the raw
        // loop counter draws every car twenty tiles too early, in front of
        // terrain that should be hiding it.
        int world_d = d - fringe * 2;

        for (uint8_t i = 0; i < now->car_count; i++) {
            gs_car c = gs_car_lerp(&prev->car[i], &now->car[i], alpha);
            if (gs_car_diagonal(&c) == world_d) {
                gs_draw_car(ren, &cam, t, &c, i, 1.0f);
            }
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
    // Cars beyond even the fringe are past every diagonal the sweep covered, so
    // they are drawn after everything - which is where they are.
    for (uint8_t i = 0; i < now->car_count; i++) {
        gs_car c = gs_car_lerp(&prev->car[i], &now->car[i], alpha);
        int cd = gs_car_diagonal(&c);
        if (cd >= diagonals - fringe * 2 || cd < -fringe * 2) {
            gs_draw_car(ren, &cam, t, &c, i, 1.0f);
        }
    }

    // The landing arc last of all, over everything, and only for the driver of
    // this view. Predicted from the settled state rather than the interpolated
    // one: the arc is a question about the simulation, and half way between two
    // ticks is not a state the simulation was ever in.
    if (view->show_arc && view->car < now->car_count) {
        gs_draw_arc(ren, &cam, t, now, view->car);
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

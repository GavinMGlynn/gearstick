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
// How far to the side of the start line the light tree stands, in tiles. Beside
// the road rather than outside the gate: a gate spans the road and then some,
// and hanging the tree off *its* edge put the tree off the side of the screen
// the moment gates were widened.
#define GS_LIGHTS_OUT     5.0f

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
// --- Winning, made obvious ---------------------------------------------------
//
// **"It is not obvious that you have won the race."** Crossing the line moved
// a number on a panel and nothing else: no moment, no reward, and the whole
// point of a race arriving without any ceremony at all. So a finish now
// *happens* - the flags wave, fireworks go up over the line, and confetti
// falls across the view.
//
// **All of it derived, none of it stored.** Every particle's position is a
// function of the finishing tick, the world's tick and its own index, so
// there is no particle system, no allocation, nothing in gs_world and no
// golden hash moved - and two machines watching the same race, or one
// machine watching a replay of it, see the identical celebration because
// they are looking at the same arithmetic. The simulation does not know that
// anybody is cheering.

// How long the party lasts, in ticks, and how long a single firework does.
#define GS_PARTY_TICKS   (GS_TICK_HZ * 8u)
#define GS_ROCKET_TICKS  (GS_TICK_HZ / 2u)
#define GS_ROCKETS       7
#define GS_CONFETTI      140

// A cheap deterministic hash, for scattering things that must scatter the
// same way on every machine. Not the simulation's RNG: this decides nothing
// and is allowed to be whatever is fastest to read.
static float gs_party_rand(uint32_t seed) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return (float)(seed & 0xffffffu) / (float)0x1000000u;
}

// How far into the party this car is, 0 before it finished and past 1 once
// the celebrating is done. The one number every piece below is drawn from.
static float gs_party_age(const gs_world *w, const gs_car *c) {
    if (c->finish_tick == 0 || w->tick < c->finish_tick) return 0.0f;
    const uint32_t since = (uint32_t)w->tick - c->finish_tick;
    return (float)since / (float)GS_PARTY_TICKS;
}

// Is anybody on this screen celebrating, and how far along is the first of
// them? The flags wave for whoever finished first, because the line belongs
// to the race rather than to a driver.
static float gs_party_here(const gs_world *w) {
    float best = 0.0f;
    for (uint8_t i = 0; i < w->car_count; i++) {
        const float age = gs_party_age(w, &w->car[i]);
        if (age > 0.0f && age < 1.0f && (best == 0.0f || age < best)) best = age;
    }
    return best;
}

static void gs_draw_flag(SDL_Renderer *ren, const gs_camera *cam,
                         const gs_track *t, float bx, float by,
                         float mid_x, float mid_y, float wave) {
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
    //
    // **And it waves once somebody has won.** The wave is a travelling sine
    // down the flag's length, so the squares furthest from the pole move
    // most - which is how cloth behaves and how a chequered flag is waved.
    // `wave` is zero at every other moment and the flag is exactly the still
    // one it always was.
    const int across = 4, down = 3;
    for (int i = 0; i < across; i++) {
        const float along = (float)i / (float)across;
        const float swing = wave * along * flag_h * 0.55f *
                            SDL_sinf(wave * 26.0f - along * 5.0f);
        for (int j = 0; j < down; j++) {
            float x0 = px + away * flag_w * (float)i / (float)across;
            float x1 = px + away * flag_w * (float)(i + 1) / (float)across;
            float y0 = top + flag_h * (float)j / (float)down + swing;
            float y1 = top + flag_h * (float)(j + 1) / (float)down + swing;

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

    // **Beside the road, not beside the line.** This stood a fixed distance
    // outside the gate's own edge, and when gates were widened to span the road
    // properly the tree went with them - three tiles further out, off the side
    // of the screen at the zoom a race is actually driven at. A player looked
    // for it and it was not there.
    //
    // Tied to the road instead, which is what it stands beside and what does
    // not change when a gate's width does. Pulled in if the gate is narrower
    // than that, so on a tight track it is never out past its own start line.
    float out = GS_LIGHTS_OUT;
    float edge = gs_to_f(g->half_width) + 0.9f;
    if (out > edge) out = edge;

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

    // **Red, then amber, then green** - the three colours a driver reads without
    // having to count anything. Red is the whole of the wait; amber is the last
    // few seconds of it and means get ready; green is go, and go is the tick
    // the simulation stops holding the cars.
    uint32_t left = gs_world_countdown(w);
    uint32_t amber_from = (uint32_t)GS_TICK_HZ * GS_AMBER_SECONDS;

    bool green = left == 0 && w->green_tick > 0 &&
                 w->tick < (uint64_t)w->green_tick + GS_GREEN_TICKS;
    bool amber = left > 0 && left <= amber_from;
    bool red = left > amber_from;

    // Within red, the lamps still fall away as the wait shortens, so the tree
    // says roughly how long is left as well as what to do - three lit a long
    // way out, one just before the amber.
    int lit = 0;
    if (red) {
        uint32_t red_for = GS_COUNTDOWN_TICKS - amber_from;
        uint32_t through = left - amber_from;          // 1 .. red_for
        lit = 1 + (int)((through * (uint32_t)GS_COUNTDOWN_LAMPS - 1u) / red_for);
        if (lit < 1) lit = 1;
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
        } else if (amber) {
            c = (SDL_FColor){ 0.98f, 0.62f, 0.08f, 1.0f };
        } else if (red && i < lit) {
            c = (SDL_FColor){ 0.96f, 0.16f, 0.12f, 1.0f };
        } else {
            c = (SDL_FColor){ 0.20f, 0.10f, 0.09f, 1.0f };
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
// **The last tile a shape actually covers, on one axis.**
//
// `floor` is wrong on the closed edge and only there: a shape whose far side
// lands exactly on a tile line - a hazard of radius one tile dropped at a whole
// number, which is where a car standing still leaves one - covers none of the
// tile beyond that line, but `floor(25.0)` says 25 and hands the shape a
// diagonal a whole tile nearer than it reaches. Sorted there it is painted
// after the car standing on it, and 62 pixels of the car go with it.
//
// `ceil(v) - 1` is the tile holding the last point inside the shape, which is
// the tile that ought to answer for it. For anything not exactly on a line it
// is `floor` to the pixel; on a line it is the tile the shape is really in.
static int gs_last_tile(float v) {
    return (int)SDL_ceilf(v) - 1;
}

static int gs_ground_quad_diagonal(const float x[4], const float y[4]) {
    int d = gs_last_tile(x[0]) + gs_last_tile(y[0]);
    for (int i = 1; i < 4; i++) {
        int e = gs_last_tile(x[i]) + gs_last_tile(y[i]);
        if (e > d) d = e;
    }
    return d;
}

// **The biggest a piece of ground paint may be, in tiles.**
//
// The sweep decides depth one tile diagonal at a time, and a shape's diagonal
// is `floor(x) + floor(y)` at its *nearest* corner. That is the right answer
// against the terrain - a mark must not sink into the ground it lies on - and
// the wrong one against anything standing between the shape's near end and its
// far end, because the whole shape is then painted at the near end's turn, on
// top of it.
//
// Half a tile is small enough that a piece and a car cannot both want the same
// diagonal and disagree about which is in front.
#define GS_MARK_PIECE 0.5f

// **A mark on the ground, drawn a piece at a time, each piece where it lies.**
//
// The chequered line was cut into blocks for exactly this reason. The arrow was
// not: a shaft and a head spanning two and a half tiles stayed one shape, so an
// arrow pointing at the camera was painted at the tile its head reached and
// took with it everything standing on its tail. A car sitting on the shaft lost
// 339 pixels of itself to it - and, driving, lost and regained them tile by
// tile, which is the flicker.
//
// Every mark goes through here now, so the next one somebody adds is cut up
// without having to remember to do it. The split is by the shape's own size, so
// a mark already smaller than a piece - a route dash, a block of the chequer -
// is still one piece and costs what it always did.
//
// **A mark is only whole across the whole sweep.** Each call draws the pieces
// belonging to the one diagonal it is handed and skips the rest, so the sweep
// calling it once per diagonal is what assembles the shape.
//
// That makes "which diagonal" a thing the caller cannot invent, and the reason
// there are two entry points below rather than one integer argument. A caller
// outside the sweep has no diagonal to give; passing the mark's own - the
// obvious thing to reach for, and what the missed-checkpoint arrow did - draws
// only the pieces sharing the furthest corner's diagonal and silently loses the
// others, a different few each time the car moves. That is a flicker, and it is
// the *second* one of this family: the note above is the first.
static void gs_ground_mark_pieces(SDL_Renderer *ren, const gs_camera *cam,
                                  const gs_track *t, const float x[4],
                                  const float y[4], float lift, SDL_FColor c,
                                  int world_d, bool every) {
    // How far the quad reaches along its own two axes rather than the world's,
    // so a mark lying diagonally is no bigger than it looks.
    float u = SDL_max(SDL_fabsf(x[1] - x[0]) + SDL_fabsf(y[1] - y[0]),
                      SDL_fabsf(x[2] - x[3]) + SDL_fabsf(y[2] - y[3]));
    float v = SDL_max(SDL_fabsf(x[3] - x[0]) + SDL_fabsf(y[3] - y[0]),
                      SDL_fabsf(x[2] - x[1]) + SDL_fabsf(y[2] - y[1]));

    int nu = SDL_clamp((int)SDL_ceilf(u / GS_MARK_PIECE), 1, 16);
    int nv = SDL_clamp((int)SDL_ceilf(v / GS_MARK_PIECE), 1, 16);

    if (nu == 1 && nv == 1) {
        if (every || gs_ground_quad_diagonal(x, y) == world_d) {
            gs_ground_quad(ren, cam, t, x, y, lift, c);
        }
        return;
    }

    // Bilinear across the quad, so a triangle - a quad with two corners in the
    // same place - divides into pieces that close up at the tip rather than
    // into something with a hole in it.
    static const int cu[4] = { 0, 1, 1, 0 }, cv[4] = { 0, 0, 1, 1 };
    for (int iv = 0; iv < nv; iv++) {
        for (int iu = 0; iu < nu; iu++) {
            const float us[2] = { (float)iu / (float)nu, (float)(iu + 1) / (float)nu };
            const float vs[2] = { (float)iv / (float)nv, (float)(iv + 1) / (float)nv };

            float px[4], py[4];
            for (int k = 0; k < 4; k++) {
                const float a = us[cu[k]], b = vs[cv[k]];
                const float tx = x[0] + (x[1] - x[0]) * a;
                const float ty = y[0] + (y[1] - y[0]) * a;
                const float bx = x[3] + (x[2] - x[3]) * a;
                const float by = y[3] + (y[2] - y[3]) * a;
                px[k] = tx + (bx - tx) * b;
                py[k] = ty + (by - ty) * b;
            }

            if (!every && gs_ground_quad_diagonal(px, py) != world_d) continue;
            gs_ground_quad(ren, cam, t, px, py, lift, c);
        }
    }
}

// **In the sweep**, at the diagonal the sweep is currently drawing. The mark
// comes out whole once every diagonal has had its turn, and sorts correctly
// against whatever is standing on it.
static void gs_ground_mark(SDL_Renderer *ren, const gs_camera *cam,
                           const gs_track *t, const float x[4], const float y[4],
                           float lift, SDL_FColor c, int world_d) {
    gs_ground_mark_pieces(ren, cam, t, x, y, lift, c, world_d, false);
}

// **After the sweep, over everything** - for a mark that is a readout rather
// than scenery, like the way back to a checkpoint that was driven past. There
// is nothing left to sort against, so every piece is drawn and there is no
// diagonal to pass or to get wrong.
static void gs_ground_mark_over_everything(SDL_Renderer *ren,
                                           const gs_camera *cam,
                                           const gs_track *t, const float x[4],
                                           const float y[4], float lift,
                                           SDL_FColor c) {
    gs_ground_mark_pieces(ren, cam, t, x, y, lift, c, 0, true);
}

// Where the chequered line's two flags stand: at its ends and a little outside
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

// What job a gate does, which is the whole difference between a track that
// says where it goes and one that does not.
//
// **A loop's start line is its finish line.** One chequered line, crossed at
// the beginning of every lap and at the end of every lap, and drawing a second
// line for the start would be inventing a distinction the track does not have.
// A path has two lines and they are a long way apart: a plain white one where
// you begin and a chequered one where you arrive.
typedef enum gs_gate_role {
    GS_GATE_WAYPOINT = 0,  // through here, on the way
    GS_GATE_START,         // a sprint's beginning
    GS_GATE_FINISH,        // a sprint's end
    GS_GATE_BOTH           // a circuit's start and finish, one line
} gs_gate_role;

static gs_gate_role gs_gate_role_of(const gs_track *t, uint8_t i) {
    if (gs_track_is_circuit(t)) {
        return i == 0 ? GS_GATE_BOTH : GS_GATE_WAYPOINT;
    }
    if (i == 0) return GS_GATE_START;
    if (i == t->gate_count - 1) return GS_GATE_FINISH;
    return GS_GATE_WAYPOINT;
}

static bool gs_gate_is_chequered(gs_gate_role role) {
    return role == GS_GATE_FINISH || role == GS_GATE_BOTH;
}

// **A gate is drawn a piece at a time, each piece where it lies.** The band
// reaches right across the track, so it covers a wide span of diagonals while
// the gate has only one - and drawn all at once at the gate's own diagonal, the
// far half of the line is painted over every car that is nearer than the gate
// centre. A player saw exactly that: "the car just went behind the finish
// line". Each block and each piece of the arrow now sorts on the ground it
// actually lies on, so a car on this side of the line is drawn in front of it
// and a car beyond it is drawn behind.
// **The route between the gates, not only at them.**
//
// A gate carries an arrow saying which way through it, and at racing zoom you
// see one arrow at a time with no road edge in frame - which is not a route
// indicator, it is a hint you have to have already understood. A player looked
// at a gentle left-to-right sprint and read it as two switchback turns, and was
// right to: nothing on the screen said otherwise. So the way round is painted
// on the ground the whole way, and reads at any zoom from anywhere on it.
//
// **Through the gates rather than between them.** A straight chord cuts the
// corner - on a four gate loop it would draw a line straight across the infield
// and tell the player to drive into the scenery - so the line is a Catmull-Rom
// through the gate positions, which is the same shape the generator lays its
// road along and close enough to a hand-built one to sit on the tarmac.
// Dashed rather than solid, because a solid line across a track reads as a kerb
// or a wall - the two things already painted as continuous strips - and because
// a dash has a direction a player can see it marching in.
#define GS_ROUTE_STEPS 16          // samples along one leg
#define GS_ROUTE_HALF  0.16f       // half the width of the line, in tiles

static void gs_draw_route(SDL_Renderer *ren, const gs_camera *cam,
                          const gs_track *t, int world_d) {
    if (t->gate_count < 2) return;

    uint8_t legs = gs_track_route_legs(t);

    // The blue the route has always been painted in - the same blue as a
    // waypoint post's head - so a player who has learned that colour keeps it.
    static const SDL_FColor ink = { 0.30f, 0.65f, 0.95f, 0.75f };

    for (uint8_t leg = 0; leg < legs; leg++) {
        for (int k = 0; k < GS_ROUTE_STEPS; k++) {
            // Every other sample, which is what makes it dashed. The first and
            // last of each leg are left out so the dashes do not run into the
            // gate's own line and arrow.
            if ((k & 1) != 0 || k == 0 || k == GS_ROUTE_STEPS - 1) continue;

            // The curve itself comes from the simulation, so the line drawn
            // here and the line drawn on the minimap are the same line.
            gs_fix fax, fay, fbx, fby;
            gs_track_route_point(t, leg, (gs_fix)((int64_t)k * GS_ONE / GS_ROUTE_STEPS),
                                 &fax, &fay);
            gs_track_route_point(t, leg, (gs_fix)((int64_t)(k + 1) * GS_ONE / GS_ROUTE_STEPS),
                                 &fbx, &fby);
            float ax = gs_to_f(fax), ay = gs_to_f(fay);
            float bx = gs_to_f(fbx), by = gs_to_f(fby);

            float dx = bx - ax, dy = by - ay;
            float len = SDL_sqrtf(dx * dx + dy * dy);
            if (len < 0.0001f) continue;
            float px = -dy / len * GS_ROUTE_HALF, py = dx / len * GS_ROUTE_HALF;

            float qx[4] = { ax + px, bx + px, bx - px, ax - px };
            float qy[4] = { ay + py, by + py, by - py, ay - py };

            // Sorted onto the ground it is painted on, like every other mark
            // the route puts down, or a dash beyond a rise floats over it.
            gs_ground_mark(ren, cam, t, qx, qy, 0.03f, ink, world_d);
        }
    }
}

static void gs_draw_gate(SDL_Renderer *ren, const gs_camera *cam,
                         const gs_track *t, const gs_gate *g,
                         gs_gate_role role, int world_d) {
    float gx = gs_to_f(g->x), gy = gs_to_f(g->y);
    float hw = gs_to_f(g->half_width);

    // The way through, and the line across it.
    float fx = gs_to_f(gs_cos(g->heading)), fy = gs_to_f(gs_sin(g->heading));
    float sx = -fy, sy = fx;

    // A line across the road, for the two gates that have one. The chequer is
    // two rows deep so the squares alternate across the line as well as along
    // it - one row of alternating blocks is a dashed line rather than a
    // chequered one, which is what this was and what a picture of it showed.
    // The start line is a single plain row, because it is not a finish and must
    // not look like one.
    bool chequered = gs_gate_is_chequered(role);
    bool lined = chequered || role == GS_GATE_START;

    if (lined) {
        float half_depth = chequered ? GS_LINE_HALF_DEPTH : GS_LINE_HALF_DEPTH * 0.5f;
        int rows = chequered ? 2 : 1;
        float row = (2.0f * half_depth) / (float)rows;

        // Square blocks: how many fit across follows the depth of a row and not
        // the width of the gate, so the chequer does not stretch on a wide one.
        int blocks = (int)((2.0f * hw) / (chequered ? row : row * 0.5f) + 0.5f);
        if (blocks < 2) blocks = 2;
        if (blocks > 28) blocks = 28;

        for (int r = 0; r < rows; r++) {
            float d0 = -half_depth + row * (float)r, d1 = d0 + row;

            for (int i = 0; i < blocks; i++) {
                float a = -hw + (2.0f * hw) * (float)i / (float)blocks;
                float b = -hw + (2.0f * hw) * (float)(i + 1) / (float)blocks;

                float x[4] = { gx + sx * a + fx * d0, gx + sx * b + fx * d0,
                               gx + sx * b + fx * d1, gx + sx * a + fx * d1 };
                float y[4] = { gy + sy * a + fy * d0, gy + sy * b + fy * d0,
                               gy + sy * b + fy * d1, gy + sy * a + fy * d1 };

                SDL_FColor c = chequered
                    ? (((i + r) & 1) ? (SDL_FColor){ 0.08f, 0.08f, 0.09f, 1.0f }
                                     : (SDL_FColor){ 0.95f, 0.95f, 0.95f, 1.0f })
                    : (SDL_FColor){ 0.95f, 0.95f, 0.95f, 1.0f };
                gs_ground_mark(ren, cam, t, x, y, 0.04f, c, world_d);
            }
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
    gs_ground_mark(ren, cam, t, ax, ay, 0.05f, tip, world_d);

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
    gs_ground_mark(ren, cam, t, hx, hy, 0.05f, tip, world_d);

    // A gate with no line across it is marked at its edges and left open in the
    // middle, the way a rally stage is.
    if (role == GS_GATE_WAYPOINT) {
        for (int k = 0; k < 2; k++) {
            float side = k == 0 ? 1.0f : -1.0f;
            float bx = gx + sx * hw * side, by = gy + sy * hw * side;
            if ((int)SDL_floorf(bx) + (int)SDL_floorf(by) == world_d) {
                gs_draw_post(ren, cam, t, bx, by);
            }
        }
    }
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

    // The same "last tile it covers" rule the ground paint answers by; a car
    // and the paint under it disagreeing on where a tile line falls is the
    // whole of what this sorting has to get right.
    int d = gs_last_tile(fx[0]) + gs_last_tile(fy[0]);
    for (int i = 1; i < 4; i++) {
        int e = gs_last_tile(fx[i]) + gs_last_tile(fy[i]);
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

// **What the splitter owns, and what it must leave alone.**
//
// It decides where each view looks and how big it is. Everything else on a
// `gs_view` belongs to whoever set it - which overlay is on, whether the arc is
// showing, the analyser's heatmap, and whether this driver has been told they
// drove past a checkpoint. Those are wiped by `out[i] = (gs_view){ 0 }` and
// then put back one field at a time by the frontend, which worked until
// somebody added a field and did not add a line: the missed-checkpoint warning
// was set every tick and destroyed every frame, so it never reached a screen.
//
// So the fields this function owns are named, and the rest is carried across
// from whatever the caller had in `out`. Adding a field to gs_view is now safe
// by default rather than safe if you remember.
static void gs_view_place(gs_view *v, uint8_t car, SDL_Rect rect) {
    v->car = car;
    v->rect = rect;
}

uint8_t gs_split_views(const gs_split *s, const gs_track *t,
                       const gs_world *prev, const gs_world *w, float alpha,
                       int win_w, int win_h, gs_view *out) {
    if (s->merge >= 1.0f || w->car_count <= 1) {
        gs_view_place(&out[0], 0, (SDL_Rect){ 0, 0, win_w, win_h });
        out[0].cam = s->shared;
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
        gs_view_place(&out[i], i, rects[i]);

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

    // **Three, and this is the answer to the open question.** Three players
    // used to get the four-player grid with one quarter of the screen left
    // blank - a quarter of a 1280x720 window, 230,400 pixels of nothing, and
    // the three people racing each squeezed into a box a quarter the size while
    // it sat there.
    //
    // Three columns instead: equal area and equal shape, because an unequal
    // pane is an advantage and this is a game people play on one sofa. Columns
    // rather than rows to match the two-player split, so going from two players
    // to three changes how many panes there are and not which way they run.
    //
    // The last one takes the remainder, so the three of them tile the window
    // exactly rather than leaving a seam that widens with the window.
    if (views == 3) {
        const int third = (w - gap * 2) / 3;
        out[0] = (SDL_Rect){ 0, 0, third, h };
        out[1] = (SDL_Rect){ third + gap, 0, third, h };
        out[2] = (SDL_Rect){ (third + gap) * 2, 0, w - (third + gap) * 2, h };
        return 3;
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

// **What a hazard looks like, and whether it has a look at all.**
//
// Pulled out of the draw so the paint and the sorting are separate questions.
static bool gs_hazard_paint(const gs_hazard *h, SDL_FColor *out) {
    const gs_hazard_kind kind = (gs_hazard_kind)h->kind;

    // **Four things, four looks**, named one by one so a fifth kind has to be
    // given one rather than inheriting whatever the last case was.
    //
    // Started at nothing and refused if it stays there. That is not
    // belt-and-braces: `kind` comes from a `uint8_t` in the world, so it can
    // hold a number this switch has no case for - and MSVC says so where gcc
    // and clang do not. The `-Wswitch` guarantee is untouched by it, because
    // there is still no `default` for a new enumerator to hide in.
    SDL_FColor col = { 0.0f, 0.0f, 0.0f, 0.0f };
    switch (kind) {
    case GS_HAZ_OIL:
        // Dark and see-through: the road is still under it, which is what makes
        // it something to be driven through rather than avoided.
        col = (SDL_FColor){ 0.05f, 0.04f, 0.10f, 0.72f };
        break;
    case GS_HAZ_MINE:
        // Small and bright. The one thing a player must be able to do is see it
        // in time.
        col = (SDL_FColor){ 1.0f, 0.45f, 0.1f, 0.95f };
        break;
    case GS_HAZ_SMOKE:
        // **Pale and nearly solid, because hiding the ground is the whole of
        // what it does.** Anything you can see the road through is a grey patch
        // rather than a screen.
        col = (SDL_FColor){ 0.78f, 0.79f, 0.82f, 0.93f };
        break;
    case GS_HAZ_FLAME:
        // Hot: yellow where a mine is orange, and wider, so the two are told
        // apart at a glance by colour and by size at once.
        col = (SDL_FColor){ 1.0f, 0.82f, 0.20f, 0.88f };
        break;
    case GS_HAZ_NONE:
    case GS_HAZ_COUNT:
        break;
    }
    if (col.a <= 0.0f) return false;

    // **Going out is something you can see.** Smoke and fire have a clock, and
    // one about to expire fades over its last third - so a driver can tell the
    // fire they can wait out from the fire they cannot.
    const uint16_t full = gs_hazard_life(kind);
    if (full > 0) {
        const float left = (float)h->life / (float)full;
        if (left < 0.34f) col.a *= left / 0.34f;
    }

    *out = col;
    return true;
}

// **The hazards on this diagonal.** Flat, because they are things on the road
// rather than things standing on it - and each one **at the size the simulation
// will actually catch you at**, asked of gs_hazard_radius rather than guessed
// at here. A slick drawn narrower than it is is a lie, and it is the kind a
// player learns to distrust the physics over.
//
// **Drawn in the sweep, which is what "on the ground" has to mean.** They used
// to be drawn in a pass of their own after it, which put every one of them on
// top of every car - so a car sitting in the slick it had just laid was painted
// out completely, all 6,491 pixels of it, and the comment here said they went
// under everything that moves. They are paint, so they go down with the paint.
static void gs_draw_hazards(SDL_Renderer *ren, const gs_camera *cam,
                            const gs_track *t, const gs_world *w, int world_d) {
    for (uint8_t i = 0; i < w->hazard_count; i++) {
        const gs_hazard *h = &w->hazard[i];
        if (h->kind == GS_HAZ_NONE || h->spent) continue;

        SDL_FColor col;
        if (!gs_hazard_paint(h, &col)) continue;

        const float hx = gs_to_f(h->x), hy = gs_to_f(h->y);
        const float r = gs_to_f(gs_hazard_radius((gs_hazard_kind)h->kind));

        static const float ox[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
        static const float oy[4] = { -1.0f, -1.0f, 1.0f, 1.0f };
        float x[4], y[4];
        for (int k = 0; k < 4; k++) {
            x[k] = hx + ox[k] * r;
            y[k] = hy + oy[k] * r;
        }

        // Cut up like every other mark: smoke is over two tiles across, which
        // is three diagonals of ground for one shape to answer for.
        gs_ground_mark(ren, cam, t, x, y, 0.02f, col, world_d);
    }
}

void gs_view_note_missed(gs_view *views, uint8_t count, const gs_track *t,
                         const gs_world *was, const gs_world *now) {
    if (views == nullptr || t == nullptr || was == nullptr || now == nullptr) return;
    if (t->gate_count == 0) return;

    for (uint8_t i = 0; i < now->car_count && i < GS_MAX_CARS; i++) {
        const gs_car *before = &was->car[i];
        const gs_car *after = &now->car[i];
        if (!after->active) continue;

        // Took the gate it owed: whatever it had missed is put right.
        const bool took_one = after->next_gate != before->next_gate;

        const gs_gate *g = &t->gate[before->next_gate % t->gate_count];
        const bool drove_past =
            !took_one &&
            gs_gate_missed(g, before->x, before->y, after->x, after->y);

        if (!took_one && !drove_past) continue;

        for (uint8_t v = 0; v < count; v++) {
            if (views[v].car != i) continue;
            views[v].missed = drove_past;
            if (drove_past) views[v].missed_at = before->next_gate;
        }
    }
}

// **A car on the tow truck's hook flashes.** Off-beats are simply not drawn:
// the flash says "this car is being moved, not driven" and it ends the frame
// the car lands, solid, at its checkpoint. Derived from the simulation's own
// counter rather than from a frame clock, so every machine watching the same
// race sees the same flashes on the same ticks.
static bool gs_tow_blinked_off(const gs_car *c) {
    return c->tow_ticks > 0 && (c->tow_ticks / GS_TOW_BLINK) % 2u == 1u;
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
    // Every mark on the ground has an alpha, and the first of them is drawn
    // before the first car is - which used to be what turned blending on.
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

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
        // The way round, painted before the gates so a gate's own line and
        // arrow sit on top of it rather than under it.
        gs_draw_route(ren, &cam, t, d - fringe * 2);

        for (uint8_t gi = 0; gi < t->gate_count; gi++) {
            gs_draw_gate(ren, &cam, t, &t->gate[gi], gs_gate_role_of(t, gi),
                         d - fringe * 2);
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
            gs_flag_feet(&t->gate[gs_track_finish_gate(t)], feet);
            for (int k = 0; k < 2; k++) {
                int fd = (int)SDL_floorf(feet[k][0]) + (int)SDL_floorf(feet[k][1]);
                if (fd == d - fringe * 2) {
                    const gs_gate *fg = &t->gate[gs_track_finish_gate(t)];
                    gs_draw_flag(ren, &cam, t, feet[k][0], feet[k][1],
                                 gs_to_f(fg->x), gs_to_f(fg->y),
                                 gs_party_here(now));
                }
            }
        }
        // The hazards on the ground of this diagonal, painted before the cars
        // that drive through them, like the route and the gates above.
        gs_draw_hazards(ren, &cam, t, now, d - fringe * 2);

        // The sweep counts from the fringe, so the world diagonal this pass is
        // drawing is `d` shifted back by the two tiles of margin it starts
        // outside on each axis. Comparing a car's own diagonal against the raw
        // loop counter draws every car twenty tiles too early, in front of
        // terrain that should be hiding it.
        int world_d = d - fringe * 2;

        for (uint8_t i = 0; i < now->car_count; i++) {
            if (gs_tow_blinked_off(&now->car[i])) continue;
            gs_car c = gs_car_lerp(&prev->car[i], &now->car[i], alpha);
            if (gs_car_diagonal(&c) == world_d) {
                gs_draw_car(ren, &cam, t, &c, i, 1.0f);
            }
        }
    }

    // Cars that have driven off the authored track are past the last diagonal,
    // so they are drawn after everything - which is where they are.
    // Cars beyond even the fringe are past every diagonal the sweep covered, so
    // they are drawn after everything - which is where they are.
    for (uint8_t i = 0; i < now->car_count; i++) {
        if (gs_tow_blinked_off(&now->car[i])) continue;
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

    // **And the way back to a checkpoint that was driven past**, over
    // everything and only for this driver, for the same reason the arc is: it
    // is a readout rather than scenery, and a player who cannot see it is a
    // player still driving a lap that will not count.
    //
    // From the car towards the gate it owes, because "which way now" is the
    // question, and pointing at a line thirty tiles behind a hairpin is not an
    // answer anybody can act on from a route drawn on the ground.
    if (view->missed && view->car < now->car_count &&
        view->missed_at < t->gate_count) {
        const gs_car *me = &now->car[view->car];
        const gs_gate *owed = &t->gate[view->missed_at];

        float dx = gs_to_f(owed->x - me->x), dy = gs_to_f(owed->y - me->y);
        float len = SDL_sqrtf(dx * dx + dy * dy);
        if (len > 0.001f) {
            dx /= len; dy /= len;
            const float sx = -dy, sy = dx;

            // Short and near the car rather than a line all the way to the
            // gate: what is wanted is a direction, and a two tile arrow says it
            // without painting over the road somebody is trying to read.
            const float ox = gs_to_f(me->x), oy = gs_to_f(me->y);
            const float from = 1.2f, to = 2.6f, wide = 0.30f, barb = 0.75f;
            const SDL_FColor warn = { 1.0f, 0.35f, 0.20f, 0.85f };

            float shaft_x[4] = { ox + dx * from + sx * wide, ox + dx * to + sx * wide,
                                 ox + dx * to - sx * wide,   ox + dx * from - sx * wide };
            float shaft_y[4] = { oy + dy * from + sy * wide, oy + dy * to + sy * wide,
                                 oy + dy * to - sy * wide,   oy + dy * from - sy * wide };
            gs_ground_mark_over_everything(ren, &cam, t, shaft_x, shaft_y,
                                           0.06f, warn);

            float head_x[4] = { ox + dx * (to + barb),
                                ox + dx * to + sx * barb,
                                ox + dx * to - sx * barb,
                                ox + dx * (to + barb) };
            float head_y[4] = { oy + dy * (to + barb),
                                oy + dy * to + sy * barb,
                                oy + dy * to - sy * barb,
                                oy + dy * (to + barb) };
            gs_ground_mark_over_everything(ren, &cam, t, head_x, head_y,
                                           0.06f, warn);
        }
    }

    // --- **And the party**, over everything, because a win you cannot see is
    // not a win. Fireworks climb out of the finish line and burst; confetti
    // falls across the whole view. Every particle is arithmetic on the
    // finishing tick, so nothing is stored and every machine sees the same
    // celebration - see the block above gs_draw_flag.
    {
        const float age = gs_party_here(now);
        if (age > 0.0f && age < 1.0f) {
            const float vw = (float)view->rect.w, vh = (float)view->rect.h;

            // Where the line is on this screen, so the rockets come from it.
            float lx = vw * 0.5f, ly = vh * 0.35f;
            if (t->gate_count > 0) {
                const gs_gate *fg = &t->gate[gs_track_finish_gate(t)];
                const float gx = gs_to_f(fg->x), gy = gs_to_f(fg->y);
                float gz = gs_to_f(gs_track_height(t, fg->x, fg->y));
                gs_iso_project(&cam, gx, gy, gz, &lx, &ly);
            }

            // **Fireworks.** Each rocket has its own moment to go up and its
            // own colour; it rises, then bursts into a ring that falls and
            // fades. Drawn as small quads, which is what everything here is.
            for (int r = 0; r < GS_ROCKETS; r++) {
                const uint32_t seed = (uint32_t)r * 2654435761u ^ 0x9e37u;
                const float when = gs_party_rand(seed) * 0.55f;
                const float life = (age - when) /
                                   ((float)GS_ROCKET_TICKS /
                                    (float)GS_PARTY_TICKS);
                if (life < 0.0f || life > 2.4f) continue;

                const float side = (gs_party_rand(seed ^ 1u) - 0.5f) * vw * 0.55f;
                const float peak = vh * (0.18f + gs_party_rand(seed ^ 2u) * 0.22f);
                const SDL_FColor hue = {
                    0.45f + gs_party_rand(seed ^ 3u) * 0.55f,
                    0.45f + gs_party_rand(seed ^ 4u) * 0.55f,
                    0.45f + gs_party_rand(seed ^ 5u) * 0.55f, 1.0f };

                if (life <= 1.0f) {
                    // Climbing: a bright head with a short tail.
                    const float y = ly - peak * life;
                    const float x = lx + side * life;
                    const float sz = 3.0f;
                    const SDL_FPoint q[4] = {
                        { x - sz, y - sz }, { x + sz, y - sz },
                        { x + sz, y + sz }, { x - sz, y + sz } };
                    gs_quad(ren, q, hue);
                } else {
                    // Burst: a ring of sparks, spreading and falling.
                    const float b = life - 1.0f;           // 0..1.4
                    const float fade = b < 1.0f ? 1.0f - b * 0.7f : 0.3f;
                    const float spread = vh * 0.16f * b;
                    const float x0 = lx + side, y0 = ly - peak;
                    for (int k = 0; k < 12; k++) {
                        const float a = (float)k / 12.0f * 6.2831853f;
                        const float x = x0 + SDL_cosf(a) * spread;
                        const float y = y0 + SDL_sinf(a) * spread +
                                        vh * 0.10f * b * b;
                        const float sz = 2.4f;
                        const SDL_FColor c = { hue.r, hue.g, hue.b, fade };
                        const SDL_FPoint q[4] = {
                            { x - sz, y - sz }, { x + sz, y - sz },
                            { x + sz, y + sz }, { x - sz, y + sz } };
                        gs_quad(ren, q, c);
                    }
                }
            }

            // **Confetti**, falling the whole width of the view and tumbling
            // as it goes - each piece a rectangle whose width breathes, which
            // reads as paper turning over without costing a rotation.
            for (int i = 0; i < GS_CONFETTI; i++) {
                const uint32_t seed = (uint32_t)i * 2246822519u ^ 0x85ebu;
                const float lane = gs_party_rand(seed);
                const float speed = 0.55f + gs_party_rand(seed ^ 7u) * 0.85f;
                const float start = gs_party_rand(seed ^ 11u);

                // Falls from above the view, wraps, and thins out as the
                // party ends so it stops rather than being switched off.
                float fall = start + age * speed * 1.6f;
                if (fall > 1.0f) fall -= SDL_floorf(fall);
                if (age > 0.75f && gs_party_rand(seed ^ 13u) < (age - 0.75f) * 4.0f) {
                    continue;
                }

                const float x = lane * vw +
                                SDL_sinf(age * 9.0f + lane * 24.0f) * vw * 0.02f;
                const float y = fall * (vh + 40.0f) - 20.0f;
                const float w2 = 1.5f + 3.0f * SDL_fabsf(
                    SDL_sinf(age * 11.0f + gs_party_rand(seed ^ 17u) * 6.28f));
                const float h2 = 4.0f;

                const SDL_FColor c = {
                    0.35f + gs_party_rand(seed ^ 19u) * 0.65f,
                    0.35f + gs_party_rand(seed ^ 23u) * 0.65f,
                    0.35f + gs_party_rand(seed ^ 29u) * 0.65f, 0.95f };
                const SDL_FPoint q[4] = {
                    { x - w2, y - h2 }, { x + w2, y - h2 },
                    { x + w2, y + h2 }, { x - w2, y + h2 } };
                gs_quad(ren, q, c);
            }
        }
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

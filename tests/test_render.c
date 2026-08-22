// test_render.c - the facts the renderer is required to keep true, checked on
// the pixels it actually produced.
//
// **Separate from test_gearstick.c, and linking a different library.** Those
// tests link the simulation and nothing else, which is the standing proof that
// the simulation does not know it is being looked at. These need a window
// system, so they live here and link `gearstick_shell`. Keeping the two apart
// is what stops the proof quietly rotting.
//
// Everything here runs under the dummy video driver and the software renderer,
// so it needs no display and runs anywhere CI does.

#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "core/gs_sim.h"
#include "core/gs_track.h"
#include "gfx/gs_render.h"
#include "gfx/gs_meshes.h"
#include "platform/gs_bind.h"
#include "ui/gs_editor.h"
#include "platform/gs_paths.h"
#include "net/gs_auth.h"
#include "ui/gs_menu.h"
#include "ui/gs_hud.h"
#include "ui/gs_style.h"
#include "core/gs_ai.h"
#include "dcimgui.h"
#include "backends/dcimgui_impl_sdl3.h"
#include "backends/dcimgui_impl_sdlrenderer3.h"

#define GS_W 640
#define GS_H 480

static int gs_failures = 0;

// The window, for the Dear ImGui backend, which wants one as well as a
// renderer. The TEST macro passes only the renderer, because until the HUD
// nothing here drew anything that needed to know about a window.
static SDL_Window *gs_win = nullptr;
static const char *gs_current = "";

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("  FAIL %s\n    %s:%d: %s\n", gs_current, __FILE__,         \
                   __LINE__, #cond);                                           \
            gs_failures++;                                                     \
        }                                                                      \
    } while (0)

#define TEST(name)                                                             \
    static void name(SDL_Renderer *ren);                                       \
    static void run_##name(SDL_Renderer *ren) {                                \
        gs_current = #name;                                                    \
        name(ren);                                                             \
    }                                                                          \
    static void name(SDL_Renderer *ren)

// ---------------------------------------------------------------------------
// Rendering one frame into memory
// ---------------------------------------------------------------------------

typedef struct gs_frame {
    uint8_t *px;      // RGBA, GS_W * GS_H
    SDL_Surface *own;
} gs_frame;

static void gs_frame_free(gs_frame *f) {
    if (f->own != nullptr) SDL_DestroySurface(f->own);
    *f = (gs_frame){ 0 };
}

// The background is pure black so that "is this pixel terrain" is a question
// with an obvious answer. Nothing the renderer draws is anywhere near it.
static bool gs_is_background(const uint8_t *p) {
    return p[0] < 8 && p[1] < 8 && p[2] < 8;
}

// Car zero is drawn red, and nothing else in these scenes is: pavement is grey
// and the sky is black. So "how much of car zero can be seen" is a pixel count.
static bool gs_is_car0(const uint8_t *p) {
    return p[0] > 100 && p[0] > 2 * p[1] && p[0] > 2 * p[2];
}

// Car one is drawn blue. Ice is pale blue too, so the test is on the *ratio* to
// red rather than on brightness: ice has plenty of red in it and the car has
// almost none.
static bool gs_is_car1(const uint8_t *p) {
    return p[2] > 150 && p[2] > 2 * p[0];
}

static int gs_count_car1(const gs_frame *f) {
    int n = 0;
    for (int i = 0; i < GS_W * GS_H; i++) {
        if (gs_is_car1(&f->px[i * 4])) n++;
    }
    return n;
}

static gs_frame gs_render_frame(SDL_Renderer *ren, const gs_track *t,
                                const gs_world *prev, const gs_world *now,
                                float alpha, const gs_camera *cam) {
    gs_view view = { 0 };
    view.cam = *cam;
    view.rect = (SDL_Rect){ 0, 0, GS_W, GS_H };
    view.car = 0;

    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    gs_render_view(ren, t, prev, now, alpha, &view);

    gs_frame f = { 0 };
    SDL_Surface *raw = SDL_RenderReadPixels(ren, nullptr);
    if (raw == nullptr) return f;

    f.own = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(raw);
    if (f.own != nullptr) f.px = (uint8_t *)f.own->pixels;
    return f;
}

static int gs_count_car0(const gs_frame *f) {
    int n = 0;
    for (int i = 0; i < GS_W * GS_H; i++) {
        if (gs_is_car0(&f->px[i * 4])) n++;
    }
    return n;
}

// Where car zero is on screen, averaged over its pixels. Returns false if it is
// not visible at all.
static bool gs_car0_centroid(const gs_frame *f, double *cx, double *cy) {
    double sx = 0, sy = 0;
    int n = 0;
    for (int y = 0; y < GS_H; y++) {
        for (int x = 0; x < GS_W; x++) {
            if (gs_is_car0(&f->px[(y * GS_W + x) * 4])) {
                sx += x; sy += y; n++;
            }
        }
    }
    if (n == 0) return false;
    *cx = sx / n; *cy = sy / n;
    return true;
}

// Mean absolute difference per channel between two frames.
static double gs_frame_diff(const gs_frame *a, const gs_frame *b) {
    long total = 0;
    for (int i = 0; i < GS_W * GS_H * 4; i++) {
        int d = (int)a->px[i] - (int)b->px[i];
        total += d < 0 ? -d : d;
    }
    return (double)total / (double)(GS_W * GS_H * 4);
}

static void gs_flat_pavement(gs_track *t, uint8_t w, uint8_t h) {
    gs_track_init(t, w, h, GS_SURF_PAVEMENT);
}

static void gs_park_car(gs_world *w, const gs_track *t, gs_fix x, gs_fix y) {
    gs_world_init(w, GS_ONE);
    gs_world_add_car(w, t, (uint8_t)GS_VEH_STOCK_CAR, x, y, 0);
}

static gs_camera gs_camera_on(float cx, float cy, float cz) {
    return (gs_camera){ .cx = cx, .cy = cy, .cz = cz, .zoom = 1.0f,
                        .vw = (float)GS_W, .vh = (float)GS_H };
}

// ---------------------------------------------------------------------------

TEST(a_car_behind_a_rise_is_hidden_by_it) {
    // Isometric depth runs along x + y, with larger values nearer the viewer.
    // The car sits at depth 40; the wall in front of it at depth 44 and five
    // tiles tall. If the painter's sweep is drawing cars after the terrain
    // instead of among it, the car shows through the wall.
    //
    // **Out in the middle of a big track on purpose.** This counts red pixels
    // anywhere in the frame, which is what makes it strict - and the kerb that
    // marks the edge of a track is red and white, so a scene with an edge in it
    // would have this counting kerb as car. Twenty tiles of ground on every
    // side puts the edge out of shot and leaves the rule alone.
    static gs_track open_ground, walled;
    gs_flat_pavement(&open_ground, 48, 48);
    gs_flat_pavement(&walled, 48, 48);

    for (uint8_t y = 0; y <= walled.h; y++) {
        for (uint8_t x = 0; x <= walled.w; x++) {
            int d = (int)x + (int)y;
            if (d >= 44 && d <= 46) gs_track_set_corner(&walled, x, y, GS_INT(5));
        }
    }

    gs_world w;
    gs_park_car(&w, &open_ground, GS_INT(20), GS_INT(20));
    gs_camera cam = gs_camera_on(21.0f, 21.0f, 0.0f);

    gs_frame clear_view = gs_render_frame(ren, &open_ground, &w, &w, 1.0f, &cam);
    gs_frame blocked = gs_render_frame(ren, &walled, &w, &w, 1.0f, &cam);

    int visible = gs_count_car0(&clear_view);
    int through_the_wall = gs_count_car0(&blocked);

    CHECK(visible > 200);          // it is plainly there with nothing in the way
    CHECK(through_the_wall == 0);  // and not visible at all through five tiles of rock

    gs_frame_free(&clear_view);
    gs_frame_free(&blocked);
}

TEST(the_view_does_not_jump_as_a_car_crosses_a_tile_boundary) {
    // The failure this guards against is a camera quantised to whole tiles,
    // which makes the world lurch 32 pixels every time the car crosses a
    // boundary. It passes every unit test and is invisible in a screenshot -
    // you only see it in motion, so the test has to look at motion.
    static gs_track t;
    gs_flat_pavement(&t, 24, 24);

    // A ramp to give the ground some shape, so a frame-to-frame difference is
    // actually measurable rather than a field of identical grey.
    for (uint8_t y = 0; y <= t.h; y++) {
        for (uint8_t x = 0; x <= t.w; x++) {
            gs_track_set_corner(&t, x, y, (gs_fix)((int64_t)GS_INT(1) * x / 3));
        }
    }

    const int steps = 24;
    double diff[24];
    gs_frame prev_frame = { 0 };

    for (int i = 0; i <= steps; i++) {
        // From x = 8.5 to x = 9.5, straight over the boundary at 9.
        gs_fix x = GS_INT(8) + GS_HALF + (gs_fix)((int64_t)GS_ONE * i / steps);

        gs_world w;
        gs_park_car(&w, &t, x, GS_INT(12));
        gs_view v = { 0 };
        v.cam.zoom = 1.0f;
        gs_render_track_camera(&v, &t, &w, &w, 1.0f);

        gs_camera cam = gs_camera_on(v.cam.cx, v.cam.cy, v.cam.cz);
        gs_frame f = gs_render_frame(ren, &t, &w, &w, 1.0f, &cam);

        if (i > 0) diff[i - 1] = gs_frame_diff(&prev_frame, &f);
        gs_frame_free(&prev_frame);
        prev_frame = f;
    }
    gs_frame_free(&prev_frame);

    // Every step is the same size in world terms, so every frame-to-frame
    // difference should be about the same. A quantised camera would show one
    // step of nothing, then one enormous one.
    double biggest = 0, total = 0;
    for (int i = 0; i < steps; i++) {
        if (diff[i] > biggest) biggest = diff[i];
        total += diff[i];
    }
    double mean = total / steps;

    CHECK(mean > 0.0);                 // the view is moving at all
    CHECK(biggest < mean * 3.0);       // and no single step lurches
}

TEST(interpolation_places_a_car_between_the_two_ticks_it_sits_between) {
    // The world advances 120 times a second and frames do not. Without this the
    // difference shows as stutter at every frame rate that is not a multiple of
    // 120 - which is most of them.
    static gs_track t;
    gs_flat_pavement(&t, 24, 24);

    gs_world before, after;
    gs_park_car(&before, &t, GS_INT(8), GS_INT(12));
    gs_park_car(&after, &t, GS_INT(9), GS_INT(12));

    // A camera that does not follow the car, so what moves in the frame is the
    // car and not the world.
    gs_camera cam = gs_camera_on(8.5f, 12.0f, 0.0f);

    gs_frame f0 = gs_render_frame(ren, &t, &before, &after, 0.0f, &cam);
    gs_frame fh = gs_render_frame(ren, &t, &before, &after, 0.5f, &cam);
    gs_frame f1 = gs_render_frame(ren, &t, &before, &after, 1.0f, &cam);

    double x0 = 0, xh = 0, x1 = 0, y0 = 0, yh = 0, y1 = 0;
    bool got = gs_car0_centroid(&f0, &x0, &y0) &&
               gs_car0_centroid(&fh, &xh, &yh) &&
               gs_car0_centroid(&f1, &x1, &y1);
    CHECK(got);

    if (got) {
        CHECK(x0 < xh && xh < x1);
        // Halfway is halfway, to within a pixel or so of rasterisation.
        CHECK(SDL_fabs(xh - (x0 + x1) * 0.5) < 2.0);
    }

    gs_frame_free(&f0);
    gs_frame_free(&fh);
    gs_frame_free(&f1);
}

TEST(a_ramp_is_drawn_with_no_seam_and_no_hole) {
    // The reason terrain is emitted as geometry rather than assembled from a
    // tile atlas: arbitrary elevation joins have to close, and a hand-authored
    // tile set always has an edge or a hole baked into the wrong pick. A gap
    // would show here as background showing through the middle of the ground.
    static gs_track t;
    gs_flat_pavement(&t, 24, 24);

    for (uint8_t y = 0; y <= t.h; y++) {
        for (uint8_t x = 0; x <= t.w; x++) {
            gs_fix h;
            if (x <= 6) h = 0;
            else if (x >= 14) h = GS_INT(3);
            else h = (gs_fix)((int64_t)GS_INT(3) * (x - 6) / 8);
            // A cross-slope as well, so the tiles are twisted rather than
            // conveniently coplanar.
            h += (gs_fix)((int64_t)GS_INT(1) * y / 12);
            gs_track_set_corner(&t, x, y, h);
        }
    }

    gs_world w;
    gs_park_car(&w, &t, GS_INT(2), GS_INT(2));      // parked well out of shot
    gs_camera cam = gs_camera_on(11.0f, 11.0f, 1.5f);
    gs_frame f = gs_render_frame(ren, &t, &w, &w, 1.0f, &cam);

    // Down each column, find the first and last pixel of ground and count any
    // background between them. Solid ground has none.
    int holes = 0, columns_with_ground = 0;
    for (int x = 0; x < GS_W; x++) {
        int first = -1, last = -1;
        for (int y = 0; y < GS_H; y++) {
            if (!gs_is_background(&f.px[(y * GS_W + x) * 4])) {
                if (first < 0) first = y;
                last = y;
            }
        }
        if (first < 0 || last - first < 8) continue;
        columns_with_ground++;
        for (int y = first; y <= last; y++) {
            if (gs_is_background(&f.px[(y * GS_W + x) * 4])) holes++;
        }
    }

    CHECK(columns_with_ground > GS_W / 2);   // there is ground to inspect
    CHECK(holes == 0);

    gs_frame_free(&f);
}

TEST(picking_a_pixel_finds_the_ground_that_was_drawn_there) {
    (void)ren;   // this one is arithmetic, not pixels

    static gs_track t;
    gs_flat_pavement(&t, 24, 24);

    // Sloped, so the iteration in gs_iso_pick has something to converge on. A
    // flat track would pass even if the height feedback were missing entirely.
    for (uint8_t y = 0; y <= t.h; y++)
        for (uint8_t x = 0; x <= t.w; x++)
            gs_track_set_corner(&t, x, y, (gs_fix)((int64_t)GS_INT(1) * x / 4));

    gs_camera cam = gs_camera_on(12.0f, 12.0f, 1.0f);

    // Project a spread of known ground points, then pick the pixel each landed
    // on and check we get back where we started. Round trip, both directions.
    double worst = 0.0;
    for (float wy = 4.0f; wy < 20.0f; wy += 1.7f) {
        for (float wx = 4.0f; wx < 20.0f; wx += 1.3f) {
            gs_fix h = gs_track_height(&t, (gs_fix)(wx * GS_ONE), (gs_fix)(wy * GS_ONE));

            float sx = 0, sy = 0;
            gs_iso_project(&cam, wx, wy, gs_to_f(h), &sx, &sy);

            float px = 0, py = 0;
            bool on = gs_iso_pick(&cam, &t, sx, sy, &px, &py);
            CHECK(on);

            // Widened explicitly. SDL_fabs takes a double, so passing floats
            // promotes them silently - which GCC lets past and Clang does not,
            // and -Werror means that difference is a broken build on one
            // platform only.
            double err = SDL_fabs((double)px - (double)wx) +
                         SDL_fabs((double)py - (double)wy);
            if (err > worst) worst = err;
        }
    }
    // A hundredth of a tile is far finer than a cursor needs to be.
    CHECK(worst < 0.01);

    // And a pixel well outside the track reports itself as outside rather than
    // clamping silently onto the edge.
    float ox = 0, oy = 0;
    CHECK(!gs_iso_pick(&cam, &t, -10000.0f, 0.0f, &ox, &oy));

    // Ground far steeper than the projection ray - a wall, which the editor can
    // build - is where the iteration stops converging. It must still return.
    static gs_track wall;
    gs_flat_pavement(&wall, 24, 24);
    for (uint8_t y = 0; y <= wall.h; y++)
        for (uint8_t x = 0; x <= wall.w; x++)
            gs_track_set_corner(&wall, x, y, x >= 12 ? GS_INT(30) : 0);

    for (float sy = 0.0f; sy < (float)GS_H; sy += 37.0f) {
        float px = 0, py = 0;
        gs_iso_pick(&cam, &wall, (float)GS_W * 0.5f, sy, &px, &py);
        CHECK(px > -5000.0f && px < 5000.0f);   // finite, and it came back
    }
}

TEST(a_track_built_with_the_brushes_saves_reloads_and_races) {
    (void)ren;

    // The whole loop, end to end: build it with the brushes the editor gives a
    // player, write it, read it back, and drive on what came back.
    static gs_track built, loaded;
    gs_flat_pavement(&built, 32, 12);

    gs_editor ed;
    CHECK(gs_editor_init(&ed, 8192));

    // A ramp, raised a strip at a time exactly as dragging the brush would.
    ed.brush = GS_BRUSH_RAISE;
    ed.radius = 0;
    ed.step = 0.25f;
    gs_edit_begin(ed.log);
    for (int pass = 0; pass < 4; pass++) {
        for (float x = 9.0f + (float)pass; x < 14.0f; x += 1.0f) {
            for (float y = 0.0f; y < 13.0f; y += 1.0f) {
                gs_editor_paint(&ed, &built, x, y);
            }
        }
    }
    gs_edit_end(ed.log);

    // A field of ice past the landing, and a low-gravity pocket over the jump.
    ed.brush = GS_BRUSH_SURFACE;
    ed.surface = GS_SURF_ICE;
    ed.radius = 2;
    gs_edit_begin(ed.log);
    for (float x = 20.0f; x < 28.0f; x += 1.0f) gs_editor_paint(&ed, &built, x, 6.0f);
    gs_edit_end(ed.log);

    ed.brush = GS_BRUSH_GRAVITY;
    ed.gravity = 0.35f;
    gs_edit_begin(ed.log);
    for (float x = 15.0f; x < 19.0f; x += 1.0f) gs_editor_paint(&ed, &built, x, 6.0f);
    gs_edit_end(ed.log);

    // Three strokes, three undo steps - not several hundred.
    CHECK(gs_edit_undo_depth(ed.log) == 3);

    // The brushes actually changed the ground, the surface and the gravity.
    CHECK(gs_track_height(&built, GS_INT(13), GS_INT(6)) > 0);
    CHECK(gs_track_surface(&built, GS_INT(24), GS_INT(6)) == GS_SURF_ICE);
    CHECK(gs_track_gravity(&built, GS_INT(16), GS_INT(6)) < GS_ONE);

    // And the radius is a radius. Checked off the line the brush was dragged
    // along, because a tile on that line is painted by a brush of any size at
    // all - including one that ignored the setting entirely.
    CHECK(gs_track_surface(&built, GS_INT(24), GS_INT(4)) == GS_SURF_ICE);
    CHECK(gs_track_surface(&built, GS_INT(24), GS_INT(1)) == GS_SURF_PAVEMENT);

    // Saves, and reloads to the same track.
    CHECK(gs_editor_save(&ed, &built));
    gs_track_init(&loaded, 4, 4, GS_SURF_DIRT);      // deliberately not the same
    CHECK(gs_editor_load(&ed, &loaded));
    CHECK(gs_track_hash(&loaded) == gs_track_hash(&built));

    // And races. Not "loads without crashing" - a car driven over the ramp that
    // was built has to leave the ground, which is the only thing that proves
    // the shape survived the trip.
    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &loaded, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(2), GS_INT(6), 0);

    bool flew = false;
    gs_fix highest = 0;
    for (int i = 0; i < GS_TICK_HZ * 12; i++) {
        gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, 0, 0, 0 };
        gs_world_step(&w, &loaded, in);
        if (!w.car[0].grounded) flew = true;
        if (w.car[0].z > highest) highest = w.car[0].z;
    }

    CHECK(w.car[0].x > GS_INT(14));   // it got past the ramp
    CHECK(highest > 0);               // it climbed what was built
    CHECK(flew);                      // and left the ground at the top of it

    gs_editor_quit(&ed);
}

TEST(the_elevation_brush_moves_the_ground_by_exactly_the_step_it_is_set_to) {
    (void)ren;

    static gs_track t;
    gs_flat_pavement(&t, 24, 12);

    gs_editor ed;
    CHECK(gs_editor_init(&ed, 4096));
    ed.brush = GS_BRUSH_RAISE;
    ed.radius = 0;
    ed.step = 0.25f;

    // One application is one step. Not "about a step" - the number in the
    // panel is what happens, or the panel is lying about what the tool does.
    gs_editor_paint(&ed, &t, 5.0f, 5.0f);
    CHECK(gs_track_height(&t, GS_INT(5), GS_INT(5)) == GS_INT(1) / 4);

    for (int i = 0; i < 3; i++) gs_editor_paint(&ed, &t, 5.0f, 5.0f);
    CHECK(gs_track_height(&t, GS_INT(5), GS_INT(5)) == GS_INT(1));

    ed.brush = GS_BRUSH_LOWER;
    gs_editor_paint(&ed, &t, 5.0f, 5.0f);
    gs_editor_paint(&ed, &t, 5.0f, 5.0f);
    CHECK(gs_track_height(&t, GS_INT(5), GS_INT(5)) == GS_INT(1) / 2);

    // And it goes below the datum, because a dip is as buildable as a hill.
    for (int i = 0; i < 4; i++) gs_editor_paint(&ed, &t, 5.0f, 5.0f);
    CHECK(gs_track_height(&t, GS_INT(5), GS_INT(5)) < 0);

    gs_editor_quit(&ed);
}

TEST(a_ramp_drawn_in_the_editor_drives_like_the_ramp_that_was_drawn) {
    (void)ren;

    // The claim: what the editor drew is what the car meets. Not "a ramp
    // appears" - the slope the tool was asked for is the slope the physics
    // sees, and the jump it produces is the one that slope predicts.
    static gs_track t;
    gs_flat_pavement(&t, 32, 12);

    gs_editor ed;
    CHECK(gs_editor_init(&ed, 8192));
    ed.brush = GS_BRUSH_RAISE;
    ed.radius = 0;
    ed.step = 0.25f;

    // A ramp climbing a quarter tile per tile between x = 8 and x = 12, drawn
    // the way a person draws one: each column raised one more time than the
    // column before it.
    const float drawn_slope = 0.25f;
    gs_edit_begin(ed.log);
    for (int col = 1; col <= 4; col++) {
        for (int rep = 0; rep < col; rep++) {
            for (float y = 0.0f; y <= 12.0f; y += 1.0f) {
                gs_editor_paint(&ed, &t, 8.0f + (float)col, y);
            }
        }
    }
    // Past the crest it stays up, so the far side is a flat table rather than a
    // slope back down - the jump has to be the ramp's doing, not the landing's.
    for (float x = 13.0f; x <= 32.0f; x += 1.0f) {
        for (int rep = 0; rep < 4; rep++) {
            for (float y = 0.0f; y <= 12.0f; y += 1.0f) {
                gs_editor_paint(&ed, &t, x, y);
            }
        }
    }
    gs_edit_end(ed.log);

    // What the track says the slope is, where the ramp is.
    gs_fix dzdx = 0, dzdy = 0;
    gs_track_slope(&t, GS_INT(10) + GS_HALF, GS_INT(6), &dzdx, &dzdy);
    CHECK(SDL_fabs((double)gs_to_f(dzdx) - (double)drawn_slope) < 0.01);
    CHECK(SDL_fabs((double)gs_to_f(dzdy)) < 0.01);

    // And what a car makes of it. Drag and rolling resistance dialled out, so
    // the arc is the ballistic one and the comparison is with arithmetic rather
    // than with aerodynamics.
    gs_world w;
    gs_world_init(&w, GS_ONE);
    w.drag_scale = 0;
    w.friction_scale = 0;
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(2), GS_INT(6), 0);
    w.car[0].vx = GS_INT(5);

    double launch_x = 0, launch_vx = 0, launch_vz = 0, range = -1.0;
    bool flew = false;
    for (int i = 0; i < GS_TICK_HZ * 20; i++) {
        bool was_air = !w.car[0].grounded;
        gs_world_step(&w, &t, nullptr);
        bool is_air = !w.car[0].grounded;

        if (!was_air && is_air) {
            launch_x = (double)gs_to_f(w.car[0].x);
            launch_vx = (double)gs_to_f(w.car[0].vx);
            launch_vz = (double)gs_to_f(w.car[0].vz);
            flew = true;
        }
        if (was_air && !is_air && flew) {
            range = (double)gs_to_f(w.car[0].x) - launch_x;
            break;
        }
    }

    CHECK(flew);
    CHECK(range > 0.0);

    // The car left the ramp climbing at the slope that was drawn: vz is vx
    // times the gradient. This is the line that ties the tool to the physics.
    CHECK(SDL_fabs(launch_vz / launch_vx - (double)drawn_slope) < 0.02);

    // And the arc is the one that launch predicts.
    double gravity = (double)gs_to_f(w.gravity);
    double predicted = 2.0 * launch_vx * launch_vz / gravity;
    CHECK(SDL_fabs(range - predicted) < predicted * 0.06);

    gs_editor_quit(&ed);
}

// Paint a broad band across a flat track with whatever brush the editor is
// currently set to, so the two halves of a comparison differ only by the paint.
static void gs_paint_band(gs_editor *ed, gs_track *t, float x0, float x1) {
    gs_edit_begin(ed->log);
    for (float x = x0; x <= x1; x += 1.0f)
        for (float y = 0.0f; y <= 12.0f; y += 1.0f)
            gs_editor_paint(ed, t, x, y);
    gs_edit_end(ed->log);
}

TEST(painting_ice_changes_what_the_car_does_when_it_gets_there) {
    (void)ren;

    static gs_track plain, painted;
    gs_flat_pavement(&plain, 32, 12);
    gs_flat_pavement(&painted, 32, 12);

    gs_editor ed;
    CHECK(gs_editor_init(&ed, 8192));
    ed.brush = GS_BRUSH_SURFACE;
    ed.surface = GS_SURF_ICE;
    ed.radius = 1;
    gs_paint_band(&ed, &painted, 0.0f, 31.0f);

    // Two identical cars, sliding sideways, driven identically. The only
    // difference between the runs is what the brush did.
    gs_fix slip[2];
    for (int variant = 0; variant < 2; variant++) {
        const gs_track *t = variant == 0 ? &plain : &painted;
        gs_world w;
        gs_world_init(&w, GS_ONE);
        gs_world_add_car(&w, t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(4), GS_INT(6), 0);
        w.car[0].vx = GS_INT(4);
        w.car[0].vy = GS_INT(4);

        // Two seconds: pavement bears about 2.7 tiles/s^2 sideways, so it has
        // four tiles/s of slip gone well inside that, and ice bears a sixth of
        // it and is still going.
        for (int i = 0; i < GS_TICK_HZ * 2; i++) gs_world_step(&w, t, nullptr);
        slip[variant] = gs_fix_abs(w.car[0].vy);
    }

    CHECK(slip[0] == 0);              // pavement caught it
    CHECK(slip[1] > GS_INT(2));       // the painted ice did not
    gs_editor_quit(&ed);
}

TEST(painting_gravity_changes_how_far_the_car_flies_over_it) {
    (void)ren;

    static gs_track plain, painted;
    gs_flat_pavement(&plain, 40, 12);
    gs_flat_pavement(&painted, 40, 12);

    gs_editor ed;
    CHECK(gs_editor_init(&ed, 16384));

    // The same ramp drawn on both, with the same brush, so the launch is
    // identical and only the air the car flies through differs.
    for (int which = 0; which < 2; which++) {
        gs_track *t = which == 0 ? &plain : &painted;
        ed.brush = GS_BRUSH_RAISE;
        ed.radius = 0;
        ed.step = 0.25f;
        gs_edit_begin(ed.log);
        for (int col = 1; col <= 4; col++)
            for (int rep = 0; rep < col; rep++)
                for (float y = 0.0f; y <= 12.0f; y += 1.0f)
                    gs_editor_paint(&ed, t, 8.0f + (float)col, y);
        for (float x = 13.0f; x <= 40.0f; x += 1.0f)
            for (int rep = 0; rep < 4; rep++)
                for (float y = 0.0f; y <= 12.0f; y += 1.0f)
                    gs_editor_paint(&ed, t, x, y);
        gs_edit_end(ed.log);
    }

    // A pocket of one-third gravity painted over the landing - the brush being
    // used as a design material rather than as a race setting.
    ed.brush = GS_BRUSH_GRAVITY;
    ed.gravity = 0.33f;
    ed.radius = 1;
    gs_paint_band(&ed, &painted, 14.0f, 26.0f);

    double flight[2];
    for (int variant = 0; variant < 2; variant++) {
        const gs_track *t = variant == 0 ? &plain : &painted;
        gs_world w;
        gs_world_init(&w, GS_ONE);
        w.drag_scale = 0;
        w.friction_scale = 0;
        gs_world_add_car(&w, t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(2), GS_INT(6), 0);
        w.car[0].vx = GS_INT(5);

        double launch = 0;
        bool flew = false;
        flight[variant] = 0.0;
        for (int i = 0; i < GS_TICK_HZ * 20; i++) {
            bool was_air = !w.car[0].grounded;
            gs_world_step(&w, t, nullptr);
            bool is_air = !w.car[0].grounded;
            if (!was_air && is_air) { launch = (double)gs_to_f(w.car[0].x); flew = true; }
            if (was_air && !is_air && flew) {
                flight[variant] = (double)gs_to_f(w.car[0].x) - launch;
                break;
            }
        }
        CHECK(flew);
    }

    CHECK(flight[0] > 0.0);
    // Gravity is sampled where the car is, every tick, so a pocket painted
    // under the flight path lengthens the jump without touching the race dial.
    CHECK(flight[1] > flight[0] * 1.5);
    gs_editor_quit(&ed);
}

TEST(the_gate_brush_places_a_route_where_the_pointer_is) {
    (void)ren;

    static gs_track t;
    gs_flat_pavement(&t, 32, 12);

    gs_editor ed;
    CHECK(gs_editor_init(&ed, 4096));
    ed.brush = GS_BRUSH_GATE;
    ed.gate_heading = 90.0f;      // travelling along +y
    ed.gate_width = 3.0f;

    gs_editor_paint(&ed, &t, 8.0f, 4.0f);
    CHECK(t.gate_count == 1);
    CHECK(t.gate[0].x == GS_INT(8));
    CHECK(t.gate[0].y == GS_INT(4));
    CHECK(t.gate[0].half_width == GS_INT(3));

    // Ninety degrees is a quarter turn, and a car driving that way goes through
    // it while one crossing the other way does not.
    CHECK(t.gate[0].heading == GS_QUARTER);
    CHECK(gs_gate_crossed(&t.gate[0], GS_INT(8), GS_INT(3), GS_INT(8), GS_INT(5)));
    CHECK(!gs_gate_crossed(&t.gate[0], GS_INT(7), GS_INT(4), GS_INT(9), GS_INT(4)));

    // Placed in the order they are clicked, which is the order they are driven.
    gs_editor_paint(&ed, &t, 20.0f, 4.0f);
    CHECK(t.gate_count == 2);
    CHECK(t.gate[1].x == GS_INT(20));

    // And the route has a ceiling the editor reports rather than overruns.
    for (int i = 0; i < GS_TRACK_MAX_GATES + 4; i++) {
        gs_editor_paint(&ed, &t, 4.0f, 4.0f);
    }
    CHECK(t.gate_count == GS_TRACK_MAX_GATES);

    gs_editor_quit(&ed);
}

TEST(a_test_drive_starts_where_you_were_looking_and_changes_nothing) {
    (void)ren;

    static gs_track t;
    gs_flat_pavement(&t, 32, 16);
    gs_track_add_gate(&t, GS_INT(4), GS_INT(8), GS_QUARTER, GS_INT(3));
    gs_track_add_gate(&t, GS_INT(24), GS_INT(8), GS_QUARTER, GS_INT(3));

    gs_editor ed;
    CHECK(gs_editor_init(&ed, 4096));

    // Build something, so there is work that has to survive the drive.
    ed.brush = GS_BRUSH_RAISE;
    ed.radius = 1;
    ed.step = 0.5f;
    gs_edit_begin(ed.log);
    for (float x = 10.0f; x < 16.0f; x += 1.0f) gs_editor_paint(&ed, &t, x, 8.0f);
    gs_edit_end(ed.log);

    uint64_t built = gs_track_hash(&t);
    uint32_t history = gs_edit_undo_depth(ed.log);

    // The pointer is over the middle of the track: a test drive starts *there*,
    // because the question being asked is about the corner you are looking at,
    // not about the track from the beginning.
    ed.hover_on = true;
    ed.hover_x = 13.0f;
    ed.hover_y = 8.0f;

    gs_fix sx = 0, sy = 0;
    gs_angle heading = 0;
    CHECK(gs_editor_drive_start(&ed, &t, &sx, &sy, &heading));
    CHECK(sx == GS_INT(13));
    CHECK(sy == GS_INT(8));
    CHECK(heading == GS_QUARTER);        // facing the way the start line does

    // Drive on the very track being edited - no copy, no reload.
    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR, sx, sy, heading);
    for (int i = 0; i < GS_TICK_HZ * 3; i++) {
        gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, 0, 0, 0 };
        gs_world_step(&w, &t, in);
    }
    CHECK(gs_car_speed(&w.car[0]) > 0);

    // And back. The track is exactly what it was, and so is the history: the
    // edits made before the drive survive returning from it.
    CHECK(gs_track_hash(&t) == built);
    CHECK(gs_edit_undo_depth(ed.log) == history);

    CHECK(gs_edit_undo(ed.log, &t));
    CHECK(gs_track_hash(&t) != built);
    CHECK(gs_edit_redo(ed.log, &t));
    CHECK(gs_track_hash(&t) == built);

    // With the pointer off the track, a drive starts at the start line instead.
    ed.hover_on = false;
    CHECK(gs_editor_drive_start(&ed, &t, &sx, &sy, &heading));
    CHECK(sx == GS_INT(4) && sy == GS_INT(8));

    // And with no route and no cursor there is nowhere sensible to begin.
    static gs_track bare;
    gs_flat_pavement(&bare, 8, 8);
    CHECK(!gs_editor_drive_start(&ed, &bare, &sx, &sy, &heading));

    gs_editor_quit(&ed);
}

TEST(coming_back_from_a_drive_returns_you_to_where_you_were_building) {
    (void)ren;

    static gs_track t;
    gs_flat_pavement(&t, 40, 40);

    gs_editor ed;
    CHECK(gs_editor_init(&ed, 1024));

    gs_view view = { 0 };
    view.rect = (SDL_Rect){ 0, 0, GS_W, GS_H };
    view.cam = gs_camera_on(5.0f, 5.0f, 0.0f);

    // First time in, the editor adopts the view - otherwise the world moves out
    // from under you at the moment you switch.
    gs_editor_toggle(&ed, &view);
    CHECK(ed.active);
    CHECK(ed.cam_x == 5.0f && ed.cam_y == 5.0f);

    // Pan off somewhere to work.
    ed.cam_x = 28.0f;
    ed.cam_y = 31.0f;

    // Out for a drive. The car goes a long way, and the race camera follows it.
    gs_editor_toggle(&ed, &view);
    CHECK(!ed.active);
    view.cam = gs_camera_on(36.0f, 2.0f, 0.0f);

    // Back in - and back to the part of the track being built, not to wherever
    // the car happened to stop. That is what makes it a snap back rather than
    // a journey home, and it is the difference between a test drive you take
    // twenty times an hour and one you avoid.
    gs_editor_toggle(&ed, &view);
    CHECK(ed.active);
    CHECK(ed.cam_x == 28.0f);
    CHECK(ed.cam_y == 31.0f);

    gs_editor_quit(&ed);
}

// Drive the ghost the way the frontend drives it: a couple of ticks a frame,
// many frames - not one enormous step.
static void gs_ghost_run(gs_editor *e, const gs_track *t, uint32_t ticks) {
    for (uint32_t i = 0; i < ticks; i += 2) gs_editor_ghost_step(e, t, 2);
}

TEST(raising_a_ramp_changes_where_the_ghost_lands_without_being_told_to) {
    (void)ren;

    static gs_track t;
    gs_flat_pavement(&t, 40, 12);
    gs_track_add_gate(&t, GS_INT(2), GS_INT(6), 0, GS_INT(4));
    gs_track_add_gate(&t, GS_INT(30), GS_INT(6), 0, GS_INT(4));

    gs_editor ed;
    CHECK(gs_editor_init(&ed, 16384));
    CHECK(ed.ghost_on);

    // Stepped two ticks at a time, a hundred and eighty times - which is how
    // the frontend drives it, two ticks a frame at sixty frames a second.
    //
    // Not 360 ticks in one call. A ghost that restarts once per *call* looks
    // identical under a single big call and never moves at all under the real
    // usage, so a test that does not step the way the game steps cannot see the
    // difference. It could not, until this changed.
    gs_ghost_run(&ed, &t, 360);
    const gs_car *g = gs_editor_ghost_car(&ed);
    CHECK(g != nullptr);

    gs_fix flat_x = g->x;
    gs_fix flat_z = g->z;
    CHECK(flat_x > GS_INT(4));      // it got going
    CHECK(flat_z == 0);             // and stayed on the floor

    // Build a ramp in its path. **Nothing tells the ghost.** It notices by the
    // track's own hash, which is what makes it live rather than a thing you
    // remember to re-run.
    ed.brush = GS_BRUSH_RAISE;
    ed.radius = 0;
    ed.step = 0.3f;
    gs_edit_begin(ed.log);
    for (int col = 1; col <= 5; col++)
        for (int rep = 0; rep < col; rep++)
            for (float y = 0.0f; y <= 12.0f; y += 1.0f)
                gs_editor_paint(&ed, &t, 9.0f + (float)col, y);
    gs_edit_end(ed.log);

    gs_ghost_run(&ed, &t, 360);
    g = gs_editor_ghost_car(&ed);
    CHECK(g != nullptr);

    // Same number of ticks into the run, and it is somewhere else entirely:
    // higher up, having climbed what was just drawn.
    CHECK(g->z > flat_z);
    CHECK(g->x != flat_x);

    // Undo it, and the ghost goes back to the flat run - again with nobody
    // telling it anything.
    CHECK(gs_edit_undo(ed.log, &t));
    gs_ghost_run(&ed, &t, 360);
    g = gs_editor_ghost_car(&ed);
    CHECK(g->x == flat_x);
    CHECK(g->z == flat_z);

    // Switched off, it does not run at all.
    ed.ghost_on = false;
    CHECK(gs_editor_ghost_car(&ed) == nullptr);

    // And a track with no start line has nowhere to put one.
    static gs_track routeless;
    gs_flat_pavement(&routeless, 16, 16);
    ed.ghost_on = true;
    gs_ghost_run(&ed, &routeless, 120);
    CHECK(gs_editor_ghost_car(&ed) == nullptr);

    gs_editor_quit(&ed);
}

TEST(a_track_can_be_built_from_a_pad_with_no_mouse_at_all) {
    (void)ren;

    static gs_track t;
    gs_flat_pavement(&t, 32, 16);

    gs_editor ed;
    CHECK(gs_editor_init(&ed, 8192));
    ed.hover_x = 4.0f;
    ed.hover_y = 8.0f;
    ed.hover_on = true;
    ed.brush = GS_BRUSH_RAISE;
    ed.radius = 1;
    ed.step = 0.5f;

    const float dt = 1.0f / 60.0f;
    gs_pad_edit pad = { 0 };
    pad.present = true;

    // Push the stick right and hold the paint button: a stroke of raised ground
    // across the track, from a pad, with no pointer involved.
    pad.x = 1.0f;
    pad.paint = true;
    for (int frame = 0; frame < 60; frame++) {
        CHECK(!gs_editor_pad_input(&ed, &t, &pad, dt));
    }
    pad.paint = false;
    pad.x = 0.0f;
    gs_editor_pad_input(&ed, &t, &pad, dt);

    // The cursor moved, the ground rose, and the whole drag is one undo step.
    CHECK(ed.hover_x > 5.0f);
    CHECK(gs_track_height(&t, GS_INT(4), GS_INT(8)) > 0);
    CHECK(gs_edit_undo_depth(ed.log) == 1);

    uint64_t built = gs_track_hash(&t);

    // Undo and redo from the shoulder buttons, on the press rather than the
    // hold - a held undo firing every frame would walk back through an
    // afternoon in under a second.
    pad.undo = true;
    gs_editor_pad_input(&ed, &t, &pad, dt);
    CHECK(gs_track_hash(&t) != built);

    pad.undo = false;
    pad.redo = true;
    gs_editor_pad_input(&ed, &t, &pad, dt);
    CHECK(gs_track_hash(&t) == built);
    pad.redo = false;

    // The brush cycles from the pad, so every tool is reachable without a
    // mouse - including the gate brush, which is what makes a route placeable.
    int first = ed.brush;
    pad.next_brush = true;
    gs_editor_pad_input(&ed, &t, &pad, dt);
    CHECK(ed.brush != first);
    pad.next_brush = false;

    bool saw_gate = ed.brush == GS_BRUSH_GATE;
    for (int i = 0; i < GS_BRUSH_COUNT; i++) {
        pad.next_brush = true;
        gs_editor_pad_input(&ed, &t, &pad, dt);
        pad.next_brush = false;
        if (ed.brush == GS_BRUSH_GATE) saw_gate = true;
    }
    CHECK(saw_gate);

    // Start asks for a test drive rather than doing something to the track.
    pad.drive = true;
    CHECK(gs_editor_pad_input(&ed, &t, &pad, dt));
    pad.drive = false;

    // With no pad plugged in, none of this happens at all.
    uint64_t before = gs_track_hash(&t);
    gs_pad_edit none = { 0 };
    none.paint = true;
    none.undo = true;
    CHECK(!gs_editor_pad_input(&ed, &t, &none, dt));
    CHECK(gs_track_hash(&t) == before);

    gs_editor_quit(&ed);
}

TEST(moving_a_dial_restarts_the_ghost_under_the_new_one) {
    (void)ren;

    static gs_track t;
    gs_flat_pavement(&t, 40, 12);
    gs_track_add_gate(&t, GS_INT(2), GS_INT(6), 0, GS_INT(4));
    gs_track_add_gate(&t, GS_INT(30), GS_INT(6), 0, GS_INT(4));

    // A ramp, so gravity has something visible to change.
    gs_editor ed;
    CHECK(gs_editor_init(&ed, 8192));
    ed.brush = GS_BRUSH_RAISE;
    ed.radius = 0;
    ed.step = 0.3f;
    gs_edit_begin(ed.log);
    for (int col = 1; col <= 5; col++)
        for (int rep = 0; rep < col; rep++)
            for (float y = 0.0f; y <= 12.0f; y += 1.0f)
                gs_editor_paint(&ed, &t, 9.0f + (float)col, y);
    gs_edit_end(ed.log);

    gs_ghost_run(&ed, &t, 400);
    const gs_car *g = gs_editor_ghost_car(&ed);
    CHECK(g != nullptr);
    gs_fix earth_x = g->x, earth_z = g->z;

    // Turn gravity down. The *track* has not changed, so a ghost that only
    // watches the track hash would carry on flying under the old gravity -
    // which is a wrong answer that looks like a right one.
    ed.dial_gravity = 0.2f;
    gs_ghost_run(&ed, &t, 400);
    g = gs_editor_ghost_car(&ed);
    CHECK(g != nullptr);
    CHECK(g->x != earth_x || g->z != earth_z);

    // And back again, to the same place as before: it is the dial doing this
    // and not merely the restart.
    ed.dial_gravity = 1.0f;
    gs_ghost_run(&ed, &t, 400);
    g = gs_editor_ghost_car(&ed);
    CHECK(g->x == earth_x);
    CHECK(g->z == earth_z);

    gs_editor_quit(&ed);
}

// ---------------------------------------------------------------------------
// Two on one machine
// ---------------------------------------------------------------------------

TEST(two_cars_take_their_input_from_different_places_and_go_different_ways) {
    (void)ren;

    static gs_track t;
    gs_flat_pavement(&t, 40, 40);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(10), GS_INT(20), 0);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(10), GS_INT(24), 0);
    CHECK(w.car_count == 2);

    // One accelerates and turns left, the other accelerates and turns right.
    // Nothing here is shared but the track.
    for (int i = 0; i < GS_TICK_HZ * 3; i++) {
        gs_input in[GS_MAX_CARS] = {
            (gs_input)(GS_IN_ACCEL | GS_IN_LEFT),
            (gs_input)(GS_IN_ACCEL | GS_IN_RIGHT),
            0, 0
        };
        gs_world_step(&w, &t, in);
    }

    CHECK(gs_car_speed(&w.car[0]) > 0);
    CHECK(gs_car_speed(&w.car[1]) > 0);

    // Turned opposite ways, and a long way apart because of it.
    int32_t left = gs_angle_delta(0, w.car[0].heading);
    int32_t right = gs_angle_delta(0, w.car[1].heading);
    CHECK(left < 0);
    CHECK(right > 0);
    CHECK(gs_fix_len2(w.car[0].x - w.car[1].x, w.car[0].y - w.car[1].y) > GS_INT(8));

    // And a car with no input does nothing, so the byte really is per car.
    gs_world idle;
    gs_world_init(&idle, GS_ONE);
    gs_world_add_car(&idle, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(10), GS_INT(20), 0);
    gs_world_add_car(&idle, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(10), GS_INT(24), 0);
    for (int i = 0; i < GS_TICK_HZ; i++) {
        gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, 0, 0, 0 };
        gs_world_step(&idle, &t, in);
    }
    CHECK(gs_car_speed(&idle.car[0]) > 0);
    CHECK(gs_car_speed(&idle.car[1]) == 0);
}

TEST(the_second_pad_drives_the_second_car) {
    (void)ren;

    // Two pads, each pressing something different.
    gs_input pads[GS_MAX_CARS] = { GS_IN_ACCEL, GS_IN_BRAKE, 0, 0 };
    gs_input none[2] = { 0, 0 };
    gs_input out[GS_MAX_CARS];

    gs_input_combine(pads, 2, none, 2, out, 2);
    CHECK(out[0] == GS_IN_ACCEL);
    CHECK(out[1] == GS_IN_BRAKE);

    // A pad nobody has plugged in drives nothing, and cars past the count are
    // left alone rather than fed somebody else's buttons.
    gs_input_combine(pads, 1, none, 2, out, 2);
    CHECK(out[0] == GS_IN_ACCEL);
    CHECK(out[1] == 0);

    gs_input_combine(pads, 2, none, 2, out, 1);
    CHECK(out[0] == GS_IN_ACCEL);
    CHECK(out[1] == 0);

    // The keyboard is added rather than substituted: a pad and the arrow keys
    // can drive the same car, and neither switches the other off.
    gs_input keys[2] = { GS_IN_LEFT, GS_IN_RIGHT };
    gs_input_combine(pads, 2, keys, 2, out, 2);
    CHECK(out[0] == (gs_input)(GS_IN_ACCEL | GS_IN_LEFT));
    CHECK(out[1] == (gs_input)(GS_IN_BRAKE | GS_IN_RIGHT));

    // And with no pads at all, one keyboard still drives two cars - which is
    // how most of this gets tested and a good deal of it will be played.
    gs_input_combine(pads, 0, keys, 2, out, 2);
    CHECK(out[0] == GS_IN_LEFT);
    CHECK(out[1] == GS_IN_RIGHT);
}

TEST(each_half_of_a_split_screen_shows_its_own_car) {
    static gs_track t;
    gs_flat_pavement(&t, 40, 40);
    // Something to tell the two halves apart by, since flat grey looks the same
    // wherever you point it.
    for (uint8_t x = 0; x < 20; x++)
        for (uint8_t y = 0; y < 40; y++)
            gs_track_set_surface(&t, x, y, GS_SURF_DIRT);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(8), GS_INT(20), 0);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(32), GS_INT(20), 0);

    // Two views, each following its own car, side by side across one window.
    gs_frame half[2];
    for (int i = 0; i < 2; i++) {
        gs_view v = { 0 };
        v.car = (uint8_t)i;
        v.cam.zoom = GS_ISO_DEFAULT_ZOOM;
        v.rect = (SDL_Rect){ 0, 0, GS_W, GS_H };
        gs_render_track_camera(&v, &t, &w, &w, 1.0f);

        // The camera went to *its* car, not to car zero.
        CHECK(SDL_fabs((double)v.cam.cx - (double)gs_to_f(w.car[i].x)) < 0.01);

        half[i] = gs_render_frame(ren, &t, &w, &w, 1.0f, &v.cam);
        CHECK(half[i].px != nullptr);
    }

    // The two halves are looking at different places, so they do not match.
    CHECK(gs_frame_diff(&half[0], &half[1]) > 1.0);

    // Each half has *its own* car in it, and not the other one: they are
    // twenty-four tiles apart, so neither is in the other's view at all.
    CHECK(gs_count_car0(&half[0]) > 100);
    CHECK(gs_count_car1(&half[0]) == 0);

    CHECK(gs_count_car1(&half[1]) > 100);
    CHECK(gs_count_car0(&half[1]) == 0);

    gs_frame_free(&half[0]);
    gs_frame_free(&half[1]);
}

// Is this pixel roughly the colour car `index` is drawn in? The bodies are
// shaded, so this compares which channel dominates rather than exact values.
static bool gs_looks_like_car(const uint8_t *p, uint8_t index) {
    switch (index & 3) {
    case 0: return p[0] > 100 && p[0] > 2 * p[1] && p[0] > 2 * p[2];      // red
    case 1: return p[2] > 150 && p[2] > 2 * p[0];                          // blue
    case 2: return p[0] > 140 && p[1] > 110 && p[2] < p[0] / 2;            // yellow
    default: return p[1] > 110 && p[1] > p[0] + 40 && p[1] > p[2] + 40;    // green
    }
}

static int gs_count_car(const gs_frame *f, uint8_t index) {
    int n = 0;
    for (int i = 0; i < GS_W * GS_H; i++) {
        if (gs_looks_like_car(&f->px[i * 4], index)) n++;
    }
    return n;
}

TEST(four_players_get_four_views_that_tile_the_window_without_overlapping) {
    (void)ren;

    SDL_Rect r[GS_MAX_CARS];

    CHECK(gs_render_layout(1, 1280, 720, r) == 1);
    CHECK(r[0].w == 1280 && r[0].h == 720);

    CHECK(gs_render_layout(2, 1280, 720, r) == 2);
    CHECK(r[0].h == 720 && r[1].h == 720);
    CHECK(r[0].x + r[0].w < r[1].x);            // a gap, not an overlap

    // Three and four take the same grid: a player joining should not rearrange
    // everybody else's screen.
    SDL_Rect three[GS_MAX_CARS], four[GS_MAX_CARS];
    CHECK(gs_render_layout(3, 1280, 720, three) == 3);
    CHECK(gs_render_layout(4, 1280, 720, four) == 4);
    for (int i = 0; i < 3; i++) {
        CHECK(three[i].x == four[i].x && three[i].y == four[i].y);
        CHECK(three[i].w == four[i].w && three[i].h == four[i].h);
    }

    // Four quarters, none overlapping, all inside the window.
    for (int i = 0; i < 4; i++) {
        CHECK(four[i].w > 0 && four[i].h > 0);
        CHECK(four[i].x >= 0 && four[i].y >= 0);
        CHECK(four[i].x + four[i].w <= 1280);
        CHECK(four[i].y + four[i].h <= 720);
        for (int j = i + 1; j < 4; j++) {
            bool apart = four[i].x + four[i].w <= four[j].x ||
                         four[j].x + four[j].w <= four[i].x ||
                         four[i].y + four[i].h <= four[j].y ||
                         four[j].y + four[j].h <= four[i].y;
            CHECK(apart);
        }
    }
}

TEST(each_of_four_views_shows_its_own_car_and_costs_no_more_than_one_full_one) {
    static gs_track t;
    gs_flat_pavement(&t, 48, 48);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    // Far apart, so nobody is in anybody else's view by accident.
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(8), GS_INT(8), 0);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(40), GS_INT(8), 0);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(8), GS_INT(40), 0);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(40), GS_INT(40), 0);
    CHECK(w.car_count == 4);

    for (uint8_t i = 0; i < 4; i++) {
        gs_view v = { 0 };
        v.car = i;
        v.cam.zoom = GS_ISO_DEFAULT_ZOOM;
        v.rect = (SDL_Rect){ 0, 0, GS_W, GS_H };
        gs_render_track_camera(&v, &t, &w, &w, 1.0f);

        gs_frame f = gs_render_frame(ren, &t, &w, &w, 1.0f, &v.cam);
        CHECK(f.px != nullptr);

        // Its own car is there, and it is the only one.
        CHECK(gs_count_car(&f, i) > 50);
        for (uint8_t other = 0; other < 4; other++) {
            if (other != i) CHECK(gs_count_car(&f, other) == 0);
        }
        gs_frame_free(&f);
    }

    // Four quarter-sized views are a window's worth of pixels between them, so
    // four-up must not cost four times one-up. Counted rather than timed: the
    // first version of this ran a stopwatch over both and failed one run in ten
    // on a busy machine, which is worse than no test at all - a green tick that
    // means "the machine was quiet" is not evidence.
    SDL_Rect quarters[GS_MAX_CARS];
    gs_render_layout(4, GS_W, GS_H, quarters);

    gs_view full = { 0 };
    full.car = 0;
    full.cam.zoom = GS_ISO_DEFAULT_ZOOM;
    full.rect = (SDL_Rect){ 0, 0, GS_W, GS_H };
    gs_render_track_camera(&full, &t, &w, &w, 1.0f);

    gs_render_reset_stats();
    gs_render_view(ren, &t, &w, &w, 1.0f, &full);
    uint32_t one_view_tiles = gs_render_stats_now().tiles;

    gs_render_reset_stats();
    for (uint8_t i = 0; i < 4; i++) {
        gs_view q = { 0 };
        q.car = i;
        q.cam.zoom = GS_ISO_DEFAULT_ZOOM;
        q.rect = quarters[i];
        gs_render_track_camera(&q, &t, &w, &w, 1.0f);
        gs_render_view(ren, &t, &w, &w, 1.0f, &q);
    }
    uint32_t four_view_tiles = gs_render_stats_now().tiles;

    // The track is 48 by 48, so an unculled view submits 2304 tiles and four of
    // them submit 9216. The claim worth making is not a ratio picked to pass:
    // it is that *four* split views between them build less geometry than a
    // single view of the whole track would - which is only possible because
    // each is culled to what it can actually see.
    const uint32_t whole_track = 48u * 48u;

    CHECK(one_view_tiles > 0);
    CHECK(one_view_tiles < whole_track);
    CHECK(four_view_tiles < whole_track);

    // Four quarters do cost somewhat more than one full view - each carries its
    // own margin of off-screen tiles, and four small viewports have more edge
    // between them than one large one. Two and a bit times, not four.
    CHECK(four_view_tiles < one_view_tiles * 3);
}

TEST(the_screen_merges_when_the_cars_are_close_and_splits_when_they_are_not) {
    (void)ren;

    static gs_track t;
    gs_flat_pavement(&t, 60, 60);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(30), GS_INT(30), 0);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(32), GS_INT(30), 0);

    gs_split sp;
    gs_split_init(&sp);
    const float dt = 1.0f / 60.0f;

    // Side by side: one view.
    for (int i = 0; i < 120; i++) gs_split_update(&sp, &t, &w, &w, 1.0f, GS_W, GS_H, dt);
    gs_view v[GS_MAX_CARS];
    CHECK(sp.merge == 1.0f);
    CHECK(gs_split_views(&sp, &t, &w, &w, 1.0f, GS_W, GS_H, v) == 1);

    // Drive them apart: two.
    w.car[1].x = GS_INT(30) + GS_INT(30);
    for (int i = 0; i < 120; i++) gs_split_update(&sp, &t, &w, &w, 1.0f, GS_W, GS_H, dt);
    CHECK(sp.merge == 0.0f);
    CHECK(gs_split_views(&sp, &t, &w, &w, 1.0f, GS_W, GS_H, v) == 2);

    // Back together: one again.
    w.car[1].x = GS_INT(32);
    for (int i = 0; i < 120; i++) gs_split_update(&sp, &t, &w, &w, 1.0f, GS_W, GS_H, dt);
    CHECK(gs_split_views(&sp, &t, &w, &w, 1.0f, GS_W, GS_H, v) == 1);

    // A single car is always one view, whatever it does.
    gs_world solo;
    gs_world_init(&solo, GS_ONE);
    gs_world_add_car(&solo, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(5), GS_INT(5), 0);
    gs_split_init(&sp);
    gs_split_update(&sp, &t, &solo, &solo, 1.0f, GS_W, GS_H, dt);
    CHECK(gs_split_views(&sp, &t, &solo, &solo, 1.0f, GS_W, GS_H, v) == 1);
}

TEST(cars_hovering_at_the_threshold_do_not_flicker_the_screen_in_half) {
    (void)ren;

    static gs_track t;
    gs_flat_pavement(&t, 60, 60);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(30), GS_INT(30), 0);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(30), GS_INT(30), 0);

    gs_split sp;
    gs_split_init(&sp);
    const float dt = 1.0f / 60.0f;

    // Jiggle across the *merge* threshold while already merged. With two
    // thresholds the screen stays whole until the cars are properly apart; with
    // one it splits and re-joins every other frame, which is unwatchable.
    //
    // Straddling that threshold is the point: a wobble that sits entirely
    // inside the hysteresis band passes whether the band exists or not.
    int changes = 0;
    uint8_t was = 1;
    for (int i = 0; i < 600; i++) {
        float wobble = (i % 2 == 0) ? 10.6f : 11.4f;
        w.car[1].x = GS_INT(30) + (gs_fix)(wobble * (float)GS_ONE);
        gs_split_update(&sp, &t, &w, &w, 1.0f, GS_W, GS_H, dt);

        gs_view v[GS_MAX_CARS];
        uint8_t n = gs_split_views(&sp, &t, &w, &w, 1.0f, GS_W, GS_H, v);
        if (n != was) { changes++; was = n; }
    }
    CHECK(changes == 0);
}

TEST(the_view_does_not_jump_when_the_screen_merges_or_splits) {
    (void)ren;

    static gs_track t;
    gs_flat_pavement(&t, 90, 60);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(20), GS_INT(30), 0);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(20), GS_INT(30), 0);

    gs_split sp;
    gs_split_init(&sp);
    const float dt = 1.0f / 60.0f;

    // Car one drives steadily away and steadily back, crossing the threshold in
    // both directions.
    //
    // What is measured is not how far the camera moves in a frame - during the
    // transition it moves quite quickly on purpose, and a test that objected to
    // that would be objecting to the feature. It is how much that *changes*
    // between frames. A pan accelerates gently; a hard switch is one enormous
    // frame with small ones either side, and that is what shows here.
    double worst_change = 0.0;
    double prev_delta = 0.0;
    float prev_cx = 0, prev_cy = 0;
    int frames = 0;

    for (int i = 0; i < 900; i++) {
        float away = (i < 450) ? (float)i * 0.08f : (float)(900 - i) * 0.08f;
        w.car[1].x = GS_INT(20) + (gs_fix)(away * (float)GS_ONE);

        gs_split_update(&sp, &t, &w, &w, 1.0f, GS_W, GS_H, dt);
        gs_view v[GS_MAX_CARS];
        gs_split_views(&sp, &t, &w, &w, 1.0f, GS_W, GS_H, v);

        // Pane zero is what a player watches throughout: the whole screen while
        // merged, the left half while split.
        if (frames > 0) {
            double delta = SDL_fabs((double)v[0].cam.cx - (double)prev_cx) +
                           SDL_fabs((double)v[0].cam.cy - (double)prev_cy);
            if (frames > 1) {
                double change = SDL_fabs(delta - prev_delta);
                if (change > worst_change) worst_change = change;
            }
            prev_delta = delta;
        }
        prev_cx = v[0].cam.cx;
        prev_cy = v[0].cam.cy;
        frames++;
    }

    // A hard switch teleports the camera the whole distance between the shared
    // view and one car - several tiles - in a single frame, next to frames that
    // moved almost nothing. This stays under a tenth of a tile.
    CHECK(worst_change < 0.1);
}

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------

TEST(every_control_can_be_moved_and_every_player_can_drive_from_a_pad_alone) {
    (void)ren;

    gs_bindings b;
    gs_bind_defaults(&b);

    bool keys[SDL_SCANCODE_COUNT] = { false };

    // Every player has a complete pad layout out of the box, because a pad
    // belongs to one person and the fourth of them should not have to set one
    // up before they can play.
    for (uint8_t p = 0; p < GS_MAX_CARS; p++) {
        for (int a = 0; a < GS_ACT_COUNT; a++) {
            CHECK(b.button[p][a] != GS_BUTTON_NONE);
        }

        uint32_t all = 0;
        for (int a = 0; a < GS_ACT_COUNT; a++) all |= 1u << (uint32_t)b.button[p][a];

        gs_input in = gs_bind_resolve(&b, p, nullptr, 0, all);
        CHECK(in == (gs_input)(GS_IN_ACCEL | GS_IN_BRAKE | GS_IN_LEFT |
                               GS_IN_RIGHT | GS_IN_FIRE));
    }

    // The default keyboard drives the first two cars and nothing else, so a
    // stray key cannot move somebody else's car.
    keys[SDL_SCANCODE_UP] = true;
    CHECK(gs_bind_resolve(&b, 0, keys, SDL_SCANCODE_COUNT, 0) == GS_IN_ACCEL);
    CHECK(gs_bind_resolve(&b, 1, keys, SDL_SCANCODE_COUNT, 0) == 0);
    keys[SDL_SCANCODE_UP] = false;

    // Move a control. The old key stops doing it and the new one starts.
    gs_bind_set_key(&b, 0, GS_ACT_ACCEL, SDL_SCANCODE_SPACE);
    keys[SDL_SCANCODE_UP] = true;
    CHECK(gs_bind_resolve(&b, 0, keys, SDL_SCANCODE_COUNT, 0) == 0);
    keys[SDL_SCANCODE_UP] = false;
    keys[SDL_SCANCODE_SPACE] = true;
    CHECK(gs_bind_resolve(&b, 0, keys, SDL_SCANCODE_COUNT, 0) == GS_IN_ACCEL);

    // Binding a key that another action on the same player already uses takes
    // it away from that one. Two actions on one key is a control scheme nobody
    // set out to make, and it would be discovered mid-corner.
    gs_bind_set_key(&b, 0, GS_ACT_BRAKE, SDL_SCANCODE_SPACE);
    CHECK(gs_bind_resolve(&b, 0, keys, SDL_SCANCODE_COUNT, 0) == GS_IN_BRAKE);
    keys[SDL_SCANCODE_SPACE] = false;

    // The same rule for pad buttons.
    gs_bind_set_button(&b, 2, GS_ACT_LEFT, (int16_t)SDL_GAMEPAD_BUTTON_NORTH);
    gs_bind_set_button(&b, 2, GS_ACT_RIGHT, (int16_t)SDL_GAMEPAD_BUTTON_NORTH);
    CHECK(b.button[2][GS_ACT_LEFT] == GS_BUTTON_NONE);
    CHECK(gs_bind_resolve(&b, 2, nullptr, 0, 1u << SDL_GAMEPAD_BUTTON_NORTH) ==
          GS_IN_RIGHT);

    // A control can be cleared outright, not merely moved - both halves of it.
    gs_bind_set_button(&b, 2, GS_ACT_RIGHT, GS_BUTTON_NONE);
    CHECK(gs_bind_resolve(&b, 2, nullptr, 0, 1u << SDL_GAMEPAD_BUTTON_NORTH) == 0);

    gs_bindings cleared;
    gs_bind_defaults(&cleared);
    gs_bind_set_key(&cleared, 0, GS_ACT_ACCEL, GS_KEY_NONE);
    gs_bind_set_button(&cleared, 0, GS_ACT_ACCEL, GS_BUTTON_NONE);
    for (int k = 0; k < SDL_SCANCODE_COUNT; k++) keys[k] = true;
    CHECK((gs_bind_resolve(&cleared, 0, keys, SDL_SCANCODE_COUNT, 0xffffffffu)
           & GS_IN_ACCEL) == 0);
    for (int k = 0; k < SDL_SCANCODE_COUNT; k++) keys[k] = false;

    // Keyboard and pad both count at once, so a player can use either without
    // switching modes.
    gs_bindings mixed;
    gs_bind_defaults(&mixed);
    keys[SDL_SCANCODE_LEFT] = true;
    gs_input both = gs_bind_resolve(&mixed, 0, keys, SDL_SCANCODE_COUNT,
                                    1u << SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(both == (gs_input)(GS_IN_LEFT | GS_IN_ACCEL));
    keys[SDL_SCANCODE_LEFT] = false;

    // Every action has a name to show in a rebinding screen.
    for (int a = 0; a < GS_ACT_COUNT; a++) {
        CHECK(gs_action_name((gs_action)a)[0] != '?');
    }
}

// ---------------------------------------------------------------------------
// The analyser's heatmap, on screen
// ---------------------------------------------------------------------------

// A green channel well ahead of red and blue: the warm end of the heat ramp,
// and nothing else in these scenes is green - pavement is grey and the sky is
// black.
static bool gs_is_hot(const uint8_t *p) {
    return p[1] > 90 && p[1] > p[0] + 25 && p[1] > p[2] + 25;
}

static int gs_count_hot(const gs_frame *f) {
    int n = 0;
    for (int i = 0; i < GS_W * GS_H; i++) {
        if (gs_is_hot(&f->px[i * 4])) n++;
    }
    return n;
}

static void gs_routed_pavement(gs_track *t) {
    gs_flat_pavement(t, 40, 16);
    gs_track_add_gate(t, GS_INT(4), GS_INT(8), 0, GS_INT(5));
    gs_track_add_gate(t, GS_INT(34), GS_INT(8), 0, GS_INT(5));
}

// ---------------------------------------------------------------------------
// The generated vehicle meshes
// ---------------------------------------------------------------------------

TEST(every_vehicle_has_a_mesh_and_no_two_are_the_same_shape) {
    (void)ren;

    // A mesh per vehicle, and each one closed enough to be worth drawing.
    for (uint8_t v = 0; v < GS_VEH_COUNT; v++) {
        const gs_mesh *m = gs_mesh_for(v);
        CHECK(m != nullptr);
        if (m == nullptr) continue;

        CHECK(m->vertex_count > 8);
        CHECK(m->tri_count >= 12);
        CHECK(SDL_strcmp(m->name, gs_vehicle(v)->name) == 0);

        // Every index in range - a mesh that reaches past its own vertices
        // reads whatever is next in the binary and draws it.
        for (uint16_t i = 0; i < m->tri_count; i++) {
            CHECK(m->tri[i].a < m->vertex_count);
            CHECK(m->tri[i].b < m->vertex_count);
            CHECK(m->tri[i].c < m->vertex_count);
            CHECK(m->tri[i].paint < GS_PAINT_COUNT);
        }

        // Inside the size the collision and the shadow assume. A car whose
        // geometry is bigger than its footprint is a car that gets hit by
        // things that visibly missed it.
        for (uint16_t i = 0; i < m->vertex_count; i++) {
            CHECK(SDL_fabsf(m->vertex[i].x) <= 0.75f);
            CHECK(SDL_fabsf(m->vertex[i].y) <= 0.45f);
            CHECK(m->vertex[i].z >= -0.01f);
            CHECK(m->vertex[i].z <= 1.0f);
        }

        // Wheels on the ground, not hovering: something has to touch z = 0.
        bool touches = false;
        for (uint16_t i = 0; i < m->vertex_count; i++) {
            if (m->vertex[i].z < 0.02f) touches = true;
        }
        CHECK(touches);
    }

    // And they are six different shapes, not one shape six times. Compared by
    // their extent, which is what the silhouette is made of.
    for (uint8_t a = 0; a < GS_VEH_COUNT; a++) {
        for (uint8_t b = (uint8_t)(a + 1); b < GS_VEH_COUNT; b++) {
            const gs_mesh *ma = gs_mesh_for(a), *mb = gs_mesh_for(b);
            bool same = ma->vertex_count == mb->vertex_count &&
                        ma->tri_count == mb->tri_count;
            if (same) {
                same = SDL_memcmp(ma->vertex, mb->vertex,
                                  ma->vertex_count * sizeof *ma->vertex) == 0;
            }
            CHECK(!same);
        }
    }
}

TEST(a_car_is_drawn_from_its_mesh_and_faces_where_it_is_pointing) {
    static gs_track t;
    gs_flat_pavement(&t, 24, 16);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(12), GS_INT(8), 0);

    gs_camera cam = { 0 };
    cam.zoom = 4.0f;
    cam.vw = GS_W; cam.vh = GS_H;
    cam.cx = 12.0f; cam.cy = 8.0f;

    gs_render_reset_stats();
    gs_frame f = gs_render_frame(ren, &t, &w, &w, 1.0f, &cam);
    CHECK(f.px != nullptr);

    gs_render_stats st = gs_render_stats_now();
    CHECK(st.cars == 1);

    // Culled, not merely submitted: a closed mesh has more than half its
    // triangles facing away at any angle, so drawing all of them would mean the
    // culling is not working.
    const gs_mesh *m = gs_mesh_for((uint8_t)GS_VEH_STOCK_CAR);
    CHECK(st.tris > 0);
    CHECK(st.tris < m->tri_count);

    if (f.px != nullptr) {
        // The car is on the screen and it is red.
        int red = gs_count_car0(&f);
        CHECK(red > 200);

        // Turned ninety degrees, it covers a visibly different area - which is
        // the whole reason to draw geometry rather than one sprite.
        double cx0 = 0, cy0 = 0;
        CHECK(gs_car0_centroid(&f, &cx0, &cy0));

        gs_world turned = w;
        turned.car[0].heading = GS_DEG(90);
        gs_frame g = gs_render_frame(ren, &t, &turned, &turned, 1.0f, &cam);
        CHECK(g.px != nullptr);
        if (g.px != nullptr) {
            int red2 = gs_count_car0(&g);
            CHECK(red2 > 200);
            CHECK(gs_frame_diff(&f, &g) > 0.5);
        }
        gs_frame_free(&g);

        // And a different vehicle is a different picture. The meshes differ -
        // that is checked above - but this is what says the renderer actually
        // reaches for the one belonging to the car it is drawing, rather than
        // drawing every car as a stock car in six colours.
        gs_world bike = w;
        bike.car[0].vehicle = (uint8_t)GS_VEH_MOTORCYCLE;
        gs_frame h = gs_render_frame(ren, &t, &bike, &bike, 1.0f, &cam);
        CHECK(h.px != nullptr);
        if (h.px != nullptr) {
            int bike_px = gs_count_car0(&h);
            CHECK(bike_px > 50);
            // A motorcycle is a great deal less car than a stock car is.
            CHECK(bike_px < red * 3 / 4);
        }
        gs_frame_free(&h);
    }
    gs_frame_free(&f);
}

TEST(a_car_on_a_slope_leans_with_the_ground) {
    static gs_track ramp;
    gs_flat_pavement(&ramp, 24, 16);
    for (uint8_t y = 0; y <= ramp.h; y++)
        for (uint8_t x = 0; x <= ramp.w; x++)
            gs_track_set_corner(&ramp, x, y, (gs_fix)((int64_t)GS_INT(1) * x / 3));

    gs_camera cam = { 0 };
    cam.zoom = 4.0f;
    cam.vw = GS_W; cam.vh = GS_H;
    cam.cx = 12.0f; cam.cy = 8.0f;
    cam.cz = gs_to_f(gs_track_height(&ramp, GS_INT(12), GS_INT(8)));

    // **The same terrain in both frames.** Comparing a car on a ramp against a
    // car on flat ground compares the *ground*, and passes whatever the car
    // does - which is exactly how the first version of this test passed while
    // the lean was disabled. The only thing that differs here is whether the
    // car is standing on the slope or flying over it.
    gs_frame shots[2];
    for (int variant = 0; variant < 2; variant++) {
        gs_world w;
        gs_world_init(&w, GS_ONE);
        gs_world_add_car(&w, &ramp, (uint8_t)GS_VEH_STOCK_CAR,
                         GS_INT(12), GS_INT(8), 0);
        w.car[0].z = gs_track_height(&ramp, GS_INT(12), GS_INT(8));
        w.car[0].grounded = variant == 0;

        shots[variant] = gs_render_frame(ren, &ramp, &w, &w, 1.0f, &cam);
        CHECK(shots[variant].px != nullptr);
    }

    if (shots[0].px != nullptr && shots[1].px != nullptr) {
        CHECK(gs_count_car0(&shots[0]) > 200);
        CHECK(gs_count_car0(&shots[1]) > 200);

        // Only the car's own pixels are compared, so the identical ground
        // cannot carry the test.
        int only_grounded = 0, only_airborne = 0;
        for (int i = 0; i < GS_W * GS_H; i++) {
            bool a = gs_is_car0(&shots[0].px[i * 4]);
            bool b = gs_is_car0(&shots[1].px[i * 4]);
            if (a && !b) only_grounded++;
            if (b && !a) only_airborne++;
        }
        CHECK(only_grounded > 40);
        CHECK(only_airborne > 40);
    }
    gs_frame_free(&shots[0]);
    gs_frame_free(&shots[1]);
}

// ---------------------------------------------------------------------------
// The front end
// ---------------------------------------------------------------------------

static gs_menu gs_m;

// ---------------------------------------------------------------------------
// The player's guide
// ---------------------------------------------------------------------------

TEST(the_guide_tells_people_to_press_the_keys_the_game_listens_for) {
    (void)ren;

    // **A guide that has drifted is worse than no guide.** It sends somebody to
    // press a key that does nothing and lets them conclude the game is broken,
    // and nothing in a build catches it - so this reads the guide as shipped
    // and checks its control table against the bindings the game actually
    // starts with.
    size_t len = 0;
    void *raw = SDL_LoadFile(GS_SOURCE_DOCS "/GUIDE.md", &len);
    CHECK(raw != nullptr);
    if (raw == nullptr) return;

    const char *doc = (const char *)raw;

    gs_bindings b;
    gs_bind_defaults(&b);

    // Each row of the table: the two keys the guide names, and the action.
    static const struct {
        const char *row;
        gs_action   action;
        SDL_Scancode one, two;
    } rows[] = {
        { "| accelerate | Up | W |",              GS_ACT_ACCEL,
          SDL_SCANCODE_UP, SDL_SCANCODE_W },
        { "| brake / reverse | Down | S |",       GS_ACT_BRAKE,
          SDL_SCANCODE_DOWN, SDL_SCANCODE_S },
        { "| steer left | Left | A |",            GS_ACT_LEFT,
          SDL_SCANCODE_LEFT, SDL_SCANCODE_A },
        { "| steer right | Right | D |",          GS_ACT_RIGHT,
          SDL_SCANCODE_RIGHT, SDL_SCANCODE_D },
        { "| drop a hazard | Right Shift | Left Shift |", GS_ACT_FIRE,
          SDL_SCANCODE_RSHIFT, SDL_SCANCODE_LSHIFT },
    };

    for (size_t i = 0; i < SDL_arraysize(rows); i++) {
        // The guide says this row...
        CHECK(SDL_strstr(doc, rows[i].row) != nullptr);
        // ...and the game agrees.
        CHECK(b.key[0][rows[i].action] == rows[i].one);
        CHECK(b.key[1][rows[i].action] == rows[i].two);
    }

    // The guide says players three and four have no keyboard controls. If that
    // ever stops being true, the sentence saying so has to go.
    CHECK(SDL_strstr(doc, "Players three and four need a gamepad") != nullptr);
    for (int a = 0; a < GS_ACT_COUNT; a++) {
        CHECK(b.key[2][a] == GS_KEY_NONE);
        CHECK(b.key[3][a] == GS_KEY_NONE);
    }

    // And the commands it tells people to type have to be commands. These are
    // spelled exactly as the guide spells them, so a rename breaks the test
    // rather than the reader.
    static const char *const commands[] = {
        "gearstick_cli selftest --verify",
        "gearstick --host 47000 4",
        "gearstick --join their-address 47000",
    };
    for (size_t i = 0; i < SDL_arraysize(commands); i++) {
        CHECK(SDL_strstr(doc, commands[i]) != nullptr);
    }

    SDL_free(raw);
}

TEST(the_release_notes_say_what_is_actually_shipped) {
    (void)ren;

    // The release document tells people what they are downloading and what
    // their computer will say about it. The names of the files are the part
    // most likely to drift - a rename in CPack and this quietly starts
    // describing files nobody has - so they are checked against the pattern
    // CMakeLists.txt builds them from.
    size_t len = 0;
    void *raw = SDL_LoadFile(GS_SOURCE_DOCS "/RELEASES.md", &len);
    CHECK(raw != nullptr);
    if (raw == nullptr) return;

    const char *doc = (const char *)raw;

    // **Against the build, not against the document.** The first version of
    // this checked that the notes contained the strings the notes contained,
    // which is a test that can only ever pass - and it duly passed while the
    // notes named a Windows file the packaging has never produced. This asks
    // the build what it calls a package on this platform and looks for that.
    char expected[128];
    SDL_snprintf(expected, sizeof expected, "gearstick-VERSION-%s.",
                 GS_PACKAGE_PLATFORM);
    CHECK(SDL_strstr(doc, expected) != nullptr);

    static const char *const must_say[] = {
        // The other two platforms, which this machine cannot ask the build
        // about. CI runs this test on all three, so between them every name is
        // checked against the build that makes it.
        "gearstick-VERSION-linux-x86_64.tar.gz",
        "gearstick-VERSION-macos-arm64.dmg",
        "gearstick-VERSION-windows-x64.zip",
        // The two programs inside it.
        "gearstick_cli",
        "gearstick_cli selftest --verify",
        // And the honest part, which is the reason the document exists.
        "not code-signed",
        "gh attestation verify",
        "Run anyway",          // what Windows makes you click
        "Open Anyway",         // what macOS makes you click
    };
    for (size_t i = 0; i < SDL_arraysize(must_say); i++) {
        CHECK(SDL_strstr(doc, must_say[i]) != nullptr);
    }

    SDL_free(raw);
}

TEST(a_time_reads_the_way_people_say_it) {
    (void)ren;
    char text[32];

    gs_time_text(text, sizeof text, 0);
    CHECK(SDL_strcmp(text, "-") == 0);            // never finished

    gs_time_text(text, sizeof text, GS_TICK_HZ * 5);
    CHECK(SDL_strcmp(text, "5.00") == 0);

    gs_time_text(text, sizeof text, GS_TICK_HZ * 65 + GS_TICK_HZ / 2);
    CHECK(SDL_strcmp(text, "1:05.50") == 0);

    // A minute is where the format changes, so it is the interesting one.
    gs_time_text(text, sizeof text, GS_TICK_HZ * 59);
    CHECK(SDL_strcmp(text, "59.00") == 0);
    gs_time_text(text, sizeof text, GS_TICK_HZ * 60);
    CHECK(SDL_strcmp(text, "1:00.00") == 0);
}

TEST(a_finished_race_becomes_a_table_in_the_order_it_finished) {
    (void)ren;

    static gs_track t;
    gs_flat_pavement(&t, 40, 16);
    gs_track_add_gate(&t, GS_INT(6), GS_INT(8), 0, GS_INT(6));
    gs_track_add_gate(&t, GS_INT(30), GS_INT(8), 0, GS_INT(6));

    gs_menu_init(&gs_m);
    CHECK(gs_profile_add(&gs_m.profiles, "ada", GS_COLOUR_ORANGE, 0) == 0);
    CHECK(gs_profile_add(&gs_m.profiles, "bez", GS_COLOUR_PURPLE, 0) == 1);
    gs_m.setup.players = 3;
    gs_m.setup.profile[0] = 0;
    gs_m.setup.profile[1] = 1;
    gs_m.setup.profile[2] = -1;      // a guest

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_set_mode(&w, GS_MODE_RACE);
    gs_world_set_laps(&w, 3);
    for (uint8_t i = 0; i < 3; i++) {
        gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR,
                         GS_INT(4), GS_INT(6) + GS_INT(2) * i, 0);
    }

    // Car 1 finished first, car 0 second, car 2 never finished at all - which
    // is the case that matters, because "did not finish" still has a position.
    w.car[0].finish_tick = 2000; w.car[0].laps = 3; w.car[0].best_lap = 600;
    w.car[1].finish_tick = 1500; w.car[1].laps = 3; w.car[1].best_lap = 480;
    w.car[2].finish_tick = 0;    w.car[2].laps = 2; w.car[2].wrecked = true;

    gs_menu_finish(&gs_m, &w, &t);

    CHECK(gs_m.result_count == 3);
    CHECK(gs_m.result[0].car == 1);
    CHECK(gs_m.result[0].place == 1);
    CHECK(gs_m.result[1].car == 0);
    CHECK(gs_m.result[2].car == 2);
    CHECK(gs_m.result[2].wrecked);

    // The first person round set both records; the second beat neither.
    CHECK(gs_m.result[0].beat_lap);
    CHECK(gs_m.result[0].beat_race);
    CHECK(!gs_m.result[1].beat_lap);
    CHECK(!gs_m.result[1].beat_race);

    // Two rows, not three: a guest has not said who they are, and inventing a
    // row for them would fill the table with "guest".
    CHECK(gs_m.records.count == 2);

    // And the people who drove have a history now.
    CHECK(gs_m.profiles.entry[1].races == 1);
    CHECK(gs_m.profiles.entry[1].wins == 1);
    CHECK(gs_m.profiles.entry[0].wins == 0);
    CHECK(gs_m.profiles.entry[0].races == 1);
}

TEST(the_store_remembers_drivers_and_records_between_runs) {
    (void)ren;

    gs_menu_init(&gs_m);
    gs_profile_add(&gs_m.profiles, "ada", GS_COLOUR_ORANGE,
                   (uint8_t)GS_VEH_BAJA_BUG);
    gs_profile_raced(&gs_m.profiles, 0, true, true, false, 500, 1700000000ull);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_records_submit(&gs_m.records, 0xfeedULL, gs_conditions_hash(&w),
                      (uint8_t)GS_VEH_BAJA_BUG, (uint8_t)GS_MODE_RACE, 3,
                      4200, 13000, "ada", 1700000000ull);

    static uint8_t buf[sizeof(gs_profiles) + sizeof(gs_records) + 4096];
    size_t n = gs_menu_save(&gs_m, buf, sizeof buf);
    CHECK(n > 0);

    // A different program, starting cold.
    static gs_menu back;
    gs_menu_init(&back);
    CHECK(gs_menu_load(&back, buf, n));

    CHECK(back.profiles.count == 1);
    CHECK(SDL_strcmp(back.profiles.entry[0].name, "ada") == 0);
    CHECK(back.profiles.entry[0].colour == GS_COLOUR_ORANGE);
    CHECK(back.profiles.entry[0].wins == 1);
    CHECK(back.profiles.entry[0].tiles == 500);

    const gs_record *r = gs_records_best_lap(&back.records, 0xfeedULL,
                                             gs_conditions_hash(&w));
    CHECK(r != nullptr);
    if (r != nullptr) {
        CHECK(r->lap == 4200);
        CHECK(SDL_strcmp(r->who, "ada") == 0);
    }

    // Rubbish is refused rather than half-read, which matters here more than
    // most places: a half-read store is somebody's history with holes in it.
    CHECK(!gs_menu_load(&back, buf, 4));
    buf[0] ^= 0xffu;
    CHECK(!gs_menu_load(&back, buf, n));
}

TEST(a_store_with_tracks_in_it_is_saved_whole) {
    (void)ren;

    // **The library is the big half of a store and was the half nobody sized
    // for.** One track serialises to about four kilobytes, the twenty-two that
    // ship to about ninety, and the frontend held the roster, the records and
    // four kilobytes of slack - so every save refused and nothing anybody did
    // was ever written down. It looked like a disk problem in the log, because
    // the message printed SDL's last error and SDL had not been asked to do
    // anything.
    gs_menu_init(&gs_m);
    CHECK(gs_profile_add(&gs_m.profiles, "ada", GS_COLOUR_ORANGE,
                         (uint8_t)GS_VEH_BAJA_BUG) == 0);

    // **Tracks that ship, not tracks made of nothing.** A flat track built in
    // a loop compresses to almost nothing and would fit in any buffer, which
    // would make this pass while saying nothing at all. These are the real
    // files, each copy nudged by one corner so that it is its own track.
    const char *assets = gs_assets_dir();
    CHECK(assets != nullptr);
    if (assets == nullptr) return;

    static const char *const shipped[] = {
        "first-light", "the-long-drop", "ice-house", "jupiter-run",
    };

    static gs_track t;
    for (int i = 0; i < GS_LIBRARY_MAX; i++) {
        char path[1024];
        SDL_snprintf(path, sizeof path, "%s/tracks/%s.gstrack", assets,
                     shipped[(size_t)i % SDL_arraysize(shipped)]);
        size_t len = 0;
        void *bytes = SDL_LoadFile(path, &len);
        CHECK(bytes != nullptr);
        if (bytes == nullptr) return;
        CHECK(gs_track_deserialize(&t, (const uint8_t *)bytes, len));
        SDL_free(bytes);

        t.corner[i] = (int16_t)(t.corner[i] + 1);
        char name[GS_LIBRARY_NAME];
        SDL_snprintf(name, sizeof name, "track number %d", i + 1);
        CHECK(gs_library_put(&gs_m.library, &t, name, "ada") >= 0);
    }
    CHECK(gs_m.library.count == GS_LIBRARY_MAX);

    // What the old guess was: everything except the thing that takes the room.
    // It has to be refused rather than half-written - a store with three of
    // somebody's four tracks in it is worse than one that failed loudly.
    static uint8_t guessed[sizeof(gs_profiles) + sizeof(gs_records) + 4096];
    CHECK(gs_menu_size(&gs_m) > sizeof guessed);
    CHECK(gs_menu_save(&gs_m, guessed, sizeof guessed) == 0);

    // And what the size says, which is the number the frontend now asks for.
    size_t cap = gs_menu_size(&gs_m);
    uint8_t *buf = (uint8_t *)SDL_malloc(cap);
    CHECK(buf != nullptr);
    if (buf == nullptr) return;

    size_t n = gs_menu_save(&gs_m, buf, cap);
    CHECK(n > 0);
    CHECK(n <= cap);

    static gs_menu back;
    gs_menu_init(&back);
    CHECK(gs_menu_load(&back, buf, n));
    CHECK(back.library.count == GS_LIBRARY_MAX);
    CHECK(back.profiles.count == 1);

    // Every track by name and by identity, because a library that comes back
    // with four entries and the wrong tracks in them would pass a count.
    for (int i = 0; i < GS_LIBRARY_MAX; i++) {
        const gs_library_entry *was = gs_library_at(&gs_m.library, i);
        const gs_library_entry *now = gs_library_at(&back.library, i);
        CHECK(was != nullptr && now != nullptr);
        if (was == nullptr || now == nullptr) continue;
        CHECK(was->hash == now->hash);
        CHECK(SDL_strcmp(was->name, now->name) == 0);
    }

    SDL_free(buf);
}

TEST(the_stock_tracks_ship_and_are_worth_racing) {
    (void)ren;

    // **The tracks that ship are data, not C.** This reads the files as
    // installed - if the frontend went back to carrying a track, or the tracks
    // stopped being copied into a package, this is what notices.
    char dir[1024];
    const char *assets = gs_assets_dir();
    CHECK(assets != nullptr);
    if (assets == nullptr) return;

    static const char *const names[] = {
        "first-light", "the-long-drop", "ice-house", "jupiter-run",
    };

    for (size_t i = 0; i < SDL_arraysize(names); i++) {
        SDL_snprintf(dir, sizeof dir, "%s/tracks/%s.gstrack", assets, names[i]);

        size_t len = 0;
        void *bytes = SDL_LoadFile(dir, &len);
        CHECK(bytes != nullptr);
        if (bytes == nullptr) continue;

        static gs_track t;
        CHECK(gs_track_deserialize(&t, (const uint8_t *)bytes, len));
        SDL_free(bytes);

        // A route somebody can actually drive, which is the difference between
        // a track and a field.
        CHECK(gs_track_validate(&t).problem == GS_TRACK_OK);
        CHECK(t.gate_count >= 2);
        CHECK(t.w >= 24 && t.h >= 12);

        // And it is not flat: a stock track with no elevation would mean the
        // generator wrote nothing and nobody looked.
        bool raised = false;
        for (uint8_t y = 0; y <= t.h && !raised; y++) {
            for (uint8_t x = 0; x <= t.w; x++) {
                if (t.corner[(size_t)y * GS_CORNER_STRIDE + x] != 0) {
                    raised = true;
                    break;
                }
            }
        }
        CHECK(raised);
    }
}

TEST(choosing_a_track_from_the_library_changes_what_is_raced) {
    (void)ren;

    // The library screen hands a choice to the frontend once, and the frontend
    // loads it. Driven here without ImGui, because what is being checked is the
    // handover rather than the drawing - a screenshot shows the drawing.
    gs_menu_init(&gs_m);

    static gs_track t[3];
    for (int i = 0; i < 3; i++) {
        gs_flat_pavement(&t[i], (uint8_t)(30 + i * 2), 16);
        gs_track_set_corner(&t[i], (uint8_t)(5 + i), 8, GS_INT(2));
        gs_track_add_gate(&t[i], GS_INT(4), GS_INT(8), 0, GS_INT(5));
        char name[GS_LIBRARY_NAME];
        SDL_snprintf(name, sizeof name, "track %d", i);
        gs_library_put(&gs_m.library, &t[i], name, "ada");
    }

    // Nothing chosen yet.
    CHECK(gs_menu_take_choice(&gs_m) == -1);

    // **Not the first one**, because loading the first track and loading
    // nothing look identical when the first track is what is already there.
    gs_m.picked = 2;
    gs_m.take = gs_m.picked;

    int take = gs_menu_take_choice(&gs_m);
    CHECK(take == 2);

    const gs_track *picked = gs_library_track(&gs_m.library, take);
    CHECK(picked != nullptr);
    if (picked != nullptr) {
        CHECK(gs_track_hash(picked) == gs_track_hash(&t[2]));
        CHECK(gs_track_hash(picked) != gs_track_hash(&t[0]));
    }

    // Taken once. A choice acted on every frame would reload the track
    // continuously and undo any editing the moment it happened.
    CHECK(gs_menu_take_choice(&gs_m) == -1);

    // And an out-of-range choice is nothing rather than something.
    CHECK(gs_library_track(&gs_m.library, 99) == nullptr);
    CHECK(gs_library_track(&gs_m.library, -1) == nullptr);
}

TEST(loading_a_track_throws_away_the_undo_history) {
    (void)ren;

    // **Correctness, not tidiness.** The log records a cell changing from one
    // value to another, and those values belong to the track that was being
    // edited. Undoing after a load would apply one track's edits to another.
    static gs_track a, b;
    gs_flat_pavement(&a, 24, 12);
    gs_flat_pavement(&b, 24, 12);
    gs_track_set_corner(&b, 3, 3, GS_INT(4));

    gs_editor ed;
    CHECK(gs_editor_init(&ed, 4096));

    gs_edit_begin(ed.log);
    gs_edit_corner(ed.log, &a, 10, 6, GS_INT(2));
    gs_edit_end(ed.log);
    CHECK(gs_edit_can_undo(ed.log));

    // A different track is loaded.
    uint64_t before = gs_track_hash(&b);
    gs_edit_reset(ed.log);

    CHECK(!gs_edit_can_undo(ed.log));
    CHECK(!gs_edit_can_redo(ed.log));
    CHECK(gs_edit_undo_depth(ed.log) == 0);

    // Undo now does nothing at all, rather than reaching into the new track.
    CHECK(!gs_edit_undo(ed.log, &b));
    CHECK(gs_track_hash(&b) == before);

    // And the log still works afterwards.
    gs_edit_begin(ed.log);
    gs_edit_corner(ed.log, &b, 8, 4, GS_INT(1));
    gs_edit_end(ed.log);
    CHECK(gs_edit_can_undo(ed.log));
    CHECK(gs_edit_undo(ed.log, &b));
    CHECK(gs_track_hash(&b) == before);

    gs_editor_quit(&ed);
}

TEST(the_store_carries_the_library_too) {
    (void)ren;

    gs_menu_init(&gs_m);
    gs_profile_add(&gs_m.profiles, "ada", GS_COLOUR_ORANGE, 0);

    static gs_track t[3];
    for (int i = 0; i < 3; i++) {
        gs_flat_pavement(&t[i], (uint8_t)(30 + i), 16);
        gs_track_set_corner(&t[i], (uint8_t)(5 + i), 8, GS_INT(2));
        gs_track_add_gate(&t[i], GS_INT(4), GS_INT(8), 0, GS_INT(5));

        char name[GS_LIBRARY_NAME];
        SDL_snprintf(name, sizeof name, "track %d", i);
        CHECK(gs_library_put(&gs_m.library, &t[i], name, "ada") == i);
    }
    CHECK(gs_m.library.count == 3);

    static uint8_t buf[sizeof(gs_profiles) + sizeof(gs_records) +
                       GS_LIBRARY_MAX * (GS_TRACK_TILES * 4 + 4096) + 8192];
    size_t n = gs_menu_save(&gs_m, buf, sizeof buf);
    CHECK(n > 0);

    // A different program, starting cold.
    static gs_menu back;
    gs_menu_init(&back);
    CHECK(gs_menu_load(&back, buf, n));

    CHECK(back.profiles.count == 1);
    CHECK(back.library.count == 3);
    for (int i = 0; i < 3; i++) {
        const gs_library_entry *e = gs_library_at(&back.library, i);
        CHECK(e != nullptr);
        if (e == nullptr) continue;
        CHECK(e->hash == gs_track_hash(&t[i]));
        CHECK(SDL_strcmp(e->author, "ada") == 0);
    }

    // The three are still three different tracks, not one repeated.
    CHECK(gs_library_at(&back.library, 0)->hash !=
          gs_library_at(&back.library, 1)->hash);
    CHECK(gs_library_at(&back.library, 1)->hash !=
          gs_library_at(&back.library, 2)->hash);
}

TEST(an_empty_store_round_trips_rather_than_failing) {
    (void)ren;

    // The first run of the game on a new machine, saved and read back. An empty
    // store that could not be written would mean nothing was ever remembered.
    gs_menu_init(&gs_m);
    static uint8_t buf[256];
    size_t n = gs_menu_save(&gs_m, buf, sizeof buf);
    CHECK(n > 0);

    static gs_menu back;
    gs_menu_init(&back);
    CHECK(gs_menu_load(&back, buf, n));
    CHECK(back.profiles.count == 0);
    CHECK(back.records.count == 0);
}

TEST(a_track_goes_out_through_the_clipboard_and_comes_back_the_same) {
    (void)ren;

    static gs_track t;
    gs_flat_pavement(&t, 32, 20);
    for (uint8_t x = 8; x < 14; x++)
        for (uint8_t y = 0; y <= t.h; y++)
            gs_track_set_corner(&t, x, y, GS_INT(2));
    gs_track_set_surface(&t, 20, 10, GS_SURF_ICE);
    gs_track_add_gate(&t, GS_INT(4), GS_INT(10), 0, GS_INT(5));
    gs_track_add_gate(&t, GS_INT(26), GS_INT(10), 0, GS_INT(5));
    uint64_t sent = gs_track_hash(&t);

    gs_editor ed;
    CHECK(gs_editor_init(&ed, 65536));
    gs_editor_copy_code(&ed, &t);
    CHECK(SDL_strstr(ed.status, "copied") != nullptr);

    // Somebody else's editor, with their own track in it.
    static gs_track theirs;
    gs_flat_pavement(&theirs, 12, 8);
    CHECK(gs_track_hash(&theirs) != sent);

    gs_editor them;
    CHECK(gs_editor_init(&them, 65536));
    gs_editor_paste_code(&them, &theirs);
    CHECK(gs_track_hash(&theirs) == sent);
    CHECK(theirs.w == t.w && theirs.h == t.h);
    CHECK(theirs.gate_count == t.gate_count);

    // And it is one undo step, not thirty thousand.
    gs_edit_undo(them.log, &theirs);
    CHECK(gs_track_hash(&theirs) != sent);

    // Rubbish on the clipboard is refused rather than half-applied.
    CHECK(SDL_SetClipboardText("not a track code"));
    uint64_t before = gs_track_hash(&theirs);
    gs_editor_paste_code(&them, &theirs);
    CHECK(gs_track_hash(&theirs) == before);
    CHECK(SDL_strstr(them.status, "not a code") != nullptr);

    gs_editor_quit(&ed);
    gs_editor_quit(&them);
}

TEST(the_heatmap_puts_the_line_everybody_drove_on_the_screen) {
    static gs_track t;
    gs_routed_pavement(&t);

    gs_editor ed;
    CHECK(gs_editor_init(&ed, 8192));

    // Nothing to paint until the sweep has run.
    CHECK(gs_editor_heat(&ed) == nullptr);

    gs_editor_analyse(&ed, &t);
    CHECK(ed.analysed);
    CHECK(ed.heat.completable);
    CHECK(gs_editor_heat(&ed) != nullptr);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(4), GS_INT(8), 0);

    gs_camera cam = { 0 };
    cam.zoom = GS_ISO_DEFAULT_ZOOM;
    cam.vw = GS_W; cam.vh = GS_H;
    cam.cx = 20.0f; cam.cy = 8.0f;

    gs_view view = { 0 };
    view.cam = cam;
    view.rect = (SDL_Rect){ 0, 0, GS_W, GS_H };

    // Cold first: the same scene with the heatmap off has nothing green in it.
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    gs_render_view(ren, &t, &w, &w, 1.0f, &view);
    SDL_Surface *raw = SDL_RenderReadPixels(ren, nullptr);
    CHECK(raw != nullptr);
    gs_frame cold = { 0 };
    if (raw != nullptr) {
        cold.own = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(raw);
        if (cold.own != nullptr) cold.px = (uint8_t *)cold.own->pixels;
    }

    view.heat = gs_editor_heat(&ed);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    gs_render_view(ren, &t, &w, &w, 1.0f, &view);
    raw = SDL_RenderReadPixels(ren, nullptr);
    CHECK(raw != nullptr);
    gs_frame warm = { 0 };
    if (raw != nullptr) {
        warm.own = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(raw);
        if (warm.own != nullptr) warm.px = (uint8_t *)warm.own->pixels;
    }

    if (cold.px != nullptr && warm.px != nullptr) {
        CHECK(gs_count_hot(&cold) == 0);
        // Not "some green somewhere": the used line is a band across a forty
        // tile track, so it is thousands of pixels.
        CHECK(gs_count_hot(&warm) > 500);
        // And it is a *line*, not a wash over everything - most of the track
        // is ground nobody had any reason to drive on.
        CHECK(gs_count_hot(&warm) < GS_W * GS_H / 4);
    }
    gs_frame_free(&cold);
    gs_frame_free(&warm);

    // Switching it off puts the terrain back exactly as it was.
    ed.heat_on = false;
    CHECK(gs_editor_heat(&ed) == nullptr);

    gs_editor_quit(&ed);
}

TEST(the_analyser_refuses_a_track_with_no_route_rather_than_guessing) {
    (void)ren;

    static gs_track t;
    gs_flat_pavement(&t, 24, 12);   // no gates at all

    gs_editor ed;
    CHECK(gs_editor_init(&ed, 4096));
    gs_editor_analyse(&ed, &t);

    CHECK(!ed.analysed);
    CHECK(gs_editor_heat(&ed) == nullptr);
    CHECK(SDL_strstr(ed.status, "route") != nullptr);

    // Give it a route and it has something to say.
    gs_track_add_gate(&t, GS_INT(4), GS_INT(6), 0, GS_INT(4));
    gs_track_add_gate(&t, GS_INT(18), GS_INT(6), 0, GS_INT(4));
    gs_editor_analyse(&ed, &t);
    CHECK(ed.analysed);
    CHECK(ed.heat_track == gs_track_hash(&t));

    // And it knows when it is out of date, which is the difference between a
    // stale heatmap and a lie.
    gs_track_set_corner(&t, 10, 6, GS_INT(3));
    CHECK(ed.heat_track != gs_track_hash(&t));

    gs_editor_quit(&ed);
}

TEST(changed_controls_survive_being_written_and_read_back) {
    (void)ren;

    gs_bindings b, back;
    gs_bind_defaults(&b);
    gs_bind_set_key(&b, 3, GS_ACT_FIRE, SDL_SCANCODE_F);
    gs_bind_set_button(&b, 1, GS_ACT_ACCEL, (int16_t)SDL_GAMEPAD_BUTTON_NORTH);
    gs_bind_set_key(&b, 0, GS_ACT_BRAKE, GS_KEY_NONE);

    uint8_t buf[512];
    size_t n = gs_bind_serialize(&b, buf, sizeof buf);
    CHECK(n == gs_bind_size());
    CHECK(gs_bind_deserialize(&back, buf, n));

    for (uint8_t p = 0; p < GS_MAX_CARS; p++) {
        for (int a = 0; a < GS_ACT_COUNT; a++) {
            CHECK(back.key[p][a] == b.key[p][a]);
            CHECK(back.button[p][a] == b.button[p][a]);
        }
    }

    // A refused file leaves the player driving with the controls they had,
    // rather than with half of somebody else's.
    gs_bindings kept;
    gs_bind_defaults(&kept);
    SDL_Scancode was = kept.key[0][GS_ACT_ACCEL];

    CHECK(!gs_bind_deserialize(&kept, buf, 4));
    buf[0] ^= 0xffu;
    CHECK(!gs_bind_deserialize(&kept, buf, n));
    CHECK(kept.key[0][GS_ACT_ACCEL] == was);

    CHECK(gs_bind_serialize(&b, buf, 8) == 0);
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The HUD
// ---------------------------------------------------------------------------

// Dear ImGui, started once for the tests that need it. The HUD is drawn through
// it, so a test that measured the HUD without one would be measuring an empty
// frame and passing.
static bool gs_imgui_ready = false;

static void gs_imgui_start(SDL_Window *win, SDL_Renderer *ren) {
    if (gs_imgui_ready) return;
    ImGui_CreateContext(nullptr);
    gs_style_menu();
    ImGui_GetIO()->IniFilename = nullptr;
    gs_imgui_ready = cImGui_ImplSDL3_InitForSDLRenderer(win, ren) &&
                     cImGui_ImplSDLRenderer3_Init(ren);
}

// One frame: the race, then the HUD over it, captured.
static void gs_hud_frame(SDL_Renderer *ren, const gs_track *t, const gs_world *w,
                         const gs_view *v, gs_frame *out) {
    cImGui_ImplSDLRenderer3_NewFrame();
    cImGui_ImplSDL3_NewFrame();
    ImGui_NewFrame();

    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    gs_render_view(ren, t, w, w, 1.0f, v);
    gs_hud_draw(w, t, v, (uint32_t)w->tick, 0.0f, false);

    ImGui_Render();
    cImGui_ImplSDLRenderer3_RenderDrawData(ImGui_GetDrawData(), ren);

    SDL_Surface *raw = SDL_RenderReadPixels(ren, nullptr);
    *out = (gs_frame){ 0 };
    if (raw != nullptr) {
        out->own = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(raw);
        if (out->own != nullptr) out->px = (uint8_t *)out->own->pixels;
    }
}

// How many pixels differ inside the corner the HUD occupies. Only that corner,
// because the cars move between the frames being compared and the whole-frame
// difference would then be true whatever the HUD did.
#define GS_HUD_BOX_W 170
#define GS_HUD_BOX_H 340

static int gs_hud_pixels_differing(const gs_frame *a, const gs_frame *b) {
    if (a->px == nullptr || b->px == nullptr) return -1;
    int n = 0;
    for (int y = 0; y < GS_HUD_BOX_H && y < GS_H; y++) {
        for (int x = 0; x < GS_HUD_BOX_W && x < GS_W; x++) {
            size_t at = ((size_t)y * (size_t)GS_W + (size_t)x) * 4;
            if (a->px[at] != b->px[at] || a->px[at + 1] != b->px[at + 1] ||
                a->px[at + 2] != b->px[at + 2]) {
                n++;
            }
        }
    }
    return n;
}

// How much of the frame is the arc's colour: a warm yellow nothing else in the
// scene uses.
static int gs_count_arc(const gs_frame *f) {
    int n = 0;
    for (int i = 0; i < GS_W * GS_H; i++) {
        const uint8_t *px = &f->px[i * 4];
        if (px[0] > 150 && px[1] > 110 && px[2] < 130 && px[0] > px[2] + 60) n++;
    }
    return n;
}

TEST(the_landing_arc_is_off_until_it_is_asked_for) {
    // **Off by default and not by accident.** The arc being not negotiable is
    // what makes the take-off decision matter, so a permanent readout of where
    // you are going to land would turn a judgement into a number to follow.
    static gs_track t;
    gs_track_init(&t, 64, 16, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= t.h; y++) {
        for (uint8_t x = 0; x <= t.w; x++) {
            gs_track_set_corner(&t, x, y, x < 10 ? GS_INT(3) : 0);
        }
    }

    // Off the shelf and into the air.
    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_SPRINT_CAR, GS_INT(4), GS_INT(8), 0);
    for (int i = 0; i < GS_TICK_HZ * 30 && w.car[0].grounded; i++) {
        gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, 0, 0, 0 };
        gs_world_step(&w, &t, in);
    }
    CHECK(!w.car[0].grounded);

    gs_view view = { 0 };
    view.cam.zoom = GS_ISO_DEFAULT_ZOOM;
    view.cam.vw = GS_W; view.cam.vh = GS_H;
    view.cam.cx = gs_to_f(w.car[0].x) + 4.0f;
    view.cam.cy = gs_to_f(w.car[0].y);
    view.rect = (SDL_Rect){ 0, 0, GS_W, GS_H };

    int ink[2] = { 0, 0 };
    for (int on = 0; on < 2; on++) {
        view.show_arc = on != 0;

        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        gs_render_view(ren, &t, &w, &w, 1.0f, &view);

        SDL_Surface *raw = SDL_RenderReadPixels(ren, nullptr);
        CHECK(raw != nullptr);
        if (raw == nullptr) return;
        gs_frame f = { 0 };
        f.own = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(raw);
        if (f.own == nullptr) return;
        f.px = (uint8_t *)f.own->pixels;
        ink[on] = gs_count_arc(&f);
        gs_frame_free(&f);
    }

    // Nothing at all with it off, and a real path with it on.
    CHECK(ink[0] == 0);
    CHECK(ink[1] > 20);
}

TEST(there_is_no_arc_drawn_for_a_car_on_the_ground) {
    // A parked car gets no line to where it is standing.
    static gs_track t;
    gs_flat_pavement(&t, 40, 16);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(20), GS_INT(8), 0);

    gs_view view = { 0 };
    view.cam.zoom = GS_ISO_DEFAULT_ZOOM;
    view.cam.vw = GS_W; view.cam.vh = GS_H;
    view.cam.cx = 20.0f; view.cam.cy = 8.0f;
    view.rect = (SDL_Rect){ 0, 0, GS_W, GS_H };
    view.show_arc = true;

    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    gs_render_view(ren, &t, &w, &w, 1.0f, &view);

    SDL_Surface *raw = SDL_RenderReadPixels(ren, nullptr);
    CHECK(raw != nullptr);
    if (raw == nullptr) return;
    gs_frame f = { 0 };
    f.own = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(raw);
    if (f.own == nullptr) return;
    f.px = (uint8_t *)f.own->pixels;

    CHECK(gs_count_arc(&f) == 0);
    gs_frame_free(&f);
}

TEST(a_wreck_is_drawn_as_wide_as_the_obstacle_it_actually_is) {
    // The physics gives a wreck a bigger radius than the car it used to be,
    // because debris is a spread of parts. **The drawing has to agree**: a wreck
    // drawn car-sized over a wreck-sized obstacle is the worst of both - it
    // catches you on something you were shown you would clear.
    static gs_track t;
    gs_flat_pavement(&t, 40, 24);

    gs_view view = { 0 };
    view.cam.zoom = GS_ISO_DEFAULT_ZOOM;
    view.cam.vw = GS_W; view.cam.vh = GS_H;
    view.cam.cx = 20.0f; view.cam.cy = 12.0f;
    view.rect = (SDL_Rect){ 0, 0, GS_W, GS_H };

    int ink[2] = { 0, 0 };
    for (int wrecked = 0; wrecked < 2; wrecked++) {
        gs_world w;
        gs_world_init(&w, GS_ONE);
        gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(20), GS_INT(12), 0);
        if (wrecked) { w.car[0].damage = 255; w.car[0].wrecked = true; }

        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        gs_render_view(ren, &t, &w, &w, 1.0f, &view);

        SDL_Surface *raw = SDL_RenderReadPixels(ren, nullptr);
        CHECK(raw != nullptr);
        if (raw == nullptr) return;
        gs_frame f = { 0 };
        f.own = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(raw);
        if (f.own == nullptr) return;
        f.px = (uint8_t *)f.own->pixels;

        // Anything that is not the ground it is standing on: the car, its
        // shadow, the debris. The shadow matters most - it is what says how much
        // ground a thing covers - so this cannot count the paint colour alone.
        //
        // **The reference is taken from the picture, not from the palette.** The
        // terrain is shaded, so a pavement pixel is nowhere near the flat
        // pavement colour, and comparing against the palette marks every pixel
        // in the frame as interesting.
        const uint8_t *bare = &f.px[((GS_H / 2) * GS_W + 4) * 4];
        float gr = bare[0] / 255.0f, gg = bare[1] / 255.0f, gb = bare[2] / 255.0f;

        for (int i = 0; i < GS_W * GS_H; i++) {
            const uint8_t *px = &f.px[i * 4];
            float dr = px[0] / 255.0f - gr;
            float dg = px[1] / 255.0f - gg;
            float db = px[2] / 255.0f - gb;
            if (SDL_sqrtf(dr * dr + dg * dg + db * db) > 0.06f) ink[wrecked]++;
        }
        gs_frame_free(&f);
    }

    // Both drew something, and the wreck covers meaningfully more ground.
    CHECK(ink[0] > 200);
    CHECK(ink[1] > ink[0] * 5 / 4);
}

TEST(no_two_grounds_are_drawn_the_same_colour) {
    (void)ren;

    // **Measured, not looked at.** The first version of this palette had gravel,
    // dust and rock as the same grey at three brightnesses, which reads fine on
    // a flat plane and vanishes the moment the ground tilts: shading changes a
    // tile's brightness by more than those differed by, so on a hillside they
    // were one surface. A player has to know what they are about to drive onto.
    for (uint8_t a = 0; a < GS_SURF_COUNT; a++) {
        SDL_FColor ca = gs_render_surface_colour((gs_surface)a);

        // In range, and not so dark or so pale that shading has nowhere to go.
        CHECK(ca.r > 0.1f && ca.r < 0.98f);
        CHECK(ca.g > 0.1f && ca.g < 0.98f);
        CHECK(ca.b > 0.1f && ca.b < 0.98f);

        for (uint8_t b = (uint8_t)(a + 1); b < GS_SURF_COUNT; b++) {
            SDL_FColor cb = gs_render_surface_colour((gs_surface)b);
            float dr = ca.r - cb.r, dg = ca.g - cb.g, db = ca.b - cb.b;
            CHECK(SDL_sqrtf(dr * dr + dg * dg + db * db) > 0.15f);
        }
    }
}

TEST(the_hud_says_what_lap_it_is_and_changes_when_the_lap_does) {
    // **The fact the HUD exists for.** A race whose state you can only learn
    // afterwards is a scoreboard, not a race - so what the simulation knows has
    // to be on the screen while it is still worth knowing.
    gs_imgui_start(gs_win, ren);
    CHECK(gs_imgui_ready);
    if (!gs_imgui_ready) return;

    static gs_track t;
    gs_routed_pavement(&t);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_set_laps(&w, 3);

    gs_fix sx, sy; gs_angle facing;
    gs_track_grid(&t, 0, &sx, &sy, &facing);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR, sx, sy, facing);

    gs_view view = { 0 };
    view.cam.zoom = GS_ISO_DEFAULT_ZOOM;
    view.cam.vw = GS_W; view.cam.vh = GS_H;
    view.cam.cx = gs_to_f(w.car[0].x); view.cam.cy = gs_to_f(w.car[0].y);
    view.rect = (SDL_Rect){ 0, 0, GS_W, GS_H };

    // The HUD is *there*: the same frame without it differs in that corner.
    gs_frame with = { 0 }, without = { 0 };
    gs_hud_frame(ren, &t, &w, &view, &with);

    cImGui_ImplSDLRenderer3_NewFrame();
    cImGui_ImplSDL3_NewFrame();
    ImGui_NewFrame();
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    gs_render_view(ren, &t, &w, &w, 1.0f, &view);
    ImGui_Render();
    cImGui_ImplSDLRenderer3_RenderDrawData(ImGui_GetDrawData(), ren);
    SDL_Surface *raw = SDL_RenderReadPixels(ren, nullptr);
    if (raw != nullptr) {
        without.own = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(raw);
        if (without.own != nullptr) without.px = (uint8_t *)without.own->pixels;
    }

    // Thousands, not "some": a panel with five numbers on it is a lot of pixels,
    // and a handful would mean an artefact rather than a HUD.
    CHECK(gs_hud_pixels_differing(&with, &without) > 2000);
    gs_frame_free(&without);

    // Drive until the lap counter really moves - the HUD has to be showing a
    // lap the simulation counted, not a number a test wrote in.
    uint16_t was = w.car[0].laps;
    gs_car parked = w.car[0];
    for (int i = 0; i < GS_TICK_HZ * 60 && w.car[0].laps == was; i++) {
        gs_input in[GS_MAX_CARS] = { gs_ai_drive(&w, &t, 0), 0, 0, 0 };
        gs_world_step(&w, &t, in);
    }
    CHECK(w.car[0].laps > was);

    // **Put the car back where it was drawn before.** Otherwise the two frames
    // differ because the car moved across the corner the HUD sits in, and the
    // comparison passes whatever the HUD says - which is a test that does not
    // test its rule. Everything the HUD reads is left alone.
    gs_fix ax = w.car[0].x, ay = w.car[0].y, az = w.car[0].z;
    gs_angle ah = w.car[0].heading;
    w.car[0].x = parked.x; w.car[0].y = parked.y; w.car[0].z = parked.z;
    w.car[0].heading = parked.heading;

    gs_frame later = { 0 };
    gs_hud_frame(ren, &t, &w, &view, &later);
    w.car[0].x = ax; w.car[0].y = ay; w.car[0].z = az; w.car[0].heading = ah;

    // The corner changed, and the only thing that could have changed it is the
    // numbers on the HUD.
    CHECK(gs_hud_pixels_differing(&with, &later) > 100);

    gs_frame_free(&with);
    gs_frame_free(&later);
}

TEST(the_hud_says_what_place_you_are_in_and_changes_when_you_are_passed) {
    gs_imgui_start(gs_win, ren);
    if (!gs_imgui_ready) return;

    static gs_track t;
    gs_routed_pavement(&t);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_set_laps(&w, 3);

    gs_fix sx, sy; gs_angle facing;
    gs_track_grid(&t, 0, &sx, &sy, &facing);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR, sx, sy, facing);

    // **The rival is parked off the far end of the track**, thirty tiles away
    // and well outside the view. What moves it up and down the order is its lap
    // count, which is not drawn anywhere - so the only thing that can differ
    // between the two frames below is the position on the HUD.
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_DUNE_BUGGY,
                     GS_INT(t.w - 3), GS_INT(2), facing);

    gs_view view = { 0 };
    view.cam.zoom = GS_ISO_DEFAULT_ZOOM;
    view.cam.vw = GS_W; view.cam.vh = GS_H;
    view.cam.cx = gs_to_f(w.car[0].x); view.cam.cy = gs_to_f(w.car[0].y);
    view.rect = (SDL_Rect){ 0, 0, GS_W, GS_H };

    // Built rather than raced into position: a test that waited for an overtake
    // would be a test that sometimes checked nothing.
    CHECK(gs_world_place(&w, &t, 0) == 1);

    gs_frame leading = { 0 };
    gs_hud_frame(ren, &t, &w, &view, &leading);

    // A lap up on everybody puts the rival in front, wherever it is standing.
    w.car[1].laps = 1;
    CHECK(gs_world_place(&w, &t, 0) == 2);
    CHECK(gs_world_place(&w, &t, 1) == 1);

    gs_frame passed = { 0 };
    gs_hud_frame(ren, &t, &w, &view, &passed);

    CHECK(gs_hud_pixels_differing(&leading, &passed) > 50);

    gs_frame_free(&leading);
    gs_frame_free(&passed);
}

// The damage bar's green, which nothing in the scene shares: bright green, and
// far more green than red or blue.
static int gs_count_bar(const gs_frame *f, int from_x, int to_x) {
    if (f->px == nullptr) return -1;
    int n = 0;
    for (int y = 0; y < GS_HUD_BOX_H && y < GS_H; y++) {
        for (int x = from_x; x < to_x && x < GS_W; x++) {
            const uint8_t *px = &f->px[((size_t)y * (size_t)GS_W + (size_t)x) * 4];
            if (px[1] > 150 && px[0] < 150 && px[2] < 120 &&
                px[1] > px[0] + 60 && px[1] > px[2] + 60) {
                n++;
            }
        }
    }
    return n;
}

// Where a car lands on its own view, in that view's pixels.
static void gs_car_on_screen(const gs_view *v, const gs_world *w, uint8_t car,
                             float *sx, float *sy) {
    const gs_car *c = &w->car[car];
    gs_camera cam = v->cam;
    cam.vw = (float)v->rect.w;
    cam.vh = (float)v->rect.h;
    gs_iso_project(&cam, gs_to_f(c->x), gs_to_f(c->y), gs_to_f(c->z), sx, sy);
}

TEST(every_driver_can_see_their_own_car_on_ground_that_is_not_at_height_zero) {
    (void)ren;

    // **The fault this pins was invisible on flat ground at height zero, which
    // is every track anybody had captured.** The race camera set its height to
    // zero rather than to the ground's, so a car standing on ground eight tiles
    // up was drawn eight tiles up: three hundred and eighty pixels above the
    // middle of the window, off the top of it, on a perfectly good race with a
    // camera that was - in x and y - exactly where the car was. A player racing
    // a served track saw ground and no cars at all.
    static gs_track t;
    gs_track_init(&t, 40, 40, GS_SURF_PAVEMENT);
    for (int i = 0; i < GS_TRACK_CORNERS; i++) t.corner[i] = (int16_t)(8 << GS_HEIGHT_SHIFT);

    for (uint8_t cars = 1; cars <= GS_MAX_CARS; cars++) {
        static gs_world w;
        gs_world_init(&w, GS_ONE);
        for (uint8_t i = 0; i < cars; i++) {
            // Far enough apart that the screen splits for two or more, which is
            // the arrangement with the most ways to be wrong.
            gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR,
                             GS_INT(6 + 12 * i), GS_INT(6 + 5 * i), 0);
        }

        gs_split sp = { 0 };
        for (int i = 0; i < 240; i++) {
            gs_split_update(&sp, &t, &w, &w, 1.0f, GS_W, GS_H, 1.0f / 60.0f);
        }

        gs_view v[GS_MAX_CARS];
        uint8_t n = gs_split_views(&sp, &t, &w, &w, 1.0f, GS_W, GS_H, v);
        CHECK(n >= 1);

        for (uint8_t i = 0; i < n; i++) {
            float sx = 0.0f, sy = 0.0f;
            gs_car_on_screen(&v[i], &w, v[i].car, &sx, &sy);
            CHECK(sx >= 0.0f && sx <= (float)v[i].rect.w);
            CHECK(sy >= 0.0f && sy <= (float)v[i].rect.h);
        }
    }
}

TEST(a_car_down_a_drop_does_not_take_the_camera_off_the_other_one) {
    (void)ren;

    // **One of them is wrecked twenty tiles down and the other is still
    // racing.** Two cars a few tiles apart on the map and twenty apart in
    // height are not "together", and a shared view of that pair is a view of
    // the air between them with a car above the top edge and a car below the
    // bottom one. That is what a player saw with one car in the run-off.
    static gs_track t;
    gs_track_init(&t, 40, 40, GS_SURF_PAVEMENT);

    static gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(10), GS_INT(10), 0);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(14), GS_INT(12), 0);
    // The second one has gone over the lip and stopped where it ended up, which
    // is what the simulation does with a car that leaves the world.
    w.car[1].z -= GS_INT(20);

    gs_split sp = { 0 };
    for (int i = 0; i < 240; i++) {
        gs_split_update(&sp, &t, &w, &w, 1.0f, GS_W, GS_H, 1.0f / 60.0f);
    }

    gs_view v[GS_MAX_CARS];
    uint8_t n = gs_split_views(&sp, &t, &w, &w, 1.0f, GS_W, GS_H, v);

    // Whether that is answered by splitting the screen or by pulling back far
    // enough to hold both, every driver still has to be able to see their own
    // car - which is the rule, and the arrangement is the renderer's business.
    for (uint8_t i = 0; i < n; i++) {
        float sx = 0.0f, sy = 0.0f;
        gs_car_on_screen(&v[i], &w, v[i].car, &sx, &sy);
        CHECK(sx >= 0.0f && sx <= (float)v[i].rect.w);
        CHECK(sy >= 0.0f && sy <= (float)v[i].rect.h);
    }
}

TEST(a_car_in_the_air_climbs_its_own_screen_rather_than_leaving_it) {
    (void)ren;

    // The other half of the same rule. The camera takes on the ground fully so
    // that a raised track is still centred, and only part of the air, so that
    // the gap between a car and its shadow is what says it is flying - which is
    // the single most readable thing in the frame and was true on a C64.
    static gs_track t;
    gs_track_init(&t, 40, 40, GS_SURF_PAVEMENT);
    for (int i = 0; i < GS_TRACK_CORNERS; i++) t.corner[i] = (int16_t)(4 << GS_HEIGHT_SHIFT);

    static gs_world grounded, flying;
    gs_world_init(&grounded, GS_ONE);
    gs_world_add_car(&grounded, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(10), GS_INT(10), 0);
    flying = grounded;
    flying.car[0].z += GS_INT(3);

    float on_ground = 0.0f, in_air = 0.0f, sx = 0.0f;
    for (int which = 0; which < 2; which++) {
        const gs_world *w = which == 0 ? &grounded : &flying;
        gs_split sp = { 0 };
        for (int i = 0; i < 240; i++) {
            gs_split_update(&sp, &t, w, w, 1.0f, GS_W, GS_H, 1.0f / 60.0f);
        }
        gs_view v[GS_MAX_CARS];
        CHECK(gs_split_views(&sp, &t, w, w, 1.0f, GS_W, GS_H, v) == 1);
        gs_car_on_screen(&v[0], w, 0, &sx, which == 0 ? &on_ground : &in_air);

        CHECK(sx >= 0.0f && sx <= (float)v[0].rect.w);
    }

    // Up the screen is a smaller y, and it has to still be on the screen.
    CHECK(in_air < on_ground);
    CHECK(in_air > 0.0f);
    CHECK(on_ground > 0.0f && on_ground < (float)GS_H);

    // **And a car that is airborne for good is still on the screen.** A wreck
    // over the drop stops where it is rather than falling to the bottom, so it
    // can hang twelve tiles up for the rest of the race - and a camera that
    // takes on a third of the air leaves it above the top edge and keeps it
    // there. Partial follow is for reading a jump, not for losing the car.
    static gs_world stuck;
    stuck = grounded;
    stuck.car[0].z += GS_INT(12);

    gs_split sp = { 0 };
    for (int i = 0; i < 240; i++) {
        gs_split_update(&sp, &t, &stuck, &stuck, 1.0f, GS_W, GS_H, 1.0f / 60.0f);
    }
    gs_view v[GS_MAX_CARS];
    CHECK(gs_split_views(&sp, &t, &stuck, &stuck, 1.0f, GS_W, GS_H, v) == 1);

    float hung = 0.0f;
    gs_car_on_screen(&v[0], &stuck, 0, &sx, &hung);
    CHECK(sx >= 0.0f && sx <= (float)v[0].rect.w);
    CHECK(hung >= 0.0f && hung <= (float)v[0].rect.h);

    // Still higher up the screen than the one on the ground, so the reason for
    // the partial follow survives the cap on it.
    CHECK(hung < on_ground);
}

TEST(the_camera_holds_the_car_still_between_ticks) {
    (void)ren;

    // **The judder a recording showed.** The world advances 120 times a second
    // and frames do not, so the renderer draws cars interpolated between the
    // last two states - and the camera was reading the settled state instead.
    // That is a camera pointed a fraction of a tick away from what is drawn,
    // with the fraction changing every frame: the car wobbles against a world
    // that is otherwise smooth. A camera that follows a car holds it still on
    // the screen, whatever fraction of a tick the frame lands on.
    static gs_track t;
    gs_flat_pavement(&t, 40, 40);

    static gs_world prev, now;
    gs_world_init(&prev, GS_ONE);
    gs_world_add_car(&prev, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(10), GS_INT(10), 0);

    // One tick of travel, which is what a frame lands somewhere inside.
    now = prev;
    now.car[0].x += GS_INT(1) / 8;
    now.car[0].y += GS_INT(1) / 16;

    gs_split sp = { 0 };
    for (int i = 0; i < 240; i++) {
        gs_split_update(&sp, &t, &prev, &now, 1.0f, GS_W, GS_H, 1.0f / 60.0f);
    }

    float first_x = 0.0f, first_y = 0.0f;
    for (int step = 0; step <= 8; step++) {
        float alpha = (float)step / 8.0f;

        // dt of zero: the merge is settled, so what is measured is the
        // interpolation and not a transition still in progress.
        gs_split_update(&sp, &t, &prev, &now, alpha, GS_W, GS_H, 0.0f);

        gs_view v[GS_MAX_CARS];
        CHECK(gs_split_views(&sp, &t, &prev, &now, alpha, GS_W, GS_H, v) == 1);

        // Where the renderer draws it: between the two states, by alpha.
        float px = gs_to_f(prev.car[0].x), py = gs_to_f(prev.car[0].y);
        float pz = gs_to_f(prev.car[0].z);
        float cx = px + (gs_to_f(now.car[0].x) - px) * alpha;
        float cy = py + (gs_to_f(now.car[0].y) - py) * alpha;
        float cz = pz + (gs_to_f(now.car[0].z) - pz) * alpha;

        gs_camera cam = v[0].cam;
        float sx = 0.0f, sy = 0.0f;
        gs_iso_project(&cam, cx, cy, cz, &sx, &sy);

        if (step == 0) { first_x = sx; first_y = sy; continue; }

        // Half a pixel is rounding; anything more is a wobble.
        CHECK(sx > first_x - 0.5f && sx < first_x + 0.5f);
        CHECK(sy > first_y - 0.5f && sy < first_y + 0.5f);
    }
}

TEST(a_start_line_and_a_finish_line_are_different_things) {
    (void)ren;

    // **"It is confusing whether beginning and end is."** The grid sits behind
    // the line a lap is measured on, and that line was the only one drawn -
    // chequered, a few tiles in front of cars that had not moved yet. So the
    // first thing a player saw on arriving was a chequered flag line, which is
    // the universal sign for *finished*.
    //
    // There are two lines now and they must not look alike. Both are sampled
    // where they actually lie rather than counted over the whole frame: a band
    // behind the gate, which is the grid's plain white line and must have no
    // black in it, and the gate itself, which is the chequer and must have
    // both.
    static gs_track t;
    gs_flat_pavement(&t, 48, 48);
    gs_track_add_gate(&t, GS_INT(24), GS_INT(24), 0, GS_INT(6));

    // Parked well clear, so nothing is standing on either line.
    static gs_world w;
    gs_park_car(&w, &t, GS_INT(24), GS_INT(40));

    gs_camera cam = gs_camera_on(23.0f, 24.0f, 0.0f);
    cam.zoom = 1.4f;

    gs_frame f = gs_render_frame(ren, &t, &w, &w, 1.0f, &cam);
    CHECK(f.px != nullptr);
    if (f.px == nullptr) return;

    // Walk each line across the road and ask what colour the ground is there.
    // The gate faces +x, so the line across it runs along y.
    int grid_white = 0, grid_black = 0, gate_white = 0, gate_black = 0;

    for (int i = -30; i <= 30; i++) {
        float across = 24.0f + (float)i * 0.09f;

        // Behind the gate: anywhere in the band the grid's line lives in.
        for (int b = 0; b < 24; b++) {
            float back = 1.5f + (float)b * 0.04f;
            float sx = 0.0f, sy = 0.0f;
            gs_iso_project(&cam, 24.0f - back, across, 0.0f, &sx, &sy);
            if (sx < 0 || sy < 0 || sx >= GS_W || sy >= GS_H) continue;
            const uint8_t *p = &f.px[((int)sy * GS_W + (int)sx) * 4];
            if (p[0] > 225 && p[1] > 225 && p[2] > 225) grid_white++;
            if (p[0] < 30 && p[1] < 30 && p[2] < 30) grid_black++;
        }

        // And on the gate itself.
        for (int b = 0; b < 24; b++) {
            float along = -0.5f + (float)b * 0.04f;
            float sx = 0.0f, sy = 0.0f;
            gs_iso_project(&cam, 24.0f + along, across, 0.0f, &sx, &sy);
            if (sx < 0 || sy < 0 || sx >= GS_W || sy >= GS_H) continue;
            const uint8_t *p = &f.px[((int)sy * GS_W + (int)sx) * 4];
            if (p[0] > 225 && p[1] > 225 && p[2] > 225) gate_white++;
            if (p[0] < 30 && p[1] < 30 && p[2] < 30) gate_black++;
        }
    }

    // The grid has a line and it is plain: white, with no chequer in it.
    CHECK(grid_white > 40);
    CHECK(grid_black == 0);

    // The finish is chequered, which means both colours together.
    CHECK(gate_white > 40);
    CHECK(gate_black > 40);

    gs_frame_free(&f);
}

TEST(a_car_on_the_near_side_of_a_line_is_drawn_in_front_of_it) {
    (void)ren;

    // **"The car just went behind the finish line."** The band reaches right
    // across the track, so it lies on a wide span of diagonals - but the gate
    // has only one, and the whole band was drawn at it. Every part of the line
    // nearer the camera than the gate's centre was therefore painted after any
    // car nearer than the centre, and swallowed it.
    //
    // The car here sits under the band but five tiles short of the gate's
    // centre, so the block above it belongs to a diagonal the sweep reaches
    // well before the gate's own. Photographed against the same car on the same
    // ground with no route on it at all.
    static gs_track bare, lined;
    gs_flat_pavement(&bare, 48, 48);
    gs_flat_pavement(&lined, 48, 48);
    gs_track_add_gate(&lined, GS_INT(24), GS_INT(24), 0, GS_INT(8));

    static gs_world w;
    gs_park_car(&w, &bare, GS_INT(24), GS_INT(19));

    gs_camera cam = gs_camera_on(24.0f, 19.0f, 0.0f);

    gs_frame clear_ground = gs_render_frame(ren, &bare, &w, &w, 1.0f, &cam);
    gs_frame on_the_line = gs_render_frame(ren, &lined, &w, &w, 1.0f, &cam);
    CHECK(clear_ground.px != nullptr);
    CHECK(on_the_line.px != nullptr);
    if (clear_ground.px == nullptr || on_the_line.px == nullptr) {
        gs_frame_free(&clear_ground);
        gs_frame_free(&on_the_line);
        return;
    }

    int alone = gs_count_car0(&clear_ground);
    int over_the_line = gs_count_car0(&on_the_line);
    CHECK(alone > 500);

    // Painting a line under a car must not cost the car any of itself.
    CHECK(over_the_line > alone * 95 / 100);

    gs_frame_free(&clear_ground);
    gs_frame_free(&on_the_line);
}

TEST(a_car_is_drawn_whole_wherever_it_sits_within_its_tile) {
    (void)ren;

    // **"The background comes over the bonnet every few seconds."** The terrain
    // is swept one diagonal at a time and a car was drawn when the sweep
    // reached the car's *centre* tile - but a car is about 1.3 tiles long, so
    // its nose reaches into the tile in front, which is on the next diagonal
    // and therefore drawn afterwards. The ground the car is standing on then
    // lands on top of its bonnet. It came and went as the car drove because it
    // depends on where in its tile the centre happens to sit, which is exactly
    // how a player described it.
    //
    // Built to straddle a diagonal on purpose rather than hoping that a drive
    // produced one: with the car facing +x, a centre a tenth into its tile
    // keeps the whole footprint on one diagonal, and nine tenths pushes the
    // nose two diagonals past it. Both are photographed from the same distance,
    // so the same car covers the same pixels - unless some of it is painted
    // over.
    //
    // In the middle of a track big enough to have no edge in shot, because a
    // kerb is strongly red and would otherwise be counted as car.
    static gs_track t;
    gs_flat_pavement(&t, 48, 48);

    const gs_fix tenth = GS_ONE / 10;

    static gs_world aligned, straddling;
    gs_park_car(&aligned, &t, GS_INT(24) + tenth, GS_INT(24) + tenth);
    gs_park_car(&straddling, &t, GS_INT(24) + tenth * 9, GS_INT(24) + tenth * 9);

    gs_camera cam_a = gs_camera_on(24.1f, 24.1f, 0.0f);
    gs_camera cam_b = gs_camera_on(24.9f, 24.9f, 0.0f);

    gs_frame fa = gs_render_frame(ren, &t, &aligned, &aligned, 1.0f, &cam_a);
    gs_frame fb = gs_render_frame(ren, &t, &straddling, &straddling, 1.0f, &cam_b);
    CHECK(fa.px != nullptr);
    CHECK(fb.px != nullptr);
    if (fa.px == nullptr || fb.px == nullptr) {
        gs_frame_free(&fa);
        gs_frame_free(&fb);
        return;
    }

    int whole = gs_count_car0(&fa);
    int eaten = gs_count_car0(&fb);
    CHECK(whole > 500);

    // A percent or two is the car landing on different pixels; a bonnet is a
    // fifth of the car and nothing like that may go missing.
    CHECK(eaten > whole * 95 / 100);
    CHECK(eaten < whole * 105 / 100);

    gs_frame_free(&fa);
    gs_frame_free(&fb);
}

TEST(a_track_says_where_it_ends_and_which_way_it_goes) {
    (void)ren;

    // **Neither of these was on the screen at all.** A player raced a shelf
    // that ends, drove straight off it, and asked why - and the answer was that
    // nothing marked the edge, and nothing marked the route either: gates lived
    // in the simulation and in the editor's white line, and a race drew none of
    // them. Both are counted here by colour, because both are things whose
    // whole job is to be seen.
    static gs_track t;
    gs_flat_pavement(&t, 20, 20);
    gs_track_add_gate(&t, GS_INT(10), GS_INT(4), 0, GS_INT(4));
    gs_track_add_gate(&t, GS_INT(10), GS_INT(16), GS_DEG(180), GS_INT(4));

    static gs_world w;
    gs_park_car(&w, &t, GS_INT(10), GS_INT(10));

    // From above the middle, far enough out to hold the whole track.
    gs_camera cam = gs_camera_on(10.0f, 10.0f, 0.0f);
    cam.zoom = 0.6f;

    gs_frame f = gs_render_frame(ren, &t, &w, &w, 1.0f, &cam);
    CHECK(f.px != nullptr);
    if (f.px == nullptr) return;

    int kerb = 0, arrow = 0, chequer = 0;
    for (int i = 0; i < GS_W * GS_H; i++) {
        const uint8_t *p = &f.px[i * 4];
        // The kerb's red: strong, and much darker in green and blue than the
        // ground it sits on.
        if (p[0] > 120 && p[0] > 2 * p[1] && p[0] > 2 * p[2]) kerb++;
        // The arrow's yellow: red and green together, almost no blue.
        if (p[0] > 180 && p[1] > 150 && p[2] < 110) arrow++;
        // And the start line's black and white, which nothing else here is.
        if (p[0] > 220 && p[1] > 220 && p[2] > 220) chequer++;
    }

    // Generous thresholds: what is being pinned is that these are drawn at all,
    // not how many pixels of them a particular zoom produces.
    CHECK(kerb > 200);
    CHECK(arrow > 100);
    CHECK(chequer > 100);

    gs_frame_free(&f);
}

TEST(the_hud_fits_what_is_in_it_in_every_state_it_has) {
    gs_imgui_start(gs_win, ren);
    CHECK(gs_imgui_ready);
    if (!gs_imgui_ready) return;

    // **A panel sized by hand goes stale the moment somebody adds a line.** It
    // did within the hour: the first version of the wreck message had "Esc back
    // to the menu" drawn half outside the box. Every state the HUD has is drawn
    // here and asked whether any of it ended up below the bottom.
    static gs_track t;
    gs_flat_pavement(&t, 24, 12);

    static gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(6), GS_INT(6), 0);

    gs_view v = { 0 };
    v.car = 0;
    v.rect = (SDL_Rect){ 0, 0, GS_W, GS_H };
    v.cam.zoom = GS_ISO_DEFAULT_ZOOM;
    gs_render_track_camera(&v, &t, &w, &w, 1.0f);

    // Racing, wrecked, waiting, finished, and the combinations of those that a
    // race can actually produce - offline, where a wreck is offered a restart,
    // and online, where it is not.
    struct { bool wrecked; bool finished; float waited; bool online; } states[] = {
        { false, false, 0.0f,  false },
        { true,  false, 0.0f,  false },
        { true,  false, 0.0f,  true  },
        { false, false, 3.0f,  false },
        { true,  false, 3.0f,  true  },
        { false, true,  0.0f,  false },
        { true,  true,  9.0f,  false },
    };

    for (size_t i = 0; i < SDL_arraysize(states); i++) {
        w.car[0].wrecked = states[i].wrecked;
        w.car[0].damage = states[i].wrecked ? 255 : 0;
        w.car[0].finish_tick = states[i].finished ? 4200 : 0;

        gs_frame f = { 0 };
        for (int frame = 0; frame < 3; frame++) {
            gs_frame_free(&f);
            cImGui_ImplSDLRenderer3_NewFrame();
            cImGui_ImplSDL3_NewFrame();
            ImGui_NewFrame();
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            gs_render_view(ren, &t, &w, &w, 1.0f, &v);
            gs_hud_draw(&w, &t, &v, 600, states[i].waited, states[i].online);
            ImGui_Render();
            cImGui_ImplSDLRenderer3_RenderDrawData(ImGui_GetDrawData(), ren);
        }
        gs_frame_free(&f);

        CHECK(gs_hud_overflow() == 0.0f);
    }

    w.car[0].wrecked = false;
    w.car[0].damage = 0;
    w.car[0].finish_tick = 0;
}

TEST(there_is_always_a_way_back_out_of_wherever_you_are) {
    (void)ren;

    // **A wrecked car in a race that cannot end used to be a dead screen.**
    // Nothing finishes, so nothing takes the player anywhere, and the one key
    // that means "out" went to a setup screen that decides a race the server
    // owns. Escape is a rule now rather than four lines in a key handler that
    // no test could reach.
    static gs_menu m;
    gs_menu_init(&m);
    CHECK(gs_profile_add(&m.profiles, "gavin", GS_COLOUR_RED,
                         (uint8_t)GS_VEH_BAJA_BUG) == 0);
    CHECK(gs_menu_set_password(&m, 0, "a good one", "a good one"));
    CHECK(gs_menu_sign_in(&m, 0, "a good one", ""));

    // Racing on this machine: out is the screen that set the race up.
    m.online = false;
    m.screen = GS_SCREEN_RACE;
    CHECK(gs_menu_back(&m, false) == GS_SCREEN_SETUP);

    // Racing other people: out is the lobby, where there is something to press
    // to race again - and never the setup screen, which would be this machine
    // deciding a race that is not its to decide.
    m.online = true;
    CHECK(gs_menu_back(&m, false) == GS_SCREEN_LOBBY);

    // Every other screen backs out to the title...
    static const gs_screen inner[] = {
        GS_SCREEN_PROFILES, GS_SCREEN_SETUP, GS_SCREEN_RESULTS,
        GS_SCREEN_RECORDS, GS_SCREEN_LOBBY, GS_SCREEN_TRACKS,
    };
    for (size_t i = 0; i < SDL_arraysize(inner); i++) {
        m.screen = inner[i];
        CHECK(gs_menu_back(&m, false) == GS_SCREEN_TITLE);
    }

    // ...and the title and the door are where leaving belongs.
    m.screen = GS_SCREEN_TITLE;
    CHECK(gs_menu_back(&m, false) == GS_SCREEN_COUNT);
    m.screen = GS_SCREEN_LOGIN;
    CHECK(gs_menu_back(&m, false) == GS_SCREEN_COUNT);

    // The construction set is a layer over whatever is underneath it, so
    // closing it moves nobody anywhere.
    m.screen = GS_SCREEN_RACE;
    CHECK(gs_menu_back(&m, true) == GS_SCREEN_RACE);
}

TEST(the_condition_bar_stays_inside_the_hud) {
    gs_imgui_start(gs_win, ren);
    CHECK(gs_imgui_ready);
    if (!gs_imgui_ready) return;

    // **Found by looking at a race.** The bar was drawn at the window's width
    // less a padding of eight, and the style the HUD is drawn in pads a window
    // by twenty-two - so it was twenty-eight pixels wider than the panel and
    // ran off the right-hand edge of it.
    static gs_track t;
    gs_flat_pavement(&t, 24, 12);

    static gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(6), GS_INT(6), 0);

    gs_view v = { 0 };
    v.car = 0;
    v.rect = (SDL_Rect){ 0, 0, GS_W, GS_H };
    v.cam.zoom = GS_ISO_DEFAULT_ZOOM;
    gs_render_track_camera(&v, &t, &w, &w, 1.0f);

    gs_frame f = { 0 };
    for (int i = 0; i < 3; i++) {
        gs_frame_free(&f);
        gs_hud_frame(ren, &t, &w, &v, &f);
    }

    // The panel starts GS_HUD_PAD from the edge and is GS_HUD_W wide. Inside
    // it there is a bar - the control, without which "nothing at the edge"
    // would be true of a HUD that drew no bar at all.
    CHECK(gs_count_bar(&f, 0, 10 + 132) > 40);

    // **And it stops short of the frame.** Nothing is ever drawn outside an
    // ImGui window - it clips - so what a too-wide bar actually does is run
    // into the panel's own edge and sit there with no margin, which is what it
    // looked like. The last few pixels before the frame must be background.
    CHECK(gs_count_bar(&f, 10 + 132 - 10, 10 + 132) == 0);

    gs_frame_free(&f);
}

// One frame containing a single empty text box. `focus` asks for the keyboard
// on this frame; the caret only appears the frame *after* that, which is the
// whole reason this is a loop rather than one call. The box's rectangle comes
// back in `rect` so the search below looks exactly where the box was.
static bool gs_caret_active = false;

static void gs_caret_frame(SDL_Renderer *ren, char *buf, size_t cap, bool focus,
                           float *rect, gs_frame *out) {
    cImGui_ImplSDLRenderer3_NewFrame();
    cImGui_ImplSDL3_NewFrame();
    ImGui_NewFrame();

    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);

    ImGui_SetNextWindowPos((ImVec2){ 20.0f, 20.0f }, ImGuiCond_Always);
    ImGui_SetNextWindowSize((ImVec2){ 320.0f, 120.0f }, ImGuiCond_Always);
    if (ImGui_Begin("##caret", nullptr,
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoTitleBar)) {
        if (focus) ImGui_SetKeyboardFocusHere();
        ImGui_InputText("##box", buf, cap, 0);
        gs_caret_active = ImGui_IsItemActive();
        ImVec2 lo = ImGui_GetItemRectMin();
        ImVec2 hi = ImGui_GetItemRectMax();
        rect[0] = lo.x; rect[1] = lo.y; rect[2] = hi.x; rect[3] = hi.y;
    }
    ImGui_End();

    ImGui_Render();
    cImGui_ImplSDLRenderer3_RenderDrawData(ImGui_GetDrawData(), ren);

    SDL_Surface *raw = SDL_RenderReadPixels(ren, nullptr);
    *out = (gs_frame){ 0 };
    if (raw != nullptr) {
        out->own = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(raw);
        if (out->own != nullptr) out->px = (uint8_t *)out->own->pixels;
    }
}

// How many pixels inside the box are the colour text is drawn in. The caret is
// drawn with ImGuiCol_Text, and the box is empty, so in an empty box that count
// *is* the caret. Counting "pixels that changed" instead would count the blue
// the frame background turns when a box becomes active, which happens whether
// or not a caret was ever drawn.
static int gs_text_coloured_pixels(const gs_frame *f, const float *rect) {
    if (f->own == nullptr) return -1;
    int w = f->own->w, h = f->own->h;
    int x0 = (int)rect[0], y0 = (int)rect[1];
    int x1 = (int)rect[2], y1 = (int)rect[3];
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;

    int count = 0;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            const uint8_t *p = f->px + ((size_t)y * (size_t)w + (size_t)x) * 4;
            if (p[0] > 200 && p[1] > 200 && p[2] > 200) count++;
        }
    }
    return count;
}

TEST(a_text_box_shows_a_caret_when_it_has_the_keyboard) {
    gs_imgui_start(gs_win, ren);
    CHECK(gs_imgui_ready);

    static char buf[32];
    float rect[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    gs_frame f = { 0 };

    // **Nothing has the keyboard**: an empty box, so nothing in it is the
    // colour text is drawn in.
    buf[0] = '\0';
    for (int i = 0; i < 4; i++) {
        gs_frame_free(&f);
        gs_caret_frame(ren, buf, sizeof buf, false, rect, &f);
    }
    int idle = gs_text_coloured_pixels(&f, rect);
    CHECK(idle == 0);

    // Control: with text in it, the scan must find something. If this finds
    // nothing the measurement is wrong and everything else here is worthless.
    SDL_strlcpy(buf, "WWWW", sizeof buf);
    for (int i = 0; i < 4; i++) {
        gs_frame_free(&f);
        gs_caret_frame(ren, buf, sizeof buf, false, rect, &f);
    }
    CHECK(gs_text_coloured_pixels(&f, rect) > 0);

    // **Now give it the keyboard.** The ask lands on the first frame and the
    // caret appears on the next, so several frames go by before looking.
    buf[0] = '\0';
    for (int i = 0; i < 6; i++) {
        gs_frame_free(&f);
        gs_caret_frame(ren, buf, sizeof buf, i == 0, rect, &f);
    }
    int typing = gs_text_coloured_pixels(&f, rect);
    // The box really did have the keyboard, so a missing caret would be a
    // missing caret rather than a box that was never focused.
    CHECK(gs_caret_active);

    // A caret is a short vertical line, so this is a handful of pixels rather
    // than one - but the fact being pinned is that it is there at all, which is
    // what somebody staring at a box they cannot see they are typing into is
    // missing.
    CHECK(typing > idle);
    CHECK(typing > 0);

    gs_frame_free(&f);
}

TEST(the_front_end_is_shut_until_somebody_signs_in) {
    (void)ren;
    static gs_menu m;
    gs_menu_init(&m);

    // **The door is where the game starts**, and a zeroed menu already says so
    // rather than relying on somebody remembering to set it.
    CHECK(m.screen == GS_SCREEN_LOGIN);
    CHECK(m.signed_in == -1);
    CHECK(strcmp(gs_menu_driver(&m), "") == 0);

    CHECK(gs_profile_add(&m.profiles, "gavin", GS_COLOUR_RED,
                         (uint8_t)GS_VEH_BAJA_BUG) == 0);

    // **A driver carrying no password cannot sign in at all.** Not because the
    // empty string fails the check - because there is nothing to check against,
    // and a door that opens for anybody with no key is a picture of a door.
    CHECK(!gs_menu_sign_in(&m, 0, "", ""));
    CHECK(!gs_menu_sign_in(&m, 0, "anything", ""));
    CHECK(m.signed_in == -1);

    // The way through is to give them one, which is what an older roster's
    // drivers are offered rather than being turned away.
    CHECK(gs_menu_set_password(&m, 0, "a good one", "a good one"));
    CHECK(gs_menu_sign_in(&m, 0, "a good one", ""));
    CHECK(m.signed_in == 0);
    CHECK(strcmp(gs_menu_driver(&m), "gavin") == 0);

    // Signing in puts that driver in the first seat, so the race they start is
    // raced by the person who just proved they were there.
    CHECK(m.setup.profile[0] == 0);

    // Asking for somebody who is not on the roster is refused rather than
    // clamped to whoever happens to be at that index.
    CHECK(!gs_menu_sign_in(&m, 4, "x", ""));
    CHECK(!gs_menu_sign_in(&m, -1, "x", ""));
}

TEST(a_wrong_name_and_a_wrong_password_are_refused_identically) {
    (void)ren;
    static gs_menu m;
    gs_menu_init(&m);
    CHECK(gs_profile_add(&m.profiles, "gavin", GS_COLOUR_RED,
                         (uint8_t)GS_VEH_BAJA_BUG) == 0);
    CHECK(gs_menu_set_password(&m, 0, "correct horse", "correct horse"));

    // **The screen lists nobody, so the refusals must not either.** If a name
    // that exists failed differently from one that does not, the login box
    // would answer "who is on this machine" one guess at a time - which is the
    // question the list used to answer for free.
    char no_such[sizeof m.login_error];
    char wrong_pw[sizeof m.login_error];

    CHECK(!gs_menu_sign_in_named(&m, "nobody", "correct horse", ""));
    SDL_strlcpy(no_such, m.login_error, sizeof no_such);

    CHECK(!gs_menu_sign_in_named(&m, "gavin", "wrong", ""));
    SDL_strlcpy(wrong_pw, m.login_error, sizeof wrong_pw);

    CHECK(no_such[0] != '\0');
    CHECK(strcmp(no_such, wrong_pw) == 0);
    CHECK(m.signed_in == -1);

    // An empty name is refused the same way rather than matching anybody.
    CHECK(!gs_menu_sign_in_named(&m, "", "", ""));
    CHECK(strcmp(m.login_error, no_such) == 0);

    // And the right pair gets in.
    CHECK(gs_menu_sign_in_named(&m, "gavin", "correct horse", ""));
    CHECK(m.signed_in == 0);
    CHECK(strcmp(gs_menu_driver(&m), "gavin") == 0);
}

TEST(a_password_cannot_be_set_to_nothing) {
    (void)ren;
    static gs_menu m;
    gs_menu_init(&m);
    CHECK(gs_profile_add(&m.profiles, "gavin", GS_COLOUR_RED,
                         (uint8_t)GS_VEH_BAJA_BUG) == 0);

    // Empty, mismatched, and off the end of the roster are all refused, and
    // none of them leaves the driver half-changed.
    CHECK(!gs_menu_set_password(&m, 0, "", ""));
    CHECK(m.profiles.entry[0].password[0] == '\0');
    CHECK(!gs_menu_set_password(&m, 0, "one", "another"));
    CHECK(m.profiles.entry[0].password[0] == '\0');
    CHECK(!gs_menu_set_password(&m, 9, "fine", "fine"));

    CHECK(gs_menu_set_password(&m, 0, "fine", "fine"));
    CHECK(m.profiles.entry[0].password[0] == '$');

    // Changing it takes the new one and refuses the old.
    CHECK(gs_menu_set_password(&m, 0, "better", "better"));
    CHECK(gs_menu_sign_in(&m, 0, "better", ""));
    gs_menu_init(&m);
    CHECK(gs_profile_add(&m.profiles, "gavin", GS_COLOUR_RED, 0) == 0);
    CHECK(gs_menu_set_password(&m, 0, "better", "better"));
    CHECK(!gs_menu_sign_in(&m, 0, "fine", ""));
}

TEST(a_password_on_a_profile_is_actually_required) {
    (void)ren;
    static gs_menu m;
    gs_menu_init(&m);
    CHECK(gs_profile_add(&m.profiles, "gavin", GS_COLOUR_RED,
                         (uint8_t)GS_VEH_BAJA_BUG) == 0);

    // A real Argon2id hash, made the way the game makes one - not a stand-in.
    // The point of this test is that the check is wired to the real thing.
    gs_profile *p = &m.profiles.entry[0];
    CHECK(gs_auth_hash_password("correct horse", p->password, sizeof p->password));
    CHECK(p->password[0] == '$');

    // Wrong, empty, and nearly-right are all refused.
    CHECK(!gs_menu_sign_in(&m, 0, "", ""));
    CHECK(m.signed_in == -1);
    CHECK(!gs_menu_sign_in(&m, 0, "wrong", ""));
    CHECK(m.signed_in == -1);
    CHECK(!gs_menu_sign_in(&m, 0, "correct hors", ""));
    CHECK(m.signed_in == -1);
    CHECK(m.login_error[0] != '\0');

    // And the right one gets in.
    CHECK(gs_menu_sign_in(&m, 0, "correct horse", ""));
    CHECK(m.signed_in == 0);
    CHECK(m.login_error[0] == '\0');

    // **What was typed does not stay in the struct.** gs_menu is saved, copied
    // and passed around; a password left in it is a password in more places
    // than anybody intended. It is kept only until the frontend has taken it
    // for a server, and taking it is what wipes it.
    char taken[64] = { 0 };
    uint32_t code = 0;
    CHECK(gs_menu_take_server_login(&m, taken, sizeof taken, &code));
    CHECK(strcmp(taken, "correct horse") == 0);
    CHECK(m.server_password[0] == '\0');
    CHECK(!gs_menu_take_server_login(&m, taken, sizeof taken, &code));
}

TEST(a_second_factor_is_asked_for_when_the_profile_has_one) {
    (void)ren;
    static gs_menu m;
    gs_menu_init(&m);
    CHECK(gs_profile_add(&m.profiles, "gavin", GS_COLOUR_RED,
                         (uint8_t)GS_VEH_BAJA_BUG) == 0);
    gs_profile *p = &m.profiles.entry[0];

    // A known secret rather than a generated one, so the code below is the code
    // this test means and not whatever a generator produced.
    for (uint8_t i = 0; i < GS_PROFILE_TOTP; i++) p->totp[i] = (uint8_t)(i + 1);
    p->totp_len = GS_PROFILE_TOTP;

    // Every driver has a password, so the second factor sits on top of one -
    // and the right password with a wrong code must still be refused, or the
    // factor is decoration.
    CHECK(gs_menu_set_password(&m, 0, "known", "known"));

    SDL_Time now_ns = 0;
    CHECK(SDL_GetCurrentTime(&now_ns));
    int64_t now = (int64_t)(now_ns / 1000000000);

    char right[16];
    SDL_snprintf(right, sizeof right, "%06u",
                 gs_auth_code_at(p->totp, p->totp_len, gs_auth_step_of(now)));

    CHECK(!gs_menu_sign_in(&m, 0, "known", ""));        // nothing typed
    CHECK(!gs_menu_sign_in(&m, 0, "known", "000000"));  // wrong six digits
    CHECK(!gs_menu_sign_in(&m, 0, "known", "12345"));   // five is not six
    CHECK(!gs_menu_sign_in(&m, 0, "known", "12 456"));  // not all digits
    CHECK(m.signed_in == -1);

    // The right code does not rescue a wrong password either.
    CHECK(!gs_menu_sign_in(&m, 0, "guessed", right));
    CHECK(m.signed_in == -1);

    CHECK(gs_menu_sign_in(&m, 0, "known", right));
    CHECK(m.signed_in == 0);
}

// Every front-end screen, drawn once, with what its panel came out as.
//
// Three frames rather than one: ImGui settles a window's size and its scroll on
// the frame after it first sees it, and a measurement taken on the first frame
// is a measurement of a window that has not finished existing.
static gs_panel_report gs_panel_of(SDL_Renderer *ren, gs_menu *m,
                                   const gs_track *t, gs_screen screen) {
    for (int i = 0; i < 3; i++) {
        cImGui_ImplSDLRenderer3_NewFrame();
        cImGui_ImplSDL3_NewFrame();
        ImGui_NewFrame();

        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);

        // Held on the screen being measured: gs_menu_frame hands back where to
        // go next, and a screen that decided to move on would be measured
        // somewhere else.
        m->screen = screen;
        (void)gs_menu_frame(m, t);

        ImGui_Render();
        cImGui_ImplSDLRenderer3_RenderDrawData(ImGui_GetDrawData(), ren);
    }
    return m->panel;
}

// A menu with enough in it that every screen has something to draw: a driver
// signed in, a full library, a finished race and a lobby with people in it.
// A screen measured with nothing in it is a screen measured at its smallest,
// which is not the size anything goes wrong at.
static gs_lobby gs_panel_lobby;

static void gs_panel_menu(gs_menu *m, gs_track *t) {
    gs_menu_init(m);
    CHECK(gs_profile_add(&m->profiles, "gavin", GS_COLOUR_RED,
                         (uint8_t)GS_VEH_BAJA_BUG) == 0);
    CHECK(gs_menu_set_password(m, 0, "a good one", "a good one"));
    CHECK(gs_menu_sign_in(m, 0, "a good one", ""));

    // A full library, because the library is the thing that grows. Each track
    // differs by one corner so that each one hashes differently and the library
    // keeps all of them rather than folding them into one entry.
    for (int i = 0; i < GS_LIBRARY_MAX; i++) {
        gs_track_init(t, 32, 32, GS_SURF_PAVEMENT);
        t->corner[i] = (int16_t)(i + 1);
        char name[GS_LIBRARY_NAME];
        SDL_snprintf(name, sizeof name, "track number %d", i + 1);
        CHECK(gs_library_put(&m->library, t, name, "somebody") >= 0);
    }
    CHECK(m->library.count == GS_LIBRARY_MAX);
    gs_track_init(t, 32, 32, GS_SURF_PAVEMENT);

    // A finished race for the results screen, and a lobby for the lobby.
    m->result_count = GS_MAX_CARS;
    for (uint8_t i = 0; i < GS_MAX_CARS; i++) {
        m->result[i].car = i;
        m->result[i].place = (uint8_t)(i + 1);
        m->result[i].finish_tick = 60u * 120u;
        m->result[i].best_lap = 20u * 120u;
        m->result[i].laps = 3;
        // The widest a results row gets: the note that says both records went
        // to the same drive. Measuring the screen without it would be measuring
        // it at its narrowest, which is not the size it goes wrong at.
        m->result[i].beat_lap = true;
        m->result[i].beat_race = true;
    }
    gs_panel_lobby = (gs_lobby){ 0 };
    gs_panel_lobby.count = GS_PROTO_MAX_PLAYERS;
    gs_panel_lobby.capacity = GS_PROTO_MAX_PLAYERS;
    for (int i = 0; i < GS_PROTO_MAX_PLAYERS; i++) {
        SDL_snprintf(gs_panel_lobby.player[i].name,
                     sizeof gs_panel_lobby.player[i].name, "player %d", i);
        gs_panel_lobby.player[i].slot = (uint8_t)i;
        gs_panel_lobby.player[i].present = true;
    }
    m->lobby = &gs_panel_lobby;
    m->track_progress = 0.5f;
}

static const gs_screen gs_every_screen[] = {
    GS_SCREEN_LOGIN, GS_SCREEN_TITLE, GS_SCREEN_PROFILES, GS_SCREEN_SETUP,
    GS_SCREEN_RESULTS, GS_SCREEN_RECORDS, GS_SCREEN_LOBBY, GS_SCREEN_TRACKS,
};

TEST(no_screen_is_drawn_bigger_than_the_window_it_is_in) {
    gs_imgui_start(gs_win, ren);
    CHECK(gs_imgui_ready);
    if (!gs_imgui_ready) return;

    static gs_menu m;
    static gs_track t;
    gs_panel_menu(&m, &t);

    // **The window the game opens at.** These panels cannot be moved, resized
    // or collapsed, so a panel taller than the window is a panel whose top rows
    // are off the top edge with no way to reach them - which is what a library
    // of thirty-two tracks did to the one screen you choose a track on.
    CHECK(SDL_SetWindowSize(gs_win, 1280, 720));
    CHECK(SDL_SetRenderLogicalPresentation(ren, 1280, 720,
                                           SDL_LOGICAL_PRESENTATION_DISABLED));

    for (size_t i = 0; i < SDL_arraysize(gs_every_screen); i++) {
        gs_panel_report p = gs_panel_of(ren, &m, &t, gs_every_screen[i]);
        // The measurement itself is worth checking: a zero-sized panel would
        // pass everything below without a screen having been drawn at all.
        CHECK(p.w > 100.0f);
        CHECK(p.h > 100.0f);
        CHECK(p.view_w >= 1280.0f);

        CHECK(p.x >= 0.0f);
        CHECK(p.y >= 0.0f);
        CHECK(p.x + p.w <= p.view_w + 1.0f);
        CHECK(p.y + p.h <= p.view_h + 1.0f);

        // And at the size the game opens at, nothing is below the fold: a
        // button half outside the bottom of its own panel is the other half of
        // this fault, and it is what a screen grown one control at a time
        // eventually does.
        CHECK(p.hidden == 0.0f);
    }

    // Half that window, which is what somebody dragging a corner gets. The
    // panels no longer fit, and the rule that still has to hold is that they
    // are inside the window: what does not fit scrolls, rather than being drawn
    // where the mouse cannot go.
    CHECK(SDL_SetWindowSize(gs_win, GS_W, GS_H));
    CHECK(SDL_SetRenderLogicalPresentation(ren, GS_W, GS_H,
                                           SDL_LOGICAL_PRESENTATION_DISABLED));

    for (size_t i = 0; i < SDL_arraysize(gs_every_screen); i++) {
        gs_panel_report p = gs_panel_of(ren, &m, &t, gs_every_screen[i]);
        CHECK(p.w > 100.0f);
        CHECK(p.x >= 0.0f);
        CHECK(p.y >= 0.0f);
        CHECK(p.x + p.w <= p.view_w + 1.0f);
        CHECK(p.y + p.h <= p.view_h + 1.0f);
    }
}

TEST(exit_is_something_the_menu_asks_for_rather_than_does) {
    (void)ren;
    static gs_menu m;
    gs_menu_init(&m);

    // The menu does not own the loop, so it cannot end it. It raises a flag
    // and the frontend acts - the same shape as handing over a race setup.
    CHECK(!m.quit);
    m.quit = true;
    CHECK(m.quit);
}

int main(void) {
    printf("gearstick renderer tests\n");

    // No display anywhere in this: the dummy driver and the software renderer
    // are what let these run in CI on three platforms.
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("  FAIL SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *win = nullptr;
    SDL_Renderer *ren = nullptr;
    if (!SDL_CreateWindowAndRenderer("gearstick tests", GS_W, GS_H, 0, &win, &ren)) {
        printf("  FAIL SDL_CreateWindowAndRenderer: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    gs_win = win;

    run_a_car_behind_a_rise_is_hidden_by_it(ren);
    run_the_view_does_not_jump_as_a_car_crosses_a_tile_boundary(ren);
    run_interpolation_places_a_car_between_the_two_ticks_it_sits_between(ren);
    run_a_ramp_is_drawn_with_no_seam_and_no_hole(ren);
    run_picking_a_pixel_finds_the_ground_that_was_drawn_there(ren);
    run_a_track_built_with_the_brushes_saves_reloads_and_races(ren);
    run_the_elevation_brush_moves_the_ground_by_exactly_the_step_it_is_set_to(ren);
    run_a_ramp_drawn_in_the_editor_drives_like_the_ramp_that_was_drawn(ren);
    run_painting_ice_changes_what_the_car_does_when_it_gets_there(ren);
    run_painting_gravity_changes_how_far_the_car_flies_over_it(ren);
    run_the_gate_brush_places_a_route_where_the_pointer_is(ren);
    run_a_test_drive_starts_where_you_were_looking_and_changes_nothing(ren);
    run_coming_back_from_a_drive_returns_you_to_where_you_were_building(ren);
    run_raising_a_ramp_changes_where_the_ghost_lands_without_being_told_to(ren);
    run_a_track_can_be_built_from_a_pad_with_no_mouse_at_all(ren);
    run_moving_a_dial_restarts_the_ghost_under_the_new_one(ren);
    run_two_cars_take_their_input_from_different_places_and_go_different_ways(ren);
    run_the_second_pad_drives_the_second_car(ren);
    run_each_half_of_a_split_screen_shows_its_own_car(ren);
    run_four_players_get_four_views_that_tile_the_window_without_overlapping(ren);
    run_each_of_four_views_shows_its_own_car_and_costs_no_more_than_one_full_one(ren);
    run_the_screen_merges_when_the_cars_are_close_and_splits_when_they_are_not(ren);
    run_cars_hovering_at_the_threshold_do_not_flicker_the_screen_in_half(ren);
    run_the_view_does_not_jump_when_the_screen_merges_or_splits(ren);
    run_every_control_can_be_moved_and_every_player_can_drive_from_a_pad_alone(ren);
    run_changed_controls_survive_being_written_and_read_back(ren);
    run_every_vehicle_has_a_mesh_and_no_two_are_the_same_shape(ren);
    run_a_car_is_drawn_from_its_mesh_and_faces_where_it_is_pointing(ren);
    run_a_car_on_a_slope_leans_with_the_ground(ren);
    run_the_guide_tells_people_to_press_the_keys_the_game_listens_for(ren);
    run_the_release_notes_say_what_is_actually_shipped(ren);
    run_a_time_reads_the_way_people_say_it(ren);
    run_a_finished_race_becomes_a_table_in_the_order_it_finished(ren);
    run_the_store_remembers_drivers_and_records_between_runs(ren);
    run_the_stock_tracks_ship_and_are_worth_racing(ren);
    run_choosing_a_track_from_the_library_changes_what_is_raced(ren);
    run_loading_a_track_throws_away_the_undo_history(ren);
    run_the_store_carries_the_library_too(ren);
    run_an_empty_store_round_trips_rather_than_failing(ren);
    run_a_track_goes_out_through_the_clipboard_and_comes_back_the_same(ren);
    run_a_text_box_shows_a_caret_when_it_has_the_keyboard(ren);
    run_the_front_end_is_shut_until_somebody_signs_in(ren);
    run_a_wrong_name_and_a_wrong_password_are_refused_identically(ren);
    run_a_password_cannot_be_set_to_nothing(ren);
    run_a_password_on_a_profile_is_actually_required(ren);
    run_a_second_factor_is_asked_for_when_the_profile_has_one(ren);
    run_exit_is_something_the_menu_asks_for_rather_than_does(ren);
    run_the_heatmap_puts_the_line_everybody_drove_on_the_screen(ren);
    run_the_landing_arc_is_off_until_it_is_asked_for(ren);
    run_there_is_no_arc_drawn_for_a_car_on_the_ground(ren);
    run_a_wreck_is_drawn_as_wide_as_the_obstacle_it_actually_is(ren);
    run_no_two_grounds_are_drawn_the_same_colour(ren);
    run_the_hud_says_what_lap_it_is_and_changes_when_the_lap_does(ren);
    run_the_hud_says_what_place_you_are_in_and_changes_when_you_are_passed(ren);
    run_the_analyser_refuses_a_track_with_no_route_rather_than_guessing(ren);
    run_no_screen_is_drawn_bigger_than_the_window_it_is_in(ren);
    run_a_store_with_tracks_in_it_is_saved_whole(ren);
    run_the_condition_bar_stays_inside_the_hud(ren);
    run_there_is_always_a_way_back_out_of_wherever_you_are(ren);
    run_the_hud_fits_what_is_in_it_in_every_state_it_has(ren);
    run_a_start_line_and_a_finish_line_are_different_things(ren);
    run_a_car_on_the_near_side_of_a_line_is_drawn_in_front_of_it(ren);
    run_a_car_is_drawn_whole_wherever_it_sits_within_its_tile(ren);
    run_a_track_says_where_it_ends_and_which_way_it_goes(ren);
    run_the_camera_holds_the_car_still_between_ticks(ren);
    run_every_driver_can_see_their_own_car_on_ground_that_is_not_at_height_zero(ren);
    run_a_car_in_the_air_climbs_its_own_screen_rather_than_leaving_it(ren);
    run_a_car_down_a_drop_does_not_take_the_camera_off_the_other_one(ren);

    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();

    if (gs_failures == 0) {
        printf("all renderer tests passed\n");
        return 0;
    }
    printf("%d check%s failed\n", gs_failures, gs_failures == 1 ? "" : "s");
    return 1;
}

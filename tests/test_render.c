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

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "core/gs_sim.h"
#include "core/gs_track.h"
#include "gfx/gs_render.h"
#include "gfx/gs_meshes.h"
#include "platform/gs_bind.h"
#include "ui/gs_editor.h"
#include <SDL3_image/SDL_image.h>

#include "platform/gs_paths.h"
#include "net/gs_auth.h"
#include "ui/gs_menu.h"
#include "ui/gs_hud.h"
#include "ui/gs_style.h"
#include "ui/gs_ui_probe.h"
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

TEST(the_light_tree_counts_down_and_then_goes_green) {
    (void)ren;

    // **"We should actually show a real racing tree with red, orange, green
    // lights."** Counting lamps down one a second told you how long was left
    // and nothing about what to do with it. Red, amber and green are read
    // without being counted, and every driver already knows them: red while
    // there is time to wait, amber for the last of it, green to go - and green
    // is the tick the simulation stops holding the cars.
    static gs_track t;
    gs_flat_pavement(&t, 48, 48);
    gs_track_add_gate(&t, GS_INT(24), GS_INT(24), 0, GS_INT(5));

    static gs_world w;
    gs_fix sx = 0, sy = 0;
    gs_angle facing = 0;
    gs_track_grid(&t, 0, &sx, &sy, &facing);
    gs_park_car(&w, &t, sx, sy);
    gs_world_set_countdown(&w, GS_COUNTDOWN_TICKS);

    gs_camera cam = gs_camera_on(21.5f, 27.0f, 0.0f);
    cam.zoom = 1.2f;

    const uint32_t amber_at = GS_COUNTDOWN_TICKS - (uint32_t)GS_TICK_HZ * GS_AMBER_SECONDS;

    struct { uint32_t at; const char *want; } when[] = {
        { 0,                                          "red"   },
        { amber_at / 2u,                              "red"   },
        { amber_at + 10u,                             "amber" },
        { GS_COUNTDOWN_TICKS - 5u,                    "amber" },
        { GS_COUNTDOWN_TICKS + 10u,                   "green" },
        { GS_COUNTDOWN_TICKS + GS_GREEN_TICKS + 10u,  "dark"  },
    };

    int most_red = 0;
    for (size_t k = 0; k < sizeof when / sizeof when[0]; k++) {
        gs_world shown = w;
        shown.tick = when[k].at;

        gs_frame f = gs_render_frame(ren, &t, &shown, &shown, 1.0f, &cam);
        CHECK(f.px != nullptr);
        if (f.px == nullptr) return;

        int red = 0, amber = 0, green = 0;
        for (int i = 0; i < GS_W * GS_H; i++) {
            const uint8_t *p = &f.px[i * 4];
            // Amber is red *and* green together; red is red without green.
            if (p[0] > 200 && p[1] < 60 && p[2] < 50) red++;
            else if (p[0] > 200 && p[1] > 120 && p[1] < 200 && p[2] < 60) amber++;
            else if (p[1] > 200 && p[0] < 80 && p[2] < 100) green++;
        }

        if (SDL_strcmp(when[k].want, "red") == 0) {
            CHECK(red > 0);
            CHECK(amber == 0);
            CHECK(green == 0);
            if (red > most_red) most_red = red;
        } else if (SDL_strcmp(when[k].want, "amber") == 0) {
            CHECK(amber > 0);
            CHECK(red == 0);
            CHECK(green == 0);
        } else if (SDL_strcmp(when[k].want, "green") == 0) {
            CHECK(green > 0);
            CHECK(red == 0);
            CHECK(amber == 0);
        } else {
            CHECK(red == 0);
            CHECK(amber == 0);
            CHECK(green == 0);
        }

        gs_frame_free(&f);
    }

    // The tree is on the screen at all, rather than three lamps' worth of
    // nothing being counted as agreement.
    CHECK(most_red > 20);
}

TEST(a_start_line_and_a_finish_line_are_different_things) {
    (void)ren;

    // **"It is confusing whether beginning and end is."** A track drew one
    // chequered line and it was gate zero, which is where the cars are gridded
    // - so the first thing anybody saw on arriving was a chequered flag, which
    // everywhere means *finished*.
    //
    // What a line looks like now depends on the job it does, and the job
    // depends on what kind of route the track has. A path begins on a plain
    // white line and ends on a chequered one a long way away. A loop has one
    // line doing both, because a lap starts and finishes in the same place and
    // drawing two would invent a distinction the track does not have.
    static gs_track path, loop;
    gs_flat_pavement(&path, 48, 48);
    gs_track_add_gate(&path, GS_INT(12), GS_INT(24), 0, GS_INT(6));
    gs_track_add_gate(&path, GS_INT(36), GS_INT(24), 0, GS_INT(6));
    path.route = (uint8_t)GS_ROUTE_SPRINT;

    gs_flat_pavement(&loop, 48, 48);
    gs_track_add_gate(&loop, GS_INT(12), GS_INT(24), 0, GS_INT(6));
    gs_track_add_gate(&loop, GS_INT(36), GS_INT(24), 0, GS_INT(6));
    loop.route = (uint8_t)GS_ROUTE_CIRCUIT;

    // Parked well clear, so nothing stands on either line.
    static gs_world w;
    gs_park_car(&w, &path, GS_INT(24), GS_INT(44));

    int white[2][2] = { { 0, 0 }, { 0, 0 } };
    int black[2][2] = { { 0, 0 }, { 0, 0 } };

    for (int which = 0; which < 2; which++) {
        const gs_track *t = which == 0 ? &path : &loop;

        for (int gate = 0; gate < 2; gate++) {
            float at = gate == 0 ? 12.0f : 36.0f;

            gs_camera cam = gs_camera_on(at, 24.0f, 0.0f);
            cam.zoom = 1.6f;

            gs_frame f = gs_render_frame(ren, t, &w, &w, 1.0f, &cam);
            CHECK(f.px != nullptr);
            if (f.px == nullptr) return;

            // Sampled where the line lies, rather than counted over the frame.
            for (int i = -34; i <= 34; i++) {
                float across = 24.0f + (float)i * 0.08f;
                for (int b = 0; b < 20; b++) {
                    float along = -0.4f + (float)b * 0.04f;
                    float sx = 0.0f, sy = 0.0f;
                    gs_iso_project(&cam, at + along, across, 0.0f, &sx, &sy);
                    if (sx < 0 || sy < 0 || sx >= GS_W || sy >= GS_H) continue;
                    const uint8_t *p = &f.px[((int)sy * GS_W + (int)sx) * 4];
                    if (p[0] > 225 && p[1] > 225 && p[2] > 225) white[which][gate]++;
                    if (p[0] < 30 && p[1] < 30 && p[2] < 30) black[which][gate]++;
                }
            }
            gs_frame_free(&f);
        }
    }

    // The path: a plain white line where it starts, with no chequer in it, and
    // a chequered one where it ends.
    CHECK(white[0][0] > 40);
    CHECK(black[0][0] == 0);
    CHECK(white[0][1] > 40);
    CHECK(black[0][1] > 40);

    // The loop: one chequered line doing both jobs, and the gate that was the
    // path's finish is only a waypoint here - no line across it at all.
    CHECK(white[1][0] > 40);
    CHECK(black[1][0] > 40);
    CHECK(white[1][1] == 0);
    CHECK(black[1][1] == 0);
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

// The lobby the panel menu points at, filled the same way every time.
static void gs_panel_lobby_fill(void) {
    gs_panel_lobby = (gs_lobby){ 0 };
    gs_panel_lobby.count = GS_PROTO_MAX_PLAYERS;
    gs_panel_lobby.capacity = GS_PROTO_MAX_PLAYERS;
    for (int i = 0; i < GS_PROTO_MAX_PLAYERS; i++) {
        SDL_snprintf(gs_panel_lobby.player[i].name,
                     sizeof gs_panel_lobby.player[i].name, "player %d", i);
        gs_panel_lobby.player[i].slot = (uint8_t)i;
        gs_panel_lobby.player[i].present = true;
    }
}

static void gs_panel_menu(gs_menu *m, gs_track *t) {
    // **Built once and copied after that**, for two reasons and both matter.
    //
    // It costs two runs of argon2 over 64 MB - one to hash the driver's
    // password and one to check it on the way in - which is the point of argon2
    // and is most of a second under sanitizers. The walk builds this menu for
    // every seed, and the passes that go back to a state build it again for
    // every control they visit; at a second each that was costing more than the
    // walk itself.
    //
    // And a password is hashed over a **random salt**, so two menus built from
    // the same instructions differ in those bytes and in nothing else. Copying
    // makes every seed byte-for-byte the menu the walk started from, which is
    // what lets a path recorded during the walk lead back to the same state
    // afterwards.
    static gs_menu built;
    static bool have_built = false;
    if (have_built) {
        *m = built;
        gs_track_init(t, 32, 32, GS_SURF_PAVEMENT);
        gs_panel_lobby_fill();
        m->lobby = &gs_panel_lobby;
        return;
    }

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

    // **With one of them picked, and online.** The tracks screen only draws
    // what you can do *to* a track once there is one chosen - the name, the
    // code, the publishing and who to hand it to - and only offers the last two
    // where there is a server to share into. Measured with nothing picked, it
    // is measured at its smallest, which is the size nothing goes wrong at.
    m->picked = 0;
    m->online = true;

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
    gs_panel_lobby_fill();
    m->lobby = &gs_panel_lobby;
    m->track_progress = 0.5f;

    built = *m;
    have_built = true;
}

// ---------------------------------------------------------------------------
// Driving the front end the way a person does
//
// **"These types of issues are pretty basic... can we formally test all the
// paths through the UI. It is inefficient for me to trace all of these
// behaviours."** Every navigation fault found by hand this week was a decision
// buried in drawing code where nothing could reach it: a Race button offered
// before the server had answered, a results screen that put itself back on
// screen, Back from the records table throwing away the results behind it.
// None of them is subtle. All of them needed somebody to sit and click.
//
// So the menu is driven here by *keyboard navigation*, which Dear ImGui already
// supports and which reaches every control a player can reach. Focus is moved
// with Tab and the arrows and a control is activated with Space, exactly as
// somebody walking the panel with a pad would - so what is tested is the real
// screen, the real buttons and the real conditions on them, rather than a model
// of them that can drift.
//
// No new dependency: ImGuiIO_AddKeyEvent is what a backend uses to report a
// keystroke, and a test is just another backend.

typedef struct gs_ui {
    gs_menu  *m;
    gs_track *t;
    SDL_Renderer *ren;
} gs_ui;

// One frame of the real menu, with whatever input has been queued.
//
// **Nothing here is drawn, on purpose.** What a walk needs from a frame is the
// layout, where the focus went and what a press did to the menu - and all three
// are settled by the time the draw data has been built. Rasterising that draw
// data through the software renderer cost about eight milliseconds a frame,
// which is the whole reason walking the front end exhaustively looked
// unaffordable; it also queued commands SDL only hands back on a present, which
// is how a test came to take the machine down instead of failing. Laying out
// costs microseconds and queues nothing. The tests that want pixels have
// gs_panel_of and a frame of their own.
static void gs_ui_frame(gs_ui *ui) {
    cImGui_ImplSDLRenderer3_NewFrame();
    cImGui_ImplSDL3_NewFrame();
    ImGui_NewFrame();

    gs_screen next = gs_menu_frame(ui->m, ui->t);
    ui->m->screen = next;

    // Not optional even with nobody drawing: ImGui_Render is what ends the
    // frame, and ending the frame is what settles focus for the next one.
    ImGui_Render();
}

static void gs_ui_begin(gs_ui *ui, gs_menu *m, gs_track *t, SDL_Renderer *ren) {
    ui->m = m;
    ui->t = t;
    ui->ren = ren;

    // Keyboard nav, so focus can be walked. The game itself enables the pad;
    // this is the same navigation reached by a different key.
    ImGui_GetIO()->ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // A couple of frames to settle: ImGui needs one to lay a window out before
    // anything in it can take focus.
    gs_ui_frame(ui);
    gs_ui_frame(ui);
}

static void gs_ui_key(gs_ui *ui, ImGuiKey key) {
    ImGuiIO *io = ImGui_GetIO();
    ImGuiIO_AddKeyEvent(io, key, true);
    gs_ui_frame(ui);
    ImGuiIO_AddKeyEvent(io, key, false);
    gs_ui_frame(ui);
}

// **Where every control on a screen leads.** Focus is walked forward `n` times
// from a fresh start and then activated, for every `n` a panel could have - so
// what comes back is the set of destinations the screen's controls actually
// reach, without the test needing to know what any of them is called.
//
// That is the map these bugs were hiding in. A screen whose only exits are
// itself is a trap; a screen with no exits at all is a worse one; and a control
// that lands somewhere nobody expected shows up here as a destination nobody
// wrote down.
// Enough to walk past a full library and still reach the buttons under it. The
// tracks screen puts thirty-two selectable rows before its controls, so a
// smaller number here reports "no way off this screen" when the way off is
// simply further down the order than the walk went - which is worth knowing
// about for a player on a pad, and is not the same thing as a trap.
#define GS_UI_MAX_STEPS 72

static int gs_ui_exits(gs_ui *ui, gs_menu *fresh, gs_track *t, SDL_Renderer *ren,
                       gs_screen from, bool *reached) {
    for (int i = 0; i < GS_SCREEN_COUNT; i++) reached[i] = false;

    int found = 0;
    for (int steps = 0; steps < GS_UI_MAX_STEPS; steps++) {
        // A clean menu every time, so one activation cannot colour the next -
        // signing out, say, would change what every later press did.
        gs_menu m = *fresh;
        m.screen = from;
        gs_ui_begin(ui, &m, t, ren);

        for (int i = 0; i < steps; i++) gs_ui_key(ui, ImGuiKey_Tab);
        gs_ui_key(ui, ImGuiKey_Space);

        // **A control that lands somewhere that is not a screen fails here.**
        // This is one of the three things the walk was written to prove and it
        // was the one not being proved: the same condition that filtered
        // self-transitions out was quietly dropping an out-of-range
        // destination, so a button that set a screen number nothing draws
        // would have passed.
        CHECK(m.screen < GS_SCREEN_COUNT);

        if (m.screen != from && m.screen < GS_SCREEN_COUNT) {
            if (!reached[m.screen]) {
                reached[m.screen] = true;
                found++;
            }
        }
    }
    return found;
}

// **What one frame of a screen actually drew, by name.** Not what Tab can get
// to - everything, including the controls drawn dead and the ones nav skips,
// because a screen's controls are what it has and not what a keyboard is
// willing to visit.
#define GS_UI_MAX_ITEMS 512

static int gs_ui_controls(gs_ui *ui, gs_menu *fresh, gs_track *t,
                          SDL_Renderer *ren, gs_screen from, gs_ui_item *into,
                          int cap) {
    gs_menu m = *fresh;
    m.screen = from;
    gs_ui_begin(ui, &m, t, ren);

    gs_ui_probe_start(into, cap);
    gs_ui_probe_frame();
    gs_ui_frame(ui);
    int n = gs_ui_probe_count();
    gs_ui_probe_stop();
    return n;
}

// The same map gs_ui_exits builds, built by pressing each control by its name
// instead of by counting Tabs to it. One frame per control rather than n.
static void gs_ui_exits_by_name(gs_ui *ui, gs_menu *fresh, gs_track *t,
                                SDL_Renderer *ren, gs_screen from,
                                const gs_ui_item *items, int n, bool *reached) {
    for (int i = 0; i < GS_SCREEN_COUNT; i++) reached[i] = false;

    for (int i = 0; i < n; i++) {
        if (!items[i].reachable || items[i].disabled) continue;

        gs_menu m = *fresh;
        m.screen = from;
        gs_ui_begin(ui, &m, t, ren);

        gs_ui_probe_press(items[i].id);
        gs_ui_frame(ui);
        gs_ui_frame(ui);

        if (m.screen != from && m.screen < GS_SCREEN_COUNT) {
            // **A control that leads somewhere has a name.** This is the half
            // of "known by name" worth asserting: ImGui leaves its own
            // structural items anonymous - table cells, child regions, groups -
            // and those are welcome to be, because nobody wrote them and
            // nothing follows from pressing them. A control that moves a player
            // between screens is one somebody wrote, and a map that has to call
            // it 4128762891 is a map nobody can read.
            CHECK(items[i].label[0] != 0);

            reached[m.screen] = true;
        }
    }
}

static const gs_screen gs_every_screen[] = {
    GS_SCREEN_LOGIN, GS_SCREEN_TITLE, GS_SCREEN_PROFILES, GS_SCREEN_SETUP,
    GS_SCREEN_RESULTS, GS_SCREEN_RECORDS, GS_SCREEN_LOBBY, GS_SCREEN_TRACKS,
};

TEST(every_screen_has_a_way_off_it_and_the_ways_lead_somewhere_real) {
    // **The test that should have existed before any of this week's
    // navigation faults.** Every one of them was a decision buried in drawing
    // code where nothing could reach it, and every one needed a person to sit
    // and click to find. What is walked here is the real menu, with the real
    // buttons, activated the way a player with a pad activates them.
    //
    // Three properties, and they are the ones that were broken:
    //
    //  - every screen has at least one control that leaves it, or it is a trap;
    //  - no exit lands on a screen that is not a screen;
    //  - the title is reachable from everywhere, so there is always a way home.
    static gs_menu fresh;
    static gs_track t;
    gs_panel_menu(&fresh, &t);

    CHECK(SDL_SetWindowSize(gs_win, 1280, 720));
    CHECK(SDL_SetRenderLogicalPresentation(ren, 1280, 720,
                                           SDL_LOGICAL_PRESENTATION_DISABLED));

    gs_ui ui;
    bool reached[GS_SCREEN_COUNT];

    // Who leads where, so the second half can ask about the whole graph rather
    // than one screen at a time.
    static bool edges[GS_SCREEN_COUNT][GS_SCREEN_COUNT];
    for (int a = 0; a < GS_SCREEN_COUNT; a++) {
        for (int b = 0; b < GS_SCREEN_COUNT; b++) edges[a][b] = false;
    }

    for (size_t i = 0; i < SDL_arraysize(gs_every_screen); i++) {
        gs_screen from = gs_every_screen[i];
        int found = gs_ui_exits(&ui, &fresh, &t, ren, from, reached);

        // **Not a trap.** A screen you can reach and cannot leave is the worst
        // thing a front end can do, and it is what the results screen became
        // when it kept putting itself back.
        // **Two screens are allowed no button that changes screen**, and both
        // for reasons written down rather than discovered:
        //
        //  - the sign-in door leaves by signing in, which needs a password
        //    typed, or by quitting, which is not a screen at all;
        //  - the tracks screen puts a whole library of selectable rows before
        //    its buttons, so a pad reaches Back only after walking every track
        //    somebody owns. That is a real complaint about that screen and it
        //    is written up as one - but it is a long walk rather than a trap,
        //    and Escape leaves it either way.
        bool may_have_none = from == GS_SCREEN_LOGIN || from == GS_SCREEN_TRACKS;
        if (!may_have_none) CHECK(found > 0);

        for (int to = 0; to < GS_SCREEN_COUNT; to++) {
            edges[from][to] = reached[to];
        }
    }

    // **Home is always reachable.** Breadth-first from every screen: if the
    // title cannot be got to, somebody is stuck wherever they are.
    for (size_t i = 0; i < SDL_arraysize(gs_every_screen); i++) {
        bool seen[GS_SCREEN_COUNT] = { false };
        gs_screen queue[GS_SCREEN_COUNT];
        int head = 0, tail = 0;

        queue[tail++] = gs_every_screen[i];
        seen[gs_every_screen[i]] = true;

        if (gs_every_screen[i] == GS_SCREEN_LOGIN ||
            gs_every_screen[i] == GS_SCREEN_TRACKS) {
            continue;      // see above
        }

        bool home = false;
        while (head < tail) {
            gs_screen at = queue[head++];
            if (at == GS_SCREEN_TITLE) { home = true; break; }
            for (int to = 0; to < GS_SCREEN_COUNT; to++) {
                if (!edges[at][to] || seen[to]) continue;
                seen[to] = true;
                queue[tail++] = (gs_screen)to;
            }
        }
        CHECK(home);
    }
}

// ---------------------------------------------------------------------------
// The walk, going as deep as the front end does
// ---------------------------------------------------------------------------
//
// **Breadth-first over states rather than one press from a fresh menu.** The
// map above knows the first move out of every screen and nothing after it, so a
// fault that needs two presses to reach - pick a track, then race - is not
// something it can see at all.
//
// A state is a menu, and a menu is 600 kilobytes, so states are not kept: what
// is kept is the *path* to one, and a path is replayed to stand in it again.
// That is affordable now only because a frame costs a layout rather than a
// rasterisation. The hash is what says whether a state is somewhere new.

#define GS_WALK_STATES 4096
#define GS_WALK_DEPTH  96
#define GS_WALK_SLOTS  16384        // a power of two, comfortably over the states

// **What a walk can do, which is more than press things.** Tab and Space reach
// the controls; they do not reach Escape, they do not move a slider, and they
// cannot type a password - so a door that needs one is a door a press-only walk
// is carried through rather than opens.
//
// The words are not arbitrary text. What a password box has to be tried with is
// the right password and a wrong one; typing rubbish into it at length is a
// bigger walk that learns nothing the wrong one does not.
static const char *const gs_walk_words[] = {
    "gavin",                // the seeded driver's name: the door wants both
    "a good one",           // and the password that goes with it
    "not the password",
    // **A number, because one box on the door takes nothing else.** The code a
    // server asks for is six digits and the box filters everything that is not
    // one, so a walk carrying only words types into it and nothing arrives -
    // which reads exactly like a box that does not work.
    "123456",
};

static const ImGuiKey gs_walk_keys[] = {
    ImGuiKey_Escape,        // the way out of screens that have no button for it
    ImGuiKey_LeftArrow,
    ImGuiKey_RightArrow,
    ImGuiKey_UpArrow,
    ImGuiKey_DownArrow,
};

typedef struct gs_act {
    uint32_t id;        // a control to press, or 0
    int16_t  word;      // which word to type into it afterwards, or -1
    uint16_t key;       // a key to send afterwards, or 0
} gs_act;

typedef struct gs_walk_path {
    gs_act    act[GS_WALK_DEPTH];
    int       len;
    uint64_t  hash;                 // what standing here hashed to when found
    gs_screen screen;               // and which screen that was
} gs_walk_path;

#define GS_WALK_CTRLS 4096          // a power of two, ids offered anywhere

// **How many different states are explored per offering.**
//
// Keying the walk on the offering alone converges and cannot open the front
// door: the login form draws the same controls whatever has been typed into it,
// so picking a driver, typing a password and pressing the button are three acts
// that never change what is on screen until the last one works. A walk that
// only queues new-looking screens never does the second of them.
//
// Keying on the menu itself reaches everything and never finishes - millions of
// values, measured at 1.8 million presses without ending.
//
// So: states are told apart by the menu, and **each distinct offering is
// entered from at most this many of them**. Enough for a sequence to be walked
// through, far short of every value the fields behind it can hold. What is
// skipped by the bound is counted and printed rather than passed over.
#define GS_WALK_PER_SHAPE 8      // what a walk asking about sequences wants

typedef struct gs_shape_seen {
    uint64_t shape;
    int      count;
} gs_shape_seen;

// **A control, as something a person could be told about.** The walk knows an
// id, which is a hash and says nothing to anybody; a report naming what it
// found has to say "the Earth button on the setup screen". Kept beside the
// counts rather than looked up afterwards, because the screen a control was
// drawn on is gone by the time the walk ends.
typedef struct gs_walk_named {
    uint32_t id;
    char     label[GS_UI_LABEL];
    char     window[GS_UI_WINDOW];
    int      presses;       // how many times the walk did something to it
    bool     visible;       // was on screen at least once, rather than clipped
    bool     heading;       // was a table's column heading every time it appeared

    // **Where it was standing the last time pressing it did nothing.** A
    // control that never changes anything has to be tried again somewhere, and
    // somewhere is a path plus the two things that make a path mean anything:
    // the seed menu it was walked from and the screen it started on.
    gs_walk_path where;
    int          seed;
    gs_screen    from;
    bool         idled;         // pressing it there changed nothing
    bool         hidden;        // it was drawn there, scrolled out of sight
    bool         stranded;      // drawn off a panel that cannot scroll
    int          tick;          // and how far down the window it was found
    int          company;       // how many other live controls stood with it
} gs_walk_named;

typedef struct gs_walk {
    uint64_t     slot[GS_WALK_SLOTS];
    gs_shape_seen shape[GS_WALK_SLOTS];
    int          capped;
    uint32_t     offered[GS_WALK_CTRLS];
    uint32_t     pressed[GS_WALK_CTRLS];
    uint32_t     never[GS_WALK_CTRLS];

    // **Drawn, and out of sight where it was drawn.** Kept apart from `never`,
    // which means drawn dead or unreachable by the keyboard. Folding the two
    // together turns "six controls are dead in one state and live in another"
    // - the number the seeding exists for - into three hundred and seventy
    // four, most of them a table row that was simply scrolled past.
    uint32_t     unseen[GS_WALK_CTRLS];
    int          n_offered, n_pressed, n_never, n_unseen;
    int          n_stranded;    // drawn where no scroll and no key can reach

    // **What a press did, as against that it happened.** `pressed` is a fact
    // about the walk. `moved` is a fact about the front end: pressing this
    // control changed the menu, from somewhere, at least once. A control
    // offered and pressed and never in here did nothing every single time.
    uint32_t     moved[GS_WALK_CTRLS];
    int          n_moved;
    gs_walk_named named[GS_WALK_CTRLS];

    // **Which seed and which screen this walk is running from.** Written by the
    // caller and carried, so that a state can be stood in again after the walk
    // has finished: a path is only a path from the menu it started at.
    int          seed_at;
    gs_screen    seed_from;
    gs_walk_path queue[GS_WALK_STATES];
    int          head, tail;
    int          states;
    int          edges;
    int          deepest;
    int          did_nothing;
    int          typed;
    bool         ran_out;
    bool         fine;              // tell states apart by what has been typed
    int          words;             // how many of the words to try in each box
    int          per_shape;         // how many states may share one offering
    gs_screen    stop_at;           // give up the moment this is reached...
    bool         stop_set;          // ...if anybody asked for one
    bool         reached[GS_SCREEN_COUNT];

    // **Where each press led.** The front end as a graph, which is what turns
    // "the walk pressed everything" into statements about the game rather than
    // about the walk.
    bool         edge[GS_SCREEN_COUNT][GS_SCREEN_COUNT];
} gs_walk;

// Open addressing, and a zero slot means empty - so a hash of zero is nudged
// rather than lost.
static bool gs_walk_seen(gs_walk *w, uint64_t h) {
    if (h == 0) h = 1;
    size_t i = (size_t)h & (GS_WALK_SLOTS - 1);
    while (w->slot[i] != 0) {
        if (w->slot[i] == h) return true;
        i = (i + 1) & (GS_WALK_SLOTS - 1);
    }
    w->slot[i] = h;
    w->states++;
    return false;
}

// How many times this offering has been entered, and take one more if there is
// room. Zero is the empty marker, so a shape hashing to zero is nudged.
static bool gs_walk_room(gs_walk *w, uint64_t shape) {
    if (shape == 0) shape = 1;
    size_t i = (size_t)shape & (GS_WALK_SLOTS - 1);
    while (w->shape[i].shape != 0) {
        if (w->shape[i].shape == shape) {
            if (w->shape[i].count >= w->per_shape) return false;
            w->shape[i].count++;
            return true;
        }
        i = (i + 1) & (GS_WALK_SLOTS - 1);
    }
    w->shape[i].shape = shape;
    w->shape[i].count = 1;
    return true;
}

// The same, for control ids. A zero id never happens - ImGui does not mint one -
// so zero can mean empty here too.
static bool gs_walk_mark(uint32_t *tab, uint32_t id, int *count) {
    size_t i = (size_t)id & (GS_WALK_CTRLS - 1);
    while (tab[i] != 0) {
        if (tab[i] == id) return true;
        i = (i + 1) & (GS_WALK_CTRLS - 1);
    }
    tab[i] = id;
    (*count)++;
    return false;
}

// Is it in there already, without putting it in. The same table, asked rather
// than told.
static bool gs_walk_has(const uint32_t *tab, uint32_t id) {
    size_t i = (size_t)id & (GS_WALK_CTRLS - 1);
    while (tab[i] != 0) {
        if (tab[i] == id) return true;
        i = (i + 1) & (GS_WALK_CTRLS - 1);
    }
    return false;
}

// What this control is called, kept the first time it is seen. The two flags
// accumulate over every sighting rather than recording the first: a row clipped
// out of sight in one state and on screen in another **is** on screen, and a
// heading is only a heading if it was one every time.
static void gs_walk_note(gs_walk *w, const gs_ui_item *it) {
    size_t i = (size_t)it->id & (GS_WALK_CTRLS - 1);
    while (w->named[i].id != 0) {
        if (w->named[i].id == it->id) {
            if (it->visible) w->named[i].visible = true;
            if (!it->heading) w->named[i].heading = false;
            // **A name learned later is still its name.** ImGui reports a
            // label when the widget runs, and a widget clipped out of sight
            // returns before it gets that far - so the first sighting of a
            // scrolled-away row is nameless and every later one is not. Keeping
            // the first would file half the library under "unnamed structure".
            if (w->named[i].label[0] == '\0' && it->label[0] != '\0') {
                SDL_strlcpy(w->named[i].label, it->label,
                            sizeof w->named[i].label);
            }
            return;
        }
        i = (i + 1) & (GS_WALK_CTRLS - 1);
    }
    w->named[i].id = it->id;
    SDL_strlcpy(w->named[i].label, it->label, sizeof w->named[i].label);
    SDL_strlcpy(w->named[i].window, it->window, sizeof w->named[i].window);
    w->named[i].presses = 0;
    w->named[i].visible = it->visible;
    w->named[i].heading = it->heading;
    w->named[i].idled   = false;
    w->named[i].hidden  = false;
    w->named[i].stranded = false;
    w->named[i].tick    = 0;
    w->named[i].company = -1;
}

// The record for an id, or nothing if the walk never saw it.
static gs_walk_named *gs_walk_named_of(gs_walk *w, uint32_t id) {
    size_t i = (size_t)id & (GS_WALK_CTRLS - 1);
    while (w->named[i].id != 0) {
        if (w->named[i].id == id) return &w->named[i];
        i = (i + 1) & (GS_WALK_CTRLS - 1);
    }
    return nullptr;
}

// **A window's own furniture is not one of the front end's controls.** ImGui
// gives every window a title bar you can drag and a collapse arrow, and it
// keeps an implicit "Debug" window for anything submitted outside a Begin.
// Those come back from the probe looking exactly like controls - reachable, not
// disabled, with a name - and they are not: pressing the title bar of the parts
// box is not a thing a person does to build a track, and counting it as a
// control the walk failed to press would be padding the denominator with
// somebody else's widgets.
//
// Taken by name and window rather than by item, because both walks ask it: the
// editor's, of what it is about to press, and the menu's, of what it wrote
// down. One list, or it is two lists that disagree by next month.
//
// Named here rather than quietly skipped, because an exclusion nobody can see
// is how a coverage number stops meaning anything.
static bool gs_chrome(const char *label, const char *window) {
    // ImGui's own implicit window, for anything submitted outside a Begin.
    if (SDL_strncmp(window, "Debug", 5) == 0) return true;

    // A title bar: the item whose name is the window's name.
    if (SDL_strcmp(label, window) == 0) return true;

    return false;
}

// One thing a walk can do: press a control, type at it, send a key - in that
// order, because typing goes to whatever the press just put the caret in.
static void gs_act_do(gs_ui *ui, const gs_act *a) {
    if (a->id != 0) {
        gs_ui_probe_press(a->id);
        gs_ui_frame(ui);
        gs_ui_frame(ui);
    }
    if (a->word >= 0) {
        gs_ui_probe_type(gs_walk_words[a->word]);
        gs_ui_frame(ui);
        gs_ui_frame(ui);
        gs_ui_key(ui, ImGuiKey_Enter);      // a typed box is committed, as a person would
    }
    if (a->key != 0) {
        gs_ui_key(ui, (ImGuiKey)a->key);
    }
}

// Stand where a path leads, from the seed.
static void gs_walk_to(gs_ui *ui, gs_menu *m, const gs_menu *seed, gs_screen from,
                       const gs_walk_path *p, gs_track *t, SDL_Renderer *ren) {
    *m = *seed;
    m->screen = from;
    gs_ui_probe_settle();
    gs_ui_begin(ui, m, t, ren);

    for (int i = 0; i < p->len; i++) gs_act_do(ui, &p->act[i]);
}

// **What a state *is*, for the purpose of walking it.**
//
// Not the menu's bytes. Walking those does not terminate and cannot: the front
// end offers thirty-two tracks against eight vehicles against sixteen colours
// against four player slots, so the states a menu can hold run to the millions
// and a walk over them covers a vanishing fraction however long it is left. A
// measured run bore that out - the title screen alone was still going after
// nine and a half minutes and 1.8 million presses.
//
// What a walk is actually for is the controls and where they lead, so a state
// here is **what the front end is showing and what it will let you press**: the
// screen, every control on it, and whether each one is live or dead. Two menus
// offering the same controls in the same conditions are the same place to be
// standing, whichever of the thirty-two tracks is highlighted - and *that*
// space is finite, which is what lets a walk finish and claim it covered it.
//
// Which track, which vehicle, which colour and how many players are covered as
// values rather than as states. That is a separate item and it is in the plan.
static uint64_t gs_shape(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint64_t)b[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

// What the state being stood in has on it.
static int gs_walk_controls(gs_ui *ui, gs_ui_item *into, int cap) {
    gs_ui_probe_start(into, cap);
    gs_ui_probe_frame();
    gs_ui_frame(ui);
    int n = gs_ui_probe_count();
    gs_ui_probe_stop();
    return n;
}

// **The menu, less the three things that make it explode.**
//
// `profiles`, `records` and `library` are the bulk of a menu and the whole of
// its combinatorics: thirty-two tracks, sixteen drivers, a table of times. Walk
// on those and there is no end. Everything else - which screen, what is picked,
// what has been typed into which box, which flags a form has set - is small,
// and is exactly what decides where a press goes next.
//
// Leaving the big three out is also what stops the Delete chain: emptying the
// library changes the library and nothing else a walk steers by, so the
// fourteenth deletion is not a new place to be.
static uint64_t gs_walk_menu_key(const gs_menu *m) {
    const uint8_t *b = (const uint8_t *)m;
    uint64_t h = 0xcbf29ce484222325ULL;

    h = gs_shape(h, b, offsetof(gs_menu, profiles));
    h = gs_shape(h, b + offsetof(gs_menu, chosen),
                 offsetof(gs_menu, lobby) - offsetof(gs_menu, chosen));
    h = gs_shape(h, b + offsetof(gs_menu, lobby_slot),
                 offsetof(gs_menu, track_progress) - offsetof(gs_menu, lobby_slot));
    h = gs_shape(h, b + offsetof(gs_menu, server_text),
                 offsetof(gs_menu, panel) - offsetof(gs_menu, server_text));
    return h;
}

// The screen and everything it is offering, as one number.
static uint64_t gs_walk_shape_of(gs_ui *ui, const gs_menu *m, gs_ui_item *items,
                                 int cap, int *out_n, gs_walk *w, bool *anything_new,
                                 bool fine) {
    const int n = gs_walk_controls(ui, items, cap);
    const int held = n < cap ? n : cap;

    // **Which question is being asked decides how finely states are told
    // apart**, and both questions are real.
    //
    //  - *Was every control pressed?* Then a state is what is on offer, and
    //    which of thirty-two tracks is highlighted is not part of it. That
    //    converges, and it is the walk that covers the front end.
    //  - *Can the front end be got into at all?* Then what has been typed into
    //    which box is the entire question, and the offering is identical the
    //    whole way through signing in. That does not converge over the whole
    //    front end - it is bounded instead by stopping the moment it is in.
    uint64_t h;
    if (fine) {
        h = gs_walk_menu_key(m);
    } else {
        h = 0xcbf29ce484222325ULL;
        const int screen = (int)m->screen;
        h = gs_shape(h, &screen, sizeof screen);
    }
    for (int i = 0; i < held; i++) {
        h = gs_shape(h, &items[i].id, sizeof items[i].id);
        h = gs_shape(h, &items[i].disabled, sizeof items[i].disabled);
        h = gs_shape(h, &items[i].reachable, sizeof items[i].reachable);
    }
    // **What counts as offered is what could be pressed.** A control drawn dead
    // in every state it appears in, one nav can never land on, or one scrolled
    // out of sight is not a gap in the walk - it is counted separately so that
    // the coverage number means what it says.
    //
    // **Scrolled out of sight belongs in that list and was missing from it.**
    // A table submits every row it holds and ImGui drops the ones off-screen,
    // so the fourteenth track in a list showing ten came back looking exactly
    // like a control - and the walk pressed it, and the press did nothing,
    // because the row had already returned. Every one of those was a press that
    // could not happen counted as a press that did.
    if (w != nullptr) {
        for (int i = 0; i < held; i++) {
            gs_walk_note(w, &items[i]);
            if (!items[i].reachable || items[i].disabled) {
                gs_walk_mark(w->never, items[i].id, &w->n_never);
            } else if (!items[i].visible) {
                gs_walk_mark(w->unseen, items[i].id, &w->n_unseen);
            } else {
                gs_walk_mark(w->offered, items[i].id, &w->n_offered);
            }
        }
    }

    // **Somewhere worth standing is somewhere offering a control nobody has
    // pressed yet.** Novelty on its own does not converge: deleting the
    // fourteenth track offers a shorter list than deleting the thirteenth did,
    // for ever, and every one of those is the same Delete button doing the same
    // thing. What is being covered here is the controls, so what makes a state
    // worth queueing is a control on it that has not been pressed.
    if (anything_new != nullptr) {
        *anything_new = false;
        for (int i = 0; i < held && w != nullptr; i++) {
            if (!items[i].reachable || items[i].disabled || !items[i].visible) continue;
            size_t at = (size_t)items[i].id & (GS_WALK_CTRLS - 1);
            bool found = false;
            while (w->pressed[at] != 0) {
                if (w->pressed[at] == items[i].id) { found = true; break; }
                at = (at + 1) & (GS_WALK_CTRLS - 1);
            }
            if (!found) { *anything_new = true; break; }
        }
    }

    if (out_n != nullptr) *out_n = n;
    return h;
}

static void gs_walk_screen(gs_ui *ui, const gs_menu *seed, gs_track *t,
                           SDL_Renderer *ren, gs_screen from, gs_walk *w) {
    static gs_menu m;
    static gs_ui_item items[GS_UI_MAX_ITEMS];

    w->head = 0;
    w->tail = 0;
    w->queue[w->tail].len    = 0;
    w->queue[w->tail].hash   = 0;
    w->queue[w->tail].screen = from;
    w->tail++;

    {
        static gs_ui_item seed_items[GS_UI_MAX_ITEMS];
        gs_walk_to(ui, &m, seed, from, &w->queue[0], t, ren);
        w->queue[0].hash = gs_menu_hash(&m);
        bool unused = false;
        gs_walk_seen(w, w->queue[0].hash);
        gs_walk_room(w, gs_walk_shape_of(ui, &m, seed_items, GS_UI_MAX_ITEMS,
                                         nullptr, w, &unused, w->fine));
    }

    while (w->head < w->tail) {
        const gs_walk_path here = w->queue[w->head++];

        gs_walk_to(ui, &m, seed, from, &here, t, ren);

        // **A path leads back to the state it was found in.** If it does not,
        // something in the front end answers differently to the same presses,
        // and a walk that cannot return to a state cannot claim to have
        // covered what is past it.
        CHECK(gs_menu_hash(&m) == here.hash);

        // **The state this path leads to, taken before anything else is
        // drawn.** An earlier attempt took it after the enumeration frame
        // below, which is one frame further on than the path describes - so
        // every check that it was the same state failed, and the conclusion
        // drawn was that restoring does not work. Restoring works; snapshotting
        // a frame late does not.
        static gs_menu at;
        at = m;

        if (here.len > w->deepest) w->deepest = here.len;

        const int n = gs_walk_controls(ui, items, GS_UI_MAX_ITEMS);
        CHECK(n <= GS_UI_MAX_ITEMS);
        const int held = n < GS_UI_MAX_ITEMS ? n : GS_UI_MAX_ITEMS;

        // **A control drawn here and scrolled past.** Remembered with the way
        // back to it, in case nowhere else ever puts it on screen - which is
        // the case for most of a track library, because a walk that only
        // presses things never moves a table.
        //
        // **And if the window it is in does not scroll, it is not scrolled
        // past - it is simply gone.** No wheel reaches it, no key reaches it,
        // and no amount of walking will; the screen has laid a control out
        // somewhere nobody can press it, in this state, for good. That is a
        // fault in the screen rather than a gap in the walk, so it is counted
        // apart and asserted at zero.
        for (int i = 0; i < held; i++) {
            if (items[i].visible) continue;
            if (!items[i].reachable || items[i].disabled) continue;

            float scroll = 0.0f, reach = 0.0f;
            if (gs_ui_probe_scroll_at(items[i].window, &scroll, &reach) &&
                reach <= 0.0f) {
                gs_walk_named *lost = gs_walk_named_of(w, items[i].id);
                if (lost == nullptr || !lost->stranded) {
                    w->n_stranded++;
                    printf("  OFF THE PANEL '%s' in '%s' on screen %s, where "
                           "nothing scrolls\n",
                           items[i].label[0] != '\0' ? items[i].label : "(unnamed)",
                           items[i].window, gs_screen_name(m.screen));
                }
                if (lost != nullptr) lost->stranded = true;
            }

            gs_walk_note(w, &items[i]);
            gs_walk_named *nm = gs_walk_named_of(w, items[i].id);
            if (nm == nullptr || nm->hidden || nm->idled) continue;
            nm->where  = here;
            nm->seed   = w->seed_at;
            nm->from   = w->seed_from;
            nm->hidden = true;
        }

        // **Everything this state can be done to.** Every live control pressed;
        // every box that takes text pressed and then typed into, with each word
        // the walk knows; and the keys that belong to no control at all - Escape
        // above all, which is the way off screens that were never given a
        // button for it.
        static gs_act acts[GS_UI_MAX_ITEMS * 3 + 8];
        int n_acts = 0;

        // How much company a control has here, for the retry to choose the
        // state it goes back to.
        int live = 0;
        for (int i = 0; i < held; i++) {
            if (items[i].reachable && !items[i].disabled && items[i].visible) live++;
        }

        for (int i = 0; i < held; i++) {
            if (!items[i].reachable || items[i].disabled) continue;
            if (!items[i].visible) continue;      // it has already returned
            acts[n_acts].id = items[i].id;
            acts[n_acts].word = -1;
            acts[n_acts].key = 0;
            n_acts++;

            if (!items[i].typable) continue;

            // **How many words a box is tried with depends on what is being
            // asked.** Opening the door needs the right name, the right
            // password and a wrong one, because which of them was typed is the
            // whole question. Pressing every control does not: a box is pressed
            // by being typed into once, and trying three strings in every box
            // on every screen multiplies the states without covering one more
            // control. What is *in* the box is the dials item, not this one.
            const int words = w->words > 0 ? w->words : 1;
            for (int k = 0; k < words &&
                            k < (int)SDL_arraysize(gs_walk_words); k++) {
                acts[n_acts].id = items[i].id;
                acts[n_acts].word = (int16_t)k;
                acts[n_acts].key = 0;
                n_acts++;
            }
        }
        for (size_t k = 0; k < SDL_arraysize(gs_walk_keys); k++) {
            acts[n_acts].id = 0;
            acts[n_acts].word = -1;
            acts[n_acts].key = (uint16_t)gs_walk_keys[k];
            n_acts++;
        }

        for (int i = 0; i < n_acts; i++) {
            // **Back to the state this path leads to.** Replaying the whole
            // path for every control on it costs the depth of the path each
            // time, which is what a walk with a form in it turns into. This is
            // one memcpy - and it is only sound if what comes out is the state
            // that was left, which is what the check below is for. A menu is
            // the whole of its own state and not the whole of the state on
            // screen, so ImGui is put back to a standing start as well.
            m = at;
            gs_ui_probe_settle();
            CHECK(gs_menu_hash(&m) == here.hash);

            gs_act_do(ui, &acts[i]);
            w->edges++;
            if (acts[i].word >= 0) w->typed++;
            if (acts[i].id != 0) {
                gs_walk_mark(w->pressed, acts[i].id, &w->n_pressed);
                gs_walk_named *nm = gs_walk_named_of(w, acts[i].id);
                if (nm != nullptr) nm->presses++;
            }
            if (m.screen < GS_SCREEN_COUNT) w->reached[m.screen] = true;
            if (here.screen < GS_SCREEN_COUNT && m.screen < GS_SCREEN_COUNT &&
                m.screen != here.screen) {
                w->edge[here.screen][m.screen] = true;
            }
            if (w->stop_set && w->reached[w->stop_at]) return;

            const uint64_t h = gs_menu_hash(&m);

            // **A press that changed nothing at all is worth knowing about**,
            // and it is not somewhere new to stand.
            if (h == here.hash) {
                w->did_nothing++;

                // Where it was standing when it did nothing, kept so that a
                // control which does nothing *everywhere* can be tried again
                // here rather than described as dead on this evidence.
                //
                // **The busiest screen it did nothing on wins.** Not the
                // first and not the last: what the retry has to work with is
                // the *other* controls standing there, because what wakes a row
                // that is already picked is pressing a different row.
                //
                // A library the walk had emptied down to one entry is the worst
                // possible place to ask - the only track there is is the one
                // that is picked, and nothing on the screen can un-pick it. The
                // same row with thirty-one others beside it is woken by any of
                // them.
                if (acts[i].id != 0 && acts[i].word < 0 && acts[i].key == 0 &&
                    !gs_walk_has(w->moved, acts[i].id)) {
                    gs_walk_named *nm = gs_walk_named_of(w, acts[i].id);
                    if (nm != nullptr && live > nm->company) {
                        nm->where   = here;
                        nm->seed    = w->seed_at;
                        nm->from    = w->seed_from;
                        nm->company = live;
                        nm->idled   = true;
                    }
                }
                continue;
            }

            // **And one that changed something is worth knowing about by
            // name.** Typing counts as the box doing something - that is what a
            // box is for, and only a box is ever typed into - so what is
            // credited is the act that was aimed at a control. A bare key
            // belongs to no control and is aimed at nobody.
            if (acts[i].id != 0) {
                gs_walk_mark(w->moved, acts[i].id, &w->n_moved);
            }


            static gs_ui_item after[GS_UI_MAX_ITEMS];
            bool worth_standing = false;
            const uint64_t shape = gs_walk_shape_of(ui, &m, after,
                                                    GS_UI_MAX_ITEMS, nullptr,
                                                    w, &worth_standing, w->fine);
            // **Somewhere not stood in before is worth standing in, full
            // stop.** An earlier version also demanded the place offer a
            // control nobody had pressed, which converges and is wrong: signing
            // in is picking a driver, then typing, then pressing a button, and
            // every one of those controls has been pressed already by the time
            // it matters. Requiring novelty *of controls* stops the walk one
            // press outside the door it was written to open. What a state
            // offers that nobody has pressed is the report, not the reason.
            (void)worth_standing;

            // Somewhere not stood in before, by the menu rather than by what is
            // drawn - and only while this offering has room for another one.
            if (gs_walk_seen(w, h)) continue;
            if (!gs_walk_room(w, shape)) {
                w->capped++;
                continue;
            }

            // **Running out is a failure that says so, never a quiet stop.** A
            // walk that hits its ceiling and carries on regardless reports
            // coverage of a front end it did not finish looking at.
            if (here.len >= GS_WALK_DEPTH || w->tail >= GS_WALK_STATES) {
                w->ran_out = true;
                continue;
            }

            gs_walk_path next = here;
            next.act[next.len] = acts[i];
            next.len++;
            next.hash   = h;
            next.screen = m.screen;
            w->queue[w->tail++] = next;
        }
    }
}

TEST(a_menu_knows_a_state_it_has_already_been_in) {
    (void)ren;      // no pixels in this one: it is arithmetic on a value

    // **The thing that lets a walk stop.** Pressing on from where you got to
    // means arriving back somewhere sooner or later, and a walk that cannot
    // tell walks forever. So a menu comes down to one number - and what is
    // deliberately left out of that number is the whole design, because a hash
    // that includes a clock makes every frame somewhere new, which is the same
    // as having no hash at all.
    static gs_menu a;
    static gs_menu b;
    static gs_track t;
    gs_panel_menu(&a, &t);

    // **A copy of a state is that state.** This is the move a walk makes on
    // every step - take the menu it is standing in, copy it, press something -
    // so it is the property the whole thing rests on. It is also the check that
    // the padding between fields is not being read as content, because a struct
    // assignment is not obliged to carry padding across.
    b = a;
    CHECK(gs_menu_hash(&b) == gs_menu_hash(&a));

    // **Two rosters built from scratch are *not* the same state**, and that is
    // right rather than a fault: making a driver mints a random salt for their
    // password, so two rosters that read the same on screen differ where it
    // counts. The hash sees that, which is the proof it is looking deeper than
    // the fields a screen happens to draw.
    //
    // Made here rather than by asking `gs_panel_menu` for two menus, because
    // that builds its menu once and copies it afterwards - deliberately, since
    // it costs two runs of argon2 over 64 MB - so two of those *are* the same
    // bytes. The claim is about making a driver, so it is made by making one.
    static gs_menu one, two;
    gs_menu_init(&one);
    CHECK(gs_profile_add(&one.profiles, "gavin", GS_COLOUR_RED,
                         (uint8_t)GS_VEH_BAJA_BUG) == 0);
    CHECK(gs_menu_set_password(&one, 0, "a good one", "a good one"));
    gs_menu_init(&two);
    CHECK(gs_profile_add(&two.profiles, "gavin", GS_COLOUR_RED,
                         (uint8_t)GS_VEH_BAJA_BUG) == 0);
    CHECK(gs_menu_set_password(&two, 0, "a good one", "a good one"));
    CHECK(gs_menu_hash(&one) != gs_menu_hash(&two));

    const uint64_t base = gs_menu_hash(&a);

    // **Every field of gs_menu, and which side of the line it falls on.** Not a
    // sample of them - all of them, so that a field nobody thought about is a
    // failing test rather than a state the walk cannot tell apart.
    static const struct {
        size_t      at;
        const char *name;
        bool        is_state;
    } fields[] = {
        { offsetof(gs_menu, screen), "screen", true },
        { offsetof(gs_menu, profiles), "profiles", true },
        { offsetof(gs_menu, records), "records", true },
        { offsetof(gs_menu, library), "library", true },
        { offsetof(gs_menu, chosen), "chosen", true },
        { offsetof(gs_menu, store_dirty), "store_dirty", true },
        { offsetof(gs_menu, setup), "setup", true },
        { offsetof(gs_menu, result), "result", true },
        { offsetof(gs_menu, result_count), "result_count", true },
        { offsetof(gs_menu, new_name), "new_name", true },
        { offsetof(gs_menu, new_colour), "new_colour", true },
        { offsetof(gs_menu, new_vehicle), "new_vehicle", true },
        { offsetof(gs_menu, editing), "editing", true },
        { offsetof(gs_menu, picking_for), "picking_for", true },
        { offsetof(gs_menu, status), "status", true },
        { offsetof(gs_menu, signed_in), "signed_in", true },
        { offsetof(gs_menu, login_pick), "login_pick", true },
        { offsetof(gs_menu, login_password), "login_password", true },
        { offsetof(gs_menu, login_confirm), "login_confirm", true },
        { offsetof(gs_menu, login_code), "login_code", true },
        { offsetof(gs_menu, login_name), "login_name", true },
        { offsetof(gs_menu, login_making), "login_making", true },
        { offsetof(gs_menu, login_wants_code), "login_wants_code", true },
        { offsetof(gs_menu, login_setting), "login_setting", true },
        { offsetof(gs_menu, focus_form), "focus_form", true },
        { offsetof(gs_menu, server_password), "server_password", true },
        { offsetof(gs_menu, server_code), "server_code", true },
        { offsetof(gs_menu, server_login_pending), "server_login_pending", true },
        { offsetof(gs_menu, online), "online", true },
        { offsetof(gs_menu, race_requested), "race_requested", true },
        { offsetof(gs_menu, tracks_for_race), "tracks_for_race", true },
        { offsetof(gs_menu, records_from), "records_from", true },
        { offsetof(gs_menu, edit_requested), "edit_requested", true },
        { offsetof(gs_menu, new_requested), "new_requested", true },
        { offsetof(gs_menu, publish_requested), "publish_requested", true },
        { offsetof(gs_menu, withdraw_requested), "withdraw_requested", true },
        { offsetof(gs_menu, share_with), "share_with", true },
        { offsetof(gs_menu, share_on), "share_on", true },
        { offsetof(gs_menu, quit), "quit", true },
        { offsetof(gs_menu, lobby_slot), "lobby_slot", true },
        { offsetof(gs_menu, lobby_ready), "lobby_ready", true },
        { offsetof(gs_menu, server_text), "server_text", true },
        { offsetof(gs_menu, picked), "picked", true },
        { offsetof(gs_menu, take), "take", true },
        { offsetof(gs_menu, track_name), "track_name", true },
        { offsetof(gs_menu, name_for), "name_for", true },
        { offsetof(gs_menu, track_code), "track_code", true },
        { offsetof(gs_menu, code_for), "code_for", true },
        { offsetof(gs_menu, lobby), "lobby", false },
        { offsetof(gs_menu, track_progress), "track_progress", false },
        { offsetof(gs_menu, knocking_for), "knocking_for", false },
        { offsetof(gs_menu, panel), "panel", false },
    };

    uint8_t *bytes = (uint8_t *)&a;
    for (size_t i = 0; i < SDL_arraysize(fields); i++) {
        bytes[fields[i].at] = (uint8_t)(bytes[fields[i].at] ^ 0xFFu);
        const uint64_t moved = gs_menu_hash(&a);
        bytes[fields[i].at] = (uint8_t)(bytes[fields[i].at] ^ 0xFFu);

        if (fields[i].is_state) {
            if (moved == base) printf("  NOT IN THE STATE: %s\n", fields[i].name);
            CHECK(moved != base);
        } else {
            if (moved != base) printf("  IN THE STATE AND SHOULD NOT BE: %s\n",
                                      fields[i].name);
            CHECK(moved == base);
        }

        // And putting it back is the state it was, not merely a state.
        CHECK(gs_menu_hash(&a) == base);
    }

    // **The message, not the address it is kept at.** A lobby error is what a
    // player reads, so it is state; the pointer to it is not, and hashing that
    // would make one message read as two different states depending on where it
    // happened to be stored.
    static char locked[]  = "the door is locked";
    static char elsewhere[] = "the door is locked";
    static char missing[] = "there is no door";

    a.lobby_error = locked;
    const uint64_t said = gs_menu_hash(&a);
    CHECK(said != base);

    a.lobby_error = elsewhere;
    CHECK(gs_menu_hash(&a) == said);

    a.lobby_error = missing;
    CHECK(gs_menu_hash(&a) != said);

    a.lobby_error = nullptr;
    CHECK(gs_menu_hash(&a) == base);

    // **And the states a walk actually moves between are different states.**
    // The byte flips above prove the fields are in the number; these are the
    // moves a person makes, spelled out as the things that must never collide.
    const gs_screen was_screen  = a.screen;
    const int       was_signed  = a.signed_in;
    const int       was_chosen  = a.chosen;
    const uint8_t   was_players = a.setup.players;

    a.screen = (was_screen == GS_SCREEN_TRACKS) ? GS_SCREEN_TITLE
                                                : GS_SCREEN_TRACKS;
    CHECK(gs_menu_hash(&a) != base);
    a.screen = was_screen;
    CHECK(gs_menu_hash(&a) == base);

    a.signed_in = was_signed + 1;
    CHECK(gs_menu_hash(&a) != base);
    a.signed_in = was_signed;
    CHECK(gs_menu_hash(&a) == base);

    a.chosen = was_chosen + 1;
    CHECK(gs_menu_hash(&a) != base);
    a.chosen = was_chosen;
    CHECK(gs_menu_hash(&a) == base);

    a.setup.players = (uint8_t)(was_players + 1u);
    CHECK(gs_menu_hash(&a) != base);
    a.setup.players = was_players;
    CHECK(gs_menu_hash(&a) == base);

    // And the two that tick on their own do not, however far they have got.
    a.track_progress = 0.5f;
    a.knocking_for   = 9.0f;
    CHECK(gs_menu_hash(&a) == base);
    a.track_progress = 0.0f;
    a.knocking_for   = 0.0f;
}

// ---------------------------------------------------------------------------
// The conditions the buttons are under
// ---------------------------------------------------------------------------
//
// **A control drawn dead is a control a walk never presses**, and gs_panel_menu
// builds the front end at its fullest on purpose - signed in, online, a full
// library with a track picked, a finished race to show and a lobby with
// everybody in it. It was written that way to measure panels at the size they
// go wrong at, and as a place to walk from it is the one state where almost
// nothing is disabled.
//
// So the walk starts from each of these in turn, sharing one set of books. Each
// is the full menu with one thing taken away, because what is being looked for
// is the control that only appears, or only wakes up, when something is absent.
typedef void (*gs_seed_fn)(gs_menu *m);

static void gs_seed_everything(gs_menu *m) { (void)m; }

static void gs_seed_signed_out(gs_menu *m) {
    m->signed_in = -1;
    m->login_pick = -1;
    m->login_password[0] = '\0';
    m->login_name[0] = '\0';
}

static void gs_seed_offline(gs_menu *m) {
    m->online = false;
    m->lobby  = nullptr;
}

static void gs_seed_empty_library(gs_menu *m) {
    m->library.count = 0;
    m->picked = -1;
    m->chosen = -1;
}

static void gs_seed_nothing_picked(gs_menu *m) {
    m->picked = -1;
    m->chosen = -1;
}

static void gs_seed_no_results(gs_menu *m) {
    m->result_count = 0;
}

// **One to four players, because the setup screen is a different screen at
// each.** A grid row is drawn per player, and each row carries a driver, a
// vehicle and a colour of its own - so the control set at four players is not
// the one at one player with more of it, it is a different set. A walk seeded
// at whatever the fresh menu happened to hold has never seen three of them.
static void gs_seed_one_player(gs_menu *m)   { m->setup.players = 1; }
static void gs_seed_two_players(gs_menu *m)  { m->setup.players = 2; }
static void gs_seed_three_players(gs_menu *m){ m->setup.players = 3; }
static void gs_seed_four_players(gs_menu *m) { m->setup.players = GS_MAX_CARS; }

// And with a guest in a seat, which is not a roster driver and does not draw
// the same row.
static void gs_seed_a_guest_racing(gs_menu *m) {
    m->setup.players = 2;
    m->setup.profile[0] = -1;
    m->setup.profile[1] = 0;
}

static void gs_seed_alone_in_the_lobby(gs_menu *m) {
    gs_panel_lobby.count = 1;
    for (int i = 1; i < GS_PROTO_MAX_PLAYERS; i++) {
        gs_panel_lobby.player[i].present = false;
    }
    m->lobby = &gs_panel_lobby;
}

// **The conditions the last four buttons are under.** Each of these draws a
// control that no other starting state does, and each was found by counting the
// controls the screens name and seeing which of them the walk had never met.
static void gs_seed_a_lobby_ready_to_race(gs_menu *m) {
    m->lobby_ready    = true;
    m->track_progress = 1.0f;
}

static void gs_seed_a_driver_with_no_password(gs_menu *m) {
    m->signed_in     = -1;
    m->login_pick    = 0;
    m->login_setting = true;
    SDL_strlcpy(m->login_name, "gavin", sizeof m->login_name);
}

static void gs_seed_a_server_asking_for_a_code(gs_menu *m) {
    m->signed_in        = -1;
    m->login_pick       = -1;
    m->login_wants_code = true;
    SDL_strlcpy(m->login_name, "gavin", sizeof m->login_name);
}

static void gs_seed_a_track_that_shipped(gs_menu *m) {
    if (m->library.count == 0) return;
    m->library.entry[0].builtin = true;
    m->picked = 0;
}

static const struct {
    const char *name;
    gs_seed_fn  set;
} gs_seeds[] = {
    { "everything",          gs_seed_everything },
    { "signed out",          gs_seed_signed_out },
    { "offline",             gs_seed_offline },
    { "an empty library",    gs_seed_empty_library },
    { "no track picked",     gs_seed_nothing_picked },
    { "no results yet",      gs_seed_no_results },
    { "alone in the lobby",  gs_seed_alone_in_the_lobby },
    { "one player",          gs_seed_one_player },
    { "two players",         gs_seed_two_players },
    { "three players",       gs_seed_three_players },
    { "four players",        gs_seed_four_players },
    { "a guest racing",      gs_seed_a_guest_racing },
    { "a lobby ready to race", gs_seed_a_lobby_ready_to_race },
    { "no password yet",     gs_seed_a_driver_with_no_password },
    { "asked for a code",    gs_seed_a_server_asking_for_a_code },
    { "a track that shipped", gs_seed_a_track_that_shipped },
};

// **A window, put back where it was.** To the top - ImGui keeps a window's
// scroll under its name, and by the time anything here runs the walk has been
// through a great many screens - and then down by the number of wheel ticks
// that had the thing in question on screen. Winding from the top each time
// rather than correcting from wherever the last press left things is the
// difference between a sweep that repeats and one that depends on what the row
// before it did to the layout. Nothing at all for a window that does not
// scroll, which is most of them.
// `want` is where it is expected to already be, or a negative number for "no
// idea" - because winding is twenty frames and asking where a window is
// scrolled to is none, and most presses leave the scroll exactly where it was.
// Returns where it ended up, to be passed back in next time.
static float gs_walk_wind(gs_ui *ui, const char *window, int tick, float want,
                          int *acted)
{
    float now = 0.0f, max = 0.0f;
    if (!gs_ui_probe_scroll_at(window, &now, &max)) return -1.0f;
    if (max <= 0.0f && tick == 0) return now;
    if (want >= 0.0f && SDL_fabsf(now - want) < 0.5f) return now;

    for (int i = 0; i < 400 && now > 0.0f; i++) {
        gs_ui_probe_wheel(window, 1.0f);
        gs_ui_frame(ui);
        if (acted != nullptr) (*acted)++;
        gs_ui_probe_scroll_at(window, &now, &max);
    }
    for (int i = 0; i < tick; i++) {
        gs_ui_probe_wheel(window, -1.0f);
        gs_ui_frame(ui);
        if (acted != nullptr) (*acted)++;
    }
    gs_ui_probe_scroll_at(window, &now, &max);
    return now;
}

// **Tried again, properly, before anything is called dead.**
//
// A control that changed nothing every time the walk pressed it is one of two
// things and they look identical from where the walk stood: a button that does
// nothing, or a button pressed only in the state where it had nothing to do.
// Earth on a setup screen already set to Earth is the second, and so is a
// library row that is already the row picked; both are *ordinary*, and the walk
// cannot tell them from the first, because standing in one state per offering
// is what lets it finish at all.
//
// So the ones that never moved are taken back to a state they did nothing in
// and tried in the three ways that make the difference, exhaustively:
//
//   - **after every other control on the screen**, one at a time. That is what
//     turns a lit radio button off, what picks a different row, and what puts
//     something in a box worth undoing. Every one of them, not a guess at which
//     one matters.
//   - **with the arrow keys**, because a slider is not pressed, it is moved,
//     and a person moves it by landing on it and using the arrows.
//   - **with every word the walk knows**, because typing a driver's own name
//     into the box that already holds it changes nothing, and the box is not
//     the thing at fault.
//
// This costs about the controls on one screen, twice, per control tried - and
// it is paid only for the handful that did nothing. The alternative is a walk
// that stands in every state of every offering, which was measured: it does not
// finish in ten minutes where this finishes in one.
static bool gs_walk_retry(gs_ui *ui, gs_track *t, SDL_Renderer *ren,
                          const gs_walk_named *nm, int *acted, const char **how,
                          gs_menu *stood)
{
    static gs_menu seed, m, at;
    static gs_ui_item items[GS_UI_MAX_ITEMS];

    gs_panel_menu(&seed, t);
    gs_seeds[nm->seed].set(&seed);
    gs_walk_to(ui, &m, &seed, nm->from, &nm->where, t, ren);
    at = m;
    if (stood != nullptr) *stood = at;

    // Where in the window it was, for the ones that live below the fold. A
    // control the walk only ever reached by winding a table down has to be
    // wound back down before it can be tried again.
    float want = gs_walk_wind(ui, nm->window, nm->tick, -1.0f, acted);

    const int n = gs_walk_controls(ui, items, GS_UI_MAX_ITEMS);
    const int held = n < GS_UI_MAX_ITEMS ? n : GS_UI_MAX_ITEMS;

    // **It is standing where it was, and the control is on the screen.** Two
    // checks and both are needed. The state coming back is what makes this a
    // retry rather than a press somewhere else - and a state that comes back
    // can still have the control scrolled off it, which is exactly the case
    // that made half of these look dead in the first place.
    CHECK(gs_menu_hash(&m) == nm->where.hash);
    const uint64_t base = gs_menu_hash(&m);

    bool typable = false, here = false;
    for (int i = 0; i < held; i++) {
        if (items[i].id != nm->id) continue;
        here    = true;
        typable = items[i].typable;
    }
    if (!here) {
        printf("  RETRY '%s' is not on the screen its path leads back to\n",
               nm->label);
        return false;
    }

    // --- the arrows, for the things that are moved rather than pressed
    static const ImGuiKey arrows[] = {
        ImGuiKey_LeftArrow, ImGuiKey_RightArrow,
        ImGuiKey_UpArrow,   ImGuiKey_DownArrow,
    };
    for (size_t k = 0; k < SDL_arraysize(arrows); k++) {
        m = at;
        gs_ui_probe_settle();
        gs_walk_wind(ui, nm->window, nm->tick, want, acted);
        const gs_act a = { nm->id, -1, (uint16_t)arrows[k] };
        gs_act_do(ui, &a);
        (*acted)++;
        if (gs_menu_hash(&m) != base) { *how = "an arrow key"; return true; }
    }

    // --- every word, for the things that take text
    if (typable) {
        for (size_t k = 0; k < SDL_arraysize(gs_walk_words); k++) {
            m = at;
            gs_ui_probe_settle();
            gs_walk_wind(ui, nm->window, nm->tick, want, acted);
            const gs_act a = { nm->id, (int16_t)k, 0 };
            gs_act_do(ui, &a);
            (*acted)++;
            if (gs_menu_hash(&m) != base) { *how = "typing"; return true; }
        }
    }

    // --- and after each of the others in turn
    int tried = 0;
    for (int i = 0; i < held; i++) {
        if (items[i].id == nm->id) continue;
        if (!items[i].reachable || items[i].disabled || !items[i].visible) continue;

        m = at;
        gs_ui_probe_settle();
        gs_walk_wind(ui, nm->window, nm->tick, want, acted);
        const gs_act first = { items[i].id, -1, 0 };
        gs_act_do(ui, &first);
        (*acted)++;

        // What the other control left behind, which is what this press is
        // measured against - not the state we started in, or the sibling's own
        // work would be credited to the control being retried.
        const uint64_t between = gs_menu_hash(&m);

        // The other control may have opened a panel and made the list shorter,
        // which slides the row being retried out from under the press.
        gs_walk_wind(ui, nm->window, nm->tick, want, acted);

        const gs_act then = { nm->id, -1, 0 };
        gs_act_do(ui, &then);
        (*acted)++;
        if (gs_menu_hash(&m) != between) { *how = "after another control"; return true; }
        tried++;
    }
    (void)tried;
    return false;
}

// **Reaching what the panel is too short to show.**
//
// A table hands ImGui every row it holds; ImGui draws the ones that fit and
// drops the rest before the widget runs. So a library of thirty-two tracks in a
// panel with room for eleven has twenty-one rows that no key can reach, no
// press can land on, and nothing in a walk that only presses things will ever
// touch. They are not hypothetical: they are most of the library.
//
// A person reaches them with the wheel, so this does. The window is wound back
// to the top - ImGui keeps a window's scroll by name and the walk has been
// through a great many screens by now - and then walked down a tick at a time.
// At each stop, everything on screen that has not been pressed yet is pressed,
// and what it did is recorded in the same books as the rest of the walk, so the
// count of controls covered means the same thing before and after.
//
// A tick is a fraction of the panel's height, so no row can slip between two
// stops: to be missed it would have to be off the top at one and off the bottom
// at the next.
static bool gs_walk_reach(gs_ui *ui, gs_track *t, SDL_Renderer *ren,
                          gs_walk *w, const gs_walk_named *nm, int *acted)
{
    static gs_menu seed, m, at;
    static gs_ui_item items[GS_UI_MAX_ITEMS];
    static struct { uint32_t id; int tick; } seen[GS_UI_MAX_ITEMS];
    char window[GS_UI_WINDOW];

    SDL_strlcpy(window, nm->window, sizeof window);

    gs_panel_menu(&seed, t);
    gs_seeds[nm->seed].set(&seed);
    gs_walk_to(ui, &m, &seed, nm->from, &nm->where, t, ren);
    CHECK(gs_menu_hash(&m) == nm->where.hash);
    at = m;

    float now = 0.0f, max = 0.0f;
    if (!gs_ui_probe_scroll_at(window, &now, &max)) return false;
    gs_walk_wind(ui, window, 0, -1.0f, acted);
    gs_ui_probe_scroll_at(window, &now, &max);

    // --- **First pass: look, touch nothing.**
    //
    // Pressing changes the menu, and on this screen picking a track opens the
    // panel underneath it - which makes the list shorter, which makes ImGui
    // clamp the scroll, which moves every row out from under the next press.
    // The first version of this pressed as it went and half the rows it
    // reported covered had already slid away. So the wind-down only writes
    // down what it saw and how far down it was.
    int found = 0;
    for (int tick = 0; tick < 400; tick++) {
        const int n = gs_walk_controls(ui, items, GS_UI_MAX_ITEMS);
        const int held = n < GS_UI_MAX_ITEMS ? n : GS_UI_MAX_ITEMS;

        for (int i = 0; i < held && found < GS_UI_MAX_ITEMS; i++) {
            if (!items[i].reachable || items[i].disabled || !items[i].visible) continue;
            gs_walk_note(w, &items[i]);
            gs_walk_mark(w->offered, items[i].id, &w->n_offered);
            if (gs_walk_has(w->pressed, items[i].id)) continue;

            bool already = false;
            for (int k = 0; k < found; k++) {
                if (seen[k].id == items[i].id) { already = true; break; }
            }
            if (already) continue;
            seen[found].id   = items[i].id;
            seen[found].tick = tick;
            found++;
        }

        if (now >= max) break;
        gs_ui_probe_wheel(window, -1.0f);
        gs_ui_frame(ui);
        (*acted)++;
        gs_ui_probe_scroll_at(window, &now, &max);
    }

    // --- **Second pass: one row at a time, from the top each time.**
    //
    // The state is put back, the window wound to the top and then down by the
    // number of ticks that had this row on screen, and the row pressed. Winding
    // from the top rather than correcting from wherever the last press left
    // things is the difference between a sweep that is reproducible and one
    // that depends on what the row before it did to the layout.
    bool got = false;
    int  last_tick = -1;
    float want = -1.0f;
    for (int k = 0; k < found; k++) {
        m = at;
        gs_ui_probe_settle();
        want = gs_walk_wind(ui, window, seen[k].tick,
                            seen[k].tick == last_tick ? want : -1.0f, acted);
        last_tick = seen[k].tick;

        const uint64_t before = gs_menu_hash(&m);

        const gs_act press = { seen[k].id, -1, 0 };
        gs_act_do(ui, &press);
        (*acted)++;

        gs_walk_mark(w->pressed, seen[k].id, &w->n_pressed);
        if (gs_menu_hash(&m) != before) {
            gs_walk_mark(w->moved, seen[k].id, &w->n_moved);
        } else {
            // It was reached and it did nothing, which on a list of tracks is
            // usually the row that is already picked. That is the retry's
            // question, not this one's - so where it stands is written down the
            // same way the walk writes it down, with how far to wind.
            gs_walk_named *did = gs_walk_named_of(w, seen[k].id);
            if (did != nullptr && found > did->company) {
                did->where   = nm->where;
                did->seed    = nm->seed;
                did->from    = nm->from;
                did->tick    = seen[k].tick;
                did->company = found;
                did->idled   = true;
            }
        }
        if (seen[k].id == nm->id) got = true;
    }

    m = at;
    gs_ui_probe_settle();
    return got;
}

// **What is inside a box that opens.**
//
// A combo carries its values in a popup, and a popup is ImGui's state rather
// than the menu's: opening one changes nothing the walk can see, so the press
// reads as having done nothing and the entries inside are never enumerated,
// never pressed and never counted. That is how "guest", "first past the flag"
// and "last one driving" came to be drawn by a screen the walk had covered
// entirely.
//
// It cannot be fixed by walking harder. The walk stands in a state by copying
// the menu back and settling ImGui, and settling closes popups - which it has
// to, or a path leads somewhere different depending on what was left open. So
// the insides are swept afterwards, like the rows below a fold: go back, open
// it, and press what appeared, one at a time from the same starting state.
static void gs_walk_open(gs_ui *ui, gs_track *t, SDL_Renderer *ren, gs_walk *w,
                         const gs_walk_named *nm, int *acted)
{
    static gs_menu seed, m, at;
    static gs_ui_item shut[GS_UI_MAX_ITEMS];
    static gs_ui_item open[GS_UI_MAX_ITEMS];

    gs_panel_menu(&seed, t);
    gs_seeds[nm->seed].set(&seed);
    gs_walk_to(ui, &m, &seed, nm->from, &nm->where, t, ren);
    at = m;
    gs_walk_wind(ui, nm->window, nm->tick, -1.0f, acted);

    const int n_shut = gs_walk_controls(ui, shut, GS_UI_MAX_ITEMS);
    const int held_shut = n_shut < GS_UI_MAX_ITEMS ? n_shut : GS_UI_MAX_ITEMS;

    const gs_act press = { nm->id, -1, 0 };
    gs_act_do(ui, &press);
    (*acted)++;

    const int n_open = gs_walk_controls(ui, open, GS_UI_MAX_ITEMS);
    const int held_open = n_open < GS_UI_MAX_ITEMS ? n_open : GS_UI_MAX_ITEMS;

    for (int i = 0; i < held_open; i++) {
        if (!open[i].reachable || open[i].disabled || !open[i].visible) continue;

        bool was_there = false;
        for (int k = 0; k < held_shut && !was_there; k++) {
            if (shut[k].id == open[i].id) was_there = true;
        }
        if (was_there) continue;

        gs_walk_note(w, &open[i]);
        gs_walk_mark(w->offered, open[i].id, &w->n_offered);
        if (gs_walk_has(w->pressed, open[i].id)) continue;

        // From the same state every time, opened again: picking one entry
        // shuts the box, and the next entry has to be reached from where the
        // first was.
        m = at;
        gs_ui_probe_settle();
        gs_walk_wind(ui, nm->window, nm->tick, -1.0f, acted);
        gs_act_do(ui, &press);
        (*acted)++;

        const uint64_t before = gs_menu_hash(&m);
        const gs_act pick = { open[i].id, -1, 0 };
        gs_act_do(ui, &pick);
        (*acted)++;

        gs_walk_mark(w->pressed, open[i].id, &w->n_pressed);
        if (gs_menu_hash(&m) != before) {
            gs_walk_mark(w->moved, open[i].id, &w->n_moved);
            continue;
        }

        // **It changed nothing, which on a list of values usually means it was
        // already the value.** The same answer as everywhere else: pick a
        // different one first, then come back for this one. Every other entry
        // is tried, because which of them differs from this one is not
        // something anybody here knows.
        for (int j = 0; j < held_open && !gs_walk_has(w->moved, open[i].id); j++) {
            if (j == i) continue;
            if (!open[j].reachable || open[j].disabled || !open[j].visible) continue;

            m = at;
            gs_ui_probe_settle();
            gs_walk_wind(ui, nm->window, nm->tick, -1.0f, acted);
            gs_act_do(ui, &press);
            const gs_act other = { open[j].id, -1, 0 };
            gs_act_do(ui, &other);
            (*acted) += 2;

            const uint64_t between = gs_menu_hash(&m);
            gs_act_do(ui, &press);
            const gs_act again = { open[i].id, -1, 0 };
            gs_act_do(ui, &again);
            (*acted) += 2;

            if (gs_menu_hash(&m) != between) {
                gs_walk_mark(w->moved, open[i].id, &w->n_moved);
            }
        }
    }

    m = at;
    gs_ui_probe_settle();
}

// **Where a coverage number comes from when it does not come from the walk.**
//
// `pressed == offered` is necessary and nowhere near sufficient, because the
// number it is out of is *what this walk reached*: a walk that sees less
// reports all of what it saw and calls it complete. One alphabet measured 727
// controls where a wider one measured 758, and both said a hundred percent.
//
// The screens name their own controls, in the file that draws them, and that
// text does not care what any walk got to. So the labels are read out of the
// source and every one of them has to have been seen by the walk - which makes
// a control added to a screen and never reached a red tree on the next run,
// with nobody adding a case for it.
//
// The calls read are the ones Dear ImGui reports a name for. The three that
// draw a list or a swatch - `BeginCombo`, `Combo` and `ColorButton` - are not
// among them: none tells the hook its label, which is why the machine choosing
// a paint sees sixty-four identical nameless squares and why the box that picks
// a ground comes back anonymous. Requiring their names would be requiring
// something the probe cannot supply. What is inside them is covered instead, by
// the sweep that opens them and by every value of every dial being pressed.
static const char *const gs_named_calls[] = {
    "ImGui_Button(",       "ImGui_ButtonEx(",    "ImGui_SelectableEx(",
    "ImGui_InputText(",    "ImGui_InputTextEx(", "ImGui_SliderInt(",
    "ImGui_SliderFloat(",  "ImGui_Checkbox(",    "ImGui_RadioButtonIntPtr(",
    "gs_wide_button(",     "gs_go_button(",
};

#define GS_SOURCE_LABELS 256

// Every string literal in the first argument of every naming call in a file.
//
// The first argument and no further, because that is where a label goes and the
// ones after it are format strings and hints. All the literals in it rather
// than the first, because a label is sometimes a choice - `builtin ? "Edit a
// copy" : "Edit"` draws one of two and the screen has both. A first argument
// with no literal in it at all is a label built at runtime, and there is
// nothing here to check it against.
static int gs_labels_in_source(const char *path, char out[][GS_UI_LABEL], int cap)
{
    size_t len = 0;
    void *raw = SDL_LoadFile(path, &len);
    if (raw == nullptr) return -1;
    const char *src = (const char *)raw;

    int n = 0;
    for (size_t c = 0; c < SDL_arraysize(gs_named_calls); c++) {
        const char *call = gs_named_calls[c];
        const size_t call_len = SDL_strlen(call);

        for (const char *at = SDL_strstr(src, call); at != nullptr;
             at = SDL_strstr(at + call_len, call)) {
            const char *p = at + call_len;
            int depth = 0;

            while (*p != '\0') {
                if (*p == '(') { depth++; p++; continue; }
                if (*p == ')') { if (depth == 0) break; depth--; p++; continue; }
                if (*p == ',' && depth == 0) break;

                if (*p != '"') { p++; continue; }

                const char *from = ++p;
                while (*p != '\0' && *p != '"') p += (*p == '\\' && p[1] != '\0') ? 2 : 1;
                const size_t got = (size_t)(p - from);
                if (*p == '"') p++;
                if (got == 0 || got > GS_UI_LABEL - 1) continue;

                char label[GS_UI_LABEL];
                SDL_memcpy(label, from, got);
                label[got] = '\0';

                bool already = false;
                for (int k = 0; k < n; k++) {
                    if (SDL_strcmp(out[k], label) == 0) { already = true; break; }
                }
                if (!already && n < cap) SDL_strlcpy(out[n++], label, GS_UI_LABEL);
            }
        }
    }

    SDL_free(raw);
    return n;
}

// How many controls were drawn dead somewhere and pressed somewhere else. This
// is the number this whole idea is for: it is exactly the set that a walk from
// one starting state cannot reach, and it is zero without seeding.
static int gs_walk_revived(const gs_walk *w) {
    int n = 0;
    for (size_t i = 0; i < GS_WALK_CTRLS; i++) {
        const uint32_t id = w->never[i];
        if (id == 0) continue;
        size_t at = (size_t)id & (GS_WALK_CTRLS - 1);
        while (w->pressed[at] != 0) {
            if (w->pressed[at] == id) { n++; break; }
            at = (at + 1) & (GS_WALK_CTRLS - 1);
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
// The construction set, walked by machine
// ---------------------------------------------------------------------------
//
// **Nothing has ever pressed a button in the editor.** The tests above drive
// gs_editor_paint and set e->brush by hand, which measures the brush engine and
// says nothing at all about the palette a player uses to choose a brush -
// gs_editor_frame is called in exactly one place in this repository, and that
// place is main.c. Every control in the construction set has been checked by
// somebody clicking it.
//
// So the same walk that presses the front end presses this. It is the real
// palette, drawn by the real function, with the brushes and their settings
// where the editor puts them.

typedef struct gs_ed {
    gs_editor      *e;
    gs_track       *t;
    gs_view         view;
    gs_input_state  input;
} gs_ed;

// One frame of the real palette, laid out and not drawn - the same trade the
// front end's walk makes, and for the same reason.
static void gs_ed_frame(gs_ed *ed) {
    // Keyboard navigation, the same as the front end's walk turns on. Without
    // it ImGui has no notion of a focused item, and activating one by name is
    // activating nothing: the palette enumerates perfectly and every press
    // lands on the floor.
    ImGui_GetIO()->ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // **The client's four lines, which the editor cannot do for itself.** The
    // view carries the camera and the editor carries where it is looking; the
    // client copies one into the other every frame. Without it the camera is a
    // zero, and a zero camera means every pixel maps to the same nowhere - the
    // panels all work and the pointer can never be over any tile at all.
    ed->view.cam.cx   = ed->e->cam_x;
    ed->view.cam.cy   = ed->e->cam_y;
    ed->view.cam.cz   = 0.0f;
    ed->view.cam.zoom = ed->e->zoom;
    ed->view.cam.vw   = (float)ed->view.rect.w;
    ed->view.cam.vh   = (float)ed->view.rect.h;

    cImGui_ImplSDLRenderer3_NewFrame();
    cImGui_ImplSDL3_NewFrame();
    ImGui_NewFrame();

    gs_style_editor();
    gs_editor_frame(ed->e, ed->t, &ed->view, &ed->input);
    gs_style_menu();

    ImGui_Render();
}

// What the editor is showing, the same question the walk asks of a menu.
static int gs_walk_controls_ed(gs_ed *ed, gs_ui_item *into, int cap) {
    gs_ui_probe_start(into, cap);
    gs_ui_probe_frame();
    gs_ed_frame(ed);
    const int n = gs_ui_probe_count();
    gs_ui_probe_stop();
    return n;
}

// A key, sent the way a backend reports one.
static void gs_ed_key_press(gs_ed *ed, ImGuiKey key) {
    ImGuiIO *io = ImGui_GetIO();
    ImGuiIO_AddKeyEvent(io, key, true);
    gs_ed_frame(ed);
    ImGuiIO_AddKeyEvent(io, key, false);
    gs_ed_frame(ed);
}

// The wheel, over one of the editor's panels: to the top, then down a tick.
static float gs_ed_wind(gs_ed *ed, const char *window, float want) {
    float now = 0.0f, max = 0.0f;
    if (!gs_ui_probe_scroll_at(window, &now, &max)) return -1.0f;
    if (max <= 0.0f) return now;
    if (want >= 0.0f && SDL_fabsf(now - want) < 0.5f) return now;

    for (int i = 0; i < 200 && now > 0.0f; i++) {
        gs_ui_probe_wheel(window, 1.0f);
        gs_ed_frame(ed);
        gs_ui_probe_scroll_at(window, &now, &max);
    }
    return now;
}

// What an editor is, for telling one state from another: the settings a player
// chooses with, and the track they are choosing them onto. **Not** the camera,
// which moves with the mouse; not the ghost or the heat map, which are a world
// and an analysis and are megabytes of derived state; and not the log pointer,
// which is an address.
static uint64_t gs_ed_key(const gs_ed *ed) {
    const gs_editor *e = ed->e;
    uint64_t h = 0xcbf29ce484222325ULL;

    h = gs_shape(h, &e->active, sizeof e->active);
    h = gs_shape(h, &e->brush, sizeof e->brush);
    h = gs_shape(h, &e->surface, sizeof e->surface);
    h = gs_shape(h, &e->gravity, sizeof e->gravity);
    h = gs_shape(h, &e->radius, sizeof e->radius);
    h = gs_shape(h, &e->step, sizeof e->step);
    h = gs_shape(h, &e->gate_heading, sizeof e->gate_heading);
    h = gs_shape(h, &e->gate_width, sizeof e->gate_width);
    h = gs_shape(h, &e->part_kind, sizeof e->part_kind);
    h = gs_shape(h, &e->part, sizeof e->part);
    h = gs_shape(h, &e->dial_gravity, sizeof e->dial_gravity);
    h = gs_shape(h, &e->dial_drag, sizeof e->dial_drag);
    h = gs_shape(h, &e->dial_friction, sizeof e->dial_friction);
    h = gs_shape(h, &e->dial_damage, sizeof e->dial_damage);
    h = gs_shape(h, &e->ghost_on, sizeof e->ghost_on);
    h = gs_shape(h, &e->heat_on, sizeof e->heat_on);
    h = gs_shape(h, &e->show_controls, sizeof e->show_controls);
    h = gs_shape(h, &e->rebind_player, sizeof e->rebind_player);
    h = gs_shape(h, &e->rebind_action, sizeof e->rebind_action);
    h = gs_shape(h, e->status, sizeof e->status);

    // And the track, because a brush that changes it has changed the state.
    const uint64_t th = gs_track_hash(ed->t);
    h = gs_shape(h, &th, sizeof th);
    return h;
}

// **One attempt at one control**, which is two attempts.
//
// Pressed by name first: that reaches what the keyboard cannot, including
// controls no Tab order visits. If nothing changed, the keyboard is walked to
// it instead and Space sent - because some controls do not answer to being
// activated by name, and a slider answers to neither by *moving*: a person
// lands on it and uses the arrows, so arriving on it is what counts as having
// reached it.
//
// Restores the configuration first, **and the track with it**, so one attempt
// cannot depend on the last. Leaving the track alone was quietly wrong: the
// first press of a gate's remove button took the gate away and every later
// attempt in that configuration was made against a route with one fewer gate on
// it - so the second remove button was offered, never removed anything, and
// counted as a control the walk had failed to press.
static bool gs_ed_attempt(gs_ed *ed, const gs_editor *at, const gs_track *at_t,
                          const gs_ui_item *it, int fanout, int *actions)
{
    *ed->e = *at;
    *ed->t = *at_t;
    gs_ed_frame(ed);
    gs_ed_frame(ed);

    const uint64_t before = gs_ed_key(ed);
    gs_ui_probe_press(it->id);
    gs_ed_frame(ed);
    gs_ed_frame(ed);
    (*actions)++;

    if (gs_ed_key(ed) != before) return true;

    *ed->e = *at;
    *ed->t = *at_t;
    gs_ui_probe_focus_window(it->window);
    gs_ed_frame(ed);
    gs_ed_frame(ed);
    gs_ed_key_press(ed, ImGuiKey_Tab);

    for (int tab = 0; tab < fanout * 2 + 8; tab++) {
        if (gs_ui_probe_focused() == it->id) break;
        gs_ed_key_press(ed, ImGuiKey_Tab);
    }
    if (gs_ui_probe_focused() != it->id) return false;

    gs_ed_key_press(ed, ImGuiKey_Space);
    (*actions)++;
    return true;
}



// Which of the track's fields a brush is *for*. The parts box is the one that
// writes several: a piece lays ground, its own surface, and where it is a piece
// of the route, a gate too.
static void gs_brush_writes(int brush, bool *height, bool *surface,
                            bool *gravity, bool *gates) {
    *height  = brush == GS_BRUSH_RAISE || brush == GS_BRUSH_LOWER ||
               brush == GS_BRUSH_PART;
    *surface = brush == GS_BRUSH_SURFACE || brush == GS_BRUSH_PART;
    *gravity = brush == GS_BRUSH_GRAVITY;
    *gates   = brush == GS_BRUSH_GATE || brush == GS_BRUSH_PART;
}

typedef struct gs_brush_cfg {
    int   brush;
    int   surface;
    int   part_kind;
    float gravity;
    float step;
    float heading;
} gs_brush_cfg;

static int gs_brush_configs(gs_brush_cfg *out, int cap) {
    int n = 0;
    for (int lower = 0; lower < 2 && n < cap; lower++) {
        for (int h = 5; h <= 200 && n < cap; h += 5) {
            out[n] = (gs_brush_cfg){ 0 };
            out[n].brush = lower ? GS_BRUSH_LOWER : GS_BRUSH_RAISE;
            out[n].step  = (float)h / 100.0f;
            n++;
        }
    }
    for (int surf = 0; surf < GS_SURF_COUNT && n < cap; surf++) {
        out[n] = (gs_brush_cfg){ 0 };
        out[n].brush   = GS_BRUSH_SURFACE;
        out[n].surface = surf;
        n++;
    }
    for (int g = 0; g <= 390 && n < cap; g += 10) {
        out[n] = (gs_brush_cfg){ 0 };
        out[n].brush   = GS_BRUSH_GRAVITY;
        out[n].gravity = (float)g / 100.0f;
        n++;
    }
    for (int deg = 0; deg < 360 && n < cap; deg += 45) {
        out[n] = (gs_brush_cfg){ 0 };
        out[n].brush   = GS_BRUSH_GATE;
        out[n].heading = (float)deg;
        n++;
    }
    for (int kind = 0; kind < GS_PART_COUNT && n < cap; kind++) {
        out[n] = (gs_brush_cfg){ 0 };
        out[n].brush     = GS_BRUSH_PART;
        out[n].part_kind = kind;
        n++;
    }
    return n;
}

static void gs_brush_set(gs_editor *e, const gs_brush_cfg *c) {
    e->brush        = c->brush;
    e->surface      = c->surface;
    e->part_kind    = c->part_kind;
    e->part         = gs_part_default((gs_part_kind)c->part_kind);
    e->gravity      = c->gravity;
    e->step         = c->step > 0.0f ? c->step : 0.25f;
    e->gate_heading = c->heading;
    e->gate_width   = 2.5f;
    e->radius       = 0;
}

TEST(a_track_is_built_from_nothing_and_raced_without_leaving_the_editor) {
    (void)ren;

    // **The whole loop, performed once as a sequence.** New, shape the ground,
    // paint a surface onto the shape, put a low-gravity pocket over the jump,
    // lay a route, save it, come back to it, and race what came back until
    // somebody takes the flag.
    //
    // The tests around this one each start halfway through: they hold an editor
    // and a track already made and ask one question about one brush. What
    // nothing asked before is whether the *loop* closes - whether a track built
    // with the tools a player has is a track that validates, survives being
    // written and read, and can be won.
    static gs_editor ed;
    CHECK(gs_editor_init(&ed, 65536));

    // --- New. The size the editor's New gives you, and flat. -----------------
    static gs_track t;
    gs_track_init(&t, 64, 64, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= t.h; y++) {
        for (uint8_t x = 0; x <= t.w; x++) gs_track_set_corner(&t, x, y, 0);
    }
    CHECK(gs_track_validate(&t).problem == GS_TRACK_NO_START);

    // --- Shape the ground: a ridge to jump, raised a strip at a time. --------
    ed.brush  = GS_BRUSH_RAISE;
    ed.radius = 1;
    ed.step   = 0.35f;
    gs_edit_begin(ed.log);
    for (float x = 40.0f; x <= 44.0f; x += 1.0f) {
        for (float y = 30.0f; y <= 48.0f; y += 1.0f) gs_editor_paint(&ed, &t, x, y);
    }
    gs_edit_end(ed.log);
    CHECK(gs_track_height(&t, GS_INT(42), GS_INT(39)) > 0);

    // --- Paint a surface onto the shape, which is the interesting order. -----
    ed.brush   = GS_BRUSH_SURFACE;
    ed.surface = GS_SURF_ICE;
    ed.radius  = 2;
    gs_edit_begin(ed.log);
    gs_editor_paint(&ed, &t, 42.0f, 39.0f);
    gs_edit_end(ed.log);
    CHECK(gs_track_surface(&t, GS_INT(42) + GS_ONE / 2,
                           GS_INT(39) + GS_ONE / 2) == GS_SURF_ICE);
    CHECK(gs_track_height(&t, GS_INT(42), GS_INT(39)) > 0);   // still a ridge

    // --- A low-gravity pocket over it, which is the feature this game is for.
    ed.brush   = GS_BRUSH_GRAVITY;
    ed.gravity = 0.35f;
    ed.radius  = 3;
    gs_edit_begin(ed.log);
    gs_editor_paint(&ed, &t, 42.0f, 39.0f);
    gs_edit_end(ed.log);
    CHECK(gs_track_gravity(&t, GS_INT(42) + GS_ONE / 2,
                           GS_INT(39) + GS_ONE / 2) < GS_ONE);

    // --- A route: two gates on the circle a car will drive round. ------------
    ed.brush        = GS_BRUSH_GATE;
    ed.gate_width   = 4.0f;
    ed.gate_heading = 0.0f;
    gs_editor_paint(&ed, &t, 32.0f, 32.0f);
    ed.gate_heading = 180.0f;
    gs_editor_paint(&ed, &t, 22.0f, 46.0f);
    CHECK(t.gate_count == 2);
    t.route = (uint8_t)GS_ROUTE_CIRCUIT;

    // **It is a track now**, and the validator agrees - which it did not before
    // the route went on.
    CHECK(gs_track_validate(&t).problem == GS_TRACK_OK);

    const uint64_t built = gs_track_hash(&t);

    // --- Undone all the way back to the blank field, and redone. ------------
    //
    // **Every edit in the sequence, taken back in order and put back again.**
    // This is the property that makes an editor safe to experiment in, and it
    // is checked against the whole build rather than against one stroke: the
    // ridge, the ice over it, the gravity over that, and the route.
    const uint32_t depth = gs_edit_undo_depth(ed.log);
    CHECK(depth > 0);
    CHECK(depth < 512);

    // **Every prefix of the build, not just the whole of it.** Undo one step at
    // a time and write down what the track was at each; then redo, and every
    // one of those states has to come back in reverse. Checking only that all
    // the way back is blank and all the way forward is the finished track would
    // pass an editor that got the middle wrong in a way that cancelled out.
    static uint64_t was[512];
    uint32_t n = 0;
    was[n++] = gs_track_hash(&t);
    while (gs_edit_undo_depth(ed.log) > 0) {
        CHECK(gs_edit_undo(ed.log, &t));
        CHECK(n < SDL_arraysize(was));
        was[n++] = gs_track_hash(&t);
    }

    for (uint8_t y = 0; y <= t.h; y++) {
        for (uint8_t x = 0; x <= t.w; x++) {
            CHECK(gs_track_height(&t, GS_INT(x), GS_INT(y)) == 0);
        }
    }
    CHECK(t.gate_count == 0);

    while (gs_edit_redo_depth(ed.log) > 0) {
        CHECK(gs_edit_redo(ed.log, &t));
        CHECK(n >= 2);
        n--;
        CHECK(gs_track_hash(&t) == was[n - 1]);
    }

    // **Put back, it is the same track it was** - which the hash is the whole
    // check for, because a track's identity is its content.
    CHECK(gs_track_hash(&t) == built);
    CHECK(gs_edit_undo_depth(ed.log) == depth);

    // --- Save it, and get it back. ------------------------------------------
    CHECK(gs_editor_save(&ed, &t));

    static gs_track back;
    gs_track_init(&back, 8, 8, GS_SURF_PAVEMENT);   // deliberately not the same
    CHECK(gs_editor_load(&ed, &back));

    // **What comes back is what was built**, corner for corner and gate for
    // gate - the hash is the whole check, because a track's identity is its
    // content.
    CHECK(gs_track_hash(&back) == built);
    CHECK(back.w == t.w && back.h == t.h);
    CHECK(back.gate_count == 2);
    CHECK(gs_track_validate(&back).problem == GS_TRACK_OK);
    CHECK(gs_track_surface(&back, GS_INT(42) + GS_ONE / 2,
                           GS_INT(39) + GS_ONE / 2) == GS_SURF_ICE);
    CHECK(gs_track_gravity(&back, GS_INT(42) + GS_ONE / 2,
                           GS_INT(39) + GS_ONE / 2) < GS_ONE);

    // --- And race it, on the track that came back off the disk. --------------
    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_set_mode(&w, GS_MODE_RACE);
    gs_world_set_laps(&w, 2);
    gs_world_add_car(&w, &back, (uint8_t)GS_VEH_SPRINT_CAR,
                     GS_INT(32), GS_INT(32), 0);

    for (uint32_t i = 0; i < GS_TICK_HZ * 60u * 15u && !w.over; i++) {
        const gs_fix vx = w.car[0].vx;
        const gs_fix vy = w.car[0].vy;
        const gs_fix speed_sq = gs_fix_mul(vx, vx) + gs_fix_mul(vy, vy);

        gs_input in[GS_MAX_CARS] = { 0 };
        in[0] = (gs_input)((speed_sq < GS_INT(16) ? (unsigned)GS_IN_ACCEL : 0u) |
                           (unsigned)GS_IN_RIGHT);
        gs_world_step(&w, &back, in);
    }

    // **Won, on a track that did not exist when this test started.**
    CHECK(w.over);
    CHECK(w.winner == 0);
    CHECK(w.car[0].finish_tick > 0);
    CHECK(gs_car_laps_done(&back, &w.car[0]) == 2);

    // **And loading cleared the history**, which is deliberate and worth
    // pinning: the steps in it describe edits to a track that is no longer
    // here, so undo cannot walk back past a load into somebody else's track.
    CHECK(gs_edit_undo_depth(ed.log) == 0);
    CHECK(gs_edit_redo_depth(ed.log) == 0);

    gs_editor_quit(&ed);
}

TEST(one_brush_never_undoes_what_another_one_did) {
    (void)ren;

    // **The faults worth finding are in what one brush did to what another had
    // already done** - ice on a slope, gravity under a ramp, a gate on ground
    // that moves afterwards. A brush tested only on flat pavement is a brush
    // tested against nothing.
    //
    // Two properties, and between them they say a brush is *for* one thing:
    //
    //  - what the second brush is not for, it leaves exactly as it found it;
    //  - what it is for, it does the same regardless of what was there before.
    //
    // Every ordered pair is walked. The pair is the unit here rather than the
    // value, because interference is a property of two brushes meeting and not
    // of the number on a slider - and every one of those numbers is walked
    // exhaustively on its own in
    // every_brush_and_every_option_it_carries_does_what_it_says.
    static gs_editor ed;
    CHECK(gs_editor_init(&ed, 65536));

    static gs_brush_cfg cfg[256];
    const int n_cfg = gs_brush_configs(cfg, (int)SDL_arraysize(cfg));
    CHECK(n_cfg > 100);

    static gs_track t;
    int pairs = 0;

    for (int a = 0; a < n_cfg; a++) {
        for (int b = 0; b < n_cfg; b++) {
            gs_flat_pavement(&t, 40, 40);
            gs_edit_reset(ed.log);

            // The first brush, and what the track looked like afterwards.
            gs_brush_set(&ed, &cfg[a]);
            gs_editor_paint(&ed, &t, 18.0f, 18.0f);

            const gs_fix     h0 = gs_track_height(&t, GS_INT(18), GS_INT(18));
            const gs_surface s0 = gs_track_surface(&t, GS_INT(18) + GS_ONE / 2,
                                                   GS_INT(18) + GS_ONE / 2);
            const gs_fix     g0 = gs_track_gravity(&t, GS_INT(18) + GS_ONE / 2,
                                                   GS_INT(18) + GS_ONE / 2);
            const uint8_t    n0 = t.gate_count;
            const uint32_t   d0 = gs_edit_undo_depth(ed.log);
            const uint64_t   hash0 = gs_track_hash(&t);

            // The second, on top of it.
            gs_brush_set(&ed, &cfg[b]);
            gs_editor_paint(&ed, &t, 18.0f, 18.0f);

            const gs_fix     h1 = gs_track_height(&t, GS_INT(18), GS_INT(18));
            const gs_surface s1 = gs_track_surface(&t, GS_INT(18) + GS_ONE / 2,
                                                   GS_INT(18) + GS_ONE / 2);
            const gs_fix     g1 = gs_track_gravity(&t, GS_INT(18) + GS_ONE / 2,
                                                   GS_INT(18) + GS_ONE / 2);
            const uint8_t    n1 = t.gate_count;

            bool wh, ws, wg, wgate;
            gs_brush_writes(cfg[b].brush, &wh, &ws, &wg, &wgate);

            // **What it is not for, it leaves alone.**
            if (!wh)    CHECK(h1 == h0);
            if (!ws)    CHECK(s1 == s0);
            if (!wg)    CHECK(g1 == g0);
            if (!wgate) CHECK(n1 == n0);

            // **What it is for, it does the same whatever was there.**
            if (cfg[b].brush == GS_BRUSH_SURFACE) {
                CHECK(s1 == (gs_surface)cfg[b].surface);
            }
            if (cfg[b].brush == GS_BRUSH_GRAVITY) {
                const gs_fix want = (gs_fix)(cfg[b].gravity * (float)GS_ONE);
                const gs_fix unit = GS_ONE / GS_GRAVITY_UNIT;
                CHECK(g1 >= want - unit && g1 <= want + unit);
            }
            if (cfg[b].brush == GS_BRUSH_RAISE || cfg[b].brush == GS_BRUSH_LOWER) {
                // Relative, on purpose: raising ground that is already raised
                // is what building a ramp *is*.
                const gs_fix delta = (gs_fix)(cfg[b].step * (float)GS_ONE);
                const gs_fix want =
                    cfg[b].brush == GS_BRUSH_LOWER ? h0 - delta : h0 + delta;
                const gs_fix quantum = GS_ONE / 256;
                CHECK(h1 >= want - quantum && h1 <= want + quantum);
            }
            if (cfg[b].brush == GS_BRUSH_GATE && n0 < GS_TRACK_MAX_GATES) {
                CHECK(n1 == (uint8_t)(n0 + 1u));
            }
            if (cfg[b].brush == GS_BRUSH_PART) {
                // A piece always says what happened, and **anything it changed
                // it can take back**. It is allowed to change nothing: a
                // straight laid on ground that is already level road of that
                // surface is the road it would have laid, and the first brush
                // in this pair often leaves it exactly so.
                CHECK(ed.status[0] != '\0');
                if (gs_track_hash(&t) != hash0) {
                    CHECK(gs_edit_undo_depth(ed.log) > d0);
                }
            }
            pairs++;
        }
    }

    printf("  COMBOS %d brush configurations, %d ordered pairs walked\n",
           n_cfg, pairs);
    CHECK(pairs == n_cfg * n_cfg);

    gs_editor_quit(&ed);
}

TEST(every_brush_and_every_option_it_carries_does_what_it_says) {
    (void)ren;

    // **Every value on every dial the construction set has**, not three
    // interesting ones. The panel's own ranges are what is walked here - radius
    // 0 to 8, step 0.05 to 2, gravity 0 to 3.9, heading 0 to 359, half width
    // 0.5 to 8 - because a range a player can drag to and nobody has tried is
    // a range nobody has tested. The continuous ones are walked at a hundredth,
    // which is finer than the panel displays and far finer than a mouse can
    // land on.
    static gs_editor ed;
    CHECK(gs_editor_init(&ed, 65536));

    static gs_track t;
    int checked = 0;

    // --- raise and lower: the ground moves by exactly the number in the panel
    for (int hundredths = 5; hundredths <= 200; hundredths++) {
        const float step = (float)hundredths / 100.0f;
        const gs_fix delta = (gs_fix)(step * (float)GS_ONE);

        for (int lower = 0; lower < 2; lower++) {
            gs_flat_pavement(&t, 24, 24);
            ed.brush  = lower ? GS_BRUSH_LOWER : GS_BRUSH_RAISE;
            ed.radius = 0;
            ed.step   = step;

            gs_editor_paint(&ed, &t, 5.0f, 5.0f);
            const gs_fix got  = gs_track_height(&t, GS_INT(5), GS_INT(5));
            const gs_fix want = lower ? -delta : delta;

            // **To the resolution the track keeps.** Corner heights are stored
            // in 256ths of a tile, so a step of 0.25 lands exactly and a step
            // of 0.07 cannot - what is being pinned is that the ground moves by
            // the number in the panel and not by some other number, which is a
            // claim about the tool rather than about the storage.
            const gs_fix quantum = GS_ONE / 256;
            CHECK(got >= want - quantum && got <= want + quantum);

            // And the direction is never in doubt, at any step.
            CHECK(lower ? (got < 0) : (got > 0));
            checked++;
        }
    }

    // --- the radius: a disc, and the tile past its edge is untouched
    for (int r = 0; r <= 8; r++) {
        gs_flat_pavement(&t, 32, 32);
        ed.brush  = GS_BRUSH_RAISE;
        ed.radius = r;
        ed.step   = 0.25f;
        gs_editor_paint(&ed, &t, 16.0f, 16.0f);

        for (int dy = -9; dy <= 9; dy++) {
            for (int dx = -9; dx <= 9; dx++) {
                const bool inside = (dx * dx + dy * dy) <= r * r + r;
                const gs_fix h = gs_track_height(&t, GS_INT(16 + dx),
                                                 GS_INT(16 + dy));
                CHECK(inside ? (h != 0) : (h == 0));
                checked++;
            }
        }
    }

    // --- every surface the game has, painted and read back
    for (int surf = 0; surf < GS_SURF_COUNT; surf++) {
        gs_flat_pavement(&t, 24, 24);
        ed.brush   = GS_BRUSH_SURFACE;
        ed.surface = surf;
        ed.radius  = 0;
        gs_editor_paint(&ed, &t, 7.0f, 7.0f);
        CHECK(gs_track_surface(&t, GS_INT(7) + GS_ONE / 2,
                               GS_INT(7) + GS_ONE / 2) == (gs_surface)surf);
        checked++;
    }

    // --- every gravity the brush can paint, to the quantisation the track keeps
    for (int hundredths = 0; hundredths <= 390; hundredths++) {
        const float g = (float)hundredths / 100.0f;
        gs_flat_pavement(&t, 24, 24);
        ed.brush   = GS_BRUSH_GRAVITY;
        ed.gravity = g;
        ed.radius  = 0;
        gs_editor_paint(&ed, &t, 9.0f, 9.0f);

        const gs_fix want = (gs_fix)(g * (float)GS_ONE);
        const gs_fix got  = gs_track_gravity(&t, GS_INT(9) + GS_ONE / 2,
                                             GS_INT(9) + GS_ONE / 2);
        // A tile keeps gravity in 64ths, so what comes back is what was asked
        // for rounded to that - and never further away than one of them.
        const gs_fix unit = GS_ONE / GS_GRAVITY_UNIT;
        CHECK(got >= want - unit && got <= want + unit);
        checked++;
    }

    // --- every heading a gate can be given, to the turn's own resolution
    for (int deg = 0; deg < 360; deg++) {
        gs_flat_pavement(&t, 32, 32);
        ed.brush        = GS_BRUSH_GATE;
        ed.gate_heading = (float)deg;
        ed.gate_width   = 2.5f;
        gs_editor_paint(&ed, &t, 10.0f, 10.0f);

        CHECK(t.gate_count == 1);
        const gs_angle want = (gs_angle)(int32_t)((float)deg / 360.0f * 65536.0f);
        CHECK(t.gate[0].heading == want);
        checked++;
    }

    // --- every half width, likewise
    for (int hundredths = 50; hundredths <= 800; hundredths++) {
        const float half = (float)hundredths / 100.0f;
        gs_flat_pavement(&t, 32, 32);
        ed.brush        = GS_BRUSH_GATE;
        ed.gate_heading = 0.0f;
        ed.gate_width   = half;
        gs_editor_paint(&ed, &t, 10.0f, 10.0f);

        CHECK(t.gate_count == 1);
        CHECK(t.gate[0].half_width == (gs_fix)(half * (float)GS_ONE));
        checked++;
    }

    // --- every piece in the parts box, dropped
    for (int kind = 0; kind < GS_PART_COUNT; kind++) {
        gs_flat_pavement(&t, 40, 40);

        // **Onto ground that is not already what the piece would lay.** A
        // straight, a corner and a crossroads all put down level road of their
        // own surface, so dropped on flat pavement they are correctly a no-op -
        // the tool says "dropped a straight" and the track is byte for byte
        // what it was, because it already *was* that straight. Testing a piece
        // against ground it happens to match is testing nothing, so the ground
        // is roughed up and painted something else first.
        ed.brush   = GS_BRUSH_SURFACE;
        ed.surface = GS_SURF_ICE;
        ed.radius  = 6;
        gs_editor_paint(&ed, &t, 18.0f, 18.0f);

        ed.brush  = GS_BRUSH_RAISE;
        ed.radius = 2;
        ed.step   = 0.5f;
        gs_editor_paint(&ed, &t, 16.0f, 17.0f);
        gs_editor_paint(&ed, &t, 20.0f, 19.0f);

        const uint64_t before = gs_track_hash(&t);
        const uint32_t depth_before = gs_edit_undo_depth(ed.log);

        ed.brush     = GS_BRUSH_PART;
        ed.part_kind = kind;
        ed.part      = gs_part_default((gs_part_kind)kind);
        gs_editor_paint(&ed, &t, 18.0f, 18.0f);

        // Every piece either changes the track or says why it would not, and
        // saying nothing at all is the failure worth catching.
        CHECK(ed.status[0] != '\0');
        if (SDL_strstr(ed.status, "will not fit") == nullptr) {
            // **It went into the history, which is the thing that matters.**
            // Not that the track changed: a straight road laid on flat pavement
            // is level ground of the same surface, so a piece can be dropped
            // correctly and leave the terrain byte for byte as it was. What
            // must never happen is a piece reporting itself dropped with
            // nothing recorded, because that is a piece that cannot be undone.
            if (gs_edit_undo_depth(ed.log) <= depth_before) {
                printf("  PARTMISS kind=%d '%s' status='%s' hash_changed=%d\n",
                       kind, gs_part_name((gs_part_kind)kind), ed.status,
                       (int)(gs_track_hash(&t) != before));
            }
            CHECK(gs_edit_undo_depth(ed.log) > depth_before);
            (void)before;
        }
        checked++;
    }

    // --- the four dials, every hundredth of their range, into a real world
    for (int hundredths = 0; hundredths <= 400; hundredths++) {
        const float v = (float)hundredths / 100.0f;
        static gs_world w;

        ed.dial_gravity  = v;
        ed.dial_drag     = v;
        ed.dial_friction = v;
        ed.dial_damage   = v;
        gs_editor_apply_dials(&ed, &w);

        const gs_fix want = (gs_fix)(v * (float)GS_ONE);

        // **The dial is a multiple of Earth and the world wants an
        // acceleration**, which is the conversion that once made every race
        // from the setup screen run at forty percent of the gravity it claimed.
        // It is gs_world_init's job, so this is the check that the editor's
        // dial goes through it rather than round it.
        CHECK(w.gravity        == gs_fix_mul(GS_GRAVITY_EARTH, want));
        CHECK(w.drag_scale     == want);
        CHECK(w.friction_scale == want);
        CHECK(w.damage_scale   == want);
        checked++;
    }

    printf("  BRUSHES %d option values checked\n", checked);
    CHECK(checked > 4000);

    gs_editor_quit(&ed);
}

TEST(every_control_in_the_construction_set_is_pressed) {
    // **Every configuration the palette has, and every control in each.**
    //
    // Searching for the editor's states does not work and is the wrong shape
    // besides: brush against surface against radius against which piece is
    // chosen runs to millions of combinations, an editor carries a ghost world
    // and a heat map so only a few hundred of them fit in a queue, and the
    // search spends itself on combinations while nine controls sit unpressed
    // because their state never came up.
    //
    // But what decides *what the palette shows* is a handful of scalars, and
    // they can simply be set. So this sweeps them - every brush, every surface
    // under the surface brush, every piece in the parts box, the panels open
    // and shut, and a route with gates on it to bring out the buttons that
    // remove them - and presses everything each one offers.
    static gs_editor e;
    CHECK(gs_editor_init(&e, 65536));

    static gs_track t;
    gs_flat_pavement(&t, 32, 32);

    static gs_ed ed;
    ed.e = &e;
    ed.t = &t;
    ed.view = (gs_view){ 0 };
    ed.view.rect = (SDL_Rect){ 0, 0, 1280, 720 };
    ed.input = (gs_input_state){ 0 };
    gs_bind_defaults(&ed.input.bind);

    CHECK(SDL_SetWindowSize(gs_win, 1280, 720));
    CHECK(SDL_SetRenderLogicalPresentation(ren, 1280, 720,
                                           SDL_LOGICAL_PRESENTATION_DISABLED));

    gs_editor_toggle(&e, &ed.view);
    CHECK(e.active);

    static gs_walk cov;
    SDL_memset(cov.offered, 0, sizeof cov.offered);
    SDL_memset(cov.pressed, 0, sizeof cov.pressed);
    SDL_memset(cov.never,   0, sizeof cov.never);
    cov.n_offered = 0;
    cov.n_pressed = 0;
    cov.n_never   = 0;

    static gs_ui_item items[GS_UI_MAX_ITEMS];
    static gs_ui_item below[GS_UI_MAX_ITEMS];

    // The panels the construction set puts on screen, for winding through.
    static const char *const gs_ed_panels[] = {
        "Construction set", "Parts box", "Controls",
    };

    // Names for the ids, so anything left unpressed can be named rather than
    // reported as a number nobody can look up.
    #define GS_ED_NAMES 256
    static uint32_t name_id[GS_ED_NAMES];
    static char     name_of[GS_ED_NAMES][GS_UI_LABEL];
    int named = 0;

    int configs = 0;
    int actions = 0;
    int moved   = 0;
    int chrome  = 0;
    int unnamed = 0;

    // Two gates, so the route list and its remove buttons are drawn.
    e.brush = GS_BRUSH_GATE;
    gs_editor_paint(&e, &t, 8.0f, 8.0f);
    gs_editor_paint(&e, &t, 20.0f, 8.0f);
    CHECK(t.gate_count == 2);

    for (int brush = 0; brush < GS_BRUSH_COUNT; brush++) {
        // The sub-choice each brush carries, walked in full: the surface brush
        // shows a surface, the parts box shows a piece, the rest show neither.
        int subs = 1;
        if (brush == GS_BRUSH_SURFACE) subs = GS_SURF_COUNT;
        else if (brush == GS_BRUSH_PART) subs = GS_PART_COUNT;

        for (int sub = 0; sub < subs; sub++) {
            for (int panel = 0; panel < 2; panel++) {
                static gs_editor at;
                static gs_track at_track;
                e.brush         = brush;
                if (brush == GS_BRUSH_SURFACE) e.surface = sub;
                if (brush == GS_BRUSH_PART)    e.part_kind = sub;
                e.show_controls = panel != 0;
                e.ghost_on      = panel != 0;
                e.heat_on       = panel == 0;

                // **Open, because a walk that presses everything presses the
                // arrow that folds a panel shut.** ImGui remembers that under
                // the window's name for the rest of the process, so one
                // configuration folding the palette leaves every configuration
                // after it walking a title bar.
                for (size_t p = 0; p < SDL_arraysize(gs_ed_panels); p++) {
                    if (gs_ui_probe_unfold(gs_ed_panels[p])) gs_ed_frame(&ed);
                    gs_ed_wind(&ed, gs_ed_panels[p], -1.0f);
                }

                gs_ed_frame(&ed);
                gs_ed_frame(&ed);
                at = e;
                at_track = t;
                configs++;

                gs_ui_probe_start(items, GS_UI_MAX_ITEMS);
                gs_ui_probe_frame();
                gs_ed_frame(&ed);
                const int n = gs_ui_probe_count();
                gs_ui_probe_stop();
                CHECK(n <= GS_UI_MAX_ITEMS);
                const int held = n < GS_UI_MAX_ITEMS ? n : GS_UI_MAX_ITEMS;

                for (int i = 0; i < held; i++) {
                    if (gs_chrome(items[i].label, items[i].window)) { chrome++; continue; }

                    // **Named and unnamed are counted apart.** ImGui names the
                    // widgets a person presses and leaves its own structure
                    // anonymous - a child region, a group, a window's resize
                    // grip. Folding the anonymous ones into the total would
                    // shrink the denominator by twenty and report a perfect
                    // score for less work; leaving them in without saying so
                    // would report a shortfall for things nobody can name. So
                    // both numbers are printed and it is the named ones that
                    // are asserted.
                    if (items[i].label[0] == 0) {
                        const int was = unnamed;
                        gs_walk_mark(cov.never, items[i].id, &unnamed);
                        (void)was;
                        continue;
                    }
                    // **Named whether or not it can be pressed.** A control
                    // the palette draws dead in every configuration walked -
                    // the heat map, until an analysis has been run - is still a
                    // control the palette draws, and the count taken from the
                    // source asks whether it was *met*, not whether it moved.
                    bool fresh = false;
                    if (items[i].reachable && !items[i].disabled) {
                        const int was = cov.n_offered;
                        gs_walk_mark(cov.offered, items[i].id, &cov.n_offered);
                        fresh = cov.n_offered != was;
                    } else {
                        const int was = cov.n_never;
                        gs_walk_mark(cov.never, items[i].id, &cov.n_never);
                        fresh = cov.n_never != was;
                    }
                    if (fresh && named < GS_ED_NAMES) {
                        name_id[named] = items[i].id;
                        SDL_snprintf(name_of[named], GS_UI_LABEL,
                                     "%s | in %s | typable=%d",
                                     items[i].label, items[i].window,
                                     (int)items[i].typable);
                        named++;
                    }
                }

                for (int i = 0; i < held; i++) {
                    if (!items[i].reachable || items[i].disabled) continue;
                    if (gs_chrome(items[i].label, items[i].window)) continue;
                    if (items[i].label[0] == 0) continue;   // ImGui's own structure

                    if (gs_ed_attempt(&ed, &at, &at_track, &items[i], held,
                                      &actions)) {
                        moved++;
                        gs_walk_mark(cov.pressed, items[i].id, &cov.n_pressed);
                    }
                }

                // **And what is below the fold of each panel.** The palette
                // is taller than the room it has: four of its buttons - save,
                // load, and the two that move a track as text - are past the
                // bottom of it, and so is the button that puts the controls
                // back to their defaults. A walk that only presses what it can
                // see had never met any of them, and said it had covered the
                // construction set.
                //
                // Only what has not been pressed yet, so the second
                // configuration onwards costs the winding and nothing else.
                for (size_t p = 0; p < SDL_arraysize(gs_ed_panels); p++) {
                    float now = 0.0f, max = 0.0f;
                    if (!gs_ui_probe_scroll_at(gs_ed_panels[p], &now, &max)) continue;
                    if (max <= 0.0f) continue;

                    gs_ed_wind(&ed, gs_ed_panels[p], -1.0f);
                    gs_ui_probe_scroll_at(gs_ed_panels[p], &now, &max);

                    for (int step = 0; step < 40; step++) {
                        const int sn = gs_walk_controls_ed(&ed, below, GS_UI_MAX_ITEMS);
                        const int sheld = sn < GS_UI_MAX_ITEMS ? sn : GS_UI_MAX_ITEMS;

                        for (int i = 0; i < sheld; i++) {
                            if (!below[i].reachable || below[i].disabled) continue;
                            if (!below[i].visible) continue;
                            if (below[i].label[0] == 0) continue;
                            if (gs_chrome(below[i].label, below[i].window)) continue;

                            const int was = cov.n_offered;
                            gs_walk_mark(cov.offered, below[i].id, &cov.n_offered);
                            if (cov.n_offered != was && named < GS_ED_NAMES) {
                                name_id[named] = below[i].id;
                                SDL_snprintf(name_of[named], GS_UI_LABEL,
                                             "%s | in %s | typable=%d",
                                             below[i].label, below[i].window,
                                             (int)below[i].typable);
                                named++;
                            }
                            if (gs_walk_has(cov.pressed, below[i].id)) continue;

                            const float where = now;
                            e = at;
                            gs_ed_frame(&ed);
                            gs_ed_wind(&ed, gs_ed_panels[p], -1.0f);
                            for (int back = 0; back < 40; back++) {
                                float at_now = 0.0f, at_max = 0.0f;
                                gs_ui_probe_scroll_at(gs_ed_panels[p], &at_now, &at_max);
                                if (at_now >= where - 0.5f) break;
                                gs_ui_probe_wheel(gs_ed_panels[p], -1.0f);
                                gs_ed_frame(&ed);
                            }

                            if (gs_ed_attempt(&ed, &at, &at_track, &below[i],
                                              sheld, &actions)) {
                                moved++;
                                gs_walk_mark(cov.pressed, below[i].id, &cov.n_pressed);
                            }
                        }

                        if (now >= max) break;
                        gs_ui_probe_wheel(gs_ed_panels[p], -1.0f);
                        gs_ed_frame(&ed);
                        gs_ui_probe_scroll_at(gs_ed_panels[p], &now, &max);
                    }
                    e = at;
                    t = at_track;
                    gs_ed_frame(&ed);
                }

                e = at;
                t = at_track;
            }
        }
    }

    printf("  EDITOR %d configurations, %d actions, %d of %d controls pressed, "
           "%d unnamed structure, %d window furniture skipped\n",
           configs, actions, cov.n_pressed, cov.n_offered, unnamed, chrome);

    for (size_t z = 0; z < GS_WALK_CTRLS; z++) {
        const uint32_t id = cov.offered[z];
        if (id == 0) continue;
        size_t at = (size_t)id & (GS_WALK_CTRLS - 1);
        bool hit = false;
        while (cov.pressed[at] != 0) {
            if (cov.pressed[at] == id) { hit = true; break; }
            at = (at + 1) & (GS_WALK_CTRLS - 1);
        }
        if (!hit) {
            const char *label = "(unnamed)";
            for (int q = 0; q < named; q++) {
                if (name_id[q] == id) { label = name_of[q]; break; }
            }
            printf("  EDLEFT '%s'\n", label);
        }
    }

    // **And the count that does not come from this walk either.** The palette
    // names its controls in the file that draws it, and that text does not care
    // what any walk reached - so every label `gs_editor.c` writes down has to
    // have been met here.
    {
        static char wanted[GS_SOURCE_LABELS][GS_UI_LABEL];
        const int n_wanted = gs_labels_in_source(GS_SOURCE_UI "/gs_editor.c",
                                                 wanted, GS_SOURCE_LABELS);
        CHECK(n_wanted > 0);
        CHECK(n_wanted < GS_SOURCE_LABELS);

        int missing = 0;
        for (int i = 0; i < n_wanted; i++) {
            const size_t len = SDL_strlen(wanted[i]);
            bool seen = false;
            for (int q = 0; q < named && !seen; q++) {
                if (SDL_strncmp(name_of[q], wanted[i], len) != 0) continue;
                if (name_of[q][len] == ' ' || name_of[q][len] == '\0') seen = true;
            }
            if (seen) continue;
            printf("  NEVER WALKED '%s', which gs_editor.c draws\n", wanted[i]);
            missing++;
        }
        printf("  EDITOR %d of %d controls named in gs_editor.c were reached\n",
               n_wanted - missing, n_wanted);
        CHECK(missing == 0);
    }

    // **What is counted is what moved.** An activation that lands on the floor
    // is not coverage: wired to count attempts instead, this same test reports
    // a perfect score having changed nothing at all.
    CHECK(moved > 0);
    CHECK(cov.n_offered > 0);
    CHECK(cov.n_pressed == cov.n_offered);

    gs_editor_quit(&e);
}

TEST(the_walk_signs_in_through_the_door_rather_than_being_put_behind_it) {
    // **The front end has one way in and it is not a button.** Everything the
    // walk covers lives behind a password, and every state it has been shown so
    // far it was *placed* in - seeded already signed in, because a walk that can
    // only press things cannot get past a box that wants text. A door nobody
    // can open is a door nobody has tested.
    static gs_menu seed;
    static gs_track t;
    gs_panel_menu(&seed, &t);

    // Signed out, at the door, with nothing filled in - and a driver in the
    // roster whose password the walk knows one of, and one wrong one.
    seed.signed_in = -1;
    seed.screen    = GS_SCREEN_LOGIN;
    seed.login_pick = -1;
    seed.login_password[0] = '\0';
    seed.login_confirm[0]  = '\0';
    seed.login_code[0]     = '\0';
    seed.login_error[0]    = '\0';
    seed.login_making      = false;
    seed.login_wants_code  = false;
    seed.login_setting     = false;

    CHECK(SDL_SetWindowSize(gs_win, 1280, 720));
    CHECK(SDL_SetRenderLogicalPresentation(ren, 1280, 720,
                                           SDL_LOGICAL_PRESENTATION_DISABLED));

    gs_ui ui;
    static gs_walk w;
    SDL_memset(w.slot, 0, sizeof w.slot);
    SDL_memset(w.shape, 0, sizeof w.shape);
    w.capped = 0;
    SDL_memset(w.offered, 0, sizeof w.offered);
    SDL_memset(w.pressed, 0, sizeof w.pressed);
    SDL_memset(w.never, 0, sizeof w.never);
    SDL_memset(w.reached, 0, sizeof w.reached);
    w.n_offered = 0;
    w.n_pressed = 0;
    w.n_never   = 0;
    w.states    = 0;
    w.edges     = 0;
    w.typed     = 0;
    w.deepest   = 0;
    w.did_nothing = 0;
    w.ran_out   = false;

    // Told apart by what has been typed, and finished the moment it is inside:
    // what is being asked here is whether the door opens, not what is behind it.
    w.fine      = true;
    w.words     = (int)SDL_arraysize(gs_walk_words);
    w.per_shape = GS_WALK_PER_SHAPE;
    w.stop_at  = GS_SCREEN_TITLE;
    w.stop_set = true;

    gs_walk_screen(&ui, &seed, &t, ren, GS_SCREEN_LOGIN, &w);

    printf("  DOOR: %d states, %d actions (%d typed), %d deep, "
           "%d of %d pressed, %d capped\n",
           w.states, w.edges, w.typed, w.deepest, w.n_pressed, w.n_offered,
           w.capped);

    CHECK(!w.ran_out);

    // **It got in, and nobody let it in.** No screen was handed to it and no
    // state was set behind the door: it picked a driver, typed a password that
    // happened to be right, and pressed the button.
    CHECK(w.reached[GS_SCREEN_TITLE]);

    // And the wrong password is in the walk too, so getting in is not something
    // that happens to anything typed at it.
    CHECK(w.typed > 0);
}

// The nth control on screen with this label, live and in sight, or zero.
static uint32_t gs_dial_nth(gs_ui *ui, gs_ui_item *items, const char *label,
                            int nth)
{
    const int n = gs_walk_controls(ui, items, GS_UI_MAX_ITEMS);
    const int held = n < GS_UI_MAX_ITEMS ? n : GS_UI_MAX_ITEMS;
    int seen = 0;
    for (int i = 0; i < held; i++) {
        if (!items[i].visible || items[i].disabled || !items[i].reachable) continue;
        if (SDL_strcmp(items[i].label, label) != 0) continue;
        if (seen++ == nth) return items[i].id;
    }
    return 0;
}

static void gs_dial_press(gs_ui *ui, uint32_t id) {
    const gs_act a = { id, -1, 0 };
    gs_act_do(ui, &a);
}

// The mouse, reported the way a backend reports it. The editor reads it through
// ImGui rather than from SDL, so this is the same road a real pointer travels.
static void gs_ed_point(gs_ed *ed, float sx, float sy) {
    ImGuiIO *io = ImGui_GetIO();
    ImGuiIO_AddMousePosEvent(io, sx, sy);
    gs_ed_frame(ed);
}

static void gs_ed_button(gs_ed *ed, bool down) {
    ImGuiIO *io = ImGui_GetIO();
    ImGuiIO_AddMouseButtonEvent(io, ImGuiMouseButton_Left, down);
    gs_ed_frame(ed);
}

// **Put the pointer over a given tile**, by moving it and looking at where the
// editor says it now is.
//
// Nothing here works out the screen position of a tile from the projection: the
// projection throws away a dimension and the answer depends on the height of
// the ground, which is the thing being changed. So the pointer is moved, the
// editor is asked what is under it, and the difference is converted back to
// pixels with the one relation that is exact - the diamond - and applied again.
// Two or three passes on flat ground, a few more on a ramp.
static bool gs_ed_over(gs_ed *ed, float tx, float ty) {
    float sx = (float)ed->view.rect.w * 0.5f;
    float sy = (float)ed->view.rect.h * 0.5f;

    for (int i = 0; i < 32; i++) {
        gs_ed_point(ed, sx, sy);
        if (!ed->e->hover_on) {
            // Off the track, or over a panel: back towards the middle and try
            // again from there.
            sx = (sx + (float)ed->view.rect.w * 0.5f) * 0.5f;
            sy = (sy + (float)ed->view.rect.h * 0.5f) * 0.5f;
            continue;
        }
        const float dx = tx - ed->e->hover_x;
        const float dy = ty - ed->e->hover_y;
        if (SDL_fabsf(dx) < 0.35f && SDL_fabsf(dy) < 0.35f) return true;

        sx += (dx - dy) * (GS_ISO_TILE_W * 0.5f) * ed->e->zoom;
        sy += (dx + dy) * (GS_ISO_TILE_H * 0.5f) * ed->e->zoom;
    }
    return false;
}

// One click: down and up without moving, which is one gate rather than one for
// every frame the button was held.
static bool gs_ed_click(gs_ed *ed, float tx, float ty) {
    if (!gs_ed_over(ed, tx, ty)) return false;
    gs_ed_button(ed, true);
    gs_ed_button(ed, false);
    return true;
}

// One stroke: down on the first tile, dragged across the rest, up at the end -
// which is also one undo step, because that is what the editor makes of a drag.
static bool gs_ed_drag(gs_ed *ed, float x0, float y0, float x1, float y1, int steps) {
    if (!gs_ed_over(ed, x0, y0)) return false;
    gs_ed_button(ed, true);

    for (int i = 1; i <= steps; i++) {
        const float f = (float)i / (float)steps;
        if (!gs_ed_over(ed, x0 + (x1 - x0) * f, y0 + (y1 - y0) * f)) {
            gs_ed_button(ed, false);
            return false;
        }
    }
    gs_ed_button(ed, false);
    return true;
}

// Is this named control on screen at all?
static bool gs_ed_has_named(gs_ed *ed, const char *label) {
    static gs_ui_item items[GS_UI_MAX_ITEMS];
    const int n = gs_walk_controls_ed(ed, items, GS_UI_MAX_ITEMS);
    for (int i = 0; i < n && i < GS_UI_MAX_ITEMS; i++) {
        if (!items[i].visible || items[i].disabled || !items[i].reachable) continue;
        if (SDL_strcmp(items[i].label, label) == 0) return true;
    }
    return false;
}

// Press a named control in the editor's panels.
static bool gs_ed_press_named(gs_ed *ed, const char *label) {
    static gs_ui_item items[GS_UI_MAX_ITEMS];
    const int n = gs_walk_controls_ed(ed, items, GS_UI_MAX_ITEMS);

    uint32_t at = 0;
    for (int i = 0; i < n && i < GS_UI_MAX_ITEMS; i++) {
        if (!items[i].visible || items[i].disabled || !items[i].reachable) continue;
        if (SDL_strcmp(items[i].label, label) == 0) at = items[i].id;
    }
    if (at == 0) return false;

    gs_ui_probe_press(at);
    gs_ed_frame(ed);
    gs_ed_frame(ed);
    return true;
}

TEST(a_track_is_built_named_saved_and_raced_by_pressing_and_dragging) {
    // **The whole loop, done the way a player does it.**
    //
    // There is a test beside this one that builds a track from nothing and
    // races it, and every step of it is a function call: `gs_editor_paint` at
    // this tile, `gs_editor_save`, `gs_world_add_car`. That proves the model
    // holds together and proves nothing at all about the construction set,
    // because no button is pressed and no ground is dragged over. A brush that
    // is unreachable from the palette passes it.
    //
    // This one presses New, chooses each brush by pressing its button, shapes
    // the ground by holding the mouse down and dragging across it, picks ice
    // out of the surface list, sets the gravity dial from a planet, lays a
    // route, keeps the result in the library, types a name for it - and then
    // races what came back out of the library. The only steps not performed by
    // pressing something are the two the client itself performs, and they are
    // named where they happen.
    static gs_menu m;
    static gs_track t;
    gs_panel_menu(&m, &t);

    CHECK(SDL_SetWindowSize(gs_win, 1280, 720));
    CHECK(SDL_SetRenderLogicalPresentation(ren, 1280, 720,
                                           SDL_LOGICAL_PRESENTATION_DISABLED));

    gs_ui ui;
    static gs_ui_item items[GS_UI_MAX_ITEMS];

    // --- **New**, pressed on the screen about tracks. ------------------------
    m.screen = GS_SCREEN_TRACKS;
    gs_ui_probe_settle();
    gs_ui_begin(&ui, &m, &t, ren);
    {
        const uint32_t at = gs_dial_nth(&ui, items, "New", 0);
        CHECK(at != 0);
        gs_dial_press(&ui, at);
    }
    CHECK(m.new_requested);

    // The client's own answer to that button, which is two lines in main.c: a
    // blank field of the size New gives, and the construction set opened on it.
    // Everything after this is done by hand on the screen.
    m.new_requested = false;
    gs_track_init(&t, 48, 40, GS_SURF_PAVEMENT);
    CHECK(gs_track_validate(&t).problem == GS_TRACK_NO_START);

    static gs_editor e;
    CHECK(gs_editor_init(&e, 65536));
    static gs_ed ed;
    ed.e = &e;
    ed.t = &t;
    ed.view = (gs_view){ 0 };
    ed.view.rect = (SDL_Rect){ 0, 0, 1280, 720 };
    ed.input = (gs_input_state){ 0 };
    gs_bind_defaults(&ed.input.bind);
    gs_editor_toggle(&e, &ed.view);
    CHECK(e.active);
    gs_ed_frame(&ed);
    gs_ed_frame(&ed);

    // Whatever ran before this may have folded a panel shut or wound it to the
    // bottom; ImGui remembers both by name for the rest of the process, and a
    // palette scrolled past its own first row has no "raise" button on it.
    if (gs_ui_probe_unfold("Construction set")) gs_ed_frame(&ed);
    if (gs_ui_probe_unfold("Parts box")) gs_ed_frame(&ed);
    gs_ed_wind(&ed, "Construction set", -1.0f);
    gs_ed_wind(&ed, "Parts box", -1.0f);
    gs_ed_frame(&ed);

    // --- **Shape the ground**, with the brush chosen off the palette. --------
    CHECK(gs_ed_press_named(&ed, "raise"));
    CHECK(e.brush == GS_BRUSH_RAISE);

    const uint64_t before_ridge = gs_track_hash(&t);
    CHECK(gs_ed_drag(&ed, 20.0f, 14.0f, 20.0f, 26.0f, 12));
    CHECK(gs_track_hash(&t) != before_ridge);
    CHECK(gs_track_height(&t, GS_INT(20), GS_INT(20)) > 0);

    // A drag is one undo step however many tiles it crossed, and undo is a
    // button on the same panel - pressed here rather than called, because a
    // history nobody can reach is not a history.
    CHECK(gs_ed_press_named(&ed, "undo"));
    CHECK(gs_track_hash(&t) == before_ridge);
    CHECK(gs_ed_press_named(&ed, "redo"));
    CHECK(gs_track_hash(&t) != before_ridge);
    const uint64_t ridge = gs_track_hash(&t);

    // --- **Ice, onto the ridge rather than beside it.** ----------------------
    CHECK(gs_ed_press_named(&ed, "surface"));
    CHECK(e.brush == GS_BRUSH_SURFACE);

    // The surface list is a combo, which ImGui does not name; it is found by
    // opening things until the surfaces turn up, and the surfaces are named.
    //
    //     **The list of grounds has no name.** ImGui reports a label for a
    //     button, a slider and a box and none at all for a combo, so the box
    //     that picks a ground is one of the anonymous items on the panel. Which
    //     one is settled by opening each in turn and seeing which offers ice -
    //     and one of the anonymous ones is the arrow that folds the palette
    //     away, which takes every other control off the screen, so the palette
    //     is put back when that happens.
    {
        static uint32_t nameless[GS_UI_MAX_ITEMS];
        int count = 0;
        const int n = gs_walk_controls_ed(&ed, items, GS_UI_MAX_ITEMS);
        for (int i = 0; i < n && i < GS_UI_MAX_ITEMS; i++) {
            if (!items[i].visible || items[i].disabled || !items[i].reachable) continue;
            if (items[i].label[0] != '\0') continue;
            if (SDL_strcmp(items[i].window, "Construction set") != 0) continue;
            nameless[count++] = items[i].id;
        }
        CHECK(count >= 1);

        bool picked = false;
        for (int i = 0; i < count && !picked; i++) {
            gs_ui_probe_settle();
            gs_ed_frame(&ed);
            gs_ui_probe_press(nameless[i]);
            gs_ed_frame(&ed);
            gs_ed_frame(&ed);

            if (gs_ed_press_named(&ed, gs_surfaces[GS_SURF_ICE].name)) {
                picked = e.surface == (int)GS_SURF_ICE;
                continue;
            }
            if (!gs_ed_has_named(&ed, "raise")) {
                gs_ui_probe_press(nameless[i]);     // fold it back open
                gs_ed_frame(&ed);
                gs_ed_frame(&ed);
            }
        }
        gs_ui_probe_settle();
        gs_ed_frame(&ed);
        CHECK(picked);
        CHECK(e.brush == GS_BRUSH_SURFACE);
        CHECK(gs_ed_has_named(&ed, "raise"));
    }
    CHECK(gs_ed_drag(&ed, 20.0f, 16.0f, 20.0f, 24.0f, 8));
    CHECK(gs_track_surface(&t, GS_INT(20) + GS_ONE / 2,
                           GS_INT(20) + GS_ONE / 2) == GS_SURF_ICE);
    CHECK(gs_track_hash(&t) != ridge);

    // --- **A low-gravity pocket over the jump**, from a planet on the dial. --
    CHECK(gs_ed_press_named(&ed, "Moon"));
    CHECK(SDL_fabsf(e.dial_gravity - gs_to_f(gs_gravity_presets[1].scale)) < 0.001f);
    CHECK(gs_ed_press_named(&ed, "gravity"));
    CHECK(e.brush == GS_BRUSH_GRAVITY);

    // **The brush carries its own gravity and it is not the race dial.** The
    // dial the planets set is what a race runs at; this slider is what the
    // brush paints into the ground, and it starts at Earth - so painting with
    // it untouched would change nothing and prove nothing. Wound down the way a
    // person without a mouse winds a slider.
    {
        static gs_ui_item pal[GS_UI_MAX_ITEMS];
        const int n = gs_walk_controls_ed(&ed, pal, GS_UI_MAX_ITEMS);
        uint32_t at = 0;
        for (int i = 0; i < n && i < GS_UI_MAX_ITEMS; i++) {
            if (!pal[i].visible || pal[i].disabled || !pal[i].reachable) continue;
            if (SDL_strcmp(pal[i].label, "gravity (x)") == 0) at = pal[i].id;
        }
        CHECK(at != 0);
        for (int i = 0; i < 20 && e.gravity > 0.3f; i++) {
            gs_ui_probe_press(at);
            gs_ed_frame(&ed);
            ImGuiIO_AddKeyEvent(ImGui_GetIO(), ImGuiKey_LeftArrow, true);
            gs_ed_frame(&ed);
            ImGuiIO_AddKeyEvent(ImGui_GetIO(), ImGuiKey_LeftArrow, false);
            gs_ed_frame(&ed);
        }
        CHECK(e.gravity < 1.0f);
    }

    CHECK(gs_ed_drag(&ed, 20.0f, 18.0f, 20.0f, 22.0f, 4));
    CHECK(gs_track_gravity(&t, GS_INT(20) + GS_ONE / 2,
                           GS_INT(20) + GS_ONE / 2) < GS_ONE);

    // --- **A route**, which is what turns a field with scenery into a track. -
    CHECK(gs_ed_press_named(&ed, "gate"));
    CHECK(e.brush == GS_BRUSH_GATE);
    CHECK(gs_ed_click(&ed, 10.0f, 20.0f));
    CHECK(gs_ed_click(&ed, 34.0f, 20.0f));
    CHECK(t.gate_count == 2);
    CHECK(gs_track_validate(&t).problem == GS_TRACK_OK);

    const uint64_t built = gs_track_hash(&t);

    // --- **Kept, and named**, on the screen about tracks. --------------------
    gs_editor_toggle(&e, &ed.view);
    CHECK(!e.active);

    m.screen = GS_SCREEN_TRACKS;
    gs_ui_probe_settle();
    gs_ui_begin(&ui, &m, &t, ren);

    // A library with room in it. The one this menu was built with is deliberately
    // full - it is the size the tracks screen is measured at - and a full
    // library refuses in words rather than losing what was built.
    m.library.count = 0;
    m.picked = -1;

    const uint16_t was = m.library.count;
    {
        const uint32_t at = gs_dial_nth(&ui, items, "Keep this one", 0);
        CHECK(at != 0);
        gs_dial_press(&ui, at);
    }
    CHECK(m.library.count == (uint16_t)(was + 1));
    CHECK(m.picked == (int)was);

    // The name box is a box: pressed, then typed into, then committed - which
    // is what a person does and what the walk does to every box it meets.
    {
        const uint32_t at = gs_dial_nth(&ui, items, "##name", 0);
        CHECK(at != 0);
        const gs_act typed = { at, 0, 0 };      // the first word the walk knows
        gs_act_do(&ui, &typed);
    }

    const gs_library_entry *kept = gs_library_at(&m.library, m.picked);
    CHECK(kept != nullptr);
    if (kept == nullptr) { gs_editor_quit(&e); return; }
    CHECK(SDL_strcmp(kept->name, gs_walk_words[0]) == 0);

    // **What is in the library is what was built**, hash for hash, with the ice
    // on the ridge and the low gravity over it.
    CHECK(gs_track_hash(&kept->track) == built);
    CHECK(gs_track_validate(&kept->track).problem == GS_TRACK_OK);

    // --- **And raced**, on the track that came back out of the library. ------
    static gs_track raced;
    raced = kept->track;

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_set_mode(&w, GS_MODE_RACE);
    gs_world_set_laps(&w, 1);

    gs_fix sx, sy;
    gs_angle heading;
    gs_track_grid(&raced, 0, &sx, &sy, &heading);
    gs_world_add_car(&w, &raced, (uint8_t)GS_VEH_SPRINT_CAR, sx, sy, heading);

    for (uint32_t i = 0; i < GS_TICK_HZ * 60u * 10u && !w.over; i++) {
        gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, 0, 0, 0 };
        gs_world_step(&w, &raced, in);
    }

    CHECK(w.over);
    CHECK(w.winner == 0);
    printf("  BUILT a track by hand: %u gates, won on tick %u\n",
           raced.gate_count, w.car[0].finish_tick);

    gs_editor_quit(&e);
}

TEST(every_value_of_every_dial_is_pressed_not_three_interesting_ones) {
    // **Every value of every dial the setup screen offers, pressed on the
    // screen rather than set in the struct behind it - and the range walked is
    // the one the game defines.**
    //
    // `GS_VEH_COUNT` machines, `GS_COLOUR_COUNT` paints, `GS_GRAVITY_PRESETS`
    // planets, the grid's own `GS_MAX_CARS` rows and the roster's own count of
    // drivers. Nothing here lists the values; it counts them out of the model,
    // so a ninth planet or a fifth colour is walked the day it is added and a
    // screen that does not draw it turns this red without anybody adding a
    // case.
    //
    // **The controls are found by what they do, not by what they are called.**
    // Dear ImGui reports a label for a button, a slider and a box, and reports
    // none at all for a combo or a colour swatch: neither `BeginCombo` nor
    // `ColorButton` tells the hook its name, so the machine picking the paint
    // for car three sees sixty-four identical nameless squares. What it can see
    // is what each one *did* - press it and the setup says which car and which
    // colour - so that is how they are told apart. It has the useful property
    // of not caring what any of them is renamed to.
    static gs_menu m;
    static gs_menu base;
    static gs_track t;
    gs_panel_menu(&m, &t);

    // **A loop, because the lap dial is drawn dead on a path** - a path is
    // raced once end to end, so the screen refuses to offer twenty laps of one.
    // A dial that is disabled is not a dial with no values; it is a dial this
    // test would otherwise skip in silence.
    t.route = (uint8_t)GS_ROUTE_CIRCUIT;
    CHECK(gs_track_is_circuit(&t));

    m.setup.players = GS_MAX_CARS;      // every row of the grid drawn
    m.setup.mode    = (uint8_t)GS_MODE_RACE;
    m.screen        = GS_SCREEN_SETUP;

    CHECK(SDL_SetWindowSize(gs_win, 1280, 720));
    CHECK(SDL_SetRenderLogicalPresentation(ren, 1280, 720,
                                           SDL_LOGICAL_PRESENTATION_DISABLED));

    gs_ui ui;
    static gs_ui_item items[GS_UI_MAX_ITEMS];
    static gs_ui_item screen[GS_UI_MAX_ITEMS];
    gs_ui_probe_settle();
    gs_ui_begin(&ui, &m, &t, ren);
    base = m;

    bool mode_seen[2] = { false };
    bool laps_seen[21] = { false };
    bool players_seen[GS_MAX_CARS + 1] = { false };
    bool gravity_seen[GS_GRAVITY_PRESETS] = { false };
    static bool colour_seen[GS_MAX_CARS][GS_COLOUR_COUNT];
    static bool vehicle_seen[GS_MAX_CARS][GS_VEH_COUNT];
    static bool driver_seen[GS_MAX_CARS][GS_PROFILES_MAX + 1];
    SDL_memset(colour_seen, 0, sizeof colour_seen);
    SDL_memset(vehicle_seen, 0, sizeof vehicle_seen);
    SDL_memset(driver_seen, 0, sizeof driver_seen);

    // --- **Every planet.** The simulation's own list, which is also the list
    //     the construction set's palette offers: one table, pressed in both
    //     places. These are buttons and ImGui does name them.
    //
    //     **Started from a different planet every time**, because a button that
    //     is already the one lit changes nothing, and "it did nothing" and "it
    //     was already so" are the same reading. The dial is put somewhere else
    //     first, so the press has work to do.
    for (int g = 0; g < GS_GRAVITY_PRESETS; g++) {
        const int away = (g + 1) % GS_GRAVITY_PRESETS;
        m = base;
        m.setup.gravity_preset = away;
        m.setup.gravity        = gs_gravity_presets[away].scale;
        gs_ui_probe_settle();

        const uint32_t at = gs_dial_nth(&ui, items, gs_gravity_presets[g].name, 0);
        CHECK(at != 0);
        if (at == 0) continue;
        gs_dial_press(&ui, at);
        if (m.setup.gravity == gs_gravity_presets[g].scale &&
            m.setup.gravity_preset == g) {
            gravity_seen[g] = true;
        }
    }

    // --- **Every lap count and every player count.** A slider is not pressed,
    //     it is moved: landed on and stepped with the arrows, which is what a
    //     person without a mouse does and where a person with one ends up.
    m = base;
    gs_ui_probe_settle();
    {
        const uint32_t laps = gs_dial_nth(&ui, items, "##laps", 0);
        CHECK(laps != 0);
        for (int i = 0; i < 40; i++) {
            const gs_act a = { laps, -1, (uint16_t)ImGuiKey_LeftArrow };
            gs_act_do(&ui, &a);
        }
        for (int i = 0; i < 40; i++) {
            if (m.setup.laps <= 20) laps_seen[m.setup.laps] = true;
            if (m.setup.laps >= 20) break;
            const gs_act a = { laps, -1, (uint16_t)ImGuiKey_RightArrow };
            gs_act_do(&ui, &a);
        }
    }

    m = base;
    gs_ui_probe_settle();
    {
        const uint32_t players = gs_dial_nth(&ui, items, "##players", 0);
        CHECK(players != 0);
        for (int i = 0; i < 20; i++) {
            const gs_act a = { players, -1, (uint16_t)ImGuiKey_LeftArrow };
            gs_act_do(&ui, &a);
        }
        for (int i = 0; i < 20; i++) {
            if (m.setup.players <= GS_MAX_CARS) players_seen[m.setup.players] = true;
            if (m.setup.players >= GS_MAX_CARS) break;
            const gs_act a = { players, -1, (uint16_t)ImGuiKey_RightArrow };
            gs_act_do(&ui, &a);
        }
    }

    // --- **Everything else on the screen, pressed to find out what it is.**
    //
    // The nameless ones are the combos and the paint swatches. Each is pressed
    // from the same starting state and judged by what it changed: a swatch says
    // which car and which colour, and a combo says nothing at all but *opens*,
    // and what is inside it is named.
    m = base;
    gs_ui_probe_settle();
    const int on_screen = gs_walk_controls(&ui, screen, GS_UI_MAX_ITEMS);
    const int held = on_screen < GS_UI_MAX_ITEMS ? on_screen : GS_UI_MAX_ITEMS;

    //
    //     **Every one of them is pressed from a state it is not already in**,
    //     for the same reason the planets are: a row already painted red has a
    //     red swatch that does nothing, and a dead swatch does nothing too. So
    //     the grid is put somewhere else first and the press has to move it.
    int combos = 0;
    for (int i = 0; i < held; i++) {
        if (!screen[i].visible || screen[i].disabled || !screen[i].reachable) continue;

        // A swatch says which car and which colour by doing it - tried from two
        // different starting colours, because it might be the one already on.
        bool paint = false;
        for (uint8_t start = 0; start < 2 && !paint; start++) {
            m = base;
            for (int r = 0; r < GS_MAX_CARS; r++) m.setup.colour[r] = start;
            gs_ui_probe_settle();
            gs_dial_press(&ui, screen[i].id);
            for (int r = 0; r < GS_MAX_CARS; r++) {
                if (m.setup.colour[r] == start) continue;
                colour_seen[r][m.setup.colour[r]] = true;
                paint = true;
            }
        }
        if (paint) continue;

        // Or a combo, which changes nothing by opening and carries its values
        // inside it. Which combo it is, is which names turned up.
        m = base;
        gs_ui_probe_settle();
        gs_dial_press(&ui, screen[i].id);

        if (gs_dial_nth(&ui, items, gs_vehicle(0)->name, 0) != 0) {
            combos++;
            for (uint8_t v = 0; v < GS_VEH_COUNT; v++) {
                const uint8_t away = (uint8_t)((v + 1u) % GS_VEH_COUNT);
                m = base;
                for (int r = 0; r < GS_MAX_CARS; r++) m.setup.vehicle[r] = away;
                gs_ui_probe_settle();
                gs_dial_press(&ui, screen[i].id);

                const uint32_t at = gs_dial_nth(&ui, items, gs_vehicle(v)->name, 0);
                CHECK(at != 0);
                if (at == 0) continue;
                gs_dial_press(&ui, at);
                for (int r = 0; r < GS_MAX_CARS; r++) {
                    if (m.setup.vehicle[r] == v) vehicle_seen[r][v] = true;
                }
            }
            continue;
        }

        if (gs_dial_nth(&ui, items, "guest", 0) != 0) {
            combos++;
            const int drivers = (int)base.profiles.count + 1;   // guest included
            for (int k = -1; k < (int)base.profiles.count; k++) {
                const int8_t away = (int8_t)(((k + 1 + 1) % drivers) - 1);
                m = base;
                for (int r = 0; r < GS_MAX_CARS; r++) m.setup.profile[r] = away;
                gs_ui_probe_settle();
                gs_dial_press(&ui, screen[i].id);

                const char *who = k < 0 ? "guest" : base.profiles.entry[k].name;
                const uint32_t at = gs_dial_nth(&ui, items, who, 0);
                CHECK(at != 0);
                if (at == 0) continue;
                gs_dial_press(&ui, at);
                for (int r = 0; r < GS_MAX_CARS; r++) {
                    if (m.setup.profile[r] == (int8_t)k) driver_seen[r][k + 1] = true;
                }
            }
            continue;
        }

        if (gs_dial_nth(&ui, items, "first past the flag", 0) != 0) {
            combos++;
            static const struct { const char *entry; uint8_t mode; } modes[] = {
                { "last one driving",    (uint8_t)GS_MODE_DESTRUCTION },
                { "first past the flag", (uint8_t)GS_MODE_RACE },
            };
            for (size_t k = 0; k < SDL_arraysize(modes); k++) {
                m = base;
                m.setup.mode = (uint8_t)(modes[k].mode == (uint8_t)GS_MODE_RACE
                                         ? GS_MODE_DESTRUCTION : GS_MODE_RACE);
                gs_ui_probe_settle();
                gs_dial_press(&ui, screen[i].id);

                const uint32_t at = gs_dial_nth(&ui, items, modes[k].entry, 0);
                CHECK(at != 0);
                if (at == 0) continue;
                gs_dial_press(&ui, at);
                if (m.setup.mode == modes[k].mode) mode_seen[k] = true;
            }
        }
    }

    // --- **And the same eight planets in the construction set's palette**,
    //     which is the reason they are one list rather than two that agree.
    static gs_editor e;
    CHECK(gs_editor_init(&e, 65536));
    static gs_track et;
    gs_flat_pavement(&et, 32, 32);

    static gs_ed ed;
    ed.e = &e;
    ed.t = &et;
    ed.view = (gs_view){ 0 };
    ed.view.rect = (SDL_Rect){ 0, 0, 1280, 720 };
    ed.input = (gs_input_state){ 0 };
    gs_bind_defaults(&ed.input.bind);
    gs_editor_toggle(&e, &ed.view);
    CHECK(e.active);
    e.show_controls = true;

    bool palette_seen[GS_GRAVITY_PRESETS] = { false };
    for (int g = 0; g < GS_GRAVITY_PRESETS; g++) {
        e.dial_gravity = -1.0f;
        gs_ed_frame(&ed);
        gs_ed_frame(&ed);

        gs_ui_probe_start(items, GS_UI_MAX_ITEMS);
        gs_ui_probe_frame();
        gs_ed_frame(&ed);
        const int n = gs_ui_probe_count();
        gs_ui_probe_stop();

        uint32_t at = 0;
        for (int i = 0; i < n && i < GS_UI_MAX_ITEMS; i++) {
            if (!items[i].visible || items[i].disabled || !items[i].reachable) continue;
            if (SDL_strcmp(items[i].label, gs_gravity_presets[g].name) == 0) {
                at = items[i].id;
            }
        }
        CHECK(at != 0);
        if (at == 0) continue;

        gs_ui_probe_press(at);
        gs_ed_frame(&ed);
        gs_ed_frame(&ed);

        const float want = gs_to_f(gs_gravity_presets[g].scale);
        if (SDL_fabsf(e.dial_gravity - want) < 0.001f) palette_seen[g] = true;
    }
    gs_editor_quit(&e);

    // --- **Counted out, and every one of them met.**
    int met = 0, space = 0;
    #define GS_DIAL_MET(what, ok, at) \
        do { space++; if (ok) met++; \
             else printf("  DIAL MISSED %s %d\n", what, at); } while (0)

    for (int q = 0; q < 2; q++)                  GS_DIAL_MET("mode", mode_seen[q], q);
    for (int q = 1; q <= 20; q++)                GS_DIAL_MET("laps", laps_seen[q], q);
    for (int q = 1; q <= GS_MAX_CARS; q++)       GS_DIAL_MET("players", players_seen[q], q);
    for (int q = 0; q < GS_GRAVITY_PRESETS; q++) GS_DIAL_MET("gravity", gravity_seen[q], q);
    for (int q = 0; q < GS_GRAVITY_PRESETS; q++) GS_DIAL_MET("palette gravity", palette_seen[q], q);
    for (int r = 0; r < GS_MAX_CARS; r++) {
        for (int q = 0; q < GS_COLOUR_COUNT; q++) GS_DIAL_MET("colour", colour_seen[r][q], r * 100 + q);
        for (int q = 0; q < GS_VEH_COUNT; q++)    GS_DIAL_MET("machine", vehicle_seen[r][q], r * 100 + q);
        for (int q = 0; q <= (int)base.profiles.count; q++)
            GS_DIAL_MET("driver", driver_seen[r][q], r * 100 + q);
    }
    #undef GS_DIAL_MET

    printf("  DIALS %d of %d values pressed, over %d combos\n", met, space, combos);
    CHECK(met == space);
    CHECK(combos == 1 + 2 * GS_MAX_CARS);   // mode, and a driver and a machine per row
}

TEST(the_walk_goes_as_deep_as_the_front_end_does) {
    // **Not a map of first moves.** Every state the front end can be got into
    // from each screen, by pressing every control on it, and then every control
    // on everything that led to - until nothing new comes back.
    static gs_menu seed;
    static gs_track t;
    gs_panel_menu(&seed, &t);

    CHECK(SDL_SetWindowSize(gs_win, 1280, 720));
    CHECK(SDL_SetRenderLogicalPresentation(ren, 1280, 720,
                                           SDL_LOGICAL_PRESENTATION_DISABLED));

    gs_ui ui;
    static gs_walk w;
    int states = 0;
    int edges  = 0;
    int alone  = 0;         // what the first seed found on its own

    // **One set of books across every seed.** From any screen you can reach the
    // others, so eight walks with eight sets of notes cover the same graph
    // eight times over - the same 639 controls, seven more times. What each
    // seed is really being asked is whether it can reach anything the ones
    // before it could not, and that question needs the notes kept.
    SDL_memset(w.slot, 0, sizeof w.slot);
    SDL_memset(w.offered, 0, sizeof w.offered);
    SDL_memset(w.pressed, 0, sizeof w.pressed);
    SDL_memset(w.never, 0, sizeof w.never);
    SDL_memset(w.unseen, 0, sizeof w.unseen);
    SDL_memset(w.moved, 0, sizeof w.moved);
    SDL_memset(w.named, 0, sizeof w.named);
    w.n_offered = 0;
    w.n_pressed = 0;
    w.n_never   = 0;
    w.n_unseen  = 0;
    w.n_stranded = 0;
    w.n_moved   = 0;
    w.states    = 0;
    w.typed     = 0;
    // **One state per offering.** Every extra one is the whole path replayed
    // again for every control on it, and what this walk is counting is the
    // controls rather than the values behind them.
    SDL_memset(w.edge, 0, sizeof w.edge);
    w.fine      = false;
    w.words     = 1;
    w.per_shape = 1;
    w.stop_set  = false;
    SDL_memset(w.reached, 0, sizeof w.reached);

    for (size_t sd = 0; sd < SDL_arraysize(gs_seeds); sd++) {
        // The full menu with one thing taken away, rebuilt each time so that
        // what the last seed's walk did to it cannot carry over.
        gs_panel_menu(&seed, &t);
        gs_seeds[sd].set(&seed);

        const int seed_states  = w.states;
        const int seed_pressed = w.n_pressed;
        int seed_edges = 0;

        for (size_t i = 0; i < SDL_arraysize(gs_every_screen); i++) {
            const gs_screen from = gs_every_screen[i];

            w.edges   = 0;
            w.deepest = 0;
            w.did_nothing = 0;
            w.ran_out = false;
            w.seed_at   = (int)sd;
            w.seed_from = from;

            gs_walk_screen(&ui, &seed, &t, ren, from, &w);

            CHECK(!w.ran_out);
            seed_edges += w.edges;
        }

        printf("  SEED %-20s %4d new states, %3d new controls, %5d actions\n",
               gs_seeds[sd].name, w.states - seed_states,
               w.n_pressed - seed_pressed, seed_edges);
        edges += seed_edges;
        if (sd == 0) alone = w.n_offered;
    }
    states = w.states;

    printf("  WALK capped %d states at %d per offering\n", w.capped,
           w.per_shape);
    // **Drawn, and scrolled past every time.** A control below the fold of a
    // panel is submitted, dropped for being off-screen, and unreachable by any
    // key - so pressing things, however thoroughly, never touches it. Each one
    // is gone back to and wound onto the screen with the wheel.
    int out_of_reach = 0, wound = 0, wheeling = 0;
    for (size_t i = 0; i < GS_WALK_CTRLS; i++) {
        const uint32_t id = w.unseen[i];
        if (id == 0 || gs_walk_has(w.offered, id)) continue;

        gs_walk_named *nm = gs_walk_named_of(&w, id);
        if (nm != nullptr && nm->hidden &&
            gs_walk_reach(&ui, &t, ren, &w, nm, &wheeling)) {
            wound++;
            continue;
        }
        if (gs_walk_has(w.offered, id)) continue;   // another sweep found it

        printf("  OUT OF REACH %-22s %s\n",
               nm != nullptr && nm->label[0] != '\0' ? nm->label : "(unnamed)",
               nm != nullptr ? nm->window : "?");
        out_of_reach++;
    }
    printf("  WALK %d controls wound onto the screen with the wheel, in %d "
           "actions; %d still out of reach\n", wound, wheeling, out_of_reach);

    // **And what is inside anything that opens.** Every control that never
    // changed the menu is opened once more where it stood, in case what it does
    // is show something rather than change something.
    {
        static uint32_t quiet[GS_WALK_CTRLS];
        int n_quiet = 0, opening = 0;
        for (size_t i = 0; i < GS_WALK_CTRLS; i++) {
            const uint32_t id = w.offered[i];
            if (id == 0 || gs_walk_has(w.moved, id)) continue;
            quiet[n_quiet++] = id;
        }

        const int had = w.n_offered;
        for (int i = 0; i < n_quiet; i++) {
            gs_walk_named *nm = gs_walk_named_of(&w, quiet[i]);
            if (nm == nullptr || !nm->idled) continue;
            gs_walk_open(&ui, &t, ren, &w, nm, &opening);
        }
        printf("  WALK %d controls opened rather than pressed, %d more controls "
               "found inside them, in %d actions\n",
               n_quiet, w.n_offered - had, opening);
    }

    // **Nothing is laid out off the edge of a panel that cannot scroll.** The
    // tracks screen did exactly that with nothing chosen: the box under THIS
    // ONE was given a height of zero, which in ImGui means every pixel that is
    // left, so the two rows of buttons under it - New and Back among them -
    // were laid out past the bottom of a window that cannot be moved, resized
    // or scrolled. Everything drew correctly and none of it could be pressed.
    printf("  WALK %d controls drawn off a panel that cannot scroll\n",
           w.n_stranded);
    CHECK(w.n_stranded == 0);

    printf("  WALK total: %d states, %d actions (%d typed), "
           "%d of %d pressable controls pressed, %d never pressable, "
           "%d drawn but out of reach\n",
           states, edges, w.typed, w.n_pressed, w.n_offered, w.n_never,
           out_of_reach);

    // **Nothing the front end draws is beyond reach.** A control nobody can get
    // to is not a control that works, and until the walk could scroll it could
    // not tell the difference between a row it had pressed and a row that had
    // already returned unpressed.
    CHECK(out_of_reach == 0);

    // **The claim, asserted rather than believed.** Every control the front end
    // offered in a state where it could be pressed, was pressed. Not a sample
    // of them and not most of them - if one is left, this is red.
    CHECK(w.n_pressed == w.n_offered);

    // **And the number it is out of cannot quietly shrink.** The line above is
    // necessary and nowhere near sufficient, because the denominator is what
    // *this walk* reached: a walk that sees less still reports all of it. A
    // narrower alphabet measured 727 controls where a wider one measured 758,
    // and both said 100%.
    //
    // So the count is pinned, the way the golden replay is pinned. Moving it up
    // is the ordinary result of the front end growing. **Moving it down is a
    // deliberate act and wants a line in PROJECT_STATUS.md saying which
    // controls stopped being reachable and why**, because the alternative is a
    // green tick over a front end nobody is walking any more.
    //
    // The honest fix is a count taken without asking a walk what it found, and
    // that is the last item of Phase 17 rather than this one.
    //
    // Moved from 727 to 750 on 2026-08-26, and deliberately: seeding the walk
    // at every player count found twenty controls no other starting state
    // draws, because the setup screen at four players is a different screen
    // from the one at two rather than the same screen with more rows. Raising
    // it because the front end genuinely grew is the point of it; raising it
    // because the last run happened to measure more is how a tripwire quietly
    // becomes a ratchet.
    //
    // **Down to 663 and back to 749 on 2026-08-26, and the round trip is the
    // point.** Table rows scrolled out of sight were being pressed and counted:
    // a table submits every row it holds and ImGui drops the ones off-screen
    // before the widget runs, so those presses landed on nothing. Taking them
    // out put the number where it honestly stood, at 663. Winding the tables
    // with the wheel then reached them for real and it came back to 749.
    //
    // It came back to 749 rather than 750, which is not a control that went
    // away: dropping eighty-seven presses from every state that had them
    // changes the order the walk explores in, and with one state allowed per
    // offering a different order reaches a different set of states. The
    // seven-hundred-and-fiftieth came back with the setup panel's height, once
    // that stopped being a fixed six hundred pixels at every player count.
    //
    // That is what a floor is for: a tripwire against the number quietly
    // falling, not a claim that two walks visit the same places.
    CHECK(w.n_offered >= 765);

    // **The controls that are dead in one state and live in another.** This is
    // the number the seeding is for, and it is what a walk from a single
    // starting state cannot reach by any amount of pressing: the front end
    // draws six of its controls disabled under conditions, and until the walk
    // was started from a menu where those conditions differ, their destinations
    // were not in the map at all.
    // -----------------------------------------------------------------------
    // **What the walk proves, said as properties of the front end.**
    //
    // Everything above is about the walk - how much of it was pressed, how far
    // it got. These are about the game: each is a thing a player would notice
    // going wrong, and each would be worth stating even if somebody had
    // established it another way.
    //
    // Only screens the walk actually stood on are asked about. One it merely
    // arrived at has no outgoing edges because nobody looked, and asking about
    // it would be asking about the walk again.
    // -----------------------------------------------------------------------

    // **No screen is a trap.** At least one thing on it leads somewhere else.
    //
    // The sign-in door is the one exception and it is not skipped: leaving it
    // means signing in, which wants a name and a password typed correctly, and
    // this walk types one word into every box it meets. That the door opens is
    // proved next door, by a walk carrying the vocabulary for it - see
    // the_walk_signs_in_through_the_door_rather_than_being_put_behind_it.
    int traps = 0;
    for (size_t i = 0; i < SDL_arraysize(gs_every_screen); i++) {
        const gs_screen from = gs_every_screen[i];
        if (!w.reached[from] || from == GS_SCREEN_LOGIN) continue;

        int ways_off = 0;
        for (int to = 0; to < GS_SCREEN_COUNT; to++) {
            if (w.edge[from][to]) ways_off++;
        }
        if (ways_off == 0) {
            printf("  TRAP screen %d has nothing on it that leaves\n", (int)from);
            traps++;
        }
    }
    printf("  WALK %d traps\n", traps);
    CHECK(traps == 0);

    // **The title is reachable from everywhere.** A screen you can leave and
    // cannot get home from strands a player just as thoroughly as a trap.
    int stranded = 0;
    for (size_t i = 0; i < SDL_arraysize(gs_every_screen); i++) {
        const gs_screen from = gs_every_screen[i];
        if (!w.reached[from] || from == GS_SCREEN_LOGIN) continue;

        bool seen[GS_SCREEN_COUNT] = { false };
        gs_screen queue[GS_SCREEN_COUNT];
        int head = 0, tail = 0;
        queue[tail++] = from;
        seen[from] = true;

        bool home = from == GS_SCREEN_TITLE;
        while (head < tail && !home) {
            const gs_screen at = queue[head++];
            for (int to = 0; to < GS_SCREEN_COUNT && !home; to++) {
                if (!w.edge[at][to] || seen[to]) continue;
                if (to == GS_SCREEN_TITLE) { home = true; break; }
                seen[to] = true;
                queue[tail++] = (gs_screen)to;
            }
        }
        if (!home) {
            printf("  STRANDED screen %d cannot get home\n", (int)from);
            stranded++;
        }
    }
    printf("  WALK %d stranded\n", stranded);
    CHECK(stranded == 0);

    // **And everywhere is reachable from the title**, which is the other
    // direction and a different claim: a screen nobody can get *to* is as
    // broken as one nobody can leave, and only this half catches it.
    //
    // Two are named rather than counted. The results screen is arrived at by
    // finishing a race and no button leads to it, which is right; and the
    // sign-in door is arrived at by signing out, which this walk cannot undo.
    {
        bool seen[GS_SCREEN_COUNT] = { false };
        gs_screen queue[GS_SCREEN_COUNT];
        int head = 0, tail = 0;
        queue[tail++] = GS_SCREEN_TITLE;
        seen[GS_SCREEN_TITLE] = true;

        while (head < tail) {
            const gs_screen at = queue[head++];
            for (int to = 0; to < GS_SCREEN_COUNT; to++) {
                if (!w.edge[at][to] || seen[to]) continue;
                seen[to] = true;
                queue[tail++] = (gs_screen)to;
            }
        }

        int unreachable = 0;
        for (size_t i = 0; i < SDL_arraysize(gs_every_screen); i++) {
            const gs_screen to = gs_every_screen[i];
            if (!w.reached[to]) continue;
            if (to == GS_SCREEN_RESULTS || to == GS_SCREEN_LOGIN) continue;
            if (seen[to]) continue;
            printf("  UNREACHABLE screen %d cannot be got to from the title\n",
                   (int)to);
            unreachable++;
        }
        printf("  WALK %d unreachable from the title\n", unreachable);
        CHECK(unreachable == 0);
    }

    // **No control does nothing everywhere.**
    //
    // A control that never changed anything, in any state it was pressed in, is
    // either dead code or a button that lies about being one. Doing nothing
    // *sometimes* is ordinary and is not what this asks: a Back that is already
    // back, a row already picked, a preset already chosen.
    //
    // Four things come back from the probe looking exactly like controls and
    // are not, and each is told apart by what ImGui itself says rather than by
    // a list of names here that would go stale the day a column is renamed.
    // They are counted, not dropped: an exclusion that starts swallowing real
    // controls shows up as a number that moved.
    // **The one control that does nothing and is right to.** The share code is
    // a box you copy a track out of, drawn read-only - so nothing anybody does
    // to it can change anything, and the button that does the copying is beside
    // it. ImGui does not report read-only as an item flag when it arrives as an
    // input-text flag, so this one is named. Named, and *asserted used*: an
    // exemption nobody needs any more turns the tree red rather than sitting
    // there covering for whatever becomes inert next.
    static const struct {
        const char *label;
        const char *window;         // matched by prefix: the child id moves
        const char *why;
        bool        found;
    } excused_start[] = {
        { "##code", "Tracks/detail",
          "read-only: a box to copy the track out of", false },
    };

    // **And one excused by the state it was found in rather than by its name.**
    //
    // A row that is the only entry in the library, and is the entry chosen.
    // Nothing on that screen can un-choose it: there is no other row to pick,
    // and keeping the loaded track again folds into the entry that is already
    // there, because a track is known by what it is. A list of one, already
    // selected, is the ordinary case the rule is written to allow - and this is
    // asserted as a condition, so the same row going quiet in a library with
    // thirty-two entries in it is not excused by anything.
    int excused_alone = 0;
    static bool excused_used[SDL_arraysize(excused_start)];
    SDL_memset(excused_used, 0, sizeof excused_used);

    {
        int inert = 0, controls = 0, chrome = 0, unnamed = 0;
        int headings = 0, clipped = 0, woken = 0, tries = 0;
        for (size_t i = 0; i < GS_WALK_CTRLS; i++) {
            const uint32_t id = w.offered[i];
            if (id == 0) continue;
            gs_walk_named *nm = gs_walk_named_of(&w, id);

            if (nm == nullptr || nm->label[0] == '\0')          { unnamed++; continue; }
            if (gs_chrome(nm->label, nm->window))               { chrome++; continue; }
            if (nm->heading)                                    { headings++; continue; }
            if (!nm->visible)                                   { clipped++; continue; }

            controls++;
            if (gs_walk_has(w.moved, id)) continue;

            // It did nothing everywhere the walk stood. That is not an answer
            // yet - go back and try it properly.
            const char *how = "";
            static gs_menu stood;
            if (nm->idled &&
                gs_walk_retry(&ui, &t, ren, nm, &tries, &how, &stood)) {
                printf("  WOKE  %-24s %-26s by %s\n", nm->label, nm->window, how);
                woken++;
                continue;
            }

            if (nm->idled && SDL_strncmp(nm->window, "Tracks/library", 14) == 0 &&
                stood.library.count == 1 && stood.picked == 0) {
                printf("  ALONE %-24s %-26s the only track there is, and the "
                       "one chosen\n", nm->label, nm->window);
                excused_alone++;
                continue;
            }

            bool excused = false;
            for (size_t k = 0; k < SDL_arraysize(excused_start); k++) {
                if (SDL_strcmp(nm->label, excused_start[k].label) != 0) continue;
                if (SDL_strncmp(nm->window, excused_start[k].window,
                                SDL_strlen(excused_start[k].window)) != 0) continue;
                printf("  EXCUSED %-22s %-26s %s\n",
                       nm->label, nm->window, excused_start[k].why);
                excused_used[k] = true;
                excused = true;
                break;
            }
            if (excused) continue;

            printf("  INERT %-24s %-26s %d presses\n",
                   nm->label, nm->window, nm->presses);
            inert++;
        }
        printf("  WALK %d of %d controls did nothing every time; %d unnamed "
               "structure, %d window furniture, %d column headings, %d clipped "
               "out of sight\n",
               inert, controls, unnamed, chrome, headings, clipped);
        printf("  WALK %d woken by being retried properly, in %d actions; "
               "%d alone in their list\n", woken, tries, excused_alone);

        // **No control does nothing everywhere**, save the one named above.
        CHECK(inert == 0);

        // And every control that had to be woken was woken by the retry rather
        // than by luck: nine of them here, and a front end where none of them
        // needs waking is a front end with no radio buttons and no sliders in
        // it, which would want looking at.
        CHECK(woken > 0);

        for (size_t k = 0; k < SDL_arraysize(excused_start); k++) {
            if (!excused_used[k]) {
                printf("  STALE EXCUSE '%s' does something now, or has gone\n",
                       excused_start[k].label);
            }
            CHECK(excused_used[k]);
        }
    }

    // **The denominator, taken from the screens rather than from the walk.**
    {
        static char wanted[GS_SOURCE_LABELS][GS_UI_LABEL];
        const int n_wanted = gs_labels_in_source(GS_SOURCE_UI "/gs_menu.c",
                                                 wanted, GS_SOURCE_LABELS);
        CHECK(n_wanted > 0);
        CHECK(n_wanted < GS_SOURCE_LABELS);      // or the list was truncated

        int missing = 0;
        for (int i = 0; i < n_wanted; i++) {
            bool seen = false;
            for (size_t k = 0; k < GS_WALK_CTRLS && !seen; k++) {
                if (w.named[k].id == 0) continue;
                if (SDL_strcmp(w.named[k].label, wanted[i]) == 0) seen = true;
            }
            if (seen) continue;
            printf("  NEVER WALKED '%s', which gs_menu.c draws\n", wanted[i]);
            missing++;
        }

        printf("  WALK %d of %d controls named in gs_menu.c were reached\n",
               n_wanted - missing, n_wanted);
        CHECK(missing == 0);
    }

    const int revived = gs_walk_revived(&w);
    printf("  WALK %d controls dead in one state and pressed in another; "
           "%d of %d found only by seeding\n",
           revived, w.n_offered - alone, w.n_offered);
    CHECK(revived > 0);

    // **Seeding reached controls that no amount of pressing from one starting
    // state could.** This is the claim, and it is the one worth asserting: the
    // walk from the full menu presses everything the full menu offers, and the
    // front end still has controls it does not draw live until something is
    // *absent*. If this ever comes back zero, either the seeds have stopped
    // differing from each other or the front end has stopped having conditions
    // on its buttons - and both of those want looking at rather than passing.
    CHECK(w.n_offered > alone);

    // **It went further than one press.** The map above found the first move
    // out of eight screens; anything at all beyond that is more than it had.
    CHECK(states > (int)SDL_arraysize(gs_every_screen));
    CHECK(edges > 0);
}

TEST(every_control_is_known_by_name_and_answers_to_it) {
    // **The end of counting Tabs.** The walk above knows a control as "the
    // fifth thing on this screen", which stops being true the day somebody
    // inserts a fourth, and it can only find the controls the keyboard visits -
    // so a control drawn dead in the one state the walk uses is a control the
    // walk has never heard of. Neither is something to claim coverage from.
    //
    // Dear ImGui reports every item it adds, and can be told to press one by
    // id. So a screen can be asked what is on it, and each of those pressed by
    // name - one frame each rather than one frame per Tab on the way there.
    static gs_menu fresh;
    static gs_track t;
    gs_panel_menu(&fresh, &t);

    CHECK(SDL_SetWindowSize(gs_win, 1280, 720));
    CHECK(SDL_SetRenderLogicalPresentation(ren, 1280, 720,
                                           SDL_LOGICAL_PRESENTATION_DISABLED));

    gs_ui ui;
    static gs_ui_item items[GS_UI_MAX_ITEMS];
    static gs_ui_item again[GS_UI_MAX_ITEMS];

    for (size_t i = 0; i < SDL_arraysize(gs_every_screen); i++) {
        gs_screen from = gs_every_screen[i];

        int n = gs_ui_controls(&ui, &fresh, &t, ren, from, items,
                               GS_UI_MAX_ITEMS);
        CHECK(n > 0);

        // **Room for all of them, or the count is a lie.** A screen with more
        // controls than there was space for is not a screen half covered, it is
        // a coverage number that quietly means something else.
        CHECK(n <= GS_UI_MAX_ITEMS);
        int held = n < GS_UI_MAX_ITEMS ? n : GS_UI_MAX_ITEMS;

        // **Nothing is nameless and nothing is known by its position.** The
        // id is the identity - ImGui's hash of how the control was made, so it
        // survives a redraw and survives an insertion above it. The label is
        // for the person reading the map, and ImGui gives one for every widget
        // a person presses; the items it does not name are the structural ones
        // it adds around them, and those are placed by their window instead.
        for (int k = 0; k < held; k++) {
            CHECK(items[k].id != 0);
            CHECK(items[k].window[0] != 0);
        }

        // **Drawn twice, the same screen names the same controls.** An id that
        // moved between redraws would make a map keyed on it worthless.
        int n2 = gs_ui_controls(&ui, &fresh, &t, ren, from, again,
                                GS_UI_MAX_ITEMS);
        CHECK(n2 == n);
        for (int k = 0; k < held && k < n2; k++) {
            CHECK(again[k].id == items[k].id);
            CHECK(SDL_strcmp(again[k].label, items[k].label) == 0);
        }


        // **Pressed by name, a control does what it did when tabbed to.** The
        // set found by name is allowed to be larger - that is the point, it can
        // reach what the keyboard cannot - but it may not be missing anything
        // the old walk found, or naming controls has lost something.
        bool by_tab[GS_SCREEN_COUNT];
        bool by_name[GS_SCREEN_COUNT];
        gs_ui_exits(&ui, &fresh, &t, ren, from, by_tab);
        gs_ui_exits_by_name(&ui, &fresh, &t, ren, from, items, held, by_name);

        for (int to = 0; to < GS_SCREEN_COUNT; to++) {
            if (by_tab[to]) CHECK(by_name[to]);
        }
    }
}

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

    // **Every screen, from every state the walk is seeded in.** A panel's
    // height is worked out from what is on it - how many tracks, whether a
    // track is chosen, how many people are racing - so measuring it in one
    // state measures one of the sizes it comes in.
    //
    // This is not hypothetical. With nothing chosen, the box under THIS ONE was
    // given a height of zero, which in ImGui means every pixel that is left
    // rather than none of them: the box swallowed the space under it and the
    // two rows of buttons went below the fold of a panel that is sized to need
    // no scrolling. Measured from the one state where a track *is* chosen, it
    // looked perfect.
    int measured = 0;
    for (size_t sd = 0; sd < SDL_arraysize(gs_seeds); sd++) {
        gs_panel_menu(&m, &t);
        gs_seeds[sd].set(&m);

        for (size_t i = 0; i < SDL_arraysize(gs_every_screen); i++) {
            gs_panel_report p = gs_panel_of(ren, &m, &t, gs_every_screen[i]);
            // The measurement itself is worth checking: a zero-sized panel
            // would pass everything below without a screen having been drawn at
            // all.
            CHECK(p.w > 100.0f);
            CHECK(p.h > 100.0f);
            CHECK(p.view_w >= 1280.0f);

            CHECK(p.x >= 0.0f);
            CHECK(p.y >= 0.0f);
            CHECK(p.x + p.w <= p.view_w + 1.0f);
            CHECK(p.y + p.h <= p.view_h + 1.0f);

            // And at the size the game opens at, nothing is below the fold: a
            // button half outside the bottom of its own panel is the other half
            // of this fault, and it is what a screen grown one control at a
            // time eventually does.
            if (p.hidden != 0.0f) {
                printf("  BELOW THE FOLD %s from '%s': %.0f hidden\n",
                       gs_screen_name(gs_every_screen[i]), gs_seeds[sd].name,
                       (double)p.hidden);
            }
            CHECK(p.hidden == 0.0f);
            measured++;
        }
    }
    printf("  PANELS %d screens measured, over %d starting states\n",
           measured, (int)SDL_arraysize(gs_seeds));

    gs_panel_menu(&m, &t);

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

TEST(the_window_has_an_icon_of_its_own) {
    (void)ren;

    // **An untitled window with the toolkit's default icon is what an
    // unfinished thing looks like**, and it is the first thing anybody sees of
    // the game. The icon is generated rather than drawn - tools/make_icon.py -
    // so what is checked here is that the file shipped, that it decodes, and
    // that it is actually a picture rather than a square of nothing.
    char path[1024];
    gs_asset_path(path, sizeof path, "icon.png");

    SDL_Surface *icon = IMG_Load(path);
    CHECK(icon != nullptr);
    if (icon == nullptr) return;

    // Square, and big enough that a desktop scaling it down for a title bar has
    // something to work with.
    CHECK(icon->w == icon->h);
    CHECK(icon->w >= 64);

    SDL_Surface *rgba = SDL_ConvertSurface(icon, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(icon);
    CHECK(rgba != nullptr);
    if (rgba == nullptr) return;

    // Something is drawn, it is not one flat colour, and it has a transparent
    // surround rather than being a full square - which is what an icon needs to
    // sit on a title bar of any colour.
    int opaque = 0, clear = 0, reddish = 0;
    const uint8_t *px = (const uint8_t *)rgba->pixels;
    for (int y = 0; y < rgba->h; y++) {
        for (int x = 0; x < rgba->w; x++) {
            const uint8_t *p = px + (size_t)y * (size_t)rgba->pitch + (size_t)x * 4;
            if (p[3] > 200) {
                opaque++;
                // The knob, in the same red the cars are painted.
                if (p[0] > 140 && p[0] > 2 * p[1] && p[0] > 2 * p[2]) reddish++;
            }
            if (p[3] < 20) clear++;
        }
    }

    int total = rgba->w * rgba->h;
    CHECK(opaque > total / 20);      // there is a picture
    CHECK(clear > total / 5);        // and it is not a solid square
    CHECK(reddish > total / 200);    // and the gear knob is on it

    SDL_DestroySurface(rgba);
}

TEST(a_lobby_nobody_answers_says_so_rather_than_knocking_forever) {
    (void)ren;

    // **A wrong key looks exactly like a slow connection.** A server that
    // refuses sends a reason and it arrives as `lobby_error`; a server that
    // cannot decrypt what we sent has nothing to reply to at all, so the screen
    // stayed on "Knocking..." for as long as somebody was willing to look at
    // it. A player sat there twice with no way to tell which was happening -
    // and the second time the key really was wrong.
    static gs_menu m;
    gs_menu_init(&m);

    static gs_lobby lobby;
    lobby = (gs_lobby){ 0 };

    // Just knocking. Not a failure yet: a handshake is allowed to take a moment.
    m.lobby = &lobby;
    m.knocking_for = 0.5f;
    CHECK(!gs_menu_lobby_unanswered(&m));

    m.knocking_for = GS_KNOCK_PATIENCE - 0.1f;
    CHECK(!gs_menu_lobby_unanswered(&m));

    // Long enough. Now it is worth saying.
    m.knocking_for = GS_KNOCK_PATIENCE + 0.1f;
    CHECK(gs_menu_lobby_unanswered(&m));

    // A lobby that arrived, however late, is a lobby that answered.
    lobby.capacity = 2;
    lobby.count = 1;
    CHECK(!gs_menu_lobby_unanswered(&m));

    // And a server that refused said why, which is a better message than this
    // one - so this one stays out of the way.
    lobby = (gs_lobby){ 0 };
    m.lobby_error = "the server is full";
    CHECK(!gs_menu_lobby_unanswered(&m));

    m.lobby_error = nullptr;
    CHECK(gs_menu_lobby_unanswered(&m));
}

TEST(the_lobby_offers_a_race_only_when_it_could_start_one) {
    (void)ren;

    // **A button that does nothing is worse than no button.** The first go at
    // the lobby's Race control asked whether the player count had reached the
    // capacity - and before the server has answered, both are zero, so `0 >= 0`
    // offered Race to somebody still knocking on the door and nothing happened
    // when they pressed it. It read through the lobby pointer without checking
    // it was there, too.
    //
    // Every state this screen can be in, and whether starting a race is a thing
    // the client could actually do in it.
    static gs_menu m;
    gs_menu_init(&m);

    // Nothing at all: no lobby yet.
    m.lobby = nullptr;
    m.lobby_ready = true;
    m.track_progress = 1.0f;
    CHECK(!gs_menu_lobby_can_race(&m));

    // Knocking: a lobby struct, but the server has not said how big it is.
    static gs_lobby lobby;
    lobby = (gs_lobby){ 0 };
    m.lobby = &lobby;
    CHECK(!gs_menu_lobby_can_race(&m));

    // Heard, and waiting for somebody else.
    lobby.capacity = 2;
    lobby.count = 1;
    m.lobby_ready = false;
    CHECK(!gs_menu_lobby_can_race(&m));

    // Everybody here, but the ground is still arriving.
    lobby.count = 2;
    m.lobby_ready = true;
    m.track_progress = 0.5f;
    CHECK(!gs_menu_lobby_can_race(&m));

    // Everybody here and the track landed: now it can.
    m.track_progress = 1.0f;
    CHECK(gs_menu_lobby_can_race(&m));

    // A one-player lobby is a full lobby, which is the case a player testing
    // alone is in and the one the fault was seen on.
    lobby.capacity = 1;
    lobby.count = 1;
    CHECK(gs_menu_lobby_can_race(&m));
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
    run_the_window_has_an_icon_of_its_own(ren);
    run_a_lobby_nobody_answers_says_so_rather_than_knocking_forever(ren);
    run_the_lobby_offers_a_race_only_when_it_could_start_one(ren);
    run_exit_is_something_the_menu_asks_for_rather_than_does(ren);
    run_the_heatmap_puts_the_line_everybody_drove_on_the_screen(ren);
    run_the_landing_arc_is_off_until_it_is_asked_for(ren);
    run_there_is_no_arc_drawn_for_a_car_on_the_ground(ren);
    run_a_wreck_is_drawn_as_wide_as_the_obstacle_it_actually_is(ren);
    run_no_two_grounds_are_drawn_the_same_colour(ren);
    run_the_hud_says_what_lap_it_is_and_changes_when_the_lap_does(ren);
    run_the_hud_says_what_place_you_are_in_and_changes_when_you_are_passed(ren);
    run_the_analyser_refuses_a_track_with_no_route_rather_than_guessing(ren);
    run_every_screen_has_a_way_off_it_and_the_ways_lead_somewhere_real(ren);
    run_a_menu_knows_a_state_it_has_already_been_in(ren);
    run_a_track_is_built_from_nothing_and_raced_without_leaving_the_editor(ren);
    run_one_brush_never_undoes_what_another_one_did(ren);
    run_every_brush_and_every_option_it_carries_does_what_it_says(ren);
    run_every_control_in_the_construction_set_is_pressed(ren);
    run_the_walk_signs_in_through_the_door_rather_than_being_put_behind_it(ren);
    run_a_track_is_built_named_saved_and_raced_by_pressing_and_dragging(ren);
    run_every_value_of_every_dial_is_pressed_not_three_interesting_ones(ren);
    run_the_walk_goes_as_deep_as_the_front_end_does(ren);
    run_every_control_is_known_by_name_and_answers_to_it(ren);
    run_no_screen_is_drawn_bigger_than_the_window_it_is_in(ren);
    run_a_store_with_tracks_in_it_is_saved_whole(ren);
    run_the_condition_bar_stays_inside_the_hud(ren);
    run_there_is_always_a_way_back_out_of_wherever_you_are(ren);
    run_the_hud_fits_what_is_in_it_in_every_state_it_has(ren);
    run_the_light_tree_counts_down_and_then_goes_green(ren);
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

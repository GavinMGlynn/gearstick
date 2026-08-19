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
#include "ui/gs_editor.h"

#define GS_W 640
#define GS_H 480

static int gs_failures = 0;
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
    // The car sits at depth 16; the wall in front of it at depth 20 and five
    // tiles tall. If the painter's sweep is drawing cars after the terrain
    // instead of among it, the car shows through the wall.
    static gs_track open_ground, walled;
    gs_flat_pavement(&open_ground, 24, 24);
    gs_flat_pavement(&walled, 24, 24);

    for (uint8_t y = 0; y <= walled.h; y++) {
        for (uint8_t x = 0; x <= walled.w; x++) {
            int d = (int)x + (int)y;
            if (d >= 20 && d <= 22) gs_track_set_corner(&walled, x, y, GS_INT(5));
        }
    }

    gs_world w;
    gs_park_car(&w, &open_ground, GS_INT(8), GS_INT(8));
    gs_camera cam = gs_camera_on(9.0f, 9.0f, 0.0f);

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
        gs_render_track_camera(&v, &w, &w, 1.0f);

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
        gs_render_track_camera(&v, &w, &w, 1.0f);

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
        gs_render_track_camera(&v, &w, &w, 1.0f);

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
    // four-up should not cost four times one-up. Measured rather than assumed,
    // because "it should be fine" is how frame rates go.
    SDL_Rect quarters[GS_MAX_CARS];
    gs_render_layout(4, GS_W, GS_H, quarters);

    uint64_t one_start = SDL_GetTicksNS();
    for (int pass = 0; pass < 20; pass++) {
        gs_view v = { 0 };
        v.car = 0;
        v.cam.zoom = GS_ISO_DEFAULT_ZOOM;
        v.rect = (SDL_Rect){ 0, 0, GS_W, GS_H };
        gs_render_track_camera(&v, &w, &w, 1.0f);
        gs_render_view(ren, &t, &w, &w, 1.0f, &v);
    }
    uint64_t one_ns = SDL_GetTicksNS() - one_start;

    uint64_t four_start = SDL_GetTicksNS();
    for (int pass = 0; pass < 20; pass++) {
        for (uint8_t i = 0; i < 4; i++) {
            gs_view v = { 0 };
            v.car = i;
            v.cam.zoom = GS_ISO_DEFAULT_ZOOM;
            v.rect = quarters[i];
            gs_render_track_camera(&v, &w, &w, 1.0f);
            gs_render_view(ren, &t, &w, &w, 1.0f, &v);
        }
    }
    uint64_t four_ns = SDL_GetTicksNS() - four_start;

    // Generous, because this is a software renderer on a shared machine. What
    // it rules out is four-up costing four times a full-window view, which is
    // what a naive "render the whole world four times" would.
    CHECK(four_ns < one_ns * 3);
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
    for (int i = 0; i < 120; i++) gs_split_update(&sp, &w, GS_W, GS_H, dt);
    gs_view v[GS_MAX_CARS];
    CHECK(sp.merge == 1.0f);
    CHECK(gs_split_views(&sp, &w, GS_W, GS_H, v) == 1);

    // Drive them apart: two.
    w.car[1].x = GS_INT(30) + GS_INT(30);
    for (int i = 0; i < 120; i++) gs_split_update(&sp, &w, GS_W, GS_H, dt);
    CHECK(sp.merge == 0.0f);
    CHECK(gs_split_views(&sp, &w, GS_W, GS_H, v) == 2);

    // Back together: one again.
    w.car[1].x = GS_INT(32);
    for (int i = 0; i < 120; i++) gs_split_update(&sp, &w, GS_W, GS_H, dt);
    CHECK(gs_split_views(&sp, &w, GS_W, GS_H, v) == 1);

    // A single car is always one view, whatever it does.
    gs_world solo;
    gs_world_init(&solo, GS_ONE);
    gs_world_add_car(&solo, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(5), GS_INT(5), 0);
    gs_split_init(&sp);
    gs_split_update(&sp, &solo, GS_W, GS_H, dt);
    CHECK(gs_split_views(&sp, &solo, GS_W, GS_H, v) == 1);
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
        gs_split_update(&sp, &w, GS_W, GS_H, dt);

        gs_view v[GS_MAX_CARS];
        uint8_t n = gs_split_views(&sp, &w, GS_W, GS_H, v);
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

        gs_split_update(&sp, &w, GS_W, GS_H, dt);
        gs_view v[GS_MAX_CARS];
        gs_split_views(&sp, &w, GS_W, GS_H, v);

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

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
#include "gs_sandbox.h"

#include <stdlib.h>   // abs, for comparing two colours
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

// **Of the car that was there, how much is still there.** Counting car-coloured
// pixels in each frame separately is not the same question and gets the wrong
// answer twice: a mine is drawn orange, which passes for car red, so a mark can
// *add* to the count while covering the car underneath. This asks only about
// the pixels the clean frame says are car.
static int gs_car_pixels_lost(const gs_frame *with, const gs_frame *without) {
    int lost = 0;
    for (int i = 0; i < GS_W * GS_H; i++) {
        if (gs_is_car0(&without->px[i * 4]) && !gs_is_car0(&with->px[i * 4])) lost++;
    }
    return lost;
}

// Every kind of paint a car can end up standing on. Named here rather than
// counted at the call site, so adding one to the renderer and not to this list
// is a compile-time hole rather than a silent one.
typedef enum {
    GS_MARK_ROUTE = 0,     // the dashed line that says which way round
    GS_MARK_START_LINE,    // the plain white line a sprint begins at
    GS_MARK_FINISH_LINE,   // the chequer at the end of one
    GS_MARK_ARROW,         // the way through a gate
    GS_MARK_OIL,           // and the four things a player drops
    GS_MARK_MINE,
    GS_MARK_SMOKE,
    GS_MARK_FLAME,
    GS_MARK_COUNT
} gs_mark_kind;

static const char *gs_mark_name(gs_mark_kind m) {
    switch (m) {
    case GS_MARK_ROUTE:       return "route dash";
    case GS_MARK_START_LINE:  return "start line";
    case GS_MARK_FINISH_LINE: return "finish line";
    case GS_MARK_ARROW:       return "gate arrow";
    case GS_MARK_OIL:         return "oil";
    case GS_MARK_MINE:        return "mine";
    case GS_MARK_SMOKE:       return "smoke";
    case GS_MARK_FLAME:       return "flame";
    case GS_MARK_COUNT:       break;
    }
    return "?";
}

static gs_hazard_kind gs_mark_hazard(gs_mark_kind m) {
    switch (m) {
    case GS_MARK_OIL:   return GS_HAZ_OIL;
    case GS_MARK_MINE:  return GS_HAZ_MINE;
    case GS_MARK_SMOKE: return GS_HAZ_SMOKE;
    case GS_MARK_FLAME: return GS_HAZ_FLAME;
    default:            return GS_HAZ_NONE;
    }
}

// One frame drawn from a view as given, rather than from a camera with a fresh
// view built round it - which is what gs_render_frame does, and would drop
// anything the test had set on the view itself.
static gs_frame gs_frame_of_view(SDL_Renderer *ren, const gs_track *t,
                                 const gs_world *w, const gs_view *v) {
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    gs_render_view(ren, t, w, w, 1.0f, v);

    gs_frame f = { 0 };
    SDL_Surface *raw = SDL_RenderReadPixels(ren, nullptr);
    if (raw == nullptr) return f;
    f.own = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(raw);
    if (f.own != nullptr) f.px = (uint8_t *)f.own->pixels;
    return f;
}

// Defined with the HUD tests further down; used here because a missed
// checkpoint is a thing the HUD has to say as well as a thing drawn on the
// ground.
static void gs_hud_frame(SDL_Renderer *ren, const gs_track *t, const gs_world *w,
                         const gs_view *v, gs_frame *out);
static int gs_hud_pixels_differing(const gs_frame *a, const gs_frame *b);
static void gs_imgui_start(SDL_Window *win, SDL_Renderer *ren);

// The warning arrow's colour, which nothing else in these scenes uses.
static int gs_count_warning(const gs_frame *f) {
    int n = 0;
    for (int i = 0; i < GS_W * GS_H; i++) {
        const uint8_t *p = &f->px[i * 4];
        if (p[0] > 200 && p[1] > 60 && p[1] < 130 && p[2] < 90) n++;
    }
    return n;
}

TEST(a_driver_who_drove_past_a_checkpoint_is_told_and_pointed_back) {
    // **A missed checkpoint used to be silent, and silence cost the race.**
    //
    // The simulation only ever tests the gate a car is *expecting*, so driving
    // past one stops every later crossing counting - the finish included. A
    // player ran wide at a corner, drove the rest of the lap, crossed the
    // chequered line and was told nothing at all; the first they knew was
    // running out of track.
    //
    // So: driving past the gate it owes latches the warning, an arrow is drawn
    // on the ground pointing back at it, and going back for the gate clears it.
    static gs_track t;
    gs_flat_pavement(&t, 48, 48);
    gs_track_add_gate(&t, GS_INT(24), GS_INT(24), 0, GS_INT(3));
    gs_track_add_gate(&t, GS_INT(40), GS_INT(24), 0, GS_INT(3));

    gs_view view = { 0 };
    view.car = 0;
    view.rect = (SDL_Rect){ 0, 0, GS_W, GS_H };
    view.cam = gs_camera_on(24.0f, 24.0f, 0.0f);
    view.cam.zoom = 2.0f;

    // Nothing has happened yet, and nothing is claimed.
    CHECK(!view.missed);

    // **Wide of the gate, past its line.** Eight tiles off centre of a gate
    // three wide, which is what running out of road at a corner looks like.
    static gs_world was, now;
    gs_park_car(&was, &t, GS_INT(22), GS_INT(32));
    now = was;
    now.car[0].x = GS_INT(26);

    gs_view_note_missed(&view, 1, &t, &was, &now);
    printf("  MISSED after driving past: %s, gate %u\n",
           view.missed ? "warned" : "silent", (unsigned)view.missed_at);
    CHECK(view.missed);
    CHECK(view.missed_at == 0);

    // **The arrow is on the ground and points back.** Counted against the same
    // frame with the warning cleared, because "some orange pixels" is a claim
    // about this scene and not about the arrow.
    gs_view quiet = view;
    quiet.missed = false;

    gs_frame warned = gs_frame_of_view(ren, &t, &now, &view);
    gs_frame silent = gs_frame_of_view(ren, &t, &now, &quiet);
    CHECK(warned.px != nullptr && silent.px != nullptr);
    if (warned.px != nullptr && silent.px != nullptr) {
        const int lit = gs_count_warning(&warned);
        const int unlit = gs_count_warning(&silent);
        printf("  MISSED arrow on the ground: %d px warned, %d px not\n",
               lit, unlit);

        // The same world and the same camera, so the arrow is the whole of the
        // difference - and the scene has none of that colour without it.
        CHECK(unlit == 0);
        CHECK(lit > 200);
    }
    gs_frame_free(&warned);
    gs_frame_free(&silent);

    // **And the HUD says it in words**, because an arrow on the ground says
    // which way and not why. Counted as a difference against the same HUD with
    // the warning cleared, so it is this row and not the panel in general.
    {
        // The HUD is drawn through ImGui, which the tests that own it start on
        // demand. This one runs before any of them, so it says so rather than
        // depending on the order they happen to be listed in - which is a
        // segmentation fault, and was.
        gs_imgui_start(gs_win, ren);

        gs_frame said, unsaid;
        gs_hud_frame(ren, &t, &now, &view, &said);
        gs_hud_frame(ren, &t, &now, &quiet, &unsaid);
        if (said.px != nullptr && unsaid.px != nullptr) {
            // The whole frame rather than the corner box the other HUD tests
            // use: the panel grows downward by a row, and where that row lands
            // depends on how many rows this race has. Nothing else differs
            // between the two frames, so every changed pixel is the row.
            int changed = 0;
            for (int i = 0; i < GS_W * GS_H; i++) {
                const uint8_t *a = &said.px[i * 4], *b = &unsaid.px[i * 4];
                if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2]) changed++;
            }
            printf("  MISSED the HUD says so: %d pixels of it\n", changed);
            CHECK(changed > 100);
        }
        gs_frame_free(&said);
        gs_frame_free(&unsaid);
    }

    // **And going back for it clears the warning**, which is what makes this a
    // thing a player can put right rather than a verdict.
    static gs_world back, through;
    back = now;
    back.car[0].x = GS_INT(22);
    back.car[0].y = GS_INT(24);
    through = back;
    through.car[0].x = GS_INT(26);
    through.car[0].next_gate = 1;      // the gate was taken this step

    gs_view_note_missed(&view, 1, &t, &back, &through);
    printf("  MISSED after going back for it: %s\n",
           view.missed ? "still warned" : "cleared");
    CHECK(!view.missed);
}

TEST(the_way_back_arrow_is_drawn_whole_wherever_the_car_is_standing) {
    // **It was drawn in pieces, and which pieces changed as the car moved.**
    //
    // Reported from play: "the go back arrow is rendering strangely ... parts
    // of it are visible and then not visible, it is like the image is
    // oscillating."
    //
    // `gs_ground_mark` cuts a mark into half-tile pieces and draws the ones
    // belonging to the diagonal it is handed, because that is how the terrain
    // sweep sorts ground paint against the things standing on it. It is called
    // once per diagonal, and across the whole sweep the mark comes out whole.
    //
    // This arrow is not drawn in the sweep. It is a readout, drawn after
    // everything, and it was passing its *own* quad's diagonal - so of the
    // pieces it was cut into, only the handful sharing the diagonal of the
    // furthest corner were ever drawn, and a different handful qualified each
    // time the car moved a tile. Hence a flickering arrow.
    //
    // The rule, which is what this pins: the arrow is the same shape wherever
    // the car is standing, so it is the same number of pixels. Not "some
    // orange" - the test above already asks that, and passed throughout.
    static gs_track t;
    gs_flat_pavement(&t, 48, 48);
    gs_track_add_gate(&t, GS_INT(24), GS_INT(24), 0, GS_INT(3));
    gs_track_add_gate(&t, GS_INT(40), GS_INT(24), 0, GS_INT(3));

    // Walked across two whole tiles in eighth-of-a-tile steps, which is what
    // driving does: every phase of the car against the tile grid, so no
    // alignment can be the lucky one that happens to look right.
    const int steps = 16;
    int least = 1 << 30, most = 0, walked = 0;

    for (int i = 0; i < steps; i++) {
        const float off = (float)i * 0.125f;

        static gs_world was, now;
        gs_park_car(&was, &t, GS_INT(22) + (gs_fix)(off * 65536.0f), GS_INT(32));
        now = was;
        now.car[0].x = was.car[0].x + GS_INT(4);

        gs_view view = { 0 };
        view.car = 0;
        view.rect = (SDL_Rect){ 0, 0, GS_W, GS_H };
        // The camera follows the car, so the arrow lands in the same place on
        // screen every time and only its alignment to the tile grid changes.
        view.cam = gs_camera_on(gs_to_f(now.car[0].x), gs_to_f(now.car[0].y), 0.0f);
        view.cam.zoom = 2.0f;

        gs_view_note_missed(&view, 1, &t, &was, &now);
        CHECK(view.missed);

        gs_frame f = gs_frame_of_view(ren, &t, &now, &view);
        CHECK(f.px != nullptr);
        if (f.px == nullptr) continue;

        const int lit = gs_count_warning(&f);
        gs_frame_free(&f);

        if (lit < least) least = lit;
        if (lit > most) most = lit;
        walked++;
    }

    CHECK(walked == steps);
    printf("  ARROW %d alignments walked; %d to %d pixels of it\n",
           walked, least, most);

    // **All of it, every time.** A whole arrow at this zoom is well over a
    // thousand pixels; the bug drew as few as a couple of hundred at some
    // alignments and most of it at others. Sub-pixel coverage moves the count a
    // little as the shape slides across the grid, so the bound is a fifth
    // rather than nothing - which is far tighter than a piece of the arrow
    // going missing and far looser than rasterisation noise.
    CHECK(least > 0);
    CHECK(most * 4 < least * 5);
}

TEST(no_paint_on_the_ground_is_drawn_over_a_car_standing_on_it) {
    // **Paint goes under the car, and this asks it of every kind of paint.**
    //
    // The ground is painted back to front one tile diagonal at a time, and a
    // shape is sorted by `floor(x) + floor(y)` at its *nearest* corner - the
    // only answer that keeps a mark out of the ground it lies on, and the wrong
    // one for anything standing between the shape's near end and its far end,
    // which is then painted over. Two kinds were caught doing it, and the only
    // reason the rest were not suspected is that nobody had asked:
    //
    //   - a gate's **arrow** ate 339 of a car's 6,491 pixels, and did it a tile
    //     at a time as the car drove, which is what a player reported as a
    //     flicker crossing an arrow or the start line;
    //   - a **hazard** ate all 6,491. Hazards were drawn in a pass after the
    //     sweep, so every one of them was on top of every car - under a comment
    //     saying they went "under everything that moves".
    //
    // So the rule is asked of all eight, in the orientation that is worst for
    // each: the gate faces the camera, which is what puts the far end of a mark
    // a whole tile behind the diagonal it is sorted at.
    //
    // **Measured against the same car on the same ground with the mark taken
    // away**, because "how much of a car can be seen" only means anything
    // against a car nothing is covering. And each case checks the mark is
    // really in the frame, or a scene that quietly failed to place one would
    // pass this without testing anything.
    //
    // Deliberately not here: the **kerb**, which is drawn one tile at a time
    // and so is already its own smallest piece; the **posts** at a waypoint's
    // edges and the **flags** and **lights** at a start line, which stand up
    // out of the world rather than lying on it and are meant to occlude a car
    // behind them; and the **landing arc**, which is drawn over everything on
    // purpose because it is a readout rather than scenery.
    const gs_angle towards_camera = (gs_angle)(65536 / 8);
    const gs_fix wide = GS_INT(3);

    int covered = 0;
    for (int m = 0; m < GS_MARK_COUNT; m++) {
        const gs_mark_kind mark = (gs_mark_kind)m;
        const gs_hazard_kind haz = gs_mark_hazard(mark);

        static gs_track marked, clear;
        gs_flat_pavement(&marked, 48, 48);
        gs_flat_pavement(&clear, 48, 48);

        // Where the car stands, and where the route has to be for the mark it
        // is standing on to be under it.
        gs_fix car_x = GS_INT(24), car_y = GS_INT(24);

        if (haz == GS_HAZ_NONE) {
            // A sprint: gate zero carries the plain line, the last gate the
            // chequer, and both carry an arrow behind them.
            const gs_fix a = GS_INT(24), b = GS_INT(32);
            gs_track_add_gate(&marked, a, a, towards_camera, wide);
            gs_track_add_gate(&marked, b, b, towards_camera, wide);

            // The same route, out of shot, so the reference frame is the car
            // and the ground and nothing else.
            gs_track_add_gate(&clear, GS_INT(44), GS_INT(44), towards_camera, wide);
            gs_track_add_gate(&clear, GS_INT(46), GS_INT(46), towards_camera, wide);

            switch (mark) {
            case GS_MARK_START_LINE:                       // on gate zero
                car_x = a; car_y = a;
                break;
            case GS_MARK_FINISH_LINE:                      // on the last gate
                car_x = b; car_y = b;
                break;
            case GS_MARK_ARROW:                            // 2.4 tiles behind it
                car_x = b - GS_RATIO(17, 10);
                car_y = b - GS_RATIO(17, 10);
                break;
            case GS_MARK_ROUTE:                            // half way between
            default:
                car_x = (a + b) / 2;
                car_y = (a + b) / 2;
                break;
            }
        }

        static gs_world on_mark, off_mark;
        gs_park_car(&on_mark, &marked, car_x, car_y);
        gs_park_car(&off_mark, &clear, car_x, car_y);

        if (haz != GS_HAZ_NONE) {
            gs_world_arm(&on_mark, haz, 1);
            on_mark.car[0].drop_cooldown = 0;
            CHECK(gs_world_drop(&on_mark, 0, haz));
            CHECK(on_mark.hazard_count == 1);
        }

        gs_camera cam = gs_camera_on(gs_to_f(car_x), gs_to_f(car_y), 0.0f);
        cam.zoom = 3.0f;

        gs_frame with = gs_render_frame(ren, &marked, &on_mark, &on_mark, 1.0f, &cam);
        gs_frame without = gs_render_frame(ren, &clear, &off_mark, &off_mark, 1.0f, &cam);
        CHECK(with.px != nullptr && without.px != nullptr);
        if (with.px == nullptr || without.px == nullptr) {
            gs_frame_free(&with);
            gs_frame_free(&without);
            continue;
        }

        const int whole = gs_count_car0(&without);
        const int lost = gs_car_pixels_lost(&with, &without);
        const double drawn = gs_frame_diff(&with, &without);

        printf("  PAINT %-12s %d px of car, %d of it painted over\n",
               gs_mark_name(mark), whole, lost);

        CHECK(whole > 2000);      // the reference car is really in the frame
        CHECK(drawn > 0.0);       // and the mark really is in the other one
        CHECK(lost == 0);         // and none of it landed on the car

        covered++;
        gs_frame_free(&with);
        gs_frame_free(&without);
    }

    // **Asserted, not believed.** A kind added to gs_mark_kind and not walked
    // here turns this red by itself.
    printf("  PAINT %d of %d kinds of ground paint walked\n", covered, GS_MARK_COUNT);
    CHECK(covered == GS_MARK_COUNT);
}

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

    // **Three and four used to take the same grid**, so that a player joining
    // would not rearrange everybody else's screen. That was the wrong trade and
    // it is worth writing down why, because the reasoning sounded right.
    //
    // A player does not join a race. The grid is settled on the setup screen or
    // in the lobby *before* the flag, and a machine that leaves a race in
    // progress goes back to the lobby rather than into it - so the
    // rearrangement being avoided happens between races, where it costs
    // nothing. What it was being paid for was a quarter of the window left
    // blank for the whole of every three-player race: 230,400 pixels at
    // 1280x720, while the three people racing were each squeezed into a box a
    // quarter the size.
    //
    // Three columns now. Every count and every window size is walked by
    // `every_number_of_players_gets_the_whole_screen_and_a_fair_share_of_it`;
    // what is pinned here is that three is no longer the four-player grid.
    SDL_Rect three[GS_MAX_CARS], four[GS_MAX_CARS];
    CHECK(gs_render_layout(3, 1280, 720, three) == 3);
    CHECK(gs_render_layout(4, 1280, 720, four) == 4);
    CHECK(three[0].h == 720);                   // full height, not a quarter
    CHECK(three[1].h == 720);
    CHECK(three[2].h == 720);
    CHECK(three[1].w != four[1].w);             // and not the four-player grid

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
    gs_view v[GS_MAX_CARS] = { 0 };
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

// Everything `gs_split_views` is allowed to have written, blanked in both
// copies so that all the rest - named or not, added today or next year - has to
// come back byte for byte.
static void gs_forget_placement(gs_view *v) {
    v->cam  = (gs_camera){ 0 };
    v->rect = (SDL_Rect){ 0 };
    v->car  = 0;
}

TEST(placing_the_views_leaves_everything_it_does_not_own_alone) {
    (void)ren;

    // **The missed-checkpoint warning was built, tested, ticked - and never
    // once reached a screen.**
    //
    // A `gs_view` carries two kinds of thing: where the view looks, which the
    // splitter decides, and what is switched on over it, which whoever set it
    // decides. The client used to hand `gs_split_views` a blank array and copy
    // the second kind back one field at a time *by name* - so the day a field
    // was added and no line was added with it, the flag was set every tick by
    // the simulation and destroyed every frame before anything could draw it.
    //
    // Every test around it passed. They drove the HUD and the ground arrow
    // directly, and the frame that wiped the flag was not on the path any of
    // them took. What was missing was not a check on the warning; it was a
    // check on **the seam the warning had to cross**.
    //
    // So the rule is pinned here rather than left to whoever adds the next
    // field: the splitter sets the car and the rectangle it owns, and leaves
    // the rest of the view exactly as it found it. Deliberately not a list of
    // today's fields - the view is filled with values nothing else would
    // produce and demanded back wholesale, so a field added next month is
    // covered without anybody remembering this file exists.
    static gs_track t;
    gs_flat_pavement(&t, 60, 60);

    static const gs_analysis borrowed = { 0 };

    gs_view seed;
    memset(&seed, 0, sizeof seed);       // padding too, so memcmp means something
    seed.show_gravity = true;
    seed.show_arc     = true;
    seed.missed       = true;
    seed.missed_at    = 7;
    seed.heat         = &borrowed;

    gs_split sp;
    const float dt = 1.0f / 60.0f;
    int checked = 0, cases = 0, merged = 0, split = 0;

    // **Both paths this function has, at every number of players.** Merged is
    // the early return; split is the loop - and the client's bug lived in
    // whichever one the frame happened to take, so neither is a sample of the
    // other.
    for (uint8_t cars = 1; cars <= GS_MAX_CARS; cars++) {
        for (int apart = 0; apart < 2; apart++) {
            gs_world w;
            gs_world_init(&w, GS_ONE);
            for (int i = 0; i < (int)cars; i++) {
                const gs_fix step = apart ? GS_INT(15) : GS_INT(2);
                gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR,
                                 GS_INT(5) + step * i, GS_INT(30), 0);
            }

            gs_split_init(&sp);
            for (int i = 0; i < 120; i++)
                gs_split_update(&sp, &t, &w, &w, 1.0f, GS_W, GS_H, dt);

            gs_view v[GS_MAX_CARS];
            for (int i = 0; i < GS_MAX_CARS; i++) v[i] = seed;

            const uint8_t n = gs_split_views(&sp, &t, &w, &w, 1.0f,
                                             GS_W, GS_H, v);
            CHECK(n >= 1);
            cases++;
            if (n == 1) merged++; else split++;

            for (uint8_t i = 0; i < n; i++) {
                // It did do its own half of the job, or the rest of this
                // proves only that a function which does nothing breaks
                // nothing.
                CHECK(v[i].rect.w > 0);
                CHECK(v[i].rect.h > 0);

                gs_view got = v[i], want = seed;
                gs_forget_placement(&got);
                gs_forget_placement(&want);

                if (memcmp(&got, &want, sizeof got) != 0)
                    printf("  PLACE %u cars, %s: a view came back changed "
                           "(missed %u, arc %u, gravity %u, heat %u)\n",
                           cars, apart ? "split" : "merged",
                           v[i].missed ? 1u : 0u, v[i].show_arc ? 1u : 0u,
                           v[i].show_gravity ? 1u : 0u,
                           v[i].heat != nullptr ? 1u : 0u);
                CHECK(memcmp(&got, &want, sizeof got) == 0);
                checked++;
            }
        }
    }

    printf("  PLACE %d views over %d arrangements (%d merged, %d split) kept "
           "everything the splitter does not own\n",
           checked, cases, merged, split);
    CHECK(cases == GS_MAX_CARS * 2);

    // **Both paths were actually taken.** The early return and the loop are
    // two separate places that write a view, and a run that only ever reached
    // one of them would pass this test while leaving the other unwatched.
    CHECK(merged > 0);
    CHECK(split > 0);
}

TEST(every_number_of_players_gets_the_whole_screen_and_a_fair_share_of_it) {
    (void)ren;

    // **The open question in FEATURES.md, answered.** *"What the merged
    // four-player camera does when it cannot merge. The failure mode is the
    // design, and it has not been thought about yet."*
    //
    // What it did was give three players the four-player grid with one quarter
    // left blank: 230,400 pixels of nothing at 1280x720, while the three people
    // racing were each squeezed into a box a quarter the size. Nobody chose
    // that; it fell out of a loop that stops at `views`.
    //
    // The decision, stated as three rules and checked here at every count and
    // at every window size the game is measured at:
    //
    //   - **every pane is the same size**, because an unequal pane is an
    //     advantage and this is a game people play on one sofa;
    //   - **the panes fill the screen**, apart from the divider between them;
    //   - **no pane overlaps another**, or two players are looking at the same
    //     pixels and one of them is wrong.
    const struct { int w, h; const char *what; } sizes[] = {
        { 1280, 720, "the window the game opens at" },
        { GS_W,  GS_H, "a window dragged smaller" },
        { 960, 600, "an ordinary one in between" },
    };

    int walked = 0;
    for (size_t z = 0; z < SDL_arraysize(sizes); z++) {
        for (uint8_t n = 1; n <= GS_MAX_CARS; n++) {
            SDL_Rect r[GS_MAX_CARS];
            const uint8_t got = gs_render_layout(n, sizes[z].w, sizes[z].h, r);
            CHECK(got == n);

            long covered = 0, biggest = 0, smallest = 0;
            for (uint8_t i = 0; i < got; i++) {
                CHECK(r[i].w > 0);
                CHECK(r[i].h > 0);

                // Inside the window, all of it.
                CHECK(r[i].x >= 0);
                CHECK(r[i].y >= 0);
                CHECK(r[i].x + r[i].w <= sizes[z].w);
                CHECK(r[i].y + r[i].h <= sizes[z].h);

                const long area = (long)r[i].w * (long)r[i].h;
                covered += area;
                if (i == 0 || area > biggest) biggest = area;
                if (i == 0 || area < smallest) smallest = area;

                // **Nobody overlapping anybody.**
                for (uint8_t k = 0; k < i; k++) {
                    const bool apart =
                        r[i].x >= r[k].x + r[k].w || r[k].x >= r[i].x + r[i].w ||
                        r[i].y >= r[k].y + r[k].h || r[k].y >= r[i].y + r[i].h;
                    if (!apart) {
                        printf("  SPLIT %u players at %dx%d: pane %u overlaps "
                               "pane %u\n", n, sizes[z].w, sizes[z].h, i, k);
                    }
                    CHECK(apart);
                }
            }

            // **The screen is used.** What may be left over is the divider
            // between panes and nothing else - a couple of pixels along one or
            // two seams, not a quarter of the window.
            const long whole = (long)sizes[z].w * (long)sizes[z].h;
            const long slack = whole - covered;
            const long seams = 4L * 2L * (sizes[z].w + sizes[z].h);
            if (slack > seams) {
                printf("  SPLIT %u players at %dx%d leaves %ld of %ld unused\n",
                       n, sizes[z].w, sizes[z].h, slack, whole);
            }
            CHECK(slack <= seams);

            // **And a fair share each.** Within a pixel row of each other,
            // which is all the rounding of an odd window width can cost.
            const long uneven = biggest - smallest;
            if (uneven > (long)sizes[z].w + (long)sizes[z].h) {
                printf("  SPLIT %u players at %dx%d: biggest pane %ld, "
                       "smallest %ld\n", n, sizes[z].w, sizes[z].h, biggest,
                       smallest);
            }
            CHECK(uneven <= (long)sizes[z].w + (long)sizes[z].h);
            walked++;
        }
    }
    printf("  SPLIT %d layouts: every player count from 1 to %d at %d window "
           "sizes, each filling the screen and sharing it evenly\n", walked,
           GS_MAX_CARS, (int)SDL_arraysize(sizes));
    CHECK(walked == GS_MAX_CARS * (int)SDL_arraysize(sizes));
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

        gs_view v[GS_MAX_CARS] = { 0 };
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
        gs_view v[GS_MAX_CARS] = { 0 };
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
        { "| drop a weapon | Right Shift | Left Shift |", GS_ACT_FIRE,
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

    // **Three people, said out loud.** A slot is the game's until somebody
    // takes it, which is what an empty grid should be - so a test about what
    // three *drivers* get out of a race has to say that all three are drivers.
    for (uint8_t i = 0; i < 3; i++) gs_m.setup.computer[i] = false;

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

        // **A corner inside the track, or it is not a different track.** Only
        // (w + 1) by (h + 1) corners are serialised, so bumping one past the
        // right-hand edge changes a byte nobody writes down: two entries hash
        // the same, the library folds them into one - it is content addressed -
        // and a loop that asked for a full library quietly got a short one.
        // Invisible while the library held thirty-two and every index was
        // inside the narrowest shipped track.
        int cx = i % (t.w + 1), cy = i / (t.w + 1);
        size_t corner = (size_t)cy * GS_CORNER_STRIDE + (size_t)cx;
        t.corner[corner] = (int16_t)(t.corner[corner] + 1);
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

// Every .gstrack in the assets directory, collected so the test can say how
// many there were as well as that each one was sound.
typedef struct gs_stock_walk {
    char  name[64][64];
    int   count;
    bool  overflowed;
} gs_stock_walk;

static SDL_EnumerationResult SDLCALL gs_take_stock(void *userdata, const char *dir,
                                                   const char *name) {
    (void)dir;
    gs_stock_walk *w = (gs_stock_walk *)userdata;

    size_t n = SDL_strlen(name);
    if (n < 9 || SDL_strcmp(name + n - 8, ".gstrack") != 0) {
        return SDL_ENUM_CONTINUE;
    }
    if (w->count >= (int)SDL_arraysize(w->name)) {
        w->overflowed = true;
        return SDL_ENUM_CONTINUE;
    }
    SDL_strlcpy(w->name[w->count++], name, sizeof w->name[0]);
    return SDL_ENUM_CONTINUE;
}

// The blue the route is painted in, as it lands in a frame. Matched loosely
// because it is drawn over whatever ground is under it with an alpha.
static bool gs_is_route_blue(const uint8_t *p) {
    return p[2] > 120 && p[2] > p[0] + 40 && p[1] > p[0] + 10 && p[1] < p[2];
}

// **The tracks that ship may not throw a car out of the world.**
//
// One car raced alone is never shoved, which is why the acceptance test could
// not see this: four abreast, seven of the seventy-two on the shipped set did
// not finish. Two kinds, and only one of them is the ground's fault - the cars
// lost deep in a race went to landings and to each other, which is racing, and
// the ones lost in the opening seconds went **over an edge**, past the run-off
// and down, while the pack was still together.
//
// So this refuses the second kind and not the first. It is also not a thing
// that can be asked of a track standing still: every track in the box lays its
// route three or four tiles from the nearest edge, the ones that lose cars and
// the ones that do not alike, so there is no margin to measure - it has to be
// raced to be seen.
TEST(no_track_that_ships_throws_a_car_off_the_world) {
    (void)ren;                       // raced, not drawn

    char dir[1024];
    const char *assets = gs_assets_dir();
    CHECK(assets != nullptr);
    if (assets == nullptr) return;

    static gs_stock_walk walk;
    walk.count = 0;
    walk.overflowed = false;
    SDL_snprintf(dir, sizeof dir, "%s/tracks/", assets);
    CHECK(SDL_EnumerateDirectory(dir, gs_take_stock, &walk));
    CHECK(walk.count >= 16);
    CHECK(!walk.overflowed);

    // **The two that do, named with their reason rather than left out.**
    //
    // Both are written by hand, and a hand-written track lays its route with
    // the same planner from a seed chosen for the ground it is built on - so
    // they cannot be fixed the way a generated candidate is, by taking the next
    // seed. Of the ninety-six seeds after theirs, only their own lays a sound
    // route on their ground. Fixing them means re-shaping two tracks or
    // widening the inset every route is laid at, which costs route length on
    // all eighteen; both are decisions about the set rather than something to
    // do quietly here.
    //
    // Required to still be failing, not merely permitted to: fixing one of them
    // turns this red so the excuse goes with the fault.
    static const char *excused[] = { "jupiter-run.gstrack", "which-way.gstrack" };
    bool excuse_used[SDL_arraysize(excused)] = { false };

    int raced = 0, lost = 0;
    for (int i = 0; i < walk.count; i++) {
        SDL_snprintf(dir, sizeof dir, "%s/tracks/%s", assets, walk.name[i]);
        size_t len = 0;
        void *bytes = SDL_LoadFile(dir, &len);
        CHECK(bytes != nullptr);
        if (bytes == nullptr) continue;

        static gs_track t;
        const bool read = gs_track_deserialize(&t, (const uint8_t *)bytes, len);
        SDL_free(bytes);
        CHECK(read);
        if (!read) continue;

        gs_world w;
        gs_world_init(&w, GS_ONE);
        for (uint8_t slot = 0; slot < GS_MAX_CARS; slot++) {
            gs_fix x, y; gs_angle heading;
            gs_track_grid(&t, slot, &x, &y, &heading);
            gs_world_add_car(&w, &t, (uint8_t)(slot % GS_VEH_COUNT), x, y, heading);
        }

        // A minute, which is how long a pack stays a pack: the cars lost over
        // an edge went at 6.5, 8.3 and 25.4 seconds and the ones lost to
        // racing went at 86 and 193.
        const gs_fix over = GS_INT(GS_RUNOFF_TILES);
        bool off = false;
        for (uint32_t k = 0; k < GS_TICK_HZ * 60u && !off; k++) {
            gs_input in[GS_MAX_CARS] = { 0 };
            for (uint8_t c = 0; c < w.car_count; c++) in[c] = gs_ai_drive(&w, &t, c);
            gs_world_step(&w, &t, in);
            for (uint8_t c = 0; c < w.car_count; c++) {
                if (w.car[c].x < -over || w.car[c].y < -over ||
                    w.car[c].x > GS_INT(t.w) + over ||
                    w.car[c].y > GS_INT(t.h) + over) {
                    off = true;
                }
            }
        }

        bool allowed = false;
        for (size_t k = 0; k < SDL_arraysize(excused); k++) {
            if (SDL_strcmp(walk.name[i], excused[k]) != 0) continue;
            allowed = true;
            if (off) excuse_used[k] = true;
        }

        if (off && !allowed) {
            printf("  OFF THE WORLD %s puts a car past the run-off\n",
                   walk.name[i]);
            lost++;
        }
        CHECK(off == false || allowed);
        raced++;
    }

    for (size_t k = 0; k < SDL_arraysize(excused); k++) {
        if (!excuse_used[k]) {
            printf("  OFF THE WORLD %s no longer needs excusing - remove it\n",
                   excused[k]);
        }
        CHECK(excuse_used[k]);
    }

    printf("  OFF THE WORLD %d tracks raced by a full grid, %d threw a car, "
           "%d named as known\n", raced, lost, (int)SDL_arraysize(excused));
    CHECK(raced == walk.count);
}

TEST(the_way_round_is_painted_between_the_gates_and_not_only_at_them) {
    // **The fault this exists for.** A gate carries an arrow saying which way
    // through it, and at racing zoom a player sees one arrow at a time with no
    // road edge in frame. Somebody read a gentle left-to-right sprint as two
    // switchback turns and was right to - nothing on the screen said otherwise.
    // So the route is painted along the ground the whole way round, and this
    // walks every leg of it rather than checking that some blue exists
    // somewhere.
    static gs_track t;
    gs_track_init(&t, 20, 16, GS_SURF_PAVEMENT);
    t.route = (uint8_t)GS_ROUTE_CIRCUIT;
    gs_track_add_gate(&t, GS_INT(4),  GS_INT(4),  0, GS_INT(3));
    gs_track_add_gate(&t, GS_INT(16), GS_INT(4),  0, GS_INT(3));
    gs_track_add_gate(&t, GS_INT(16), GS_INT(12), 0, GS_INT(3));
    gs_track_add_gate(&t, GS_INT(4),  GS_INT(12), 0, GS_INT(3));
    gs_track_face_along_route(&t);
    CHECK(gs_track_validate(&t).problem == GS_TRACK_OK);

    gs_world w;
    gs_park_car(&w, &t, GS_INT(10), GS_INT(8));

    // Small track, half zoom: the whole loop inside 640 by 480 with the dashes
    // still several pixels across, since the claim is about every leg of it and
    // a leg off the edge of the frame proves nothing either way.
    gs_camera cam = gs_camera_on(10.0f, 8.0f, 0.0f);
    cam.zoom = 0.5f;

    gs_frame f = gs_render_frame(ren, &t, &w, &w, 1.0f, &cam);
    CHECK(f.px != nullptr);
    if (f.px == nullptr) return;

    // Midway along each leg - away from the gates, where a waypoint post's own
    // blue head could otherwise answer for the line.
    int legs = 0, painted = 0;
    for (uint8_t i = 0; i < t.gate_count; i++) {
        const gs_gate *a = &t.gate[i];
        const gs_gate *b = &t.gate[(i + 1) % t.gate_count];
        float mx = (gs_to_f(a->x) + gs_to_f(b->x)) * 0.5f;
        float my = (gs_to_f(a->y) + gs_to_f(b->y)) * 0.5f;

        float sx = 0.0f, sy = 0.0f;
        gs_iso_project(&cam, mx, my, gs_to_f(gs_track_height(&t, (gs_fix)(mx * (float)GS_ONE),
                                                             (gs_fix)(my * (float)GS_ONE))),
                       &sx, &sy);

        int found = 0;
        for (int y = (int)sy - 20; y <= (int)sy + 20; y++) {
            for (int x = (int)sx - 20; x <= (int)sx + 20; x++) {
                if (x < 0 || y < 0 || x >= GS_W || y >= GS_H) continue;
                if (gs_is_route_blue(&f.px[((size_t)y * GS_W + (size_t)x) * 4])) found++;
            }
        }
        if (found > 0) painted++;
        legs++;
    }
    // How much of it landed anywhere at all, so a failure says which half is
    // wrong: none in the frame is a line that is not drawn, and some in the
    // frame but none at the midpoints is a line drawn somewhere else.
    int anywhere = 0;
    for (int i = 0; i < GS_W * GS_H; i++) {
        if (gs_is_route_blue(&f.px[(size_t)i * 4])) anywhere++;
    }
    gs_frame_free(&f);

    // Every leg, and the count said out loud, so a route drawn on three sides
    // of a square cannot pass this.
    CHECK(anywhere > 0);
    CHECK(legs == 4);
    CHECK(painted == legs);
    printf("  ROUTE %d of %d legs painted between their gates, %d pixels of it\n",
           painted, legs, anywhere);
}

TEST(the_stock_tracks_ship_and_are_worth_racing) {
    (void)ren;

    // **The tracks that ship are data, not C.** This reads the files as
    // installed - if the frontend went back to carrying a track, or the tracks
    // stopped being copied into a package, this is what notices.
    //
    // **Every one of them, not four of them.** This used to name first light,
    // the long drop, ice house and jupiter run, which is four of the twenty-four
    // that ship and none of the interesting ones. `the crossing` - a figure of
    // eight whose four gates all faced east, one of them square across the
    // route - was not in the list, so nothing ever validated it and it shipped
    // broken. A sample is not a set: the directory is walked, every file in it
    // is checked, and the count is stated so a track added next month is walked
    // by this test without anybody remembering to add it.
    char dir[1024];
    const char *assets = gs_assets_dir();
    CHECK(assets != nullptr);
    if (assets == nullptr) return;

    static gs_stock_walk walk;
    walk.count = 0;
    walk.overflowed = false;
    SDL_snprintf(dir, sizeof dir, "%s/tracks/", assets);
    CHECK(SDL_EnumerateDirectory(dir, gs_take_stock, &walk));

    // A floor, so an assets directory that could not be read cannot pass this
    // by walking nothing at all. Sixteen rather than twenty: every candidate is
    // now raced by every vehicle from every grid slot before it is allowed to
    // ship, which is a bar a route of a thousand tiles does not always clear.
    CHECK(walk.count >= 16);
    CHECK(!walk.overflowed);

    int checked = 0;
    for (int i = 0; i < walk.count; i++) {
        SDL_snprintf(dir, sizeof dir, "%s/tracks/%s", assets, walk.name[i]);

        size_t len = 0;
        void *bytes = SDL_LoadFile(dir, &len);
        CHECK(bytes != nullptr);
        if (bytes == nullptr) continue;

        static gs_track t;
        CHECK(gs_track_deserialize(&t, (const uint8_t *)bytes, len));
        SDL_free(bytes);

        // A route somebody can actually drive, which is the difference between
        // a track and a field - and, since the facing rule joined it, a route
        // whose gates face the way it goes.
        gs_track_issue issue = gs_track_validate(&t);
        if (issue.problem != GS_TRACK_OK) {
            printf("  %s: %s (gate %d)\n", walk.name[i],
                   gs_track_problem_text(issue.problem), issue.gate);
        }
        CHECK(issue.problem == GS_TRACK_OK);
        CHECK(t.gate_count >= 2);
        CHECK(t.w >= 24 && t.h >= 12);

        // **Flatness is not asked here.** It used to be, over the four tracks
        // this named, and all four are generated ground. Three of the
        // twenty-four that ship are flat on purpose - `the crossing` is a
        // figure of eight after the original's `dirt8`, and a figure of eight
        // is about the crossing rather than about the terrain. That the
        // *generator* writes ground is a claim about the generator, and it is
        // made over its two hundred seeds where it belongs.
        checked++;
    }

    // Walked as many as were found, which is the claim this test is allowed to
    // make and the one a count on its own does not.
    CHECK(checked == walk.count);
    printf("  STOCK %d track(s) on disk, %d checked, every gate facing its route\n",
           walk.count, checked);
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

TEST(every_combination_of_keys_reaches_the_car_at_once) {
    (void)ren;

    // **Accelerating and turning are one thing a driver does, not two.**
    //
    // Reported from play: "when using the arrow keys on the keyboard, I don't
    // seem to be able to accelerate and turn at the same time". Everything
    // around this was already checked - that each key does its own job, that a
    // moved binding moves, that one key cannot drive two cars - and none of it
    // pressed **two keys together**, which is what driving is.
    //
    // So: every subset of the five controls, all thirty-two of them, held at
    // once and asked for. Sampling three interesting pairs would have left the
    // same hole in a different place.
    gs_bindings b;
    gs_bind_defaults(&b);

    // What each control is worth in a gs_input, named here because the table
    // that does it is private to gs_bind.c - which is right: the mapping is
    // that file's business and this is checking the answer, not the workings.
    static const gs_input worth[GS_ACT_COUNT] = {
        [GS_ACT_ACCEL] = GS_IN_ACCEL,
        [GS_ACT_BRAKE] = GS_IN_BRAKE,
        [GS_ACT_LEFT]  = GS_IN_LEFT,
        [GS_ACT_RIGHT] = GS_IN_RIGHT,
        [GS_ACT_FIRE]  = GS_IN_FIRE,
    };

    static const SDL_Scancode arrows[GS_ACT_COUNT] = {
        [GS_ACT_ACCEL] = SDL_SCANCODE_UP,
        [GS_ACT_BRAKE] = SDL_SCANCODE_DOWN,
        [GS_ACT_LEFT]  = SDL_SCANCODE_LEFT,
        [GS_ACT_RIGHT] = SDL_SCANCODE_RIGHT,
        [GS_ACT_FIRE]  = SDL_SCANCODE_RSHIFT,
    };

    int walked = 0;
    for (unsigned held = 0; held < (1u << GS_ACT_COUNT); held++) {
        bool keys[SDL_SCANCODE_COUNT] = { false };
        gs_input want = 0;
        for (int a = 0; a < GS_ACT_COUNT; a++) {
            if ((held & (1u << (unsigned)a)) == 0) continue;
            keys[arrows[a]] = true;
            want |= worth[a];
        }

        const gs_input got = gs_bind_resolve(&b, 0, keys, SDL_SCANCODE_COUNT, 0);
        if (got != want) {
            printf("  KEYS holding %u gave %u, wanted %u\n", held,
                   (unsigned)got, (unsigned)want);
        }
        CHECK(got == want);
        walked++;
    }

    // **And through the whole path a race uses**, not only the resolver: two
    // keys held reach two cars' worth of input through gs_input_combine, which
    // is where a pad and a keyboard are merged and where one could mask the
    // other.
    gs_input from_keys[GS_MAX_CARS] = { 0 };
    gs_input from_pads[GS_MAX_CARS] = { 0 };
    gs_input out[GS_MAX_CARS] = { 0 };
    from_keys[0] = (gs_input)(GS_IN_ACCEL | GS_IN_LEFT);
    from_pads[0] = GS_IN_RIGHT;
    gs_input_combine(from_pads, 1, from_keys, GS_MAX_CARS, out, 1);
    CHECK(out[0] == (gs_input)(GS_IN_ACCEL | GS_IN_LEFT | GS_IN_RIGHT));

    printf("  KEYS %d of %d combinations of the five controls walked\n",
           walked, 1 << GS_ACT_COUNT);
    CHECK(walked == (1 << GS_ACT_COUNT));
}

TEST(a_rebind_waits_for_the_key_that_started_it_to_be_let_go) {
    (void)ren;

    // **The fault this is named for.** A capture begins the instant the "press
    // something..." control is pressed - and that control was pressed *with
    // something*. Space or Enter, if the player walked to it with the keyboard.
    // The pad's bottom button, if they walked to it with a pad. That key is
    // still down on the very next frame, when the capture reads the keyboard
    // for the first time, so the action was bound to it immediately.
    //
    // Which means a player rebinding their controls from the keyboard could
    // only ever bind Space, and a player rebinding from a pad could only ever
    // bind the button they press everything with. Those two are most of the
    // people this feature exists for: gs_bind.h calls remapping "not a luxury
    // feature here", for four on one sofa and for anybody who cannot reach the
    // default keys.
    //
    // A third of gs_capture_rebind had never run. It could not: it read SDL's
    // keyboard and a live pad, and no test has either. The rule lives in
    // gs_bind_pick now, beside the resolution it is the other half of.
    static bool keys[SDL_SCANCODE_COUNT];
    SDL_memset(keys, 0, sizeof keys);

    // Space held, the way it is when a keyboard player has just pressed the
    // control. Nothing is bound while it is still down...
    keys[SDL_SCANCODE_SPACE] = true;
    bool armed = false;
    for (int i = 0; i < 8; i++) {
        gs_rebind_pick p = gs_bind_pick(&armed, keys, SDL_SCANCODE_COUNT, 0);
        CHECK(p.what == GS_REBIND_WAIT);
        CHECK(!armed);
    }

    // ...it is let go, which arms the capture and binds nothing by itself...
    keys[SDL_SCANCODE_SPACE] = false;
    gs_rebind_pick p = gs_bind_pick(&armed, keys, SDL_SCANCODE_COUNT, 0);
    CHECK(p.what == GS_REBIND_WAIT);
    CHECK(armed);

    // ...and now the key the player actually wants is the one that lands.
    keys[SDL_SCANCODE_J] = true;
    p = gs_bind_pick(&armed, keys, SDL_SCANCODE_COUNT, 0);
    CHECK(p.what == GS_REBIND_KEY);
    CHECK(p.which == SDL_SCANCODE_J);
    keys[SDL_SCANCODE_J] = false;

    // The same on a pad, with its bottom button: held from the press that
    // started the capture, and it binds nothing until it is released.
    armed = false;
    for (int i = 0; i < 8; i++) {
        p = gs_bind_pick(&armed, keys, SDL_SCANCODE_COUNT,
                         1u << SDL_GAMEPAD_BUTTON_SOUTH);
        CHECK(p.what == GS_REBIND_WAIT);
    }
    p = gs_bind_pick(&armed, keys, SDL_SCANCODE_COUNT, 0);
    CHECK(p.what == GS_REBIND_WAIT);
    CHECK(armed);
    p = gs_bind_pick(&armed, keys, SDL_SCANCODE_COUNT,
                     1u << SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(p.what == GS_REBIND_BUTTON);
    CHECK(p.which == (int)SDL_GAMEPAD_BUTTON_SOUTH);

    // **Escape leaves it alone**, which is what the panel says it does - and
    // not while Escape is the key that is still held from starting the capture,
    // because that would cancel every rebind a keyboard player ever began.
    armed = false;
    keys[SDL_SCANCODE_ESCAPE] = true;
    p = gs_bind_pick(&armed, keys, SDL_SCANCODE_COUNT, 0);
    CHECK(p.what == GS_REBIND_WAIT);
    keys[SDL_SCANCODE_ESCAPE] = false;
    p = gs_bind_pick(&armed, keys, SDL_SCANCODE_COUNT, 0);
    CHECK(armed);
    keys[SDL_SCANCODE_ESCAPE] = true;
    p = gs_bind_pick(&armed, keys, SDL_SCANCODE_COUNT, 0);
    CHECK(p.what == GS_REBIND_CANCEL);
    keys[SDL_SCANCODE_ESCAPE] = false;

    // **Every key on the keyboard can be bound to.** Not a handful of
    // interesting ones: what a player reaches for is theirs to choose, and a
    // scancode that cannot be captured is a control somebody cannot have.
    // Escape is the one exception and it is the documented one.
    int bindable = 0, refused = 0;
    for (int k = 1; k < SDL_SCANCODE_COUNT; k++) {
        if (k == SDL_SCANCODE_ESCAPE) continue;
        armed = true;
        keys[k] = true;
        p = gs_bind_pick(&armed, keys, SDL_SCANCODE_COUNT, 0);
        keys[k] = false;
        if (p.what == GS_REBIND_KEY && p.which == k) { bindable++; continue; }
        refused++;
        printf("  REBIND scancode %d (%s) could not be bound\n", k,
               SDL_GetScancodeName((SDL_Scancode)k));
    }
    printf("  REBIND %d of %d scancodes bindable, %d refused; Escape is the "
           "one that means leave it alone\n", bindable,
           (int)SDL_SCANCODE_COUNT - 2, refused);
    CHECK(refused == 0);
    CHECK(bindable == (int)SDL_SCANCODE_COUNT - 2);

    // **And every button a pad has.** Same reason, and the pad is the half a
    // person on a sofa is holding.
    int buttons = 0;
    for (int b = 0; b < (int)SDL_GAMEPAD_BUTTON_COUNT && b < 32; b++) {
        armed = true;
        p = gs_bind_pick(&armed, keys, SDL_SCANCODE_COUNT, 1u << b);
        if (p.what != GS_REBIND_BUTTON || p.which != b) {
            printf("  REBIND pad button %d (%s) could not be bound\n", b,
                   SDL_GetGamepadStringForButton((SDL_GamepadButton)b));
        }
        CHECK(p.what == GS_REBIND_BUTTON);
        CHECK(p.which == b);
        buttons++;
    }
    printf("  REBIND all %d pad buttons bindable\n", buttons);
    CHECK(buttons == (int)SDL_GAMEPAD_BUTTON_COUNT);

    // Nothing held at all is nothing decided, however long it goes on.
    armed = true;
    for (int i = 0; i < 4; i++) {
        p = gs_bind_pick(&armed, keys, SDL_SCANCODE_COUNT, 0);
        CHECK(p.what == GS_REBIND_WAIT);
    }

    // And the keyboard wins over a pad held at the same moment: a player with
    // both in front of them pressed a key, and the pad is resting.
    armed = true;
    keys[SDL_SCANCODE_K] = true;
    p = gs_bind_pick(&armed, keys, SDL_SCANCODE_COUNT,
                     1u << SDL_GAMEPAD_BUTTON_NORTH);
    CHECK(p.what == GS_REBIND_KEY);
    CHECK(p.which == SDL_SCANCODE_K);
    keys[SDL_SCANCODE_K] = false;

    // A null keyboard is what a caller gets before SDL has one, and it is not
    // a crash.
    armed = true;
    p = gs_bind_pick(&armed, nullptr, 0, 1u << SDL_GAMEPAD_BUTTON_WEST);
    CHECK(p.what == GS_REBIND_BUTTON);
    CHECK(p.which == (int)SDL_GAMEPAD_BUTTON_WEST);
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

TEST(the_minimap_shows_the_whole_route_and_where_everybody_is_on_it) {
    // **The question the race view cannot answer.** Isometric and zoomed to the
    // car is right for driving and useless for knowing where you are: a player
    // sees about ten tiles of a track sixty across. This is the picture the
    // original game had, and it is checked in the corner it lives in - drawn
    // for a track with a route, and absent for one without, because a panel
    // that is always there proves nothing by being there.
    gs_imgui_start(gs_win, ren);
    CHECK(gs_imgui_ready);
    if (!gs_imgui_ready) return;

    static gs_track t;
    gs_track_init(&t, 40, 32, GS_SURF_PAVEMENT);
    t.route = (uint8_t)GS_ROUTE_CIRCUIT;
    gs_track_add_gate(&t, GS_INT(8),  GS_INT(8),  0, GS_INT(3));
    gs_track_add_gate(&t, GS_INT(32), GS_INT(8),  0, GS_INT(3));
    gs_track_add_gate(&t, GS_INT(32), GS_INT(24), 0, GS_INT(3));
    gs_track_add_gate(&t, GS_INT(8),  GS_INT(24), 0, GS_INT(3));
    gs_track_face_along_route(&t);
    CHECK(gs_track_validate(&t).problem == GS_TRACK_OK);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_fix sx, sy; gs_angle facing;
    gs_track_grid(&t, 0, &sx, &sy, &facing);
    gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR, sx, sy, facing);

    gs_view view = { 0 };
    view.cam.zoom = GS_ISO_DEFAULT_ZOOM;
    view.cam.vw = GS_W; view.cam.vh = GS_H;
    view.cam.cx = gs_to_f(w.car[0].x); view.cam.cy = gs_to_f(w.car[0].y);
    view.rect = (SDL_Rect){ 0, 0, GS_W, GS_H };

    gs_frame mapped;
    gs_hud_frame(ren, &t, &w, &view, &mapped);
    CHECK(mapped.px != nullptr);
    if (mapped.px == nullptr) return;

    // The corner it lives in, which is the one the stats are not in. Counted
    // there rather than over the whole frame, so the line painted on the ground
    // - the same blue, by design - cannot answer for the map.
    int in_corner = 0, elsewhere = 0;
    for (int y = 0; y < GS_H; y++) {
        for (int x = 0; x < GS_W; x++) {
            if (!gs_is_route_blue(&mapped.px[((size_t)y * GS_W + (size_t)x) * 4])) continue;
            if (x > GS_W - 180 && y < 160) in_corner++;
            else elsewhere++;
        }
    }
    (void)elsewhere;
    gs_frame_free(&mapped);
    CHECK(in_corner > 100);

    // **And it is not simply a blue rectangle.** A track with no route on it -
    // the blank field the construction set starts from - has nothing to show
    // and shows nothing, so what was counted above is the route rather than the
    // panel it is drawn in.
    static gs_track blank;
    gs_track_init(&blank, 40, 32, GS_SURF_PAVEMENT);

    gs_world empty;
    gs_world_init(&empty, GS_ONE);
    gs_world_add_car(&empty, &blank, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(10),
                     GS_INT(10), 0);

    gs_view plain = view;
    plain.cam.cx = 10.0f; plain.cam.cy = 10.0f;

    gs_frame bare;
    gs_hud_frame(ren, &blank, &empty, &plain, &bare);
    CHECK(bare.px != nullptr);
    if (bare.px == nullptr) return;

    int bare_corner = 0;
    for (int y = 0; y < 160; y++) {
        for (int x = GS_W - 179; x < GS_W; x++) {
            if (gs_is_route_blue(&bare.px[((size_t)y * GS_W + (size_t)x) * 4])) bare_corner++;
        }
    }
    gs_frame_free(&bare);
    CHECK(bare_corner == 0);

    printf("  MAP %d pixels of route in the corner, %d with no route to draw\n",
           in_corner, bare_corner);
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

        gs_view v[GS_MAX_CARS] = { 0 };
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

    gs_view v[GS_MAX_CARS] = { 0 };
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
        gs_view v[GS_MAX_CARS] = { 0 };
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
    gs_view v[GS_MAX_CARS] = { 0 };
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

        gs_view v[GS_MAX_CARS] = { 0 };
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

// The colour at a point on the ground, as the frame drew it.
static void gs_pixel_at(const gs_frame *f, const gs_camera *cam,
                        const gs_track *t, float x, float y,
                        int *r, int *g, int *b) {
    *r = *g = *b = -1;
    gs_fix z = gs_track_height(t, (gs_fix)(x * (float)GS_ONE),
                               (gs_fix)(y * (float)GS_ONE));
    float sx = 0.0f, sy = 0.0f;
    gs_iso_project(cam, x, y, gs_to_f(z), &sx, &sy);
    if (sx < 0 || sy < 0 || sx >= GS_W || sy >= GS_H) return;
    const uint8_t *p = &f->px[((int)sy * GS_W + (int)sx) * 4];
    *r = p[0]; *g = p[1]; *b = p[2];
}

TEST(every_kind_of_hazard_is_drawn_as_itself) {
    // **Two of the four were invisible.** The renderer knew oil and drew a
    // small orange dot for everything else, so smoke and fire looked like
    // mines - and smoke, whose entire job is hiding the ground, hid nothing at
    // all. Nothing could see that from the simulation's side: the hazards were
    // there, hashed, doing what they should.
    //
    // Walked from GS_HAZ_COUNT, so a fifth kind has to be given a look rather
    // than inheriting whichever case came last.
    static gs_track t;
    gs_flat_pavement(&t, 60, 60);

    static gs_world w;
    gs_park_car(&w, &t, GS_INT(50), GS_INT(50));      // parked well clear

    // One of each, spread far enough apart that none overlaps another - smoke
    // is over two tiles across, so eight tiles between them is room to spare.
    const float at[GS_HAZ_COUNT][2] = {
        { 0.0f, 0.0f },                                // GS_HAZ_NONE: unused
        { 20.0f, 20.0f }, { 28.0f, 20.0f },
        { 20.0f, 28.0f }, { 28.0f, 28.0f },
    };
    for (int k = GS_HAZ_NONE + 1; k < GS_HAZ_COUNT; k++) {
        gs_world_arm(&w, (gs_hazard_kind)k, 1);
        w.car[0].x = (gs_fix)(at[k][0] * (float)GS_ONE);
        w.car[0].y = (gs_fix)(at[k][1] * (float)GS_ONE);
        w.car[0].drop_cooldown = 0;
        CHECK(gs_world_drop(&w, 0, (gs_hazard_kind)k));
    }
    CHECK(w.hazard_count == GS_HAZ_COUNT - 1);
    w.car[0].x = GS_INT(50);
    w.car[0].y = GS_INT(50);

    gs_camera cam = gs_camera_on(24.0f, 24.0f, 0.0f);
    cam.zoom = 1.1f;
    gs_frame f = gs_render_frame(ren, &t, &w, &w, 1.0f, &cam);
    CHECK(f.px != nullptr);
    if (f.px == nullptr) return;

    // The bare ground, somewhere none of them reaches.
    int gr = 0, gg = 0, gb = 0;
    gs_pixel_at(&f, &cam, &t, 24.0f, 24.0f, &gr, &gg, &gb);
    CHECK(gr >= 0);

    int seen[GS_HAZ_COUNT][3];
    int drawn = 0;
    for (int k = GS_HAZ_NONE + 1; k < GS_HAZ_COUNT; k++) {
        gs_pixel_at(&f, &cam, &t, at[k][0], at[k][1],
                    &seen[k][0], &seen[k][1], &seen[k][2]);
        CHECK(seen[k][0] >= 0);

        // **Something is there.** A hazard that draws as the ground is a
        // hazard nobody can avoid.
        const int off = abs(seen[k][0] - gr) + abs(seen[k][1] - gg) +
                        abs(seen[k][2] - gb);
        if (off <= 30) {
            printf("  HAZARD %s draws as bare ground\n",
                   gs_hazard_name((gs_hazard_kind)k));
        }
        CHECK(off > 30);
        drawn++;
    }

    // **And no two of them look alike**, which is the half that was false: a
    // player who cannot tell fire from a mine cannot decide whether to drive
    // round it or wait it out.
    int alike = 0;
    for (int a = GS_HAZ_NONE + 1; a < GS_HAZ_COUNT; a++) {
        for (int b = a + 1; b < GS_HAZ_COUNT; b++) {
            const int off = abs(seen[a][0] - seen[b][0]) +
                            abs(seen[a][1] - seen[b][1]) +
                            abs(seen[a][2] - seen[b][2]);
            if (off > 40) continue;
            alike++;
            printf("  HAZARD %s and %s look the same: %d,%d,%d against "
                   "%d,%d,%d\n", gs_hazard_name((gs_hazard_kind)a),
                   gs_hazard_name((gs_hazard_kind)b), seen[a][0], seen[a][1],
                   seen[a][2], seen[b][0], seen[b][1], seen[b][2]);
        }
    }
    CHECK(alike == 0);

    // **Smoke hides the ground**, which is the whole of what it does and the
    // one of the four whose look *is* its behaviour. Pale and nearly solid:
    // anything the road shows through is a grey patch rather than a screen.
    CHECK(seen[GS_HAZ_SMOKE][0] > 150);
    CHECK(seen[GS_HAZ_SMOKE][1] > 150);
    CHECK(seen[GS_HAZ_SMOKE][2] > 150);

    // Oil is the opposite: dark, and the road still under it.
    CHECK(seen[GS_HAZ_OIL][0] < 90);

    printf("  HAZARDS all %d kinds drawn, told apart from the ground and from "
           "each other\n", drawn);
    CHECK(drawn == GS_HAZ_COUNT - 1);
    gs_frame_free(&f);
}

TEST(a_hazard_is_drawn_the_size_it_will_catch_you_at) {
    // **What you see is what hits you.** The renderer used to carry its own
    // idea of how wide a slick was; it asks the simulation now. A slick drawn
    // narrower than it is is the sort of lie a player learns to distrust the
    // whole physics over.
    //
    // Measured by drawing the same ground twice - once with the hazard on it
    // and once without - and comparing the *same* pixel. Comparing two
    // different patches of ground instead measures the terrain's own shading,
    // which is how this test first told itself that smoke was four tiles wider
    // than it is.
    static gs_track t;
    gs_flat_pavement(&t, 60, 60);

    int checked = 0;
    for (int k = GS_HAZ_NONE + 1; k < GS_HAZ_COUNT; k++) {
        static gs_world bare;
        gs_park_car(&bare, &t, GS_INT(50), GS_INT(50));

        static gs_world one;
        gs_park_car(&one, &t, GS_INT(50), GS_INT(50));
        gs_world_arm(&one, (gs_hazard_kind)k, 1);
        one.car[0].x = GS_INT(24);
        one.car[0].y = GS_INT(24);
        CHECK(gs_world_drop(&one, 0, (gs_hazard_kind)k));
        one.car[0].x = GS_INT(50);
        one.car[0].y = GS_INT(50);

        gs_camera cam = gs_camera_on(24.0f, 24.0f, 0.0f);
        cam.zoom = 1.6f;

        gs_frame with = gs_render_frame(ren, &t, &one, &one, 1.0f, &cam);
        gs_frame without = gs_render_frame(ren, &t, &bare, &bare, 1.0f, &cam);
        CHECK(with.px != nullptr);
        CHECK(without.px != nullptr);
        if (with.px == nullptr || without.px == nullptr) return;

        const float r = gs_to_f(gs_hazard_radius((gs_hazard_kind)k));
        CHECK(r > 0.0f);

        // Well inside the edge it catches you at, and well outside it. The
        // same number on both sides, because both come from gs_hazard_radius.
        int ar = 0, ag = 0, ab = 0, br = 0, bg = 0, bb = 0;
        gs_pixel_at(&with, &cam, &t, 24.0f + r * 0.6f, 24.0f, &ar, &ag, &ab);
        gs_pixel_at(&without, &cam, &t, 24.0f + r * 0.6f, 24.0f, &br, &bg, &bb);
        CHECK(ar >= 0 && br >= 0);
        const int inside = abs(ar - br) + abs(ag - bg) + abs(ab - bb);

        int cr = 0, cg = 0, cb = 0, dr = 0, dg = 0, db = 0;
        gs_pixel_at(&with, &cam, &t, 24.0f + r * 1.8f, 24.0f, &cr, &cg, &cb);
        gs_pixel_at(&without, &cam, &t, 24.0f + r * 1.8f, 24.0f, &dr, &dg, &db);
        CHECK(cr >= 0 && dr >= 0);
        const int outside = abs(cr - dr) + abs(cg - dg) + abs(cb - db);

        if (inside <= 20 || outside != 0) {
            printf("  HAZARD %s at radius %.2f: %d inside, %d outside\n",
                   gs_hazard_name((gs_hazard_kind)k), (double)r, inside,
                   outside);
        }
        CHECK(inside > 20);      // inside the radius, it is drawn
        CHECK(outside == 0);     // past it, the ground is exactly as it was
        checked++;

        gs_frame_free(&with);
        gs_frame_free(&without);
    }
    printf("  HAZARDS all %d drawn at the size the simulation uses\n", checked);
    CHECK(checked == GS_HAZ_COUNT - 1);
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
    //
    // **And in both modes**, because a derby draws a different set of rows -
    // one, where a race draws four - and the panel is sized from the rows it
    // has. A HUD sized for the other mode is either a box with a hole in it or
    // a box with something below the bottom of it, and this test is the thing
    // that says which.
    struct { bool wrecked; bool finished; float waited; bool online; bool derby;
             bool counting; } states[] = {
        { false, false, 0.0f,  false, false, false },
        { true,  false, 0.0f,  false, false, false },
        { true,  false, 0.0f,  true,  false, false },
        { false, false, 3.0f,  false, false, false },
        { true,  false, 3.0f,  true,  false, false },
        { false, true,  0.0f,  false, false, false },
        { true,  true,  9.0f,  false, false, false },
        { false, false, 0.0f,  false, false, true  },
        { false, false, 0.0f,  false, true,  false },
        { true,  false, 0.0f,  false, true,  false },
        { false, false, 0.0f,  false, true,  true  },
        { true,  false, 3.0f,  true,  true,  false },
    };

    // **And each of them with and without weapons**, because carrying
    // something adds a row and the panel is sized from the rows it has. Walked
    // as a dimension rather than as six more hand-written states: it is
    // independent of every other flag here, and hand-picking combinations is
    // how a state goes unmeasured.
    for (size_t si = 0; si < SDL_arraysize(states) * 2; si++) {
        const size_t i = si % SDL_arraysize(states);
        const bool carrying = si >= SDL_arraysize(states);

        for (int k = GS_HAZ_NONE + 1; k < GS_HAZ_COUNT; k++) {
            gs_world_arm(&w, (gs_hazard_kind)k, carrying ? 3 : 0);
        }
        w.car[0].wrecked = states[i].wrecked;
        w.car[0].damage = states[i].wrecked ? 255 : 0;
        w.car[0].finish_tick = states[i].finished ? 4200 : 0;
        gs_world_set_mode(&w, states[i].derby ? GS_MODE_DESTRUCTION : GS_MODE_RACE);
        gs_world_set_countdown(&w, states[i].counting ? 200u : 0u);

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

        if (gs_hud_overflow() != 0.0f) {
            printf("  HUD state %zu (%s%s%s%s%s) hides %.0f\n", i,
                   carrying ? "carrying " : "",
                   states[i].derby ? "derby " : "race ",
                   states[i].wrecked ? "wrecked " : "",
                   states[i].finished ? "finished " : "",
                   states[i].counting ? "counting" : "",
                   (double)gs_hud_overflow());
        }
        CHECK(gs_hud_overflow() == 0.0f);

        // **And no hole in it either.** A panel sized for rows it is not
        // drawing is a box with an empty half, which is what a derby HUD looked
        // like the day it stopped drawing four of its five rows. Measured as
        // the room left under the last thing drawn.
        // **A row, not a magic twelve.**
        //
        // The bound was a constant, and a constant cannot say what it is a
        // bound on. What this test is for is a panel sized for rows it is not
        // drawing - the derby HUD that stopped drawing four of its five rows -
        // and the smallest thing that failure can be is **one row's worth of
        // hole**. Below that is the sizing model's own resolution: it adds up
        // whole rounded lines and spacings, and no arrangement of them lands
        // exactly on what ImGui then advances the cursor by.
        //
        // So the bound is a line of text, asked of the font rather than
        // written down. That is what the old twelve was - this font's line
        // height on this machine - and saying so is the difference between a
        // bound that travels to a different font and one that happens to be
        // right here. It caught the fault it was written for at twelve and
        // still catches it: a missing row is thirty pixels and more.
        const float line = ImGui_GetTextLineHeight();
        const float spare = gs_hud_spare();
        if (spare > line) {
            printf("  HUD state %zu (%s%s%s%s%s) has %.1f pixels of nothing at "
                   "the bottom, against a line of %.1f\n", i,
                   carrying ? "carrying " : "",
                   states[i].derby ? "derby " : "race ",
                   states[i].wrecked ? "wrecked " : "",
                   states[i].finished ? "finished " : "",
                   states[i].counting ? "counting" : "",
                   (double)spare, (double)line);
        }
        CHECK(spare <= line);
    }

    gs_world_set_mode(&w, GS_MODE_RACE);
    gs_world_set_countdown(&w, 0);
    w.car[0].wrecked = false;
    w.car[0].damage = 0;
    w.car[0].finish_tick = 0;
}

TEST(a_hud_stays_inside_the_view_it_belongs_to) {
    gs_imgui_start(gs_win, ren);
    CHECK(gs_imgui_ready);
    if (!gs_imgui_ready) return;

    // **Every state of this panel has been measured, and always in one view
    // filling the whole window.** Four players on a screen somebody has dragged
    // smaller do not get that. The window splits four ways, each view is a
    // quarter of it, and the HUD is drawn ten pixels inside its own view at a
    // height worked out from its rows and nothing else - so a panel taller than
    // a quarter-window is drawn over the player below, reading them somebody
    // else's lap time.
    //
    // ImGui clamps a window to the viewport, which is the whole screen. It has
    // never heard of a view.
    static gs_track t;
    gs_flat_pavement(&t, 96, 96);

    // Corners of a big track, so the cars are far enough apart that the screen
    // really does split and stays split.
    const int32_t at[GS_MAX_CARS][2] = {
        { 8, 8 }, { 88, 8 }, { 8, 88 }, { 88, 88 },
    };

    // **At the size the game opens at as well as the size it can be dragged
    // to.** A quarter of 1280x720 is 638x358, which is not obviously too small
    // for anything - so if the HUD does not fit there either, that is a fault
    // in what ships rather than one in an awkward corner.
    const struct { int w, h; const char *what; } sizes[] = {
        { 1280, 720, "the window the game opens at" },
        { GS_W, GS_H, "a window dragged smaller" },
    };

    // The same states the full-window test walks, because a panel that fits
    // one way round does not follow from a panel that fits the other.
    struct { bool wrecked; bool finished; float waited; bool online; bool derby;
             bool counting; } states[] = {
        { false, false, 0.0f,  false, false, false },
        { true,  false, 0.0f,  false, false, false },
        { true,  false, 0.0f,  true,  false, false },
        { false, false, 3.0f,  false, false, false },
        { true,  false, 3.0f,  true,  false, false },
        { false, true,  0.0f,  false, false, false },
        { true,  true,  9.0f,  false, false, false },
        { false, false, 0.0f,  false, false, true  },
        { false, false, 0.0f,  false, true,  false },
        { true,  false, 0.0f,  false, true,  false },
        { false, false, 0.0f,  false, true,  true  },
        { true,  false, 3.0f,  true,  true,  false },
    };

    int measured = 0, spilled = 0;

    // **Every number of players, not the worst one.** The screen is divided by
    // the car count: two get half the window each and keep its full height,
    // three and four get a quarter each. Two is not covered by four - a HUD
    // that fits a quarter fits a half, and a HUD that fits neither is a
    // different fault in each - so all three are walked.
    for (uint8_t cars = 2; cars <= GS_MAX_CARS; cars++) {
    for (size_t z = 0; z < SDL_arraysize(sizes); z++) {
    CHECK(SDL_SetWindowSize(gs_win, sizes[z].w, sizes[z].h));
    CHECK(SDL_SetRenderLogicalPresentation(ren, sizes[z].w, sizes[z].h,
                                           SDL_LOGICAL_PRESENTATION_DISABLED));

    static gs_world w;
    gs_world_init(&w, GS_ONE);
    for (uint8_t i = 0; i < cars; i++) {
        gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR,
                         GS_INT(at[i][0]), GS_INT(at[i][1]), 0);
    }

    gs_split sp;
    gs_split_init(&sp);
    for (int i = 0; i < 240; i++) {
        gs_split_update(&sp, &t, &w, &w, 1.0f, sizes[z].w,
                        sizes[z].h, 1.0f / 60.0f);
    }
    gs_view v[GS_MAX_CARS] = { 0 };
    uint8_t views = gs_split_views(&sp, &t, &w, &w, 1.0f, sizes[z].w,
                                   sizes[z].h, v);
    CHECK(views == cars);
    if (views != cars) return;

    for (size_t si = 0; si < SDL_arraysize(states) * 2; si++) {
        const size_t i = si % SDL_arraysize(states);
        const bool carrying = si >= SDL_arraysize(states);
        for (int k = GS_HAZ_NONE + 1; k < GS_HAZ_COUNT; k++) {
            gs_world_arm(&w, (gs_hazard_kind)k, carrying ? 3 : 0);
        }
        for (uint8_t c = 0; c < cars; c++) {
            w.car[c].wrecked = states[i].wrecked;
            w.car[c].damage = states[i].wrecked ? 255 : 0;
            w.car[c].finish_tick = states[i].finished ? 4200 : 0;
        }
        gs_world_set_mode(&w, states[i].derby ? GS_MODE_DESTRUCTION : GS_MODE_RACE);
        gs_world_set_countdown(&w, states[i].counting ? 200u : 0u);

        for (int frame = 0; frame < 3; frame++) {
            cImGui_ImplSDLRenderer3_NewFrame();
            cImGui_ImplSDL3_NewFrame();
            ImGui_NewFrame();
            for (uint8_t c = 0; c < views; c++) {
                gs_hud_draw(&w, &t, &v[c], 600, states[i].waited,
                            states[i].online);
            }
            ImGui_Render();
        }

        for (uint8_t c = 0; c < views; c++) {
            char id[32];
            SDL_snprintf(id, sizeof id, "##hud%u", v[c].car);
            float x = 0.0f, y = 0.0f, pw = 0.0f, ph = 0.0f;
            CHECK(gs_ui_probe_window_box(id, &x, &y, &pw, &ph));

            const float left = (float)v[c].rect.x;
            const float top = (float)v[c].rect.y;
            const float right = left + (float)v[c].rect.w;
            const float bottom = top + (float)v[c].rect.h;

            measured++;
            if (x >= left && y >= top && x + pw <= right && y + ph <= bottom) {
                continue;
            }
            spilled++;
            printf("  HUD OUT OF ITS VIEW  %u players, state %zu "
                   "(%s%s%s%s%s) car %u: "
                   "%.0f,%.0f %.0fx%.0f in a view %.0f,%.0f %dx%d - "
                   "%.0f past the bottom, %.0f past the right (%s)\n",
                   cars, i, carrying ? "carrying " : "",
                   states[i].derby ? "derby " : "race ",
                   states[i].wrecked ? "wrecked " : "",
                   states[i].finished ? "finished " : "",
                   states[i].counting ? "counting" : "", v[c].car,
                   (double)x, (double)y, (double)pw, (double)ph,
                   (double)left, (double)top, v[c].rect.w, v[c].rect.h,
                   (double)(y + ph - bottom), (double)(x + pw - right),
                   sizes[z].what);
        }
    }

    }
    }

    // Two, three and four players: 2 + 3 + 4 views for each state and size.
    printf("  HUD %d panels measured: %d states, each with and without weapons, "
           "x (2+3+4) views x %d window sizes\n", measured,
           (int)SDL_arraysize(states), (int)SDL_arraysize(sizes));
    CHECK(measured == (int)SDL_arraysize(states) * 2 * (2 + 3 + 4) *
                      (int)SDL_arraysize(sizes));
    CHECK(spilled == 0);

    // **Back to the size the suite runs at.** The window is one thing shared by
    // every test in this binary, and the ones that read a frame back index it
    // as GS_W wide - so a test that resizes and does not put it back does not
    // fail itself, it fails whatever runs next. That is what this one did to
    // `a_start_line_and_a_finish_line_are_different_things`, five checks at
    // once, in a test about chequered paint.
    CHECK(SDL_SetWindowSize(gs_win, GS_W, GS_H));
    CHECK(SDL_SetRenderLogicalPresentation(ren, GS_W, GS_H,
                                           SDL_LOGICAL_PRESENTATION_DISABLED));
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

    // **Every screen, on a server and off one, with the construction set open
    // and shut.** This walked nine of the thirty-six, and worse than that: the
    // six it ran through a list were all measured with `online` left true from
    // two lines above, so what Escape does off a server on those six was never
    // asked at all. What it hid is below.
    static const struct {
        gs_screen screen;
        gs_screen alone;      // where back goes on this machine
        gs_screen served;     // and where it goes on somebody's server
    } way_out[] = {
        // Out of a race is the screen that set it up - unless the race belongs
        // to a server, when the setup screen is not this machine's to use and
        // the lobby is where another one is decided.
        { GS_SCREEN_RACE,     GS_SCREEN_SETUP,   GS_SCREEN_LOBBY },
        // **And out of the results, the same.** This was the title in both,
        // while the button on the screen beside it said "Back to the lobby":
        // one screen, two ways out, two different places, and the one a player
        // reaches for by reflex was the one that left the room.
        { GS_SCREEN_RESULTS,  GS_SCREEN_TITLE,   GS_SCREEN_LOBBY },
        // The rest are the same wherever the race came from.
        { GS_SCREEN_PROFILES, GS_SCREEN_TITLE,   GS_SCREEN_TITLE },
        { GS_SCREEN_SETUP,    GS_SCREEN_TITLE,   GS_SCREEN_TITLE },
        { GS_SCREEN_LOBBY,    GS_SCREEN_TITLE,   GS_SCREEN_TITLE },
        { GS_SCREEN_TRACKS,   GS_SCREEN_TITLE,   GS_SCREEN_TITLE },
        // The records screen goes back to whoever opened it, which is its own
        // rule and is walked separately below.
        { GS_SCREEN_RECORDS,  GS_SCREEN_TITLE,   GS_SCREEN_TITLE },
        // And the title and the door are where leaving belongs.
        { GS_SCREEN_TITLE,    GS_SCREEN_COUNT,   GS_SCREEN_COUNT },
        { GS_SCREEN_LOGIN,    GS_SCREEN_COUNT,   GS_SCREEN_COUNT },
    };

    // Every screen there is appears exactly once, so a screen added next year
    // is a red tree rather than a screen nobody asked about.
    CHECK((int)SDL_arraysize(way_out) == GS_SCREEN_COUNT);
    bool listed[GS_SCREEN_COUNT] = { false };
    for (size_t i = 0; i < SDL_arraysize(way_out); i++) {
        CHECK(!listed[way_out[i].screen]);
        listed[way_out[i].screen] = true;
    }
    for (int i = 0; i < GS_SCREEN_COUNT; i++) CHECK(listed[i]);

    // The two screens that are reachable from more than one place are walked
    // over every origin below; here they are set to the plain one so the table
    // above is asking about the ordinary path.
    m.records_from = GS_SCREEN_TITLE;
    m.setup_from = GS_SCREEN_TITLE;
    m.tracks_from = GS_SCREEN_TITLE;
    int walked = 0;
    for (size_t i = 0; i < SDL_arraysize(way_out); i++) {
        for (int served = 0; served < 2; served++) {
            m.screen = way_out[i].screen;
            m.online = served != 0;

            const gs_screen want = served ? way_out[i].served
                                          : way_out[i].alone;
            const gs_screen got = gs_menu_back(&m, false);
            if (got != want) {
                printf("  BACK from %s %s went to %d, wanted %d\n",
                       gs_screen_name(way_out[i].screen),
                       served ? "on a server" : "on this machine",
                       (int)got, (int)want);
            }
            CHECK(got == want);

            // **The construction set is a layer over whatever is underneath
            // it**, so closing it moves nobody anywhere - from every screen,
            // not from the one that happened to be set last.
            CHECK(gs_menu_back(&m, true) == way_out[i].screen);
            walked++;
        }
    }
    printf("  BACK %d ways out walked: %d screens, on a server and off one\n",
           walked, GS_SCREEN_COUNT);
    CHECK(walked == GS_SCREEN_COUNT * 2);

    // **And the records screen goes back to whoever opened it.** It is reached
    // from the title, from the setup screen and from the results, and it
    // remembers which - so Escape has to honour that too. It did not: the
    // button on the screen went back where you came from and Escape went to the
    // main menu, which is the same fault as the results screen wearing a
    // different hat.
    //
    // Walked over every screen as the place it came from, including the ones
    // nobody can arrive from, because the safe answer for those is the thing
    // being claimed.
    m.screen = GS_SCREEN_RECORDS;
    m.online = false;
    int froms = 0;
    for (int from = 0; from < GS_SCREEN_COUNT; from++) {
        m.records_from = (gs_screen)from;
        const bool arrivable = from == GS_SCREEN_TITLE ||
                               from == GS_SCREEN_RESULTS ||
                               from == GS_SCREEN_SETUP;
        const gs_screen want = arrivable ? (gs_screen)from : GS_SCREEN_TITLE;
        const gs_screen got = gs_menu_back(&m, false);
        if (got != want) {
            printf("  BACK records opened from %d went to %d, wanted %d\n",
                   from, (int)got, (int)want);
        }
        CHECK(got == want);
        froms++;
    }
    printf("  BACK records walked from all %d screens\n", froms);
    CHECK(froms == GS_SCREEN_COUNT);
    m.records_from = GS_SCREEN_TITLE;

    // **And the setup screen, which is the one this cost a race over.**
    //
    // Escape out of a race lands on the race setup. Back from there went to the
    // main menu - so a race stepped out of for a moment could not be stepped
    // back into, though it was still sitting in memory paused. The table above
    // walked every screen and asked the wrong question of this one: it pinned
    // "setup goes to the title" without ever asking where setup had been
    // reached *from*, so the fault was written down as the expected answer.
    //
    // Online is walked with it, because there the race belongs to the server
    // and Escape goes to the lobby - there is nothing here to step back into,
    // and claiming there is would be worse than the fault it replaced.
    m.screen = GS_SCREEN_SETUP;
    int setups = 0;
    for (int from = 0; from < GS_SCREEN_COUNT; from++) {
        for (int served = 0; served < 2; served++) {
            m.setup_from = (gs_screen)from;
            m.online = served != 0;

            const bool paused = !m.online && from == GS_SCREEN_RACE;
            const gs_screen want = paused ? GS_SCREEN_RACE : GS_SCREEN_TITLE;
            const gs_screen got = gs_menu_back(&m, false);
            if (got != want) {
                printf("  BACK setup opened from %s %s went to %d, wanted %d\n",
                       gs_screen_name((gs_screen)from),
                       served ? "on a server" : "on this machine",
                       (int)got, (int)want);
            }
            CHECK(got == want);
            CHECK(gs_menu_setup_is_paused(&m) == paused);
            setups++;
        }
    }
    printf("  BACK setup walked from all %d screens, on a server and off one\n",
           GS_SCREEN_COUNT);
    CHECK(setups == GS_SCREEN_COUNT * 2);
    m.setup_from = GS_SCREEN_TITLE;
    m.online = false;

    // **And the tracks list, which is the same fault costing a grid.** It is
    // opened from the main menu and from the setup screen, and Back went to the
    // main menu from both - so choosing a track for a race you were halfway
    // through setting up threw the setup away.
    m.screen = GS_SCREEN_TRACKS;
    int tracks = 0;
    for (int from = 0; from < GS_SCREEN_COUNT; from++) {
        m.tracks_from = (gs_screen)from;
        const gs_screen want = from == GS_SCREEN_SETUP ? GS_SCREEN_SETUP
                                                       : GS_SCREEN_TITLE;
        const gs_screen got = gs_menu_back(&m, false);
        if (got != want) {
            printf("  BACK tracks opened from %s went to %d, wanted %d\n",
                   gs_screen_name((gs_screen)from), (int)got, (int)want);
        }
        CHECK(got == want);
        tracks++;
    }
    printf("  BACK tracks walked from all %d screens\n", tracks);
    CHECK(tracks == GS_SCREEN_COUNT);
    m.tracks_from = GS_SCREEN_TITLE;
}

TEST(the_weapons_switch_on_the_setup_screen_arms_the_race) {
    (void)ren;

    // **The switch has to reach the grid**, which is the half a simulation test
    // cannot see: gs_world_arm was written and tested for a while with nothing
    // but tests calling it, which is exactly how the mine came to be
    // implemented and undroppable in the first place.
    static gs_track t;
    gs_track_init(&t, 40, 20, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= t.h; y++)
        for (uint8_t x = 0; x <= t.w; x++) gs_track_set_corner(&t, x, y, 0);

    static gs_race_setup s;
    SDL_zero(s);
    s.players = GS_MAX_CARS;
    s.mode = (uint8_t)GS_MODE_RACE;
    s.laps = 3;
    s.gravity = GS_ONE;
    for (int k = GS_HAZ_NONE + 1; k < GS_HAZ_COUNT; k++) {
        s.ammo[k] = (uint8_t)(k + 1);
    }

    // **Off is a race with nothing in it**, however the counts are set - which
    // is what makes the switch a switch rather than a thing you have to zero
    // four dials to get.
    static gs_world w;
    s.weapons = false;
    gs_setup_build(&s, &t, &w);
    CHECK(w.car_count == GS_MAX_CARS);
    for (uint8_t i = 0; i < w.car_count; i++) {
        CHECK(gs_car_selected(&w.car[i]) == GS_HAZ_NONE);
        for (int k = GS_HAZ_NONE + 1; k < GS_HAZ_COUNT; k++) {
            CHECK(gs_car_ammo(&w.car[i], (gs_hazard_kind)k) == 0);
        }
    }

    // On, and **everybody on the grid gets the same**, including the fourth car
    // added last - the loadout is set before anybody is placed so it cannot
    // depend on the order this screen happens to build a race in.
    s.weapons = true;
    gs_setup_build(&s, &t, &w);
    CHECK(w.car_count == GS_MAX_CARS);
    for (uint8_t i = 0; i < w.car_count; i++) {
        for (int k = GS_HAZ_NONE + 1; k < GS_HAZ_COUNT; k++) {
            CHECK(gs_car_ammo(&w.car[i], (gs_hazard_kind)k) == (uint8_t)(k + 1));
        }
        CHECK(gs_car_selected(&w.car[i]) == GS_HAZ_OIL);   // the first it has
    }

    // And a race with weapons is filed apart from one without, so a lap set
    // with people dropping oil is not offered beside a clean one.
    static gs_world clean;
    s.weapons = false;
    gs_setup_build(&s, &t, &clean);
    CHECK(gs_conditions_hash(&w) != gs_conditions_hash(&clean));

    // One count at zero is that weapon absent and the others still there,
    // which is what four dials rather than one switch is for.
    s.weapons = true;
    s.ammo[GS_HAZ_OIL] = 0;
    gs_setup_build(&s, &t, &w);
    CHECK(gs_car_ammo(&w.car[0], GS_HAZ_OIL) == 0);
    CHECK(gs_car_ammo(&w.car[0], GS_HAZ_MINE) > 0);
    CHECK(gs_car_selected(&w.car[0]) == GS_HAZ_MINE);
}

TEST(the_empty_seats_on_the_grid_are_filled_with_somebody) {
    (void)ren;

    // **A race set up for four with one person at the keyboard used to be one
    // car going round on its own** and three sitting on the grid. There is a
    // driver in this game and there was no way to race it: every car took its
    // input from a pad, so the AI drove only in headless self-play, the
    // editor's background ghost and the demo.
    //
    // A slot marked as the game's is driven by it, at the skill on the dial,
    // and what the setup screen means is written where a test can reach it
    // rather than in the client.
    static gs_track t;
    gs_track_init(&t, 60, 60, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= t.h; y++) {
        for (uint8_t x = 0; x <= t.w; x++) gs_track_set_corner(&t, x, y, 0);
    }
    gs_track_add_gate(&t, GS_INT(45), GS_INT(15), 0, GS_INT(6));
    gs_track_add_gate(&t, GS_INT(45), GS_INT(45), GS_QUARTER, GS_INT(6));
    gs_track_add_gate(&t, GS_INT(15), GS_INT(45), (gs_angle)(GS_QUARTER * 2), GS_INT(6));
    gs_track_add_gate(&t, GS_INT(15), GS_INT(15), (gs_angle)(GS_QUARTER * 3), GS_INT(6));

    // **A loop, and now it has to say so.** Four gates round a square with the
    // headings of a loop, left as a path: read as a path, the way through the
    // first gate is the line to the second, which is ninety degrees from where
    // it points. Validation says so now, and this fixture always meant a
    // circuit.
    t.route = (uint8_t)GS_ROUTE_CIRCUIT;
    CHECK(gs_track_validate(&t).problem == GS_TRACK_OK);

    static gs_menu m;
    gs_menu_init(&m);
    m.setup.players = GS_MAX_CARS;
    m.setup.laps = 1;
    m.setup.mode = (uint8_t)GS_MODE_RACE;
    m.setup.skill = GS_AI_SKILL_STEPS;        // opponents worth racing
    m.setup.computer[0] = false;              // the person at the keyboard
    for (uint8_t i = 1; i < GS_MAX_CARS; i++) m.setup.computer[i] = true;

    // **The world the setup screen describes**, built by the rule the client
    // uses rather than by hand here.
    gs_world w;
    gs_setup_build(&m.setup, &t, &w);
    CHECK(w.car_count == GS_MAX_CARS);
    CHECK(w.laps_to_win == 1);
    CHECK(w.mode == (uint8_t)GS_MODE_RACE);

    // **What the person presses is left exactly as it came in.** Filling in a
    // slot somebody is driving would be the game taking the wheel off them.
    {
        gs_input in[GS_MAX_CARS] = { (gs_input)GS_IN_LEFT, 0, 0, 0 };
        gs_setup_drive(&m.setup, &w, &t, in);
        CHECK(in[0] == (gs_input)GS_IN_LEFT);
        for (uint8_t i = 1; i < GS_MAX_CARS; i++) CHECK(in[i] != 0);
    }

    // And the race, with nobody at the keyboard: the person's car sits on the
    // grid, which is exactly the case this item is about.
    for (uint32_t k = 0; k < (uint32_t)GS_TICK_HZ * 240u && !w.over; k++) {
        gs_input in[GS_MAX_CARS] = { 0, 0, 0, 0 };
        gs_setup_drive(&m.setup, &w, &t, in);
        gs_world_step(&w, &t, in);
    }

    int timed = 0;
    for (uint8_t i = 0; i < w.car_count; i++) {
        if (w.car[i].finish_tick != 0) timed++;
    }
    printf("  GRID %d of %u cars finished, winner %u\n", timed, w.car_count,
           w.winner);

    // **More than one car has a time**, which is the whole claim: the empty
    // seats raced.
    CHECK(timed > 1);
    CHECK(timed == GS_MAX_CARS - 1);   // everybody who was driving got round

    // The race is *not* over, and that is right: one car is still on the grid
    // with nobody in it, and a race is over when everybody has been dealt with.
    // What matters here is that somebody won it.
    CHECK(!w.over);
    CHECK(w.winner != GS_NO_WINNER);

    // **And the winner is not the person**, who never touched a control. An
    // opponent that cannot beat a parked car is not an opponent.
    CHECK(w.winner != 0);
    CHECK(w.car[0].finish_tick == 0);

    // **Nor do the game's cars go in the records.** A table with the computer
    // at the top of it is a table nobody can get on.
    gs_menu_finish(&m, &w, &t);
    CHECK(m.result_count == GS_MAX_CARS);
    CHECK(m.records.count == 0);
}

TEST(an_opponent_finishes_every_track_that_ships_from_every_grid_slot) {
    (void)ren;

    // **The tracks in the box, not the one the test built.** A driver that gets
    // round a circuit written to suit it and then sits in the run-off on
    // something a person would actually race is a driver nobody meets. So every
    // `.gstrack` that ships is loaded and raced - from **every slot on the
    // grid**, because the slots are staggered back from the line and across it,
    // and the car in the last one has a different first corner to make.
    int count = 0;
    char **found = SDL_GlobDirectory(GS_SOURCE_ASSETS "/tracks", "*.gstrack",
                                     SDL_GLOB_CASEINSENSITIVE, &count);
    CHECK(found != nullptr);
    if (found == nullptr) return;

    // The set that ships, asserted rather than assumed: a directory that has
    // quietly emptied would otherwise pass this in no time at all.
    printf("  STOCK %d tracks in assets/tracks\n", count);
    CHECK(count >= 16);

    int raced = 0, stuck = 0;
    gs_fix shortest = INT32_MAX, longest = 0;
    for (int i = 0; i < count; i++) {
        char path[1024];
        SDL_snprintf(path, sizeof path, "%s/tracks/%s", GS_SOURCE_ASSETS, found[i]);

        size_t len = 0;
        void *raw = SDL_LoadFile(path, &len);
        CHECK(raw != nullptr);
        if (raw == nullptr) continue;

        static gs_track t;
        const bool read = gs_track_deserialize(&t, (const uint8_t *)raw, len);
        SDL_free(raw);
        CHECK(read);
        if (!read) continue;
        CHECK(gs_track_validate(&t).problem == GS_TRACK_OK);

        // **And it is a race, not a demonstration.** The tool that writes these
        // refuses anything under the floor, but the build never runs the tool -
        // it only compiles it - so until this line the eighteen files in the
        // box were held to nothing. That is exactly how a set of twenty-seven
        // second tracks shipped once already: the rule existed, in a place that
        // was not consulted by anything that ran.
        const gs_fix route = gs_track_route_length(&t);
        printf("  STOCK %-26s %4d tiles of route\n", found[i],
               (int)(route / GS_ONE));
        CHECK(route >= GS_INT(GS_STOCK_MIN_ROUTE));
        if (route < shortest) shortest = route;
        if (route > longest) longest = route;

        for (uint8_t slot = 0; slot < GS_MAX_CARS; slot++) {
            gs_world w;
            gs_world_init(&w, GS_ONE);
            gs_world_set_mode(&w, GS_MODE_RACE);
            gs_world_set_laps(&w, 1);

            gs_fix sx = 0, sy = 0;
            gs_angle facing = 0;
            gs_track_grid(&t, slot, &sx, &sy, &facing);
            gs_world_add_car(&w, &t, (uint8_t)GS_VEH_STOCK_CAR, sx, sy, facing);

            // **As long as the track says a lap of it takes.** Four minutes
            // was generous for a fifty-tile route and is a quarter of a
            // thousand-tile one, so every shipped track came back "stuck" the
            // day the routes got long. gs_analyse_seconds is the project's own
            // answer, and it scales with the route rather than with a number
            // typed here.
            uint32_t budget = gs_analyse_seconds(&t) * (uint32_t)GS_TICK_HZ;
            for (uint32_t k = 0; k < budget; k++) {
                gs_input in[GS_MAX_CARS] = { gs_ai_drive(&w, &t, 0), 0, 0, 0 };
                gs_world_step(&w, &t, in);
                if (w.car[0].finish_tick != 0) break;
            }

            if (w.car[0].finish_tick == 0) {
                printf("  STUCK on %s from slot %u, at %.1f,%.1f after %d laps\n",
                       found[i], slot, (double)w.car[0].x / 65536.0,
                       (double)w.car[0].y / 65536.0,
                       gs_car_laps_done(&t, &w.car[0]));
                stuck++;
            }
            raced++;
        }
    }
    SDL_free(found);

    printf("  STOCK %d races over %d tracks, %d of them stuck\n",
           raced, count, stuck);
    printf("  STOCK route %d to %d tiles, and the floor is %d\n",
           (int)(shortest / GS_ONE), (int)(longest / GS_ONE),
           GS_STOCK_MIN_ROUTE);
    CHECK(raced == count * GS_MAX_CARS);
    CHECK(stuck == 0);
    CHECK(shortest >= GS_INT(GS_STOCK_MIN_ROUTE));
}

TEST(no_test_writes_where_a_player_keeps_their_things) {
    (void)ren;

    // **The suite got better at pressing buttons and started deleting people's
    // work.**
    //
    // The construction set saves the track being built and the bindings
    // somebody has chosen into the preferences directory, and the walk presses
    // every control in it. Those three buttons - `save`, `load` and the one
    // that puts the controls back to their defaults - sat below the fold of a
    // panel for as long as the panel was too short, so nothing had ever pressed
    // them. The day the walk learned to wind a panel down, running `ctest`
    // began overwriting a real player's current track and their controls.
    //
    // It went unnoticed because the files it wrote happened to match the ones
    // already there. On the next machine it would not have.
    //
    // So every test runs with its preferences pointed at a throwaway inside the
    // build tree, and this says so out loud: if that ever stops being true the
    // tree goes red, rather than somebody losing what they were building.
    const char *where = gs_pref_dir();
    CHECK(where != nullptr);
    if (where == nullptr) return;

    printf("  PREFS tests keep theirs in %s\n", where);
    CHECK(SDL_strstr(where, GS_TEST_HOME) == where);
}

// A pad that does not exist, so the code that reads one can be run at all.
// SDL builds a real gamepad out of this: opened, polled and closed exactly like
// something plugged into a socket.
static SDL_JoystickID gs_fake_pad(void) {
    SDL_VirtualJoystickDesc desc;
    // SDL's own initialiser: it stamps the interface version into the struct,
    // which is how SDL tells a caller built against this header from one built
    // against a later one. Zeroing it by hand says version 0 and is refused.
    SDL_INIT_INTERFACE(&desc);
    desc.type = (Uint16)SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = (Uint16)SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = (Uint16)SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "gearstick test pad";
    return SDL_AttachVirtualJoystick(&desc);
}

// Hand SDL's own gamepad events to the game, the way the client does.
//
// **And put the keyboard down afterwards.** Draining the queue is what makes
// SDL notice a pad arriving, and it also makes SDL apply every key event some
// earlier test left sitting in it - which SDL then reports as held, forever,
// because the matching key-up was never queued. `gs_input_poll` *adds* the
// keyboard to the pads rather than choosing between them, deliberately, so a
// stray Up arrow arrives as car one accelerating and reads exactly like a pad
// driving the wrong car. It cost twenty minutes to tell those two apart.
static void gs_pad_events(gs_input_state *in) {
    SDL_PumpEvents();
    SDL_Event e;
    while (SDL_PollEvent(&e)) gs_input_event(in, &e);
}

// **Read the pads, with the keyboard definitely down.**
//
// `gs_input_poll` *adds* the keyboard to the pads rather than choosing between
// them - deliberately, so a pad and the arrow keys can drive the same car and
// neither disables the other. In this binary that means every key event some
// earlier test left sitting in SDL's queue, applied the moment anything drains
// it and then held forever because the matching key-up was never queued. It
// arrives as car one accelerating and reads exactly like a pad driving the
// wrong car, which is a confusing twenty minutes.
//
// So the keyboard is put down before every read here. This test is about pads.
static void gs_pad_poll(gs_input_state *in, gs_input *out) {
    SDL_PumpEvents();
    SDL_ResetKeyboard();
    gs_input_poll(in, out, GS_MAX_CARS);
}

TEST(a_pad_is_opened_read_and_closed_the_way_a_person_plugs_one_in) {
    (void)ren;
    // **None of this had ever run.** Coverage over the whole suite put
    // src/platform/gs_input.c at 24% of its lines: opening a pad, closing one,
    // hotplug, and every line that reads a physical control had never been
    // executed by any test, on any platform, once. Everything the game claims
    // about pads rested on code nothing had touched.
    //
    // A virtual joystick is SDL's own answer to that. It is a real gamepad as
    // far as every call below is concerned - opened, polled, closed - and its
    // buttons are set from here instead of by a thumb.
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        printf("  FAIL no gamepad subsystem: %s\n", SDL_GetError());
        CHECK(false);
        return;
    }

    static gs_input_state in;
    gs_input_init(&in);
    CHECK(in.pads == 0);

    // **Four of them, which is what this game is for.** Plugged in one at a
    // time, because that is how a fourth person arriving actually happens.
    SDL_JoystickID id[GS_MAX_CARS + 1];
    for (int i = 0; i < GS_MAX_CARS; i++) {
        id[i] = gs_fake_pad();
        CHECK(id[i] != 0);
        gs_pad_events(&in);
        CHECK(in.pads == i + 1);
    }

    // **A fifth is refused rather than remembered.** There are four cars.
    id[GS_MAX_CARS] = gs_fake_pad();
    CHECK(id[GS_MAX_CARS] != 0);
    gs_pad_events(&in);
    CHECK(in.pads == GS_MAX_CARS);

    gs_input out[GS_MAX_CARS];

    // Nothing held: nobody is asking for anything.
    SDL_UpdateGamepads();
    gs_pad_poll(&in, out);
    for (uint8_t i = 0; i < GS_MAX_CARS; i++) CHECK(out[i] == 0);

    // **Pad N drives car N, and only car N.** Walked over every pad rather
    // than checked on one, because "the second pad drives the second car" is
    // the whole claim and it is per pad.
    for (int i = 0; i < GS_MAX_CARS; i++) {
        SDL_Joystick *j = SDL_GetJoystickFromID(id[i]);
        CHECK(j != nullptr);
        if (j == nullptr) continue;

        CHECK(SDL_SetJoystickVirtualButton(j, (int)SDL_GAMEPAD_BUTTON_SOUTH,
                                           true));
        SDL_UpdateGamepads();
        gs_pad_poll(&in, out);

        for (uint8_t k = 0; k < GS_MAX_CARS; k++) {
            if ((int)k == i) CHECK((out[k] & GS_IN_ACCEL) != 0);
            else             CHECK(out[k] == 0);
        }

        CHECK(SDL_SetJoystickVirtualButton(j, (int)SDL_GAMEPAD_BUTTON_SOUTH,
                                           false));
        SDL_UpdateGamepads();
    }

    SDL_Joystick *one = SDL_GetJoystickFromID(id[0]);
    CHECK(one != nullptr);
    if (one == nullptr) return;

    // **Every button that is bound by default**, so a control that stopped
    // being read shows up here rather than in somebody's first corner.
    const struct { SDL_GamepadButton button; gs_input want; const char *what; }
    bound[] = {
        { SDL_GAMEPAD_BUTTON_SOUTH,      GS_IN_ACCEL, "accelerate" },
        { SDL_GAMEPAD_BUTTON_EAST,       GS_IN_BRAKE, "brake" },
        { SDL_GAMEPAD_BUTTON_DPAD_LEFT,  GS_IN_LEFT,  "left" },
        { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, GS_IN_RIGHT, "right" },
        { SDL_GAMEPAD_BUTTON_WEST,       GS_IN_FIRE,  "fire" },
    };
    for (size_t b = 0; b < SDL_arraysize(bound); b++) {
        CHECK(SDL_SetJoystickVirtualButton(one, (int)bound[b].button, true));
        SDL_UpdateGamepads();
        gs_pad_poll(&in, out);
        if ((out[0] & bound[b].want) == 0) {
            printf("  PAD %s did nothing\n", bound[b].what);
        }
        CHECK((out[0] & bound[b].want) != 0);
        CHECK(SDL_SetJoystickVirtualButton(one, (int)bound[b].button, false));
        SDL_UpdateGamepads();
    }

    // **The triggers stand in for the two buttons everybody drives with**, so
    // somebody who accelerates with a trigger is asking for the same thing as
    // somebody who presses the bottom button.
    //
    // **A released trigger is not zero.** A gamepad reports a trigger over 0 to
    // 32767, and SDL maps that from a joystick axis whose range is -32768 to
    // 32767 - so writing 0 to the axis, which is the obvious way to say "let
    // go", is a trigger held at half travel. Which is over the threshold, and
    // reads as accelerate and brake held together on car one, and looks exactly
    // like a pad driving the wrong car. Letting go is the minimum.
    const Sint16 released = SDL_JOYSTICK_AXIS_MIN;
    const struct { SDL_GamepadAxis axis; gs_input want; const char *what; }
    trigger[] = {
        { SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, GS_IN_ACCEL, "right trigger" },
        { SDL_GAMEPAD_AXIS_LEFT_TRIGGER,  GS_IN_BRAKE, "left trigger" },
    };
    for (size_t k = 0; k < SDL_arraysize(trigger); k++) {
        CHECK(SDL_SetJoystickVirtualAxis(one, (int)trigger[k].axis, 20000));
        SDL_UpdateGamepads();
        gs_pad_poll(&in, out);
        if ((out[0] & trigger[k].want) == 0) {
            printf("  PAD %s did nothing\n", trigger[k].what);
        }
        CHECK((out[0] & trigger[k].want) != 0);

        CHECK(SDL_SetJoystickVirtualAxis(one, (int)trigger[k].axis, released));
        SDL_UpdateGamepads();
        gs_pad_poll(&in, out);
        CHECK(out[0] == 0);            // and letting go really lets go
    }

    // **And the stick steers, past a deadzone and not before it.** A stick
    // resting slightly off centre must not steer, or a worn pad drives into a
    // wall on its own - which is the entire reason there is a deadzone.
    const struct { Sint16 at; gs_input want; const char *what; } stick[] = {
        { -30000, GS_IN_LEFT,  "hard left" },
        {  30000, GS_IN_RIGHT, "hard right" },
        {  -8000, 0,           "resting a little left" },
        {   8000, 0,           "resting a little right" },
        {      0, 0,           "centred" },
    };
    for (size_t k = 0; k < SDL_arraysize(stick); k++) {
        CHECK(SDL_SetJoystickVirtualAxis(one, (int)SDL_GAMEPAD_AXIS_LEFTX,
                                         stick[k].at));
        SDL_UpdateGamepads();
        gs_pad_poll(&in, out);
        const gs_input steer = out[0] & (gs_input)(GS_IN_LEFT | GS_IN_RIGHT);
        if (steer != stick[k].want) {
            printf("  PAD %s steered %u, wanted %u\n", stick[k].what,
                   (unsigned)steer, (unsigned)stick[k].want);
        }
        CHECK(steer == stick[k].want);
    }
    CHECK(SDL_SetJoystickVirtualAxis(one, (int)SDL_GAMEPAD_AXIS_LEFTX, 0));
    SDL_UpdateGamepads();

    // **Somebody trips over a cable.** The hole is closed rather than left, so
    // the three pads still in somebody's hands drive the first three cars -
    // and, more to the point, nothing reads a closed pad afterwards.
    CHECK(SDL_DetachVirtualJoystick(id[1]));
    gs_pad_events(&in);
    CHECK(in.pads == GS_MAX_CARS - 1);

    SDL_UpdateGamepads();
    gs_pad_poll(&in, out);
    CHECK(out[GS_MAX_CARS - 1] == 0);       // nothing drives the last car now

    // The pad that was third is second now, and it drives the second car.
    SDL_Joystick *third = SDL_GetJoystickFromID(id[2]);
    CHECK(third != nullptr);
    if (third != nullptr) {
        CHECK(SDL_SetJoystickVirtualButton(third, (int)SDL_GAMEPAD_BUTTON_SOUTH,
                                           true));
        SDL_UpdateGamepads();
        gs_pad_poll(&in, out);
        CHECK((out[1] & GS_IN_ACCEL) != 0);
        CHECK(out[0] == 0);
        CHECK(SDL_SetJoystickVirtualButton(third, (int)SDL_GAMEPAD_BUTTON_SOUTH,
                                           false));
    }

    // And unplugging one that was never opened changes nothing.
    CHECK(SDL_DetachVirtualJoystick(id[GS_MAX_CARS]));
    gs_pad_events(&in);
    CHECK(in.pads == GS_MAX_CARS - 1);

    for (int i = 0; i < GS_MAX_CARS; i++) {
        if (i == 1) continue;
        SDL_DetachVirtualJoystick(id[i]);
    }
    gs_pad_events(&in);
    CHECK(in.pads == 0);

    printf("  PAD four opened, %d bound buttons, both triggers, %d stick "
           "positions, one unplugged mid-race\n", (int)SDL_arraysize(bound),
           (int)SDL_arraysize(stick));

    gs_input_quit(&in);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

TEST(a_pad_can_leave_a_screen_without_walking_to_the_button) {
    (void)ren;

    // **Escape is a key, and a pad has none of those.**
    //
    // So somebody on a pad could only leave a screen by walking to the button
    // that says so - and on the tracks screen the nav order puts the whole
    // library between them and it, one track at a time. Thirty-two presses to
    // get out of a screen you opened by mistake.
    //
    // The pad's cancel button is back as well now. Where back goes is still
    // gs_menu_back's to say; this is only about what counts as asking.
    SDL_Event key = { 0 };
    key.type    = SDL_EVENT_KEY_DOWN;
    key.key.key = SDLK_ESCAPE;
    CHECK(gs_input_is_back(&key, false));
    CHECK(gs_input_is_back(&key, true));       // a key means it during a race too

    SDL_Event pad = { 0 };
    pad.type          = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    pad.gbutton.button = (uint8_t)SDL_GAMEPAD_BUTTON_EAST;
    CHECK(gs_input_is_back(&pad, false));

    // **Except while a race is on, where that button is the brake.** Not a
    // guess: it is the binding this game ships, checked here so that moving the
    // brake and leaving this rule behind is a failure rather than a surprise in
    // the first corner.
    gs_bindings b;
    gs_bind_defaults(&b);
    CHECK(b.button[0][GS_ACT_BRAKE] == (int16_t)SDL_GAMEPAD_BUTTON_EAST);
    CHECK(!gs_input_is_back(&pad, true));

    // **Every other button on the pad, and every other key**, because a cancel
    // that is also three other things is worse than no cancel.
    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++) {
        if (i == (int)SDL_GAMEPAD_BUTTON_EAST) continue;
        pad.gbutton.button = (uint8_t)i;
        CHECK(!gs_input_is_back(&pad, false));
        CHECK(!gs_input_is_back(&pad, true));
    }

    static const SDL_Keycode others[] = {
        SDLK_RETURN, SDLK_SPACE, SDLK_TAB, SDLK_G, SDLK_J, SDLK_R, SDLK_H,
        SDLK_M, SDLK_F5, SDLK_F9, SDLK_UP, SDLK_DOWN, SDLK_A, SDLK_Z,
    };
    for (size_t i = 0; i < SDL_arraysize(others); i++) {
        key.key.key = others[i];
        CHECK(!gs_input_is_back(&key, false));
        CHECK(!gs_input_is_back(&key, true));
    }

    // **Only a key can ask to quit.** Backing out of the title screen means
    // leaving the game, which the title screen says in as many words. A pad's
    // cancel is the button everybody presses to go back one step, reflexively,
    // and having it close the game from the title is not something anybody
    // asked for - so where there is nothing behind the screen it does nothing.
    key.type    = SDL_EVENT_KEY_DOWN;
    key.key.key = SDLK_ESCAPE;
    CHECK(gs_input_back_may_quit(&key));

    pad.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    pad.gbutton.button = (uint8_t)SDL_GAMEPAD_BUTTON_EAST;
    CHECK(gs_input_is_back(&pad, false));
    CHECK(!gs_input_back_may_quit(&pad));

    // And the title is where there is nothing behind: a key leaves the game
    // from it and a pad does not.
    {
        static gs_menu title;
        gs_menu_init(&title);
        title.screen = GS_SCREEN_TITLE;
        CHECK(gs_menu_back(&title, false) == GS_SCREEN_COUNT);
    }

    CHECK(!gs_input_back_may_quit(nullptr));

    // And a button going *up* is not a press.
    pad.type = SDL_EVENT_GAMEPAD_BUTTON_UP;
    pad.gbutton.button = (uint8_t)SDL_GAMEPAD_BUTTON_EAST;
    CHECK(!gs_input_is_back(&pad, false));
    key.type = SDL_EVENT_KEY_UP;
    key.key.key = SDLK_ESCAPE;
    CHECK(!gs_input_is_back(&key, false));

    CHECK(!gs_input_is_back(nullptr, false));
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

// How many tracks the panel menu and the walk are given. Enough that the list
// scrolls and has rows below the fold, which is the state the tracks screen has
// to be measured and walked in; deliberately not GS_LIBRARY_MAX, which costs a
// great deal and shows the walk nothing it has not already seen. See the seed
// below for the whole reason.
#define GS_WALK_LIBRARY 32

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

    // **A library longer than the panel can show, which is what the walk needs
    // from it.** It used to be GS_LIBRARY_MAX, and when that went from
    // thirty-two to sixty-four the walk stopped finishing: a list twice as long
    // takes more wheel actions to reach the end of, so paths get longer, and a
    // breadth-first walk over longer paths is not twice the work. What the
    // tracks screen has to be shown here is a list that scrolls and rows below
    // the fold - not the largest library that can exist, which is pinned by
    // a_store_with_tracks_in_it_is_saved_whole for a hundredth of the cost.
    //
    // Each track differs by one corner so that each one hashes differently and
    // the library keeps all of them rather than folding them into one entry.
    for (int i = 0; i < GS_WALK_LIBRARY; i++) {
        gs_track_init(t, 32, 32, GS_SURF_PAVEMENT);
        // Inside the track, for the reason written out in
        // a_store_with_tracks_in_it_is_saved_whole: a corner outside it is not
        // serialised and two entries then fold into one.
        t->corner[(size_t)(i / 33) * GS_CORNER_STRIDE + (size_t)(i % 33)] =
            (int16_t)(i + 1);
        char name[GS_LIBRARY_NAME];
        SDL_snprintf(name, sizeof name, "track number %d", i + 1);
        CHECK(gs_library_put(&m->library, t, name, "somebody") >= 0);
    }
    CHECK(m->library.count == GS_WALK_LIBRARY);
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
        // **A menu is a megabyte and Windows gives a thread one.** So it is
        // static and assigned rather than a local copy: at half a megabyte
        // this was inside the stack a Windows thread gets by default and at a
        // megabyte it is not, and the render tests segfaulted on MSVC while
        // every other platform, with its eight-megabyte stack, was green. The
        // header over gs_library says a heap or static object rather than a
        // local; this is that rule, and the reason it is written down.
        static gs_menu m;
        m = *fresh;
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

// What a sweep remembers about one control: whether any scroll position ever
// showed the whole of it.
typedef struct gs_reach {
    uint32_t id;
    char     label[GS_UI_LABEL];
    bool     whole;
    bool     reachable, disabled;
    float    x0, y0, x1, y1;       // where it was on the frame it arrived on
} gs_reach;

static int gs_ui_controls(gs_ui *ui, gs_menu *fresh, gs_track *t,
                          SDL_Renderer *ren, gs_screen from, gs_ui_item *into,
                          int cap) {
    // Static rather than a local, for the reason in gs_ui_exits: a menu does
    // not fit on a Windows thread's stack.
    static gs_menu m;
    m = *fresh;
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

        // Static rather than a local - see gs_ui_exits.
        static gs_menu m;
        m = *fresh;
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
        // **This walk is Tab and Space and nothing else**, which is what makes
        // it worth keeping next to the one that presses by name: it is the only
        // thing here that asks what somebody with a keyboard and no mouse can
        // reach. Where the two disagree, the disagreement is the finding.
        //
        // **Three screens have no exit it can reach**, each for a reason
        // written down rather than discovered:
        //
        //  - the sign-in door leaves by signing in, which needs a password
        //    typed, or by quitting, which is not a screen at all;
        //  - the tracks screen puts a whole library of selectable rows before
        //    its buttons;
        //  - the setup screen puts the grid table before its buttons, and Tab
        //    does not walk out of a table's rows the way it walks a plain
        //    window: pressing by name finds Back and GO exactly where the probe
        //    says they are, and no number of Tab presses lands on either. Both
        //    are real complaints about those screens and both are written up as
        //    such. Neither is a trap: the walk next door proves every screen has
        //    a way off it and can get home, Escape leaves any of them, and so
        //    does a pad's cancel button.
        bool may_have_none = from == GS_SCREEN_LOGIN || from == GS_SCREEN_TRACKS ||
                             from == GS_SCREEN_SETUP;
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
            gs_every_screen[i] == GS_SCREEN_TRACKS ||
            gs_every_screen[i] == GS_SCREEN_SETUP) {
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
        if (!home) {
            printf("  NO WAY HOME from %s, by Tab and Space alone\n",
                   gs_screen_name(gs_every_screen[i]));
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
    bool         halved;        // drawn half off the edge of one
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
    int          n_halved;      // drawn with part of it past the panel's edge

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
    w->named[i].halved  = false;
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
        // **And a control the panel cuts in half.** ImGui's clip test is an
        // overlap, so a button hanging off the right-hand edge is still
        // "visible" and can still be pressed by name - and to a person it is a
        // word cut in two. That is the fault the brush palette had, and the one
        // a gravity button had the day a slider beside it grew: the control is
        // there, the machine finds it, and nobody can read what it says.
        //
        // **Asked in pixels**, because the clip flags cannot answer it: what
        // the hook is handed has already been clipped, so a button hanging off
        // the edge arrives looking like a narrower button that fits. So the
        // item's rectangle is compared with its window's, which is the question
        // a person answers by looking.
        //
        // Only where nothing scrolls: a row at the edge of a list is cut by the
        // list, which is what a list is.
        for (int i = 0; i < held; i++) {
            if (!items[i].visible) continue;
            if (!items[i].reachable || items[i].disabled) continue;
            if (items[i].label[0] == '\0') continue;
            if (gs_chrome(items[i].label, items[i].window)) continue;

            float across = 0.0f, down = 0.0f;
            if (!gs_ui_probe_scroll_at(items[i].window, &across, &down)) continue;
            if (down > 0.0f) continue;

            float wx = 0.0f, wy = 0.0f, ww = 0.0f, wh = 0.0f;
            if (!gs_ui_probe_window_box(items[i].window, &wx, &wy, &ww, &wh)) continue;
            if (items[i].x0 >= wx - 0.5f && items[i].x1 <= wx + ww + 0.5f &&
                items[i].y0 >= wy - 0.5f && items[i].y1 <= wy + wh + 0.5f) {
                continue;
            }

            gs_walk_named *cut = gs_walk_named_of(w, items[i].id);
            if (cut == nullptr || !cut->halved) {
                w->n_halved++;
                printf("  OVER THE EDGE '%s' in '%s' on screen %s: %.0f..%.0f "
                       "against a panel %.0f..%.0f\n",
                       items[i].label, items[i].window, gs_screen_name(m.screen),
                       (double)items[i].x0, (double)items[i].x1,
                       (double)wx, (double)(wx + ww));
            }
            if (cut != nullptr) cut->halved = true;
        }

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

// **A machine nobody has driven yet**, which is what every player sees once and
// no other seed can reach: an empty roster changes the door itself.
static void gs_seed_nobody_has_driven_here(gs_menu *m) {
    m->profiles.count = 0;
    m->signed_in = -1;
    m->login_pick = -1;
    m->login_name[0] = '\0';
    m->login_password[0] = '\0';
}

// **A race waiting behind the setup screen.**
//
// Escape out of a race lands on the race setup, and there the buttons mean
// different things: GO starts a *new* race, Back returns to the one already
// running, and the main menu is its own button. No other starting state draws
// those, and the walk had never seen them - which is how "Back" came to abandon
// a paused race with nothing to say so and nothing to catch it.
static void gs_seed_a_race_to_go_back_to(gs_menu *m) {
    m->online = false;
    m->setup_from = GS_SCREEN_RACE;
}

static void gs_seed_a_track_that_shipped(gs_menu *m) {
    if (m->library.count == 0) return;
    m->library.entry[0].builtin = true;
    m->picked = 0;
}

// **The longest thing that can be typed into a field, in every field.** A
// layout measured with "gavin" in it is a layout measured at its narrowest, and
// a name is the one piece of a screen the person using it chooses the width of.
// W rather than a real name because it is the widest glyph there is: what has to
// survive is not a plausible name, it is the widest one the field will hold.
static void gs_widest(char *out, size_t cap) {
    if (cap < 2) return;
    for (size_t i = 0; i + 1 < cap; i++) out[i] = 'W';
    out[cap - 1] = '\0';
}

static void gs_seed_the_longest_names_that_fit(gs_menu *m) {
    for (uint8_t i = 0; i < m->profiles.count; i++) {
        gs_widest(m->profiles.entry[i].name, sizeof m->profiles.entry[i].name);
    }
    for (uint16_t i = 0; i < m->library.count; i++) {
        gs_widest(m->library.entry[i].name, sizeof m->library.entry[i].name);
        gs_widest(m->library.entry[i].author, sizeof m->library.entry[i].author);
    }
    for (uint16_t i = 0; i < m->records.count; i++) {
        gs_widest(m->records.entry[i].who, sizeof m->records.entry[i].who);
    }
    // The menu holds the lobby as something it reads and never writes, so the
    // names are changed where they live - in the one the test filled.
    if (m->lobby == &gs_panel_lobby) {
        for (uint8_t i = 0; i < gs_panel_lobby.count; i++) {
            gs_widest(gs_panel_lobby.player[i].name,
                      sizeof gs_panel_lobby.player[i].name);
        }
    }
    gs_widest(m->track_name, sizeof m->track_name);
    gs_widest(m->new_name, sizeof m->new_name);
    gs_widest(m->login_name, sizeof m->login_name);
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
    { "a race to go back to", gs_seed_a_race_to_go_back_to },
    { "nobody has driven here", gs_seed_nobody_has_driven_here },
    { "the longest names that fit", gs_seed_the_longest_names_that_fit },
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
    // Both facing the way the route between them goes, which is what
    // validation now asks of an edited track: a gate is a plane you cross, so
    // one turned across the route is one you drive along instead of through.
    // These used to be zero and a hundred and eighty, chosen for no reason.
    ed.brush        = GS_BRUSH_GATE;
    ed.gate_width   = 4.0f;
    ed.gate_heading = 126.0f;
    gs_editor_paint(&ed, &t, 32.0f, 32.0f);
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

    // **Driven by the simulation's own driver, not by a trick.**
    //
    // This used to hold the throttle below four tiles a second and the wheel
    // hard right, so the car drove a circle - and the two gates were laid where
    // that circle happened to pass. That is a coincidence between the gates'
    // positions and one car's turning radius, not a fact about the editor, and
    // it broke the moment the roster gained grip: the circle came out 17.04
    // tiles across against the 17.2 the gates are apart, and the car went round
    // and round for fifteen minutes a tile short of the checkpoint it owed.
    //
    // `gs_ai_drive` aims at the gate it owes, which is what the claim here
    // actually needs - that a track built with the tools a player has is one
    // that can be *raced*. It is also a pure function of the world, so this asks
    // the simulation rather than steering around it.
    for (uint32_t i = 0; i < GS_TICK_HZ * 60u * 15u && !w.over; i++) {
        gs_input in[GS_MAX_CARS] = { 0 };
        in[0] = gs_ai_drive(&w, &back, 0);
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

TEST(the_construction_set_keeps_its_panels_on_the_screen) {
    // **The editor is half the product and it was laid out for one screen.**
    //
    // Its three panels are the player's to move and resize, which is what a
    // tool panel should be - and they were placed and sized from constants
    // chosen while looking at a 1280x720 window. Measured anywhere else:
    //
    //   960x600   the parts box 304 pixels off the right-hand edge,
    //             the palette and the controls 104 below the bottom
    //   640x480   nineteen pixels of the parts box on screen and the rest not
    //
    // And nothing scrolled, in either direction, on any of them - because what
    // is off the *screen* is not what is off the *window*. A window knows what
    // did not fit inside it; ImGui clips it to the display and it never finds
    // out. Every number these panels already had said they were fine.
    //
    // ImGui keeps about nineteen pixels of a window reachable so it can always
    // be dragged back into view. That is a rescue, not a place to open in.
    static gs_editor e;
    CHECK(gs_editor_init(&e, 65536));

    static gs_track t;
    gs_flat_pavement(&t, 32, 32);

    static gs_ed ed;
    ed.e = &e;
    ed.t = &t;
    ed.view = (gs_view){ 0 };
    ed.input = (gs_input_state){ 0 };
    gs_bind_defaults(&ed.input.bind);

    gs_imgui_start(gs_win, ren);
    CHECK(gs_imgui_ready);
    if (!gs_imgui_ready) return;

    // The size the game opens at, two ordinary smaller ones, and one small
    // enough that a panel cannot have the width it wants - which is the case
    // the sideways scrollbar exists for.
    const struct { int w, h; } sizes[] = {
        { 1280, 720 }, { 960, 600 }, { GS_W, GS_H }, { 400, 300 },
    };
    static const char *const panels[] = {
        "Construction set", "Parts box", "Controls",
    };

    // The controls panel is shut by default; this is a test about panels.
    e.show_controls = true;

    // **Where they were, so they can be put back.** ImGui remembers a window's
    // position and size under its name for the rest of the process, and this
    // test moves and shrinks all three - so without this it would leave every
    // editor test after it walking panels the size of a 400x300 screen. It did,
    // and they went red in places that mention no windows at all.
    CHECK(SDL_SetWindowSize(gs_win, 1280, 720));
    CHECK(SDL_SetRenderLogicalPresentation(ren, 1280, 720,
                                           SDL_LOGICAL_PRESENTATION_DISABLED));
    ed.view.rect = (SDL_Rect){ 0, 0, 1280, 720 };
    gs_editor_toggle(&e, &ed.view);
    CHECK(e.active);
    for (int i = 0; i < 4; i++) gs_ed_frame(&ed);

    float was[3][4];
    for (size_t p = 0; p < SDL_arraysize(panels); p++) {
        CHECK(gs_ui_probe_window_box(panels[p], &was[p][0], &was[p][1],
                                     &was[p][2], &was[p][3]));
    }

    static gs_ui_item items[GS_UI_MAX_ITEMS];
    static gs_reach   reach[GS_UI_MAX_ITEMS];
    int measured = 0, outside = 0, lying = 0, controls = 0, cut = 0;

    for (size_t z = 0; z < SDL_arraysize(sizes); z++) {
        CHECK(SDL_SetWindowSize(gs_win, sizes[z].w, sizes[z].h));
        CHECK(SDL_SetRenderLogicalPresentation(ren, sizes[z].w, sizes[z].h,
                                               SDL_LOGICAL_PRESENTATION_DISABLED));
        ed.view.rect = (SDL_Rect){ 0, 0, sizes[z].w, sizes[z].h };
        if (!e.active) gs_editor_toggle(&e, &ed.view);

        // **Every brush, because the brush decides what the panel holds.** The
        // palette is one height with the gravity dial on it and another with
        // the parts list, and a panel that fits in one configuration says
        // nothing about the one next to it.
        // `e.brush` is an int because that is what ImGui edits, and the enum
        // is unsigned to clang and signed to gcc - so the cast has to name the
        // type it is being stored in rather than the one it came from.
        for (int brush = 0; brush < (int)GS_BRUSH_COUNT; brush++) {
            e.brush = brush;
            for (int i = 0; i < 4; i++) gs_ed_frame(&ed);

            for (size_t p = 0; p < SDL_arraysize(panels); p++) {
                float bx = 0.0f, by = 0.0f, bw = 0.0f, bh = 0.0f;
                CHECK(gs_ui_probe_window_box(panels[p], &bx, &by, &bw, &bh));
                CHECK(bw > 0.0f);
                CHECK(bh > 0.0f);

                measured++;

                // **On the screen, all of it.** Not "mostly", and not "there is
                // a corner you can grab".
                const float right = bx + bw - (float)sizes[z].w;
                const float bottom = by + bh - (float)sizes[z].h;
                if (bx < -0.5f || by < -0.5f || right > 0.5f || bottom > 0.5f) {
                    outside++;
                    printf("  OFF THE SCREEN '%s' at %dx%d with brush %d: "
                           "%.0f,%.0f %.0fx%.0f - %+.0f past the right, "
                           "%+.0f past the bottom\n",
                           panels[p], sizes[z].w, sizes[z].h, brush,
                           (double)bx, (double)by, (double)bw, (double)bh,
                           (double)right, (double)bottom);
                }

                // **And what does not fit scrolls, and says it scrolls.** The
                // same rule the front end's panels are held to: a bar that
                // moves nothing is furniture, and a panel that can move without
                // one is a panel only a test can scroll.
                float max_x = 0.0f, max_y = 0.0f;
                bool bar_x = false, bar_y = false;
                CHECK(gs_ui_probe_scroll_span(panels[p], nullptr, nullptr,
                                              &max_x, &max_y));
                CHECK(gs_ui_probe_scrollbars(panels[p], &bar_x, &bar_y));
                if ((max_x > 0.0f) != bar_x || (max_y > 0.0f) != bar_y) {
                    lying++;
                    printf("  SCROLLBAR '%s' at %dx%d with brush %d: can move "
                           "%.0f x %.0f, bars %d %d\n",
                           panels[p], sizes[z].w, sizes[z].h, brush,
                           (double)max_x, (double)max_y, bar_x, bar_y);
                }

                // **And every control on it can be got to.** The same standard
                // the front end's panels are held to: the panel is put at every
                // scroll position on a grid half its own size apart in both
                // directions, and each control has to be wholly on the panel at
                // one of them. A control cut in half by the edge of a tool
                // window is one nobody can read, whatever the geometry above
                // says about the window itself.
                int held = 0;
                for (float sy = 0.0f; ; sy += bh * 0.5f) {
                    if (sy > max_y) sy = max_y;
                    for (float sx = 0.0f; ; sx += bw * 0.5f) {
                        if (sx > max_x) sx = max_x;

                        gs_ui_probe_scroll_to(panels[p], sx, sy);
                        gs_ui_probe_start(items, GS_UI_MAX_ITEMS);
                        gs_ui_probe_frame();
                        gs_ed_frame(&ed);
                        const int at = gs_ui_probe_count();
                        gs_ui_probe_stop();
                        CHECK(at <= GS_UI_MAX_ITEMS);

                        for (int i = 0; i < at; i++) {
                            if (SDL_strcmp(items[i].window, panels[p]) != 0) {
                                continue;
                            }

                            // ImGui's own structure is unnamed and nobody
                            // presses it; the title bar is the window.
                            if (items[i].label[0] == '\0') continue;
                            if (gs_chrome(items[i].label, items[i].window)) {
                                continue;
                            }

                            int k = 0;
                            for (; k < held; k++) {
                                if (reach[k].id == items[i].id) break;
                            }
                            if (k == held) {
                                if (held >= GS_UI_MAX_ITEMS) continue;
                                held++;
                                reach[k].id = items[i].id;
                                reach[k].whole = false;
                                SDL_strlcpy(reach[k].label, items[i].label,
                                            sizeof reach[k].label);
                            }
                            if (items[i].whole) reach[k].whole = true;
                        }

                        if (sx >= max_x) break;
                    }
                    if (sy >= max_y) break;
                }

                for (int k = 0; k < held; k++) {
                    controls++;
                    if (reach[k].whole) continue;
                    cut++;
                    printf("  CUT IN HALF '%s' on '%s' at %dx%d with brush %d: "
                           "never wholly on the panel (can move %.0f x %.0f)\n",
                           reach[k].label, panels[p], sizes[z].w, sizes[z].h,
                           brush, (double)max_x, (double)max_y);
                }

                gs_ui_probe_scroll_to(panels[p], 0.0f, 0.0f);
                gs_ed_frame(&ed);
            }
        }
    }

    // **What was left out, named.** An item drawn inside a list or a child
    // region of a panel is clipped by that list, which is what a list is, and
    // is reached by scrolling the list rather than the panel - so it is counted
    // apart rather than folded into a number that would then mean something
    // else. The parts box has none; the palette's route list has all of them.
    printf("  EDITOR %d panels measured, %d controls on them all reachable: "
           "%d panels x %d brushes x %d window sizes. Anything inside a list on "
           "a panel scrolls with the list rather than the panel and is walked "
           "by every_control_in_the_construction_set_is_pressed instead.\n",
           measured, controls, (int)SDL_arraysize(panels), GS_BRUSH_COUNT,
           (int)SDL_arraysize(sizes));
    CHECK(measured == (int)SDL_arraysize(panels) * GS_BRUSH_COUNT *
                      (int)SDL_arraysize(sizes));
    CHECK(controls > 0);
    CHECK(outside == 0);
    CHECK(lying == 0);
    CHECK(cut == 0);

    e.brush = GS_BRUSH_RAISE;
    CHECK(SDL_SetWindowSize(gs_win, 1280, 720));
    CHECK(SDL_SetRenderLogicalPresentation(ren, 1280, 720,
                                           SDL_LOGICAL_PRESENTATION_DISABLED));
    ed.view.rect = (SDL_Rect){ 0, 0, 1280, 720 };
    for (int i = 0; i < 2; i++) gs_ed_frame(&ed);
    for (size_t p = 0; p < SDL_arraysize(panels); p++) {
        CHECK(gs_ui_probe_place(panels[p], was[p][0], was[p][1],
                                was[p][2], was[p][3]));
    }
    for (int i = 0; i < 2; i++) gs_ed_frame(&ed);
    for (size_t p = 0; p < SDL_arraysize(panels); p++) {
        float bx = 0.0f, by = 0.0f, bw = 0.0f, bh = 0.0f;
        CHECK(gs_ui_probe_window_box(panels[p], &bx, &by, &bw, &bh));
        CHECK(bx == was[p][0]);
        CHECK(by == was[p][1]);
        CHECK(bw == was[p][2]);
        CHECK(bh == was[p][3]);
    }
    e.show_controls = false;
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
    int folded  = 0;
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

                // **Nothing on a tool panel is below its fold.** The palette
                // was four hundred and sixty pixels tall and needed six
                // hundred: it ended at the route check, and save, load, the two
                // buttons that move a track as text, and the one line that
                // tells a new player what the mouse does - "Tab races it.
                // Arrows pan. Drag to paint." - were all under the bottom edge,
                // with two hundred and forty pixels of empty screen beneath the
                // window. A machine pressing by name found them. A person who
                // does not think to scroll a tool panel never did.
                //
                // Checked in every configuration, because which brush is chosen
                // decides what the panel holds.
                for (size_t p = 0; p < SDL_arraysize(gs_ed_panels); p++) {
                    float at_now = 0.0f, at_max = 0.0f;
                    if (!gs_ui_probe_scroll_at(gs_ed_panels[p], &at_now, &at_max)) {
                        continue;
                    }
                    if (at_max == 0.0f) continue;

                    // **A panel may only hide something if it is already using
                    // the whole screen.** A route can have any number of gates
                    // on it, so there is a size of track this panel cannot
                    // contain and scrolling is the honest answer to that. What
                    // is not honest is scrolling with two hundred and forty
                    // pixels of empty screen underneath, which is what it did.
                    float bx = 0.0f, by = 0.0f, bw = 0.0f, bh = 0.0f;
                    if (!gs_ui_probe_window_box(gs_ed_panels[p], &bx, &by, &bw, &bh)) {
                        continue;
                    }
                    if (by + bh >= 720.0f - 24.0f) continue;

                    printf("  BELOW THE FOLD '%s' with brush %d, sub %d, panel "
                           "%d: %.0f hidden, and it ends at %.0f of 720\n",
                           gs_ed_panels[p], brush, sub, panel, (double)at_max,
                           (double)(by + bh));
                    folded++;
                }

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

    printf("  EDITOR %d panels hiding something with room to spare\n", folded);
    CHECK(folded == 0);

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
    w.n_halved   = 0;
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
    printf("  WALK %d controls drawn off a panel that cannot scroll, "
           "%d drawn over the edge of one\n", w.n_stranded, w.n_halved);
    CHECK(w.n_stranded == 0);
    CHECK(w.n_halved == 0);

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
    CHECK(w.n_offered >= 768);

    // **The controls that are dead in one state and live in another.** This is
    // the number the seeding is for, and it is what a walk from a single
    // starting state cannot reach by any amount of pressing: the front end
    // draws six of its controls disabled under conditions, and until the walk
    // was started from a menu where those conditions differ, their destinations
    // were not in the map at all.
    // **And the door, which is not opened by pressing things.** The walk above
    // carries one word, because trying three strings in every box on every
    // screen multiplies the states without covering one more control - and one
    // word cannot sign anybody in. So the door is walked once more, here, with
    // the whole vocabulary and told to stop the moment it is through, and what
    // it learned about where the login screen leads is added to the graph.
    //
    // That is what closes the last exemption: no screen is exempt from the
    // no-trap check any more, the door included.
    {
        static gs_walk door;
        SDL_memset(door.slot, 0, sizeof door.slot);
        SDL_memset(door.shape, 0, sizeof door.shape);
        SDL_memset(door.offered, 0, sizeof door.offered);
        SDL_memset(door.pressed, 0, sizeof door.pressed);
        SDL_memset(door.never, 0, sizeof door.never);
        SDL_memset(door.unseen, 0, sizeof door.unseen);
        SDL_memset(door.moved, 0, sizeof door.moved);
        SDL_memset(door.named, 0, sizeof door.named);
        SDL_memset(door.reached, 0, sizeof door.reached);
        SDL_memset(door.edge, 0, sizeof door.edge);
        door.n_offered = 0; door.n_pressed = 0; door.n_never = 0;
        door.n_unseen = 0; door.n_moved = 0; door.n_stranded = 0;
        door.states = 0; door.edges = 0; door.typed = 0; door.deepest = 0;
        door.did_nothing = 0; door.capped = 0; door.ran_out = false;
        door.fine      = true;
        door.words     = (int)SDL_arraysize(gs_walk_words);
        door.per_shape = GS_WALK_PER_SHAPE;
        door.stop_at   = GS_SCREEN_TITLE;
        door.stop_set  = true;
        door.seed_at   = 0;
        door.seed_from = GS_SCREEN_LOGIN;

        gs_panel_menu(&seed, &t);
        gs_seed_signed_out(&seed);
        seed.screen = GS_SCREEN_LOGIN;

        gs_walk_screen(&ui, &seed, &t, ren, GS_SCREEN_LOGIN, &door);
        CHECK(!door.ran_out);
        CHECK(door.reached[GS_SCREEN_TITLE]);

        int carried = 0;
        for (int from = 0; from < GS_SCREEN_COUNT; from++) {
            for (int to = 0; to < GS_SCREEN_COUNT; to++) {
                if (!door.edge[from][to] || w.edge[from][to]) continue;
                w.edge[from][to] = true;
                carried++;
            }
        }
        printf("  WALK the door opens in %d actions, %d ways off it carried "
               "into the graph\n", door.edges, carried);
    }

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

    // **No screen is a trap, and there are no exemptions left.** At least one
    // thing on every screen leads somewhere else - the sign-in door included,
    // which is why the pass above exists: leaving it means signing in, which
    // wants a name and a password typed correctly rather than pressed.
    int traps = 0;
    for (size_t i = 0; i < SDL_arraysize(gs_every_screen); i++) {
        const gs_screen from = gs_every_screen[i];
        if (!w.reached[from]) continue;

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
        if (!w.reached[from]) continue;

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
    // **One is named rather than counted**, and it is the last exemption left
    // anywhere in these properties: the results screen is arrived at by
    // finishing a race, and no button leads to it, which is right. The sign-in
    // door used to be the other one and is not any more - it is reachable from
    // the title, by signing out, and it can be left, by signing in.
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
            if (to == GS_SCREEN_RESULTS) continue;
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

// The HUD, drawn and nothing else, so what it says can be read back.
static void gs_hud_frame_only(SDL_Renderer *ren, const gs_track *t,
                              const gs_world *w, const gs_view *v) {
    for (int frame = 0; frame < 3; frame++) {
        cImGui_ImplSDLRenderer3_NewFrame();
        cImGui_ImplSDL3_NewFrame();
        ImGui_NewFrame();
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        gs_hud_draw(w, t, v, 600, 0.0f, false);
        ImGui_Render();
    }
}

// One frame of a screen with its panel put at a given scroll position, and
// everything that frame drew.
static int gs_reach_at(gs_ui *ui, gs_screen hold, const char *window,
                       float sx, float sy, gs_ui_item *into, int cap) {
    // Held on the screen being measured, the way gs_panel_of holds one: a
    // frame that decided to move on would be measured somewhere else.
    ui->m->screen = hold;
    gs_ui_probe_scroll_to(window, sx, sy);
    gs_ui_probe_start(into, cap);
    gs_ui_probe_frame();
    gs_ui_frame(ui);
    int n = gs_ui_probe_count();
    gs_ui_probe_stop();
    return n;
}


TEST(the_hud_says_what_you_are_carrying_and_only_when_you_are) {
    gs_imgui_start(gs_win, ren);
    CHECK(gs_imgui_ready);
    if (!gs_imgui_ready) return;

    // **A hold that changes something invisible is not a control.** The tap and
    // hold is explained once, on the setup screen, which is gone by the time
    // anybody is driving - so the screen has to say what a tap would leave and
    // how many are left, or half the button is a secret.
    //
    // Read back through gs_hud_carrying rather than the item probe: the HUD is
    // plain text and ImGui names the widgets a person presses, not the words it
    // prints. Same reason gs_hud_overflow and gs_hud_spare exist.
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

    // Carrying nothing: no row at all, rather than a row that says nothing.
    gs_hud_frame_only(ren, &t, &w, &v);
    CHECK(gs_hud_carrying()[0] == '\0');

    // **Every kind, named the way the setup screen named it.** One list, so the
    // screen you choose on and the screen you race on cannot drift apart.
    int walked = 0;
    for (int k = GS_HAZ_NONE + 1; k < GS_HAZ_COUNT; k++) {
        for (int j = GS_HAZ_NONE + 1; j < GS_HAZ_COUNT; j++) {
            gs_world_arm(&w, (gs_hazard_kind)j, j == k ? 3 : 0);
        }
        CHECK(gs_car_selected(&w.car[0]) == (gs_hazard_kind)k);

        gs_hud_frame_only(ren, &t, &w, &v);
        const char *said = gs_hud_carrying();
        if (SDL_strstr(said, gs_hazard_name((gs_hazard_kind)k)) == nullptr ||
            SDL_strstr(said, "3") == nullptr) {
            printf("  HUD said '%s' while carrying three %s\n", said,
                   gs_hazard_name((gs_hazard_kind)k));
        }
        CHECK(SDL_strstr(said, gs_hazard_name((gs_hazard_kind)k)) != nullptr);
        CHECK(SDL_strstr(said, "3") != nullptr);
        walked++;
    }
    printf("  HUD names all %d kinds it can be carrying\n", walked);
    CHECK(walked == GS_HAZ_COUNT - 1);

    // The count follows what is left, so a driver knows when the last one is
    // the last one.
    for (int j = GS_HAZ_NONE + 1; j < GS_HAZ_COUNT; j++) {
        gs_world_arm(&w, (gs_hazard_kind)j, j == GS_HAZ_MINE ? 1 : 0);
    }
    gs_hud_frame_only(ren, &t, &w, &v);
    CHECK(SDL_strstr(gs_hud_carrying(), "mines") != nullptr);
    CHECK(SDL_strstr(gs_hud_carrying(), "1") != nullptr);

    // And spent, the row goes rather than sitting there saying zero.
    w.car[0].drop_cooldown = 0;
    CHECK(gs_world_drop(&w, 0, GS_HAZ_MINE));
    CHECK(gs_car_selected(&w.car[0]) == GS_HAZ_NONE);
    gs_hud_frame_only(ren, &t, &w, &v);
    CHECK(gs_hud_carrying()[0] == '\0');
}

TEST(at_the_smallest_window_every_control_can_be_scrolled_to) {
    // **The fault this was written for, and the one that hid it.** At six
    // hundred and forty by four hundred and eighty - the size somebody dragging
    // a corner in gets to - the race setup panel wants eight hundred pixels and
    // is clamped to the window. Half the gravity dial went with the clamp: Mars,
    // Venus, Neptune and Jupiter drawn past the right-hand edge, with nothing a
    // player could do to reach them. Four of the eight worlds you can race on,
    // gone, and the game looking perfectly normal.
    //
    // `no_screen_is_drawn_bigger_than_the_window_it_is_in` was watching for
    // exactly this and did not see it, because it asked only whether anything
    // was hidden *downwards*. Vertical overflow has always scrolled. Horizontal
    // overflow had nowhere to go, so a panel wider than its window simply threw
    // the difference away.
    //
    // The rule asserted here is the one that screen makes to a player: whatever
    // does not fit scrolls, in **both** directions, and scrolling gets you all
    // of it. So every control on every screen is required to be wholly on
    // screen at some scroll position the window can actually be put at.
    gs_imgui_start(gs_win, ren);
    CHECK(gs_imgui_ready);
    if (!gs_imgui_ready) return;

    CHECK(SDL_SetWindowSize(gs_win, GS_W, GS_H));
    CHECK(SDL_SetRenderLogicalPresentation(ren, GS_W, GS_H,
                                           SDL_LOGICAL_PRESENTATION_DISABLED));

    static gs_menu fresh;
    static gs_track t;

    static gs_ui_item items[GS_UI_MAX_ITEMS];
    static gs_reach   seen[GS_UI_MAX_ITEMS];
    static gs_reach   rows_here[GS_UI_MAX_ITEMS];
    gs_ui ui;

    // **Keyboard navigation off for the whole of this.** ImGui scrolls a window
    // to keep the item the keyboard is on in view, every frame - which is
    // exactly right for a person and exactly wrong here: it undoes the scroll
    // this test just set, so a list came back saying four of its thirty-two
    // rows could never be shown and a *different* four each time the frame
    // count changed. Nothing in the game is being switched off; the question
    // being asked is where things are, not where the keyboard is.
    ImGui_GetIO()->ConfigFlags &= ~(ImGuiConfigFlags)ImGuiConfigFlags_NavEnableKeyboard;

    int screens = 0, checked = 0, in_lists = 0, stranded = 0;
    int windows = 0, lying = 0, sideways = 0, downwards = 0;

    // **From every state the panels are measured from at full size**, because
    // how big a screen is depends on what is on it: the setup screen grows a
    // row per driver, the tracks screen draws a detail panel only once
    // something is chosen, and the door is a different door with an empty
    // roster. One state is one shape, and this fault is about shapes.
    for (size_t sd = 0; sd < SDL_arraysize(gs_seeds); sd++) {
    for (size_t si = 0; si < SDL_arraysize(gs_every_screen); si++) {
        gs_panel_menu(&fresh, &t);
        gs_seeds[sd].set(&fresh);
        // Static rather than a local - see gs_ui_exits.
        static gs_menu m;
        m = fresh;
        m.screen = gs_every_screen[si];
        gs_ui_begin(&ui, &m, &t, ren);

        // Probed against this menu rather than through gs_ui_controls, which
        // leaves the harness pointing at a gs_menu inside its own frame - fine
        // for a caller that begins again before its next frame, and a read of
        // dead stack for one that keeps framing, as the sweep below does.
        gs_ui_probe_start(items, GS_UI_MAX_ITEMS);
        gs_ui_probe_frame();
        gs_ui_frame(&ui);
        int n = gs_ui_probe_count();
        gs_ui_probe_stop();
        CHECK(n > 0);
        CHECK(n <= GS_UI_MAX_ITEMS);

        // The panel itself, as against ImGui's implicit debug window. Every
        // screen opens exactly one.
        char window[GS_UI_WINDOW] = "";
        for (int i = 0; i < n; i++) {
            if (SDL_strncmp(items[i].window, "Debug", 5) == 0) continue;
            SDL_strlcpy(window, items[i].window, sizeof window);
            break;
        }
        CHECK(window[0] != '\0');

        float max_x = 0.0f, max_y = 0.0f;
        CHECK(gs_ui_probe_scroll_span(window, nullptr, nullptr, &max_x, &max_y));

        // **And a panel that can move says so.** Everything below reaches a
        // control by setting the scroll position, which a test can do to any
        // window whether or not there is a scrollbar on it - so on its own it
        // proves the control is somewhere, not that anybody can get to it. A
        // window that can be scrolled has to be showing the bar that says so.
        //
        // This is also what keeps the bar from being asked for when it is not
        // needed. A window carrying the horizontal scrollbar flag shows the bar
        // whenever its contents are as wide as it is, and a rule drawn across a
        // panel *is* exactly as wide as it is - so every screen with a
        // separator on it grew a permanent scrollbar with the grip filling the
        // whole track. Fourteen pixels of furniture that scrolls nothing.
        // **Every window the screen opened, not only the panel.** A list or a
        // bordered box inside a panel is a window of its own to ImGui, with its
        // own clip rectangle and its own scroll - so a child whose contents are
        // wider than it is hides them exactly the way the panels did, and none
        // of the panel's numbers can see it. The track summary, the library and
        // the detail box are three of these.
        for (int i = 0; i < n; i++) {
            if (SDL_strncmp(items[i].window, "Debug", 5) == 0) continue;

            bool already = false;
            for (int k = 0; k < i; k++) {
                if (SDL_strcmp(items[k].window, items[i].window) == 0) {
                    already = true;
                    break;
                }
            }
            if (already) continue;

            float wx = 0.0f, wy = 0.0f;
            bool bar_x = false, bar_y = false;
            if (!gs_ui_probe_scroll_span(items[i].window, nullptr, nullptr,
                                         &wx, &wy)) {
                continue;
            }
            CHECK(gs_ui_probe_scrollbars(items[i].window, &bar_x, &bar_y));
            windows++;
            if (wx > 0.0f) sideways++;
            if (wy > 0.0f) downwards++;
            if ((wx > 0.0f) != bar_x || (wy > 0.0f) != bar_y) {
                lying++;
                printf("  SCROLLBAR %s from '%s': '%s' can move %.0f x %.0f, "
                       "bars %d %d\n",
                       gs_screen_name(gs_every_screen[si]), gs_seeds[sd].name,
                       items[i].window, (double)wx, (double)wy, bar_x, bar_y);
            }
        }

        // Steps of half a viewport, so no control can hide between two of
        // them: anything narrower than the window is wholly inside it at some
        // position, and half a window apart is close enough that one of the
        // positions swept is such a position.
        const float step_x = (float)GS_W * 0.5f;
        const float step_y = (float)GS_H * 0.5f;

        int held = 0;
        for (float sy = 0.0f; ; sy += step_y) {
            if (sy > max_y) sy = max_y;
            for (float sx = 0.0f; ; sx += step_x) {
                if (sx > max_x) sx = max_x;

                int at = gs_reach_at(&ui, gs_every_screen[si], window, sx, sy,
                                     items, GS_UI_MAX_ITEMS);
                CHECK(at <= GS_UI_MAX_ITEMS);
                for (int i = 0; i < at; i++) {
                    // Inside a list of its own, which scrolls on its own and is
                    // walked by gs_walk_reach rather than by moving the panel.
                    if (SDL_strcmp(items[i].window, window) != 0) continue;

                    int k = 0;
                    for (; k < held; k++) {
                        if (seen[k].id == items[i].id) break;
                    }
                    if (k == held) {
                        if (held >= GS_UI_MAX_ITEMS) continue;
                        held++;
                        seen[k].id = items[i].id;
                        seen[k].whole = false;
                        seen[k].reachable = items[i].reachable;
                        seen[k].disabled  = items[i].disabled;
                        seen[k].x0 = items[i].x0; seen[k].y0 = items[i].y0;
                        seen[k].x1 = items[i].x1; seen[k].y1 = items[i].y1;
                        SDL_strlcpy(seen[k].label, items[i].label,
                                    sizeof seen[k].label);
                    }
                    if (items[i].whole) seen[k].whole = true;
                }

                if (sx >= max_x) break;
            }
            if (sy >= max_y) break;
        }

        // **And the lists on it, each scrolled on its own.** A list inside a
        // panel is a window in its own right: it has its own clip rectangle and
        // its own scroll, and moving the panel does not move what is inside it.
        // These used to be counted and skipped - 1095 controls that the sweep
        // above said out loud it was not checking.
        //
        // The rule is the same one the panels are held to, one level down:
        // every control has to be wholly inside the list holding it at some
        // scroll position that list can be put at.
        for (int i = 0; i < n; i++) {
            if (SDL_strcmp(items[i].window, window) == 0) continue;
            if (SDL_strncmp(items[i].window, "Debug", 5) == 0) continue;

            bool done_this = false;
            for (int k = 0; k < i; k++) {
                if (SDL_strcmp(items[k].window, items[i].window) == 0) {
                    done_this = true;
                    break;
                }
            }
            if (done_this) continue;

            char list[GS_UI_WINDOW];
            SDL_strlcpy(list, items[i].window, sizeof list);

            float lw = 0.0f, lh = 0.0f, lmx = 0.0f, lmy = 0.0f;
            if (!gs_ui_probe_window_box(list, nullptr, nullptr, &lw, &lh)) {
                continue;
            }
            CHECK(gs_ui_probe_scroll_span(list, nullptr, nullptr, &lmx, &lmy));
            if (lw < 1.0f || lh < 1.0f) continue;

            // **Swept, because the rows are not all there to be placed.** A
            // long list uses a clipper: it submits the rows near the scroll
            // position and not the rest, so enumerating once and computing
            // where each row wants the list scrolled to only ever sees the
            // handful that exist at that moment. Thirty-two tracks came back
            // as five.
            //
            // So the list is walked, a fifth of its own height at a time. Half
            // was not enough: what has to land inside the step is the window in
            // which a row is *wholly* visible, and a list whose padding and
            // header eat into it leaves less of that than the arithmetic on the
            // box height suggests. There are 39 lists in the whole sweep, so
            // the finer step costs nothing worth counting.
            int held_here = 0;
            for (float sy = 0.0f; ; sy += lh * 0.2f) {
                if (sy > lmy) sy = lmy;

                gs_ui_probe_scroll_to(list, 0.0f, sy);
                gs_ui_probe_start(items, GS_UI_MAX_ITEMS);
                gs_ui_probe_frame();
                ui.m->screen = gs_every_screen[si];
                gs_ui_frame(&ui);
                const int at = gs_ui_probe_count();
                gs_ui_probe_stop();

                // **Inside the box it is drawn in - which is not the same as
                // ImGui's `whole`.** The probe says so itself: `whole` is only
                // meaningful where nothing scrolls, because it is measured
                // against a clip rectangle, and a scrolling list's clip stops
                // short of its own scrollbar. A full-width row therefore
                // overlaps the scrollbar column by a pixel and is never
                // "whole", which reported every row of a library as
                // unreachable when all of them are perfectly readable.
                float bx = 0.0f, by = 0.0f, bw = 0.0f, bh = 0.0f;
                CHECK(gs_ui_probe_window_box(list, &bx, &by, &bw, &bh));

                for (int j = 0; j < at && j < GS_UI_MAX_ITEMS; j++) {
                    if (SDL_strcmp(items[j].window, list) != 0) continue;
                    if (items[j].label[0] == '\0') continue;
                    if (gs_chrome(items[j].label, items[j].window)) continue;

                    int q = 0;
                    for (; q < held_here; q++) {
                        if (rows_here[q].id == items[j].id) break;
                    }
                    if (q == held_here) {
                        if (held_here >= GS_UI_MAX_ITEMS) continue;
                        held_here++;
                        rows_here[q].id = items[j].id;
                        rows_here[q].whole = false;
                        SDL_strlcpy(rows_here[q].label, items[j].label,
                                    sizeof rows_here[q].label);
                    }
                    if (items[j].visible &&
                        items[j].x0 >= bx - 0.5f && items[j].y0 >= by - 0.5f &&
                        items[j].x1 <= bx + bw + 0.5f &&
                        items[j].y1 <= by + bh + 0.5f) {
                        rows_here[q].whole = true;
                    }
                }

                if (sy >= lmy) break;
            }

            for (int q = 0; q < held_here; q++) {
                in_lists++;
                if (rows_here[q].whole) continue;
                stranded++;
                printf("  PAST THE EDGE  %s from '%s': '%s' is never wholly "
                       "inside the list '%s' that holds it, at any scroll "
                       "position it has (it can move %.0f)\n",
                       gs_screen_name(gs_every_screen[si]), gs_seeds[sd].name,
                       rows_here[q].label, list, (double)lmy);
            }

            (void)lw;
            gs_ui_probe_scroll_to(list, 0.0f, 0.0f);
            gs_ui_probe_scroll_to(window, 0.0f, 0.0f);

            // The frame after a sweep has to be the one the outer loop reads,
            // so put the panel back the way the next screen expects it.
            gs_ui_frame(&ui);
        }

        for (int k = 0; k < held; k++) {
            checked++;
            if (seen[k].whole) continue;
            stranded++;
            printf("  PAST THE EDGE  %s from '%s': '%s' id %08x never wholly "
                   "on screen - %.0f,%.0f to %.0f,%.0f (%.0f x %.0f), panel "
                   "can move %.0f x %.0f, nav %d dead %d\n",
                   gs_screen_name(gs_every_screen[si]), gs_seeds[sd].name,
                   seen[k].label[0] != '\0' ? seen[k].label : "(unnamed)",
                   seen[k].id,
                   (double)seen[k].x0, (double)seen[k].y0,
                   (double)seen[k].x1, (double)seen[k].y1,
                   (double)(seen[k].x1 - seen[k].x0),
                   (double)(seen[k].y1 - seen[k].y0),
                   (double)max_x, (double)max_y,
                   seen[k].reachable, seen[k].disabled);
        }
        screens++;
    }
    }

    printf("  SMALL %d controls over %d screens reachable at %dx%d, from %d "
           "starting states, and %d more inside the lists on them, each "
           "reachable by scrolling the list. "
           "%d windows - panels and the boxes inside them - each showing a "
           "scrollbar exactly when it has something to scroll, and %d of them "
           "can move sideways and %d down\n",
           checked, screens, GS_W, GS_H, (int)SDL_arraysize(gs_seeds), in_lists,
           windows, sideways, downwards);
    CHECK(screens == (int)SDL_arraysize(gs_every_screen) *
                     (int)SDL_arraysize(gs_seeds));
    CHECK(stranded == 0);
    CHECK(windows > screens);       // more than one window on some screen
    CHECK(lying == 0);

    // **And the rule is not vacuously true.** A check that a window shows a
    // scrollbar exactly when it can scroll proves nothing if no window in the
    // whole sweep can scroll: it would then be asserting that none of them has
    // a bar, which is a different and much weaker claim than the one written
    // above it.
    CHECK(sideways > 0);
    CHECK(downwards > 0);

    ImGui_GetIO()->ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    CHECK(SDL_SetWindowSize(gs_win, 1280, 720));
    CHECK(SDL_SetRenderLogicalPresentation(ren, 1280, 720,
                                           SDL_LOGICAL_PRESENTATION_DISABLED));
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

            // **And nothing past the right-hand edge either**, which is the
            // same fault turned ninety degrees. It is not visible in any of the
            // numbers above: a panel can sit inside the window, scroll nowhere,
            // and still draw the last button on a row cut in half. Found by
            // photographing the setup screen after a dial was added beside the
            // driver count - two of the eight planets ended in the middle of
            // their own names.
            if (p.wider != 0.0f) {
                printf("  PAST THE EDGE %s from '%s': %.0f wider than its panel\n",
                       gs_screen_name(gs_every_screen[i]), gs_seeds[sd].name,
                       (double)p.wider);
            }
            CHECK(p.wider == 0.0f);
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
    gs_sandbox();
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

    run_a_driver_who_drove_past_a_checkpoint_is_told_and_pointed_back(ren);
    run_the_way_back_arrow_is_drawn_whole_wherever_the_car_is_standing(ren);
    run_no_paint_on_the_ground_is_drawn_over_a_car_standing_on_it(ren);
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
    run_placing_the_views_leaves_everything_it_does_not_own_alone(ren);
    run_every_number_of_players_gets_the_whole_screen_and_a_fair_share_of_it(ren);
    run_cars_hovering_at_the_threshold_do_not_flicker_the_screen_in_half(ren);
    run_the_view_does_not_jump_when_the_screen_merges_or_splits(ren);
    run_every_control_can_be_moved_and_every_player_can_drive_from_a_pad_alone(ren);
    run_every_combination_of_keys_reaches_the_car_at_once(ren);
    run_a_rebind_waits_for_the_key_that_started_it_to_be_let_go(ren);
    run_changed_controls_survive_being_written_and_read_back(ren);
    run_every_vehicle_has_a_mesh_and_no_two_are_the_same_shape(ren);
    run_a_car_is_drawn_from_its_mesh_and_faces_where_it_is_pointing(ren);
    run_a_car_on_a_slope_leans_with_the_ground(ren);
    run_the_guide_tells_people_to_press_the_keys_the_game_listens_for(ren);
    run_the_release_notes_say_what_is_actually_shipped(ren);
    run_a_time_reads_the_way_people_say_it(ren);
    run_a_finished_race_becomes_a_table_in_the_order_it_finished(ren);
    run_the_store_remembers_drivers_and_records_between_runs(ren);
    run_no_track_that_ships_throws_a_car_off_the_world(ren);
    run_the_way_round_is_painted_between_the_gates_and_not_only_at_them(ren);
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
    run_the_minimap_shows_the_whole_route_and_where_everybody_is_on_it(ren);
    run_the_hud_says_what_lap_it_is_and_changes_when_the_lap_does(ren);
    run_the_hud_says_what_place_you_are_in_and_changes_when_you_are_passed(ren);
    run_the_analyser_refuses_a_track_with_no_route_rather_than_guessing(ren);
    run_every_screen_has_a_way_off_it_and_the_ways_lead_somewhere_real(ren);
    run_a_menu_knows_a_state_it_has_already_been_in(ren);
    run_a_track_is_built_from_nothing_and_raced_without_leaving_the_editor(ren);
    run_one_brush_never_undoes_what_another_one_did(ren);
    run_every_brush_and_every_option_it_carries_does_what_it_says(ren);
    run_the_construction_set_keeps_its_panels_on_the_screen(ren);
    run_every_control_in_the_construction_set_is_pressed(ren);
    run_the_walk_signs_in_through_the_door_rather_than_being_put_behind_it(ren);
    run_a_track_is_built_named_saved_and_raced_by_pressing_and_dragging(ren);
    run_every_value_of_every_dial_is_pressed_not_three_interesting_ones(ren);
    run_the_walk_goes_as_deep_as_the_front_end_does(ren);
    run_every_control_is_known_by_name_and_answers_to_it(ren);
    run_the_hud_says_what_you_are_carrying_and_only_when_you_are(ren);
    run_at_the_smallest_window_every_control_can_be_scrolled_to(ren);
    run_no_screen_is_drawn_bigger_than_the_window_it_is_in(ren);
    run_a_store_with_tracks_in_it_is_saved_whole(ren);
    run_the_condition_bar_stays_inside_the_hud(ren);
    run_a_hud_stays_inside_the_view_it_belongs_to(ren);
    run_there_is_always_a_way_back_out_of_wherever_you_are(ren);
    run_the_weapons_switch_on_the_setup_screen_arms_the_race(ren);
    run_the_empty_seats_on_the_grid_are_filled_with_somebody(ren);
    run_an_opponent_finishes_every_track_that_ships_from_every_grid_slot(ren);
    run_no_test_writes_where_a_player_keeps_their_things(ren);
    run_a_pad_is_opened_read_and_closed_the_way_a_person_plugs_one_in(ren);
    run_a_pad_can_leave_a_screen_without_walking_to_the_button(ren);
    run_the_hud_fits_what_is_in_it_in_every_state_it_has(ren);
    run_the_light_tree_counts_down_and_then_goes_green(ren);
    run_a_start_line_and_a_finish_line_are_different_things(ren);
    run_every_kind_of_hazard_is_drawn_as_itself(ren);
    run_a_hazard_is_drawn_the_size_it_will_catch_you_at(ren);
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

// gs_render.h - drawing a race.
//
// **Terrain is emitted as shaded geometry, never assembled from a tile atlas.**
// Four corner heights per tile means arbitrary elevation joins, and no
// hand-authored tile set can stitch those cleanly - there is always an edge or
// a hole baked into the wrong pick. Two triangles tinted by surface and by
// slope are correct by construction, and they make the two things that will
// arrive later - painted gravity and progressive surface wear - another input
// to the tint rather than a new art requirement.
#ifndef GS_RENDER_H
#define GS_RENDER_H

#include <SDL3/SDL.h>

#include "core/gs_sim.h"
#include "core/gs_track.h"
#include "core/gs_analyse.h"
#include "gfx/gs_iso.h"

typedef struct gs_view {
    gs_camera cam;
    SDL_Rect  rect;      // where on the window this view lives
    uint8_t   car;       // whose view it is
    bool      show_gravity;

    // The landing arc, for the driver of this view. **Off unless asked for**:
    // the arc being not negotiable is what makes the take-off decision matter,
    // and a permanent readout would turn a judgement into a number to follow.
    // It is for learning what a ramp does, and for the editor, where "does
    // anybody clear this" is the whole job.
    bool      show_arc;

    // **The checkpoint this driver has driven past, or -1.**
    //
    // The simulation only tests the gate a car is *expecting*, so one missed
    // gate silently stops every later crossing counting, the finish included -
    // a player who ran wide at a corner drove the rest of the lap, crossed the
    // chequer and was told nothing at all. This is what the HUD says so, and
    // what the arrow on the ground points back at.
    //
    // Latched by the frontend rather than kept in the world: which gate a car
    // is owed is already in the world, and *having been told about it* is a
    // property of a screen. Keeping it out of gs_world is also what stops this
    // moving the golden hash and invalidating every replay in existence.
    //
    // A flag and an index rather than an index and a sentinel, because a
    // `gs_view` is zero-initialised in a dozen places and a zero that means
    // "gate zero was missed" would put the warning on screen in every one of
    // them.
    bool     missed;
    uint8_t  missed_at;

    // The analyser's heatmap, or null for none. Borrowed, not owned: the view
    // paints whatever the editor last worked out and never runs the sweep
    // itself, because a sweep is thirty seconds of simulation and a frame is
    // eight milliseconds.
    const gs_analysis *heat;
} gs_view;

// **Notice when this view's driver has driven past the checkpoint it owes.**
//
// Given the world before a step and after it, latch `missed` on any view whose
// car crossed the plane of the gate it was expecting without going through it,
// and clear it on any whose car has just taken that gate. The race only ever
// tests the gate a car is expecting, so one missed gate silently stops every
// later crossing counting, the finish included - a player who ran wide at a
// corner drove the rest of the lap, crossed the chequer and was told nothing.
//
// Here rather than in the frontend so that it can be tested, and rather than in
// `gs_world` so that it does not move the golden hash: which gate a car owes is
// already in the world, and *having been told* is a property of a screen.
void gs_view_note_missed(gs_view *views, uint8_t count, const gs_track *t,
                         const gs_world *was, const gs_world *now);

// Draw one view of the world. `alpha` in [0,1] interpolates between the
// previous simulation state and the current one, so motion is smooth at frame
// rates that have nothing to do with 120 Hz.
void gs_render_view(SDL_Renderer *ren, const gs_track *t, const gs_world *prev,
                    const gs_world *now, float alpha, const gs_view *view);

// The split screen, and its willingness to stop being one.
//
// **Cars that are close together share a view.** Two panes showing almost the
// same thing is a waste of a screen and, worse, it is hard to read: a collision
// is legible when both cars are in one picture, and legibility is the whole
// argument for this camera.
//
// The difficulty is the transition. A hard switch from one pane to two is a
// jump, and a jump in a racing game is the thing players notice above all else.
// So the panes' cameras converge on the shared one *before* the divider goes,
// and diverge after it appears - at the moment of the switch every pane is
// already showing what the single view showed, and only the divider changes.
typedef struct gs_split {
    float     merge;     // 1 sharing one view, 0 fully split, moving between
    gs_camera shared;    // where that one view looks
} gs_split;

void gs_split_init(gs_split *s);

// Advance the merge state by `dt` seconds. Hysteresis is deliberate: cars
// hovering at the threshold must not flicker the screen in half.
// **Given the same two states and the same alpha the renderer draws with**, so
// the camera looks at where the cars are *this frame* rather than at the last
// tick's settled positions. A camera on the settled state while the cars are
// drawn between ticks has an offset that changes every frame, which looks
// exactly like a car juddering in a smooth world - and did.
void gs_split_update(gs_split *s, const gs_track *t, const gs_world *prev,
                     const gs_world *w, float alpha, int win_w, int win_h,
                     float dt);

// Fill in the views to draw. Returns how many - one while merged, one per car
// otherwise. Cameras are blended by the merge factor, which is what makes the
// switch continuous rather than sudden.
//
// **`out` is in as well as out.** This sets the camera, the rectangle and whose
// view it is, and leaves everything else on each `gs_view` exactly as it found
// it - the overlays, the landing arc, the analyser's heatmap, whether this
// driver has been told they drove past a checkpoint. So pass the views you
// already have, and initialise them if you have none: an uninitialised array
// comes back uninitialised in every field this does not own.
//
// It used to zero each view instead, which made callers safe by accident and
// the frontend wrong on purpose - it had to copy its own settings back one
// field at a time, and the day a field was added without a line added with it,
// the missed-checkpoint warning was set every tick and wiped every frame. See
// `placing_the_views_leaves_everything_it_does_not_own_alone`.
uint8_t gs_split_views(const gs_split *s, const gs_track *t,
                       const gs_world *prev, const gs_world *w, float alpha,
                       int win_w, int win_h, gs_view *out);

// Where each view goes, for one to four of them, in a window `w` by `h`.
//
// One fills the window. Two split it left and right, because a racing track is
// wider than it is tall and two tall thin views waste the shape of it. Three and
// four both take a two-by-two grid: three players leave a cell empty rather than
// get a different layout each time somebody joins, which is worth more than the
// tidiness of using every pixel.
//
// Returns how many rectangles it wrote.
uint8_t gs_render_layout(uint8_t views, int w, int h, SDL_Rect *out);

// What the last stretch of drawing actually submitted.
//
// Here so that "four quarter-sized views cost about one window" can be checked
// as a fact rather than as a stopwatch reading. The first version of that test
// timed the two and failed one run in ten on a busy machine - which is worse
// than no test, because a green tick that means "the machine was quiet" is not
// evidence of anything.
typedef struct gs_render_stats {
    uint32_t tiles;   // tiles whose geometry was built and submitted
    uint32_t cars;
    uint32_t tris;    // car triangles that survived culling and were drawn
} gs_render_stats;

// What colour each car is painted, which is a player's choice rather than the
// simulation's business - see gs_profile.h. Set it before a race and the cars
// wear it; nothing about the race changes.
void    gs_render_set_car_paint(uint8_t car, uint8_t colour);
uint8_t gs_render_car_paint(uint8_t car);

// The colour itself, for a swatch next to somebody's name on a menu.
SDL_FColor gs_render_paint_colour(uint8_t colour);

// The ground colour a surface is drawn in, before any shading. Out here so a
// test can check the whole palette apart rather than a person checking it by
// looking - three of these were the same grey at three brightnesses once, which
// is invisible on a flat plane and obvious the moment the ground tilts.
SDL_FColor gs_render_surface_colour(gs_surface surface);

void gs_render_reset_stats(void);
gs_render_stats gs_render_stats_now(void);

// Draw a single car ghosted - translucent, and without the nose flash, so it
// reads as a prediction rather than as a competitor.
void gs_render_ghost(SDL_Renderer *ren, const gs_track *t, const gs_car *c,
                     const gs_view *view);

// The same, interpolated between two ticks. A ghost you are racing has to move
// as smoothly as the car you are driving, or the thing you are chasing stutters
// and the comparison stops being fair to look at.
void gs_render_ghost_lerp(SDL_Renderer *ren, const gs_track *t,
                          const gs_car *prev, const gs_car *now, float alpha,
                          const gs_view *view);

// Put the camera where it should be for the car it follows.
void gs_render_track_camera(gs_view *view, const gs_track *t,
                            const gs_world *prev,
                            const gs_world *now, float alpha);

#endif // GS_RENDER_H

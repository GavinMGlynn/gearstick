// gs_hud.h - what a driver needs to know while they are driving.
//
// **A race you can only understand afterwards is not a race you are in.**
// Everything here was already in the simulation and was only ever shown on the
// results table: which lap you are on, where you are in the field, how the lap
// you are driving compares to your best, and how much of the car is left. Being
// told at the end that you were second is a scoreboard; being told now is a
// reason to try to overtake.
//
// It draws over a view rather than beside it. Split screen means one of these
// per player, each one clipped to the quarter of the window its driver is
// looking at - so a HUD belongs to a view, not to the window.
//
// Reads the world and never touches it. It is drawn from interpolated frames at
// whatever rate the machine manages, and the simulation runs at 120 Hz whatever
// the HUD is doing.
#ifndef GS_HUD_H
#define GS_HUD_H

#include "core/gs_sim.h"
#include "core/gs_track.h"
#include "gfx/gs_render.h"

// Draw the HUD for one view. `tick` is the simulation tick the frame is showing,
// which is what the running lap clock counts from - taken as an argument rather
// than read from the world so that a replay being scrubbed shows the time at the
// tick being looked at rather than the time at the end.
void gs_hud_draw(const gs_world *w, const gs_track *t, const gs_view *v,
                 uint32_t tick);

#endif // GS_HUD_H

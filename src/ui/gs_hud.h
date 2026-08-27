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
// `waited` is how long this machine has been waiting for another one, in
// seconds, or zero when the race is running. **A race that is waiting has to
// look like a race that is waiting**: the rollback stops the world when the
// other machine goes quiet, and a stopped world with nothing said about it is
// indistinguishable from a game that has crashed - which is what a player who
// pressed Play into a race whose other seat had been abandoned saw.
// `online` says whether this race belongs to a server, which decides what a
// wrecked driver is offered: restarting is not one machine's to do in a race
// other people are in.
void gs_hud_draw(const gs_world *w, const gs_track *t, const gs_view *v,
                 uint32_t tick, float waited, bool online);

// **How much of the last HUD drawn was below the bottom of its own panel.**
// Zero when everything fitted. The panel is sized by hand from what is going
// into it - it has to be, because an auto-fitting window is invisible on its
// first frame and a screenshot is one frame - and a size worked out by hand
// goes stale the moment somebody adds a line. It has already: the first
// version of the wreck message had "Esc back to the menu" half outside the
// box. This is what a test reads instead of counting pixels.
float gs_hud_overflow(void);

// **And how much of it was nothing.** The room left under the last thing drawn.
// A panel sized for rows it is not drawing is a box with an empty half - which
// is what the HUD looked like in a derby the day it stopped pretending to be a
// race - and no amount of asking what fell off the bottom can see it.
float gs_hud_spare(void);

#endif // GS_HUD_H

// gs_parts.h - the parts box.
//
// **The construction set's other half.** Brushes raise a corner and paint a
// tile, which is how you shape ground; they are not how you build a road. The
// original's editor had a PARTS BOX beside the course - straights, corners,
// ramps, junctions - and you dropped a piece on the map and then modified it.
// That is a different act from painting: a part knows what it is, so it can lay
// its own surface, set its own heights and place its own gate, and the thing
// you are handling is a piece of track rather than a hundred corners you have
// to keep level yourself.
//
// **A part is not a new kind of track data.** Everything a part does is corner
// moves, surface changes and gate placements - the edits that already exist -
// grouped into one transaction. So a part undoes in one step, a track built
// from parts is the same file as a track built with brushes, and nothing
// downstream needs to know parts happened. That is what keeps this a *way of
// editing* rather than a second track format.
//
// Integers only and links nothing, like everything in src/core/.
#ifndef GS_PARTS_H
#define GS_PARTS_H

#include "core/gs_edit.h"
#include "core/gs_track.h"

typedef enum gs_part_kind {
    // Road. Each lays level ground across its width and its own surface, so a
    // car is never tipped sideways by the road it is on - the same rule the
    // generator carves by.
    GS_PART_STRAIGHT = 0,
    GS_PART_CORNER,        // a quarter turn, ending at right angles to its start
    GS_PART_RAMP,          // rises along its length, for launching off the end of
    GS_PART_CREST,         // up and over: a jump with a landing on the far side
    GS_PART_DIP,           // down and out, which is a jump you arrive at faster
    GS_PART_CROSSROADS,    // two roads meeting, level, both ways open


    // The route. **The three lines are three different things** and which one a
    // track has decides what kind of race it is: a combined line makes it a
    // circuit, a separate start and finish make it a path.
    GS_PART_START,         // a path begins here
    GS_PART_FINISH,        // a path ends here
    GS_PART_START_FINISH,  // a lap begins and ends here, and the track is a loop
    GS_PART_CHECKPOINT,    // through here, on the way

    GS_PART_COUNT
} gs_part_kind;

const char *gs_part_name(gs_part_kind k);

// Does this part lay road, or is it only a line on one?
bool gs_part_is_road(gs_part_kind k);

// Does this part place a gate?
bool gs_part_is_route(gs_part_kind k);

// **What you can modify about a piece once it is chosen.** The original let you
// change a part after dropping it; these are the dials that do it here.
typedef struct gs_part {
    uint8_t kind;        // gs_part_kind
    uint8_t turn;        // quarter turns clockwise, 0 to 3
    uint8_t width;       // across the road, in tiles
    uint8_t length;      // along it, in tiles - ignored by the line pieces
    uint8_t surface;     // gs_surface the road is made of
    gs_fix  rise;        // how far it climbs, for the shaped pieces
} gs_part;

// A part with sensible numbers for its kind, so choosing one out of the box
// gives something worth dropping rather than something zero tiles long.
gs_part gs_part_default(gs_part_kind kind);

// The tiles a part would cover if dropped here, as a box in tile coordinates.
// For showing where it will land before the button goes down.
void gs_part_footprint(const gs_part *p, int32_t x, int32_t y,
                       int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1);

// **Drop a part on the track, as one undoable action.**
//
// Returns false and changes nothing if the part will not fit on the track, if
// the route is full, or if the undo log is - an edit that cannot be taken back
// is worse than an edit that did not happen, which is the rule the whole edit
// layer is built on.
//
// The route pieces do more than place a gate. A start or a finish makes the
// track a path; a combined line makes it a loop. And because gate zero is where
// a race begins, a start line placed after other gates is *moved to the front*
// rather than appended - otherwise dropping the pieces in the order they occur
// to somebody produces a track that starts in the middle of itself.
bool gs_part_place(gs_edit_log *l, gs_track *t, const gs_part *p,
                   int32_t x, int32_t y);

#endif // GS_PARTS_H

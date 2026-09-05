// gs_edit.h - editing a track, and being able to take it back.
//
// **Undo is not a feature of the editor's interface; it is a property of the
// track.** So it lives here, beside the track, in the layer that links nothing
// — which means the same undo model serves a mouse, a pad, a script, and the
// analyser trying edits to see what they do.
//
// The log stores what changed rather than what the track looked like. A track
// is 22 KB and a corner move is six bytes, so "unlimited undo" is a matter of
// how many edits fit in a buffer the caller sized, not of how much memory a
// history of snapshots would need.
//
// Edits group into transactions, because a brush stroke is one thing a person
// did even though it touched forty tiles, and undoing it a tile at a time would
// be useless.
#ifndef GS_EDIT_H
#define GS_EDIT_H

#include "core/gs_track.h"

typedef enum gs_edit_kind {
    GS_EDIT_CORNER = 0,
    GS_EDIT_SURFACE,
    GS_EDIT_GRAVITY,

    // **The route, which used to be outside all of this.** Undo covered the
    // terrain, the surfaces and the gravity, and not the gates - so "undo covers
    // everything you can change" was a promise with a footnote, and the one edit
    // that changes what a track *is* for scoring purposes was the one you could
    // not take back.
    GS_EDIT_GATE_ADD,
    GS_EDIT_GATE_REMOVE,

    // **Where a gate sits in the order is part of what the track is.** Gate
    // zero is where a race begins, so a start line dropped after the corners
    // have been laid has to become gate zero rather than the last thing on the
    // route - and moving it has to be undoable like everything else, or the
    // parts box has one action you cannot take back.
    GS_EDIT_GATE_MOVE,

    // Whether the track is a loop or a path, which decides which gate ends a
    // lap. A change to what the track *is*, so it belongs in the history.
    GS_EDIT_ROUTE_KIND,
    // How many gates make a checkpoint. One number for the whole track, so
    // it is an edit of the track rather than of a gate.
    GS_EDIT_CHECKPOINT_EVERY,

    // **So a test can say it walked all of them.** Two of the seven above had
    // never been called by any test at all - moving a gate and changing whether
    // the track is a loop - and nothing could have noticed, because the way to
    // notice is to count against a number like this one.
    GS_EDIT_COUNT
} gs_edit_kind;

typedef struct gs_edit {
    uint32_t group;          // which transaction this belongs to
    uint8_t  kind;           // gs_edit_kind
    uint8_t  x, y;           // the tile, for the three tile kinds
    uint8_t  index;          // the gate, for the two gate kinds
    int16_t  before, after;  // the stored representation, not the Q16.16 one
    uint16_t pad;

    // What the gate was, so a removal can be put back exactly where it was and
    // in the order it was in. Only meaningful for the gate kinds, and it costs
    // every entry sixteen bytes - which is a log that is twice the size and an
    // undo history with no gap in it, and the second is worth more.
    gs_gate  gate;
} gs_edit;

// A flexible array member, so the caller decides how deep the history goes and
// `src/core/` still owns no allocator. `gs_edit_log_bytes` says how much to
// provide.
typedef struct gs_edit_log {
    uint32_t cap;       // how many edits fit
    uint32_t count;     // edits recorded; anything above `cursor` is the redo tail
    uint32_t cursor;    // edits currently applied to the track
    uint32_t group;     // the transaction being recorded, or the next one
    bool     open;      // inside a begin/end pair
    gs_edit  ops[];
} gs_edit_log;

size_t gs_edit_log_bytes(uint32_t capacity);
void   gs_edit_log_init(gs_edit_log *l, uint32_t capacity);

// Everything between begin and end undoes as one action. Nesting is not a
// thing: a second begin without an end is the caller's bug and is ignored.
// Throw the history away.
//
// **Not a tidy-up: a correctness requirement.** The log records a cell changing
// from one value to another, and those values belong to the track that was
// being edited. Loading a different track and then undoing would apply somebody
// else's edits to it - "put this corner back to what it was" is meaningless
// when it was never that.
void gs_edit_reset(gs_edit_log *l);

void gs_edit_begin(gs_edit_log *l);
void gs_edit_end(gs_edit_log *l);

// Apply a change to the track and record how to take it back. An edit outside a
// begin/end pair is its own transaction, which is what makes the single-tile
// case need no ceremony.
//
// Return false only when the log is full — the change is then *not* applied,
// because an edit that cannot be undone is worse than an edit that did not
// happen. Changes that would alter nothing are silently skipped: they succeed,
// and they do not put a step in the history that appears to do nothing.
bool gs_edit_corner(gs_edit_log *l, gs_track *t, uint8_t x, uint8_t y, gs_fix height);
bool gs_edit_surface(gs_edit_log *l, gs_track *t, uint8_t x, uint8_t y, gs_surface s);
bool gs_edit_gravity(gs_edit_log *l, gs_track *t, uint8_t x, uint8_t y, gs_fix multiplier);

// Add a gate to the end of the route, or take one out of the middle, with the
// change recorded so it can be undone. Adding returns the new gate's index, or
// -1 if the route is full; removing returns whether there was one there.
int  gs_edit_add_gate(gs_edit_log *l, gs_track *t, gs_fix x, gs_fix y,
                      gs_angle heading, gs_fix half_width);
bool gs_edit_remove_gate(gs_edit_log *l, gs_track *t, uint8_t index);

// Move a gate to a different place in the route, sliding the others along to
// close the gap and open a new one. False if either index is past the end.
bool gs_edit_move_gate(gs_edit_log *l, gs_track *t, uint8_t from, uint8_t to);

// Say whether the track is a loop or a path, undoably - it decides which gate
// ends a lap and how the lines are drawn, so it is a change to the track like
// any other and not a setting beside it.
bool gs_edit_route_kind(gs_edit_log *l, gs_track *t, gs_route_kind kind);

// Every Nth gate is a checkpoint; the rest are waypoints a car may miss. One
// is what every track was before the dial existed. See gs_track_is_checkpoint.
bool gs_edit_checkpoint_every(gs_edit_log *l, gs_track *t, uint8_t every);

bool gs_edit_can_undo(const gs_edit_log *l);
bool gs_edit_can_redo(const gs_edit_log *l);

// One transaction at a time. False when there is nothing left to take back.
bool gs_edit_undo(gs_edit_log *l, gs_track *t);
bool gs_edit_redo(gs_edit_log *l, gs_track *t);

// How many transactions are behind and ahead of where the track currently is.
uint32_t gs_edit_undo_depth(const gs_edit_log *l);
uint32_t gs_edit_redo_depth(const gs_edit_log *l);

#endif // GS_EDIT_H

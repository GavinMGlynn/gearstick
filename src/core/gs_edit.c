// gs_edit.c - see gs_edit.h.

#include "core/gs_edit.h"

size_t gs_edit_log_bytes(uint32_t capacity) {
    return sizeof(gs_edit_log) + (size_t)capacity * sizeof(gs_edit);
}

void gs_edit_log_init(gs_edit_log *l, uint32_t capacity) {
    l->cap = capacity;
    l->count = 0;
    l->cursor = 0;
    l->group = 0;
    l->open = false;
}

void gs_edit_reset(gs_edit_log *l) {
    if (l == nullptr) return;
    l->count = 0;
    l->cursor = 0;
    l->group = 0;
    l->open = false;
}

void gs_edit_begin(gs_edit_log *l) {
    if (l->open) return;
    l->open = true;
    l->group++;
}

void gs_edit_end(gs_edit_log *l) {
    l->open = false;
}

// Where a change of this kind currently sits on the track, in the stored
// representation rather than the Q16.16 one - because that is what the file
// holds and what undo has to restore exactly.
static int16_t gs_edit_read(const gs_track *t, gs_edit_kind kind, uint8_t x, uint8_t y) {
    switch (kind) {
    case GS_EDIT_CORNER:
        return t->corner[(size_t)y * GS_CORNER_STRIDE + (size_t)x];
    case GS_EDIT_SURFACE:
        return (int16_t)t->surface[GS_TILE_INDEX(x, y)];
    case GS_EDIT_GRAVITY:
        return (int16_t)t->gravity[GS_TILE_INDEX(x, y)];
    case GS_EDIT_GATE_ADD:
    case GS_EDIT_GATE_REMOVE:
    case GS_EDIT_GATE_MOVE:
    case GS_EDIT_ROUTE_KIND:
    case GS_EDIT_COUNT:
        break;      // the route is not a tile; see gs_edit_route
    }
    return 0;
}

static void gs_edit_write(gs_track *t, gs_edit_kind kind, uint8_t x, uint8_t y, int16_t v) {
    switch (kind) {
    case GS_EDIT_CORNER:
        t->corner[(size_t)y * GS_CORNER_STRIDE + (size_t)x] = v;
        break;
    case GS_EDIT_SURFACE:
        t->surface[GS_TILE_INDEX(x, y)] = (uint8_t)v;
        break;
    case GS_EDIT_GRAVITY:
        t->gravity[GS_TILE_INDEX(x, y)] = (uint8_t)v;
        break;
    case GS_EDIT_GATE_ADD:
    case GS_EDIT_GATE_REMOVE:
    case GS_EDIT_GATE_MOVE:
    case GS_EDIT_ROUTE_KIND:
    case GS_EDIT_COUNT:
        break;      // the route is not a tile; see gs_edit_route
    }
}

static bool gs_edit_in_range(const gs_track *t, gs_edit_kind kind, uint8_t x, uint8_t y) {
    // Corners are one more than tiles along each axis - a 4x4 track has 5x5 of
    // them - which is the off-by-one this function exists to stop.
    if (kind == GS_EDIT_CORNER) return x <= t->w && y <= t->h;
    return x < t->w && y < t->h;
}

static bool gs_edit_apply(gs_edit_log *l, gs_track *t, gs_edit_kind kind,
                          uint8_t x, uint8_t y, int16_t after) {
    if (!gs_edit_in_range(t, kind, x, y)) return true;

    int16_t before = gs_edit_read(t, kind, x, y);
    if (before == after) return true;   // nothing happened; nothing to undo

    if (l->cursor >= l->cap) return false;

    // A bare edit is its own transaction; one inside begin/end joins that one.
    if (!l->open) l->group++;

    gs_edit *e = &l->ops[l->cursor];
    e->group = l->group;
    e->kind = (uint8_t)kind;
    e->x = x;
    e->y = y;
    e->index = 0;
    e->pad = 0;
    e->before = before;
    e->after = after;
    e->gate = (gs_gate){ 0 };

    gs_edit_write(t, kind, x, y, after);
    l->cursor++;

    // A fresh edit after an undo drops whatever was ahead: the history ends
    // here now. That is what makes it a line rather than a tree, and it is what
    // everyone expects. Assigning rather than growing is the whole mechanism.
    l->count = l->cursor;
    return true;
}

bool gs_edit_corner(gs_edit_log *l, gs_track *t, uint8_t x, uint8_t y, gs_fix height) {
    int32_t stored = height >> GS_HEIGHT_SHIFT;
    return gs_edit_apply(l, t, GS_EDIT_CORNER, x, y,
                         (int16_t)GS_CLAMP(stored, INT16_MIN, INT16_MAX));
}

bool gs_edit_surface(gs_edit_log *l, gs_track *t, uint8_t x, uint8_t y, gs_surface s) {
    if (s >= GS_SURF_COUNT) return true;
    return gs_edit_apply(l, t, GS_EDIT_SURFACE, x, y, (int16_t)s);
}

bool gs_edit_gravity(gs_edit_log *l, gs_track *t, uint8_t x, uint8_t y, gs_fix multiplier) {
    int32_t units = (int32_t)(((int64_t)multiplier * GS_GRAVITY_UNIT) >> GS_FIX_SHIFT);
    return gs_edit_apply(l, t, GS_EDIT_GRAVITY, x, y,
                         (int16_t)GS_CLAMP(units, 0, 255));
}

// --- the route ---------------------------------------------------------------
//
// Gates are a short ordered array rather than a grid, so putting one back is a
// matter of restoring both what it was and where in the order it was. Doing the
// shuffling here rather than in gs_track keeps the "insert at an index"
// operation - which only an undo ever wants - out of the track's own interface.

static void gs_gate_insert(gs_track *t, uint8_t at, const gs_gate *g) {
    if (t->gate_count >= GS_TRACK_MAX_GATES) return;
    if (at > t->gate_count) at = t->gate_count;

    for (uint8_t i = t->gate_count; i > at; i--) t->gate[i] = t->gate[i - 1];
    t->gate[at] = *g;
    t->gate_count++;
}

static void gs_gate_delete(gs_track *t, uint8_t at) {
    if (at >= t->gate_count) return;
    for (uint8_t i = at; i + 1 < t->gate_count; i++) t->gate[i] = t->gate[i + 1];
    t->gate_count--;
    t->gate[t->gate_count] = (gs_gate){ 0 };
}

// One entry in the log for a route change. The two kinds are each other's
// reverse, which is all undo and redo need to know.
static bool gs_edit_route(gs_edit_log *l, gs_edit_kind kind, uint8_t index,
                          const gs_gate *g) {
    if (l->cursor >= l->cap) return false;
    if (!l->open) l->group++;

    gs_edit *e = &l->ops[l->cursor];
    e->group = l->group;
    e->kind = (uint8_t)kind;
    e->x = 0;
    e->y = 0;
    e->index = index;
    e->pad = 0;
    e->before = 0;
    e->after = 0;
    e->gate = *g;

    l->cursor++;
    l->count = l->cursor;
    return true;
}

int gs_edit_add_gate(gs_edit_log *l, gs_track *t, gs_fix x, gs_fix y,
                     gs_angle heading, gs_fix half_width) {
    if (l->cursor >= l->cap) return -1;

    int at = gs_track_add_gate(t, x, y, heading, half_width);
    if (at < 0) return -1;

    if (!gs_edit_route(l, GS_EDIT_GATE_ADD, (uint8_t)at, &t->gate[at])) {
        // The log is full, so the change cannot be taken back - and an edit
        // that cannot be undone is worse than one that did not happen.
        gs_gate_delete(t, (uint8_t)at);
        return -1;
    }
    return at;
}

bool gs_edit_remove_gate(gs_edit_log *l, gs_track *t, uint8_t index) {
    if (index >= t->gate_count) return false;
    if (l->cursor >= l->cap) return false;

    gs_gate was = t->gate[index];
    if (!gs_edit_route(l, GS_EDIT_GATE_REMOVE, index, &was)) return false;

    gs_gate_delete(t, index);
    return true;
}

bool gs_edit_move_gate(gs_edit_log *l, gs_track *t, uint8_t from, uint8_t to) {
    if (from >= t->gate_count || to >= t->gate_count) return false;
    if (from == to) return true;

    gs_gate moving = t->gate[from];

    if (!gs_edit_route(l, GS_EDIT_GATE_MOVE, to, &moving)) return false;
    l->ops[l->cursor - 1].before = (int16_t)from;

    gs_gate_delete(t, from);
    gs_gate_insert(t, to, &moving);
    return true;
}

// The route kind rides in the same log as everything else. It is not a tile, so
// it borrows the gate record's `before` and `after` for what it was and what it
// became, and touches no gate at all.
bool gs_edit_route_kind(gs_edit_log *l, gs_track *t, gs_route_kind kind) {
    if (t->route == (uint8_t)kind) return true;
    if (l->cursor >= l->cap) return false;
    if (!l->open) l->group++;

    gs_edit *e = &l->ops[l->cursor];
    *e = (gs_edit){ 0 };
    e->group = l->group;
    e->kind = (uint8_t)GS_EDIT_ROUTE_KIND;
    e->before = (int16_t)t->route;
    e->after = (int16_t)kind;

    l->cursor++;
    l->count = l->cursor;

    t->route = (uint8_t)kind;
    return true;
}

// Undoing one entry, whichever kind it is.
static void gs_edit_reverse(gs_track *t, const gs_edit *e) {
    // Named one by one rather than defaulted: a kind of edit added later must
    // say how it is taken back, and a `default` here would have it silently
    // written to a tile instead - which for anything that is not a tile is a
    // track quietly left in the wrong state by pressing undo.
    switch ((gs_edit_kind)e->kind) {
    case GS_EDIT_GATE_ADD:    gs_gate_delete(t, e->index); break;
    case GS_EDIT_GATE_REMOVE: gs_gate_insert(t, e->index, &e->gate); break;
    case GS_EDIT_GATE_MOVE:
        gs_gate_delete(t, e->index);
        gs_gate_insert(t, (uint8_t)e->before, &e->gate);
        break;
    case GS_EDIT_ROUTE_KIND: t->route = (uint8_t)e->before; break;
    case GS_EDIT_CORNER:
    case GS_EDIT_SURFACE:
    case GS_EDIT_GRAVITY:
        gs_edit_write(t, (gs_edit_kind)e->kind, e->x, e->y, e->before);
        break;
    case GS_EDIT_COUNT: break;      // not an edit
    }
}

static void gs_edit_forward(gs_track *t, const gs_edit *e) {
    switch ((gs_edit_kind)e->kind) {
    case GS_EDIT_GATE_ADD:    gs_gate_insert(t, e->index, &e->gate); break;
    case GS_EDIT_GATE_REMOVE: gs_gate_delete(t, e->index); break;
    case GS_EDIT_GATE_MOVE:
        gs_gate_delete(t, (uint8_t)e->before);
        gs_gate_insert(t, e->index, &e->gate);
        break;
    case GS_EDIT_ROUTE_KIND: t->route = (uint8_t)e->after; break;
    case GS_EDIT_CORNER:
    case GS_EDIT_SURFACE:
    case GS_EDIT_GRAVITY:
        gs_edit_write(t, (gs_edit_kind)e->kind, e->x, e->y, e->after);
        break;
    case GS_EDIT_COUNT: break;      // not an edit
    }
}

bool gs_edit_can_undo(const gs_edit_log *l) { return l->cursor > 0; }
bool gs_edit_can_redo(const gs_edit_log *l) { return l->cursor < l->count; }

bool gs_edit_undo(gs_edit_log *l, gs_track *t) {
    if (!gs_edit_can_undo(l)) return false;

    uint32_t group = l->ops[l->cursor - 1].group;
    // Backwards, so that two edits to the same tile in one stroke unwind in the
    // order they were made and the first one's `before` wins.
    while (l->cursor > 0 && l->ops[l->cursor - 1].group == group) {
        gs_edit_reverse(t, &l->ops[l->cursor - 1]);
        l->cursor--;
    }
    return true;
}

bool gs_edit_redo(gs_edit_log *l, gs_track *t) {
    if (!gs_edit_can_redo(l)) return false;

    uint32_t group = l->ops[l->cursor].group;
    while (l->cursor < l->count && l->ops[l->cursor].group == group) {
        gs_edit_forward(t, &l->ops[l->cursor]);
        l->cursor++;
    }
    return true;
}

static uint32_t gs_edit_groups(const gs_edit_log *l, uint32_t from, uint32_t to) {
    uint32_t groups = 0;
    for (uint32_t i = from; i < to; i++) {
        if (i == from || l->ops[i].group != l->ops[i - 1].group) groups++;
    }
    return groups;
}

uint32_t gs_edit_undo_depth(const gs_edit_log *l) {
    return gs_edit_groups(l, 0, l->cursor);
}

uint32_t gs_edit_redo_depth(const gs_edit_log *l) {
    return gs_edit_groups(l, l->cursor, l->count);
}

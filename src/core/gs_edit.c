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
    e->pad = 0;
    e->before = before;
    e->after = after;

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

bool gs_edit_can_undo(const gs_edit_log *l) { return l->cursor > 0; }
bool gs_edit_can_redo(const gs_edit_log *l) { return l->cursor < l->count; }

bool gs_edit_undo(gs_edit_log *l, gs_track *t) {
    if (!gs_edit_can_undo(l)) return false;

    uint32_t group = l->ops[l->cursor - 1].group;
    // Backwards, so that two edits to the same tile in one stroke unwind in the
    // order they were made and the first one's `before` wins.
    while (l->cursor > 0 && l->ops[l->cursor - 1].group == group) {
        const gs_edit *e = &l->ops[l->cursor - 1];
        gs_edit_write(t, (gs_edit_kind)e->kind, e->x, e->y, e->before);
        l->cursor--;
    }
    return true;
}

bool gs_edit_redo(gs_edit_log *l, gs_track *t) {
    if (!gs_edit_can_redo(l)) return false;

    uint32_t group = l->ops[l->cursor].group;
    while (l->cursor < l->count && l->ops[l->cursor].group == group) {
        const gs_edit *e = &l->ops[l->cursor];
        gs_edit_write(t, (gs_edit_kind)e->kind, e->x, e->y, e->after);
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

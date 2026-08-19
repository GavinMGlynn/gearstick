#include "core/gs_profile.h"

#include <string.h>

static const char *const gs_colour_names[GS_COLOUR_COUNT] = {
    "red", "blue", "green", "yellow", "orange", "purple", "white", "black",
};

const char *gs_colour_name(uint8_t colour) {
    return colour < GS_COLOUR_COUNT ? gs_colour_names[colour] : "?";
}

void gs_profiles_clear(gs_profiles *p) {
    memset(p, 0, sizeof *p);
}

int gs_profile_find(const gs_profiles *p, const char *name) {
    if (name == nullptr) return -1;
    for (uint8_t i = 0; i < p->count; i++) {
        if (strncmp(p->entry[i].name, name, GS_PROFILE_NAME) == 0) return (int)i;
    }
    return -1;
}

int gs_profile_add(gs_profiles *p, const char *name, uint8_t colour, uint8_t vehicle) {
    if (name == nullptr || name[0] == '\0') return -1;
    if (p->count >= GS_PROFILES_MAX) return -1;
    if (gs_profile_find(p, name) >= 0) return -1;

    gs_profile *e = &p->entry[p->count];
    memset(e, 0, sizeof *e);
    strncpy(e->name, name, GS_PROFILE_NAME - 1);
    e->colour = colour < GS_COLOUR_COUNT ? colour : 0;
    e->vehicle = vehicle < GS_VEH_COUNT ? vehicle : 0;
    return (int)p->count++;
}

bool gs_profile_remove(gs_profiles *p, uint8_t index) {
    if (index >= p->count) return false;
    for (uint8_t i = index; i + 1 < p->count; i++) p->entry[i] = p->entry[i + 1];
    p->count--;
    memset(&p->entry[p->count], 0, sizeof p->entry[p->count]);
    return true;
}

void gs_profile_raced(gs_profiles *p, uint8_t index, bool won, bool podium,
                      bool wrecked, uint32_t tiles, uint64_t when) {
    if (index >= p->count) return;
    gs_profile *e = &p->entry[index];
    e->races++;
    if (won) e->wins++;
    if (podium) e->podiums++;
    if (wrecked) e->wrecks++;
    e->tiles += tiles;
    if (when != 0) e->last_raced = when;
}

// --- the wire format -------------------------------------------------------

// What one profile takes on disk, per version. Version two appended eight bytes
// for the date; everything before it is where it was, which is what lets one
// reader handle both.
#define GS_PROFILE_BYTES_V1 (GS_PROFILE_NAME + 1 + 1 + 4 + 4 + 4 + 4 + 8)
#define GS_PROFILE_BYTES    (GS_PROFILE_BYTES_V1 + 8)
#define GS_PROFILES_HEADER (4 + 4 + 4)

static void gs_put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xffu);
}

static uint32_t gs_get32(const uint8_t *p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}

static void gs_put64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xffu);
}

static uint64_t gs_get64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

size_t gs_profiles_size(const gs_profiles *p) {
    return GS_PROFILES_HEADER + (size_t)p->count * GS_PROFILE_BYTES;
}

size_t gs_profiles_serialize(const gs_profiles *p, uint8_t *buf, size_t cap) {
    size_t need = gs_profiles_size(p);
    if (cap < need) return 0;

    uint8_t *q = buf;
    gs_put32(q, GS_PROFILE_MAGIC);   q += 4;
    gs_put32(q, GS_PROFILE_VERSION); q += 4;
    gs_put32(q, p->count);           q += 4;

    for (uint8_t i = 0; i < p->count; i++) {
        const gs_profile *e = &p->entry[i];
        memcpy(q, e->name, GS_PROFILE_NAME); q += GS_PROFILE_NAME;
        *q++ = e->colour;
        *q++ = e->vehicle;
        gs_put32(q, e->races);   q += 4;
        gs_put32(q, e->wins);    q += 4;
        gs_put32(q, e->podiums); q += 4;
        gs_put32(q, e->wrecks);  q += 4;
        gs_put64(q, e->tiles);   q += 8;
        gs_put64(q, e->last_raced); q += 8;      // version 2
    }
    return need;
}

bool gs_profiles_deserialize(gs_profiles *p, const uint8_t *buf, size_t len) {
    if (len < GS_PROFILES_HEADER) return false;

    const uint8_t *q = buf;
    if (gs_get32(q) != GS_PROFILE_MAGIC) return false;
    q += 4;
    uint32_t version = gs_get32(q); q += 4;
    if (version < GS_PROFILE_OLDEST || version > GS_PROFILE_VERSION) return false;

    size_t row = version >= 2 ? GS_PROFILE_BYTES : GS_PROFILE_BYTES_V1;

    uint32_t count = gs_get32(q); q += 4;
    if (count > GS_PROFILES_MAX) return false;
    if (len < GS_PROFILES_HEADER + (size_t)count * row) return false;

    gs_profiles_clear(p);
    p->count = (uint8_t)count;

    for (uint8_t i = 0; i < p->count; i++) {
        gs_profile *e = &p->entry[i];
        memcpy(e->name, q, GS_PROFILE_NAME); q += GS_PROFILE_NAME;
        e->name[GS_PROFILE_NAME - 1] = '\0';
        e->colour = *q++;
        e->vehicle = *q++;
        if (e->colour >= GS_COLOUR_COUNT) e->colour = 0;
        if (e->vehicle >= GS_VEH_COUNT) e->vehicle = 0;
        e->races = gs_get32(q);   q += 4;
        e->wins = gs_get32(q);    q += 4;
        e->podiums = gs_get32(q); q += 4;
        e->wrecks = gs_get32(q);  q += 4;
        e->tiles = gs_get64(q);   q += 8;

        // Older rows end here, and say they do not know - which is the truth,
        // and why zero means that rather than the epoch.
        if (version >= 2) { e->last_raced = gs_get64(q); q += 8; }
        else              { e->last_raced = 0; }
    }
    return true;
}

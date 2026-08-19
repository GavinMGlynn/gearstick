#include "core/gs_library.h"

#include <string.h>

#define GS_TRACK_BYTES (GS_TRACK_TILES * 4 + 4096)

void gs_library_clear(gs_library *l) {
    memset(l, 0, sizeof *l);
}

int gs_library_find(const gs_library *l, uint64_t hash) {
    for (uint16_t i = 0; i < l->count; i++) {
        if (l->entry[i].hash == hash) return (int)i;
    }
    return -1;
}

static void gs_set_text(char *out, size_t cap, const char *in) {
    size_t n = 0;
    while (in != nullptr && in[n] != '\0' && n + 1 < cap) {
        out[n] = in[n];
        n++;
    }
    memset(out + n, 0, cap - n);
}

int gs_library_put(gs_library *l, const gs_track *t, const char *name,
                   const char *author) {
    uint64_t hash = gs_track_hash(t);

    // Already here. **The same track twice is one track** - that is what
    // content addressing is for - so this is a rename rather than a second copy.
    int at = gs_library_find(l, hash);
    if (at >= 0) {
        if (name != nullptr) gs_set_text(l->entry[at].name, GS_LIBRARY_NAME, name);
        if (author != nullptr) {
            gs_set_text(l->entry[at].author, GS_LIBRARY_AUTHOR, author);
        }
        return at;
    }

    if (l->count >= GS_LIBRARY_MAX) return -1;

    gs_library_entry *e = &l->entry[l->count];
    memset(e, 0, sizeof *e);
    e->hash = hash;
    e->track = *t;
    gs_set_text(e->name, GS_LIBRARY_NAME, name != nullptr ? name : "untitled");
    gs_set_text(e->author, GS_LIBRARY_AUTHOR, author != nullptr ? author : "");
    return (int)l->count++;
}

int gs_library_replace(gs_library *l, uint64_t was, const gs_track *now) {
    int at = gs_library_find(l, was);
    if (at < 0) return -1;

    uint64_t hash = gs_track_hash(now);

    // Edited into something already in the library. The slot being edited goes,
    // because keeping both would leave two entries that are the same track.
    int clash = gs_library_find(l, hash);
    if (clash >= 0 && clash != at) {
        gs_library_remove(l, was);
        return gs_library_find(l, hash);
    }

    l->entry[at].hash = hash;
    l->entry[at].track = *now;
    return at;
}

bool gs_library_remove(gs_library *l, uint64_t hash) {
    int at = gs_library_find(l, hash);
    if (at < 0) return false;

    for (uint16_t i = (uint16_t)at; i + 1 < l->count; i++) {
        l->entry[i] = l->entry[i + 1];
    }
    l->count--;
    memset(&l->entry[l->count], 0, sizeof l->entry[l->count]);
    return true;
}

const gs_library_entry *gs_library_at(const gs_library *l, int index) {
    if (index < 0 || index >= (int)l->count) return nullptr;
    return &l->entry[index];
}

const gs_track *gs_library_track(const gs_library *l, int index) {
    const gs_library_entry *e = gs_library_at(l, index);
    return e != nullptr ? &e->track : nullptr;
}

// --- the wire format -------------------------------------------------------

static void gs_put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xffu);
}

static uint32_t gs_get32(const uint8_t *p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}

size_t gs_library_size(const gs_library *l) {
    // The worst case, because the real size is only known once each track has
    // been serialised. A caller sizing a buffer wants the ceiling.
    return 12 + (size_t)l->count *
        (GS_LIBRARY_NAME + GS_LIBRARY_AUTHOR + 4 + GS_TRACK_BYTES);
}

size_t gs_library_serialize(const gs_library *l, uint8_t *buf, size_t cap) {
    if (cap < 12) return 0;

    size_t n = 0;
    gs_put32(buf + n, GS_LIBRARY_MAGIC);   n += 4;
    gs_put32(buf + n, GS_LIBRARY_VERSION); n += 4;
    gs_put32(buf + n, l->count);           n += 4;

    for (uint16_t i = 0; i < l->count; i++) {
        const gs_library_entry *e = &l->entry[i];

        if (n + GS_LIBRARY_NAME + GS_LIBRARY_AUTHOR + 4 > cap) return 0;
        memcpy(buf + n, e->name, GS_LIBRARY_NAME);     n += GS_LIBRARY_NAME;
        memcpy(buf + n, e->author, GS_LIBRARY_AUTHOR); n += GS_LIBRARY_AUTHOR;

        // The track, serialised rather than copied whole: the struct is
        // seventeen kilobytes and the format is a few.
        size_t wrote = gs_track_serialize(&e->track, buf + n + 4, cap - n - 4);
        if (wrote == 0) return 0;
        gs_put32(buf + n, (uint32_t)wrote);
        n += 4 + wrote;
    }
    return n;
}

bool gs_library_deserialize(gs_library *l, const uint8_t *buf, size_t len) {
    if (len < 12) return false;
    if (gs_get32(buf) != GS_LIBRARY_MAGIC) return false;
    if (gs_get32(buf + 4) != GS_LIBRARY_VERSION) return false;

    uint32_t count = gs_get32(buf + 8);
    if (count > GS_LIBRARY_MAX) return false;

    gs_library_clear(l);
    size_t n = 12;

    for (uint32_t i = 0; i < count; i++) {
        if (n + GS_LIBRARY_NAME + GS_LIBRARY_AUTHOR + 4 > len) return false;

        gs_library_entry *e = &l->entry[i];
        memset(e, 0, sizeof *e);

        memcpy(e->name, buf + n, GS_LIBRARY_NAME);
        e->name[GS_LIBRARY_NAME - 1] = '\0';
        n += GS_LIBRARY_NAME;

        memcpy(e->author, buf + n, GS_LIBRARY_AUTHOR);
        e->author[GS_LIBRARY_AUTHOR - 1] = '\0';
        n += GS_LIBRARY_AUTHOR;

        uint32_t bytes = gs_get32(buf + n);
        n += 4;
        if (bytes == 0 || n + bytes > len) return false;
        if (!gs_track_deserialize(&e->track, buf + n, bytes)) return false;
        n += bytes;

        // **Derived, never read from the file.** A library whose stored hash
        // disagreed with its stored track would be a library that lies about
        // what it holds, and the track is the thing that is actually there.
        e->hash = gs_track_hash(&e->track);
        l->count++;
    }
    return true;
}

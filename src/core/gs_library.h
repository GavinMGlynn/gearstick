// gs_library.h - the tracks you have, rather than the track you saved.
//
// **Keyed by content, like everything else here.** A track already knows what
// it is, so the library needs no naming authority and no ids: storing the same
// track twice stores it once, and two people who built the same thing have the
// same entry. A name and an author sit *beside* the track rather than
// identifying it - rename a track and it is the same track, which is what
// anybody would expect and what an id-keyed library gets wrong.
//
// Editing is the interesting case. A changed track has a changed hash, so it is
// honestly a different track; `gs_library_replace` is for the case where that
// is not what the person meant - they were working on *this* one and want the
// slot to follow. Both exist because both happen.
#ifndef GS_LIBRARY_H
#define GS_LIBRARY_H

#include "core/gs_track.h"

#define GS_LIBRARY_MAGIC   0x424C5347u   // "GSLB"
// Version 2 carries whether an entry came with the game. Version 1 libraries
// still load, with every entry treated as the player's own - which is what they
// all were before the game shipped any.
#define GS_LIBRARY_VERSION 2u

#define GS_LIBRARY_MAX    32
#define GS_LIBRARY_NAME   48
#define GS_LIBRARY_AUTHOR 24

typedef struct gs_library_entry {
    uint64_t hash;                       // the identity; derived, never set
    char     name[GS_LIBRARY_NAME];
    char     author[GS_LIBRARY_AUTHOR];

    // **Did this come with the game?** A track that shipped is not the
    // player's to change or throw away: editing one makes a copy and edits
    // that, and deleting one is refused. It is not a property of the track -
    // the same ground built by hand is an ordinary track - it is a property of
    // where this entry came from, which is why it lives here beside the name
    // rather than anywhere near the content hash.
    bool     builtin;

    gs_track track;
} gs_library_entry;

// About half a megabyte, so a heap or static object rather than a local - the
// same rule the replay carries. On disk it is far smaller: each track is
// serialised, and a track compresses to a few kilobytes.
typedef struct gs_library {
    uint16_t         count;
    gs_library_entry entry[GS_LIBRARY_MAX];
} gs_library;

void gs_library_clear(gs_library *l);

// Add a track, or update the name and author of one already here. Returns its
// index, or -1 if the library is full. The hash is taken from the track rather
// than given, because it is not a thing a caller gets to decide.
int gs_library_put(gs_library *l, const gs_track *t, const char *name,
                   const char *author);

// The same, for a track that came with the game. Kept separate rather than
// given a flag argument, because every existing caller is a person keeping
// their own work and none of them should have to say so.
int gs_library_put_builtin(gs_library *l, const gs_track *t, const char *name,
                           const char *author);

// Is this one the game's rather than the player's? False for anything not here,
// which is the safe way round: an entry nobody can find is not protected.
bool gs_library_is_builtin(const gs_library *l, uint64_t hash);

// Replace what is in a slot with an edited version of it, keeping the name and
// author. Returns the new index, or -1 if `was` is not here.
int gs_library_replace(gs_library *l, uint64_t was, const gs_track *now);

int  gs_library_find(const gs_library *l, uint64_t hash);
bool gs_library_remove(gs_library *l, uint64_t hash);

const gs_library_entry *gs_library_at(const gs_library *l, int index);
const gs_track         *gs_library_track(const gs_library *l, int index);

// On disk. Each track is serialised rather than copied whole, so a library of
// thirty tracks is a hundred kilobytes rather than half a megabyte.
size_t gs_library_size(const gs_library *l);
size_t gs_library_serialize(const gs_library *l, uint8_t *buf, size_t cap);
bool   gs_library_deserialize(gs_library *l, const uint8_t *buf, size_t len);

#endif // GS_LIBRARY_H

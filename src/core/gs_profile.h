// gs_profile.h - who is playing.
//
// A name, a colour and a favourite machine, kept between sessions. Small, and
// worth having for a reason bigger than convenience: a record with a name on it
// is somebody's record. "Best lap 0:42.1" is a number; "Gavin, 0:42.1, baja
// bug" is a thing to beat.
//
// **Nothing here is in the simulation.** A colour cannot change where a car
// ends up, so it is not part of the state two machines have to agree about and
// it is not in the hash - which is also why a profile can be renamed and
// recoloured without invalidating a single replay or record.
#ifndef GS_PROFILE_H
#define GS_PROFILE_H

#include "core/gs_common.h"
#include "core/gs_vehicle.h"

#define GS_PROFILE_MAGIC   0x50525347u   // "GSRP" with a P
#define GS_PROFILE_VERSION 1u

#define GS_PROFILES_MAX 16
#define GS_PROFILE_NAME 16

// The colours a car can be painted. Eight, so four players can all be different
// and still have something to argue about, and chosen to be told apart at the
// size a car is drawn rather than to look good on a chart.
typedef enum gs_paint_colour {
    GS_COLOUR_RED = 0,
    GS_COLOUR_BLUE,
    GS_COLOUR_GREEN,
    GS_COLOUR_YELLOW,
    GS_COLOUR_ORANGE,
    GS_COLOUR_PURPLE,
    GS_COLOUR_WHITE,
    GS_COLOUR_BLACK,
    GS_COLOUR_COUNT
} gs_paint_colour;

const char *gs_colour_name(uint8_t colour);

typedef struct gs_profile {
    char    name[GS_PROFILE_NAME];
    uint8_t colour;         // gs_paint_colour
    uint8_t vehicle;        // the one they are handed at the setup screen

    // A little history, because a profile that only remembers a colour is a
    // settings entry rather than a person.
    uint32_t races;
    uint32_t wins;
    uint32_t podiums;
    uint32_t wrecks;
    uint64_t tiles;         // how far they have driven, all told
} gs_profile;

typedef struct gs_profiles {
    uint8_t    count;
    gs_profile entry[GS_PROFILES_MAX];
} gs_profiles;

void gs_profiles_clear(gs_profiles *p);

// Add somebody. Returns their index, or -1 if the roster is full or the name is
// already taken - two people called the same thing would be two people sharing
// a record.
int gs_profile_add(gs_profiles *p, const char *name, uint8_t colour, uint8_t vehicle);

// Find by name, or -1.
int gs_profile_find(const gs_profiles *p, const char *name);

// Remove one, keeping the rest in order. Their records stay: a record belongs
// to the track, not to the roster, and deleting a profile should not quietly
// rewrite the history of a track.
bool gs_profile_remove(gs_profiles *p, uint8_t index);

// After a race.
void gs_profile_raced(gs_profiles *p, uint8_t index, bool won, bool podium,
                      bool wrecked, uint32_t tiles);

size_t gs_profiles_size(const gs_profiles *p);
size_t gs_profiles_serialize(const gs_profiles *p, uint8_t *buf, size_t cap);
bool   gs_profiles_deserialize(gs_profiles *p, const uint8_t *buf, size_t len);

#endif // GS_PROFILE_H

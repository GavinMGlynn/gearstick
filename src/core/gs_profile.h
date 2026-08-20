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
// **Three, because a profile can now be locked.**
//
// Version one is still read, which is the point of there being a number. See
// gs_records.h: a format that only recognises itself is a format that loses
// somebody's history the first time a field is added, and a field always is.
// A roster written by version two loads with every profile unlocked, which is
// the truth about it rather than a default.
#define GS_PROFILE_VERSION 3u

// The oldest layout still read.
#define GS_PROFILE_OLDEST 1u

#define GS_PROFILES_MAX 16
#define GS_PROFILE_NAME 16

// Deliberately the same numbers as GS_AUTH_HASH_BYTES and GS_AUTH_SECRET_BYTES
// in src/net/gs_auth.h, written out rather than included: core including a
// header from src/net/ would be the layering violation cmake/Layering.cmake
// exists to catch. A static assert in gs_auth.c pins the two together, so they
// cannot drift apart quietly.
#define GS_PROFILE_PASSWORD 128
#define GS_PROFILE_TOTP     20

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

    // The last time they raced, as a Unix time; zero for "never recorded",
    // which is what every profile written before version two says. Passed in
    // from outside, because src/core/ links nothing and cannot read a clock.
    uint64_t last_raced;

    // **What locks the profile, carried but never understood here.** These are
    // opaque bytes to src/core/: an Argon2id encoded hash and an RFC 4238
    // shared secret, both produced and checked in src/net/gs_auth.c, because
    // core links nothing and libsodium is a something. Storing them is a
    // memcpy, which core can do; deciding whether a password is right is not.
    //
    // An empty `password` means the profile has no password, and that is a
    // supported state rather than an unfinished one - a machine with one person
    // on it should not have to type anything to play.
    char    password[GS_PROFILE_PASSWORD];
    uint8_t totp[GS_PROFILE_TOTP];
    uint8_t totp_len;               // 0 when there is no second factor
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

// After a race. `when` is a Unix time, or zero from a caller with no clock -
// which is everything inside src/core/, because it links nothing.
void gs_profile_raced(gs_profiles *p, uint8_t index, bool won, bool podium,
                      bool wrecked, uint32_t tiles, uint64_t when);

size_t gs_profiles_size(const gs_profiles *p);
size_t gs_profiles_serialize(const gs_profiles *p, uint8_t *buf, size_t cap);
bool   gs_profiles_deserialize(gs_profiles *p, const uint8_t *buf, size_t len);

#endif // GS_PROFILE_H

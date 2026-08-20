// gs_store.h - what the server remembers.
//
// **SQLite, on the server only.** The client's store is the same versioned flat
// file it has always been and the simulation still links nothing; this is a
// server-side concern and stays there. See `cmake/Sqlite.cmake` for how it is
// obtained - the single-file amalgamation, pinned by SHA-256, compiled like any
// other source file.
//
// Three things live here, and the shape of each is decided by what it is rather
// than by what is convenient:
//
//   drivers   a name, a colour, a machine, and what they have done
//   records   a time on a track under conditions over a distance - the key is
//             all four, because a lap set at a sixth of gravity is not a lap
//             and a three-lap time is not a five-lap time
//   tracks    content-addressed blobs, because a track already knows its own
//             name: two people uploading the same track collide correctly
//             rather than making two rows of it
#ifndef GS_STORE_H
#define GS_STORE_H

#include "core/gs_common.h"

#define GS_STORE_NAME 32

typedef struct gs_store gs_store;

// Open, creating the schema if the file is new. `path` may be ":memory:",
// which is what the tests use. Null if it cannot be opened.
gs_store *gs_store_open(const char *path);
void      gs_store_close(gs_store *s);

// The last thing that went wrong, for a log a person will actually read.
const char *gs_store_error(const gs_store *s);

// What schema version the file is at.
int gs_store_version(const gs_store *s);

// --- drivers ---------------------------------------------------------------

// Remember somebody, or update what is remembered about them. Returns their id,
// or 0. A name is the identity here, because a name is what a record carries.
int64_t gs_store_put_driver(gs_store *s, const char *name, uint8_t colour,
                            uint8_t vehicle);
int64_t gs_store_find_driver(gs_store *s, const char *name);
bool    gs_store_driver(gs_store *s, int64_t id, char *name, size_t cap,
                        uint8_t *colour, uint8_t *vehicle);
int     gs_store_driver_count(gs_store *s);

// --- records ---------------------------------------------------------------

// A time, offered. Kept only where it beats what that driver already has, which
// is the same rule the client's table keeps. Returns true if anything changed.
bool gs_store_put_record(gs_store *s, uint64_t track, uint64_t conditions,
                         uint16_t laps, const char *who, uint8_t vehicle,
                         uint32_t lap_ticks, uint32_t race_ticks);

// The best on a track under conditions. Zero if nobody has been round it.
// `who` may be null.
uint32_t gs_store_best_lap(gs_store *s, uint64_t track, uint64_t conditions,
                           char *who, size_t cap);
uint32_t gs_store_best_race(gs_store *s, uint64_t track, uint64_t conditions,
                            uint16_t laps, char *who, size_t cap);
int gs_store_record_count(gs_store *s);

// --- tracks ----------------------------------------------------------------

// Keep a track. The hash is the key, so storing the same track twice stores it
// once - and two people who built the same thing are storing the same thing.
bool gs_store_put_track(gs_store *s, uint64_t hash, const char *name,
                        const char *author, const uint8_t *bytes, size_t len);

// Fetch one. `out` may be null to ask only whether it is there and how big.
bool gs_store_get_track(gs_store *s, uint64_t hash, uint8_t *out, size_t cap,
                        size_t *len);
bool gs_store_has_track(gs_store *s, uint64_t hash);

// When a track was added, as a Unix time. Set explicitly only by the tool that
// builds the library shipped with the server: **a file that is committed has to
// be the same file every time it is built**, and "now" is the one thing in this
// schema that is not. The date a stock track was added is meaningless anyway -
// it shipped with the game.
bool gs_store_set_added(gs_store *s, uint64_t hash, int64_t when);

// And reading it back. Negative if there is no such track.
int64_t gs_store_added(gs_store *s, uint64_t hash);
int  gs_store_track_count(gs_store *s);

typedef struct gs_track_row {
    uint64_t hash;
    char     name[64];
    char     author[GS_STORE_NAME];
    uint32_t bytes;
} gs_track_row;

// The library, newest first. Returns how many rows were written.
int gs_store_list_tracks(gs_store *s, gs_track_row *out, int cap);

// --- sessions ---------------------------------------------------------------
//
// **A nonce the server issued to somebody, once.** A submission carries the one
// it was given, and a nonce that was never issued, was issued to somebody else,
// has already been spent or has expired buys nothing.
//
// Kept here rather than in memory for the same reason as everything else the
// server knows: a server that forgot its sessions on restart could not say
// whether a nonce had been spent, and a nonce nobody can retire can be handed in
// for ever - which is exactly what it exists to stop.

// Record one. False if that nonce is already known, which is the collision case
// and means the caller should pick another.
bool gs_store_issue_session(gs_store *s, uint64_t nonce, const char *who,
                            int64_t now, int64_t lifetime);

// Spend one. **True only if it was issued, to this person, unspent and still in
// date** - all four checked in the one statement, because reading a row and then
// updating it leaves a gap, and the gap is where the same nonce is spent twice.
bool gs_store_spend_session(gs_store *s, uint64_t nonce, const char *who,
                            int64_t now);

// Throw away the ones that have expired. Returns how many went.
int gs_store_forget_sessions(gs_store *s, int64_t before);
int gs_store_session_count(gs_store *s);

// --- who this server is -----------------------------------------------------
//
// **The server's static secret key**, which is the thing every client's
// handshake is aimed at. In the database with everything else the server knows,
// for the same reason as the sessions: a server that generated a new identity
// on every restart would be a different server every time, and every client
// that had been told which server to trust would be right to refuse it.
//
// `gs_store_identity` returns false when there is none yet, which is how the
// server knows to make one.
#define GS_STORE_IDENTITY_BYTES 32

bool gs_store_identity(gs_store *s, uint8_t *secret);
bool gs_store_set_identity(gs_store *s, const uint8_t *secret);

// --- publishing ------------------------------------------------------------
//
// **Published is a separate thing from stored.** The server holds every track
// it has been handed, because it needs them to verify times on them; publishing
// is somebody saying "and let people have this one". Taking it down stops it
// being listed and leaves the track where it is, so times set on it stay
// checkable.

bool gs_store_publish(gs_store *s, uint64_t hash, const char *name,
                      const char *author);

// Only by whoever put it up. False if it is not theirs, or not there.
bool gs_store_withdraw(gs_store *s, uint64_t hash, const char *author);

bool gs_store_is_published(gs_store *s, uint64_t hash);
int  gs_store_list_published(gs_store *s, gs_track_row *out, int cap);

#endif // GS_STORE_H

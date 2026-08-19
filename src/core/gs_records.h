// gs_records.h - what has been done on a track, and by whom.
//
// **Keyed by the track's content hash**, which is the whole reason this is
// simple. A track already carries an identity that two people who built the
// same thing arrive at independently, so a record set on your copy of a track
// is a record on mine, and a track edited by one corner is honestly a different
// track with its own empty table rather than a place to keep beating a record
// on ground that has moved.
//
// A record is a *time on a track under conditions*. Gravity, drag, friction and
// damage are all dials a player can move, and a lap set at a third of Earth
// gravity is not a lap - so the conditions are part of the key. The same
// discipline as the golden replay, for the same reason.
//
// No allocation and no file I/O, like everything else in src/core/: this owns
// the table and the rules, and the frontend owns the disk.
#ifndef GS_RECORDS_H
#define GS_RECORDS_H

#include "core/gs_sim.h"

#define GS_RECORDS_MAGIC   0x43525347u   // "GSRC"
// **Two, because a record now says when it was set.**
//
// Version one is still read, and that is the point of there being a number at
// all. A format that only recognised itself meant somebody's history vanished
// the first time it moved - which is what "refuses to load anything it does not
// recognise" amounts to, however correct it looks written down.
#define GS_RECORDS_VERSION 2u

// The oldest layout this can still read. Everything from here to the current
// version loads; anything else is refused, because guessing at a format is how
// a table of times becomes a table of noise.
#define GS_RECORDS_OLDEST 1u

// Enough for a lot of tracks, and small enough to be a static object: about
// forty kilobytes.
#define GS_RECORDS_MAX  512
#define GS_NAME_MAX     16

typedef struct gs_record {
    uint64_t track;          // the track's content hash
    uint64_t conditions;     // a hash of the dials it was set under

    uint32_t lap;            // best lap, in ticks; 0 for none
    uint32_t race;           // best full race, in ticks; 0 for none
    uint16_t laps;           // how long that race was
    uint8_t  vehicle;
    uint8_t  mode;           // gs_mode

    char     who[GS_NAME_MAX];

    // When it was set, as a Unix time; zero for "not recorded", which is what
    // every record written before version two says. **Passed in rather than
    // read**: src/core/ links nothing, and a simulation that could read a clock
    // is a simulation whose result depends on when it ran.
    uint64_t when;
} gs_record;

typedef struct gs_records {
    uint16_t count;
    gs_record entry[GS_RECORDS_MAX];
} gs_records;

void gs_records_clear(gs_records *r);

// The conditions a world is being raced under, as one number. Two races
// comparable on the same table are two races with the same answer here.
uint64_t gs_conditions_hash(const gs_world *w);

// Offer a result. Returns what it beat - both can be true, and both false when
// it beat nothing. `lap` or `race` may be zero for "did not set one".
typedef struct gs_record_beat {
    bool lap;
    bool race;
} gs_record_beat;

// `when` is a Unix time, or zero if the caller has no clock worth trusting -
// which every caller inside src/core/ is, since it links nothing.
gs_record_beat gs_records_submit(gs_records *r, uint64_t track, uint64_t conditions,
                                 uint8_t vehicle, uint8_t mode, uint16_t laps,
                                 uint32_t lap_ticks, uint32_t race_ticks,
                                 const char *who, uint64_t when);

// The best on this track under these conditions, or null if nobody has been
// round it. The lap record is across every vehicle; a per-vehicle one is a
// different question and a longer table.
const gs_record *gs_records_best_lap(const gs_records *r, uint64_t track,
                                     uint64_t conditions);
const gs_record *gs_records_best_race(const gs_records *r, uint64_t track,
                                      uint64_t conditions, uint16_t laps);

// Everything known about one track, newest-best first, for a table on screen.
// Returns how many were written.
uint16_t gs_records_for(const gs_records *r, uint64_t track, uint64_t conditions,
                        const gs_record **out, uint16_t cap);

// On disk, explicit and little-endian, so records written on one machine read
// on another - which they have to, because the track they are about travels.
size_t gs_records_size(const gs_records *r);
size_t gs_records_serialize(const gs_records *r, uint8_t *buf, size_t cap);
bool   gs_records_deserialize(gs_records *r, const uint8_t *buf, size_t len);

#endif // GS_RECORDS_H

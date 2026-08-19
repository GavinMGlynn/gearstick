// gs_carrier.h - a track, in pieces, over a datagram socket.
//
// A track is a few kilobytes and a datagram is not, so it travels in chunks and
// is put back together at the other end. **The hash is the whole reassembly
// protocol**: every chunk carries the hash of the track it belongs to, and the
// rebuilt track has to hash to it. A track already knows what it is, so nothing
// here needs a transfer id, a session, or any way for two transfers to be
// confused with each other.
//
// Loss is handled the way everything else here handles it - by asking again
// rather than by acknowledging. The receiver knows which pieces are missing
// because it knows how many there are, so it asks for the track again and the
// sender resends; a chunk that arrives twice is written twice to the same
// place and costs nothing.
//
// No sockets and no SDL, like the rest of src/net/.
#ifndef GS_CARRIER_H
#define GS_CARRIER_H

#include "core/gs_replay.h"
#include "core/gs_track.h"
#include "net/gs_proto.h"

// **Big enough for the largest thing that travels this way, which is not a
// track.** A track is about four kilobytes; the replay behind a claimed time is
// one byte per car per tick and a three-lap race is tens of thousands of ticks.
// Sizing this from the track format looked reasonable and quietly truncated
// every proof: the chunks past the end were refused, the transfer never
// completed, and an honest time was never verified. Sized from the format that
// is actually biggest, and derived rather than guessed.
#define GS_CARRIER_MAX_BYTES \
    (GS_REPLAY_MAX_TICKS * GS_MAX_CARS + 256)
#define GS_CARRIER_MAX_CHUNKS \
    ((GS_CARRIER_MAX_BYTES + GS_CHUNK_BYTES - 1) / GS_CHUNK_BYTES)

typedef struct gs_carrier {
    uint64_t hash;              // what is being received
    uint16_t chunks;            // how many pieces it comes in
    bool     have[GS_CARRIER_MAX_CHUNKS];
    uint16_t got;               // how many of them are here
    size_t   len;               // bytes written so far
    uint8_t  bytes[GS_CARRIER_MAX_BYTES];
    bool     complete;
} gs_carrier;

// Start expecting a track. Forgets anything half-received: a client that was
// sent a different track mid-transfer wants the new one, not a mixture.
void gs_carrier_expect(gs_carrier *c, uint64_t hash);

// How many chunks a track of this size takes.
uint16_t gs_carrier_chunks(size_t len);

// One chunk of `bytes` to send, as a datagram. Returns its length, or 0.
size_t gs_carrier_chunk(uint8_t *buf, size_t cap, uint64_t hash,
                        const uint8_t *bytes, size_t len, uint16_t chunk);

// Take a chunk that has arrived. False if it is not for the track being
// expected, which is how a stray chunk of somebody else's track is ignored
// rather than mixed in.
bool gs_carrier_take(gs_carrier *c, const uint8_t *buf, size_t len);

// Everything arrived, and the rebuilt track hashes to what was promised. This
// is the only way a transfer is declared finished: the pieces being counted is
// not the same as the track being right.
bool gs_carrier_done(const gs_carrier *c);

// Rebuild it. False if the bytes are not a track, or are not *the* track.
bool gs_carrier_track(const gs_carrier *c, gs_track *out);

// How far along, for something to show a person.
float gs_carrier_progress(const gs_carrier *c);

#endif // GS_CARRIER_H

// gs_proto.h - what clients and the server say to each other.
//
// **Encoding only. No sockets, no SDL, no allocation.** The same discipline the
// rollback session keeps, and for the same reason: a protocol that can be
// exercised without a network is a protocol whose edge cases can be tested at
// all. Something else carries the bytes - see src/platform/gs_wire.c for the
// client's socket and src/frontend/server/ for the server's.
//
// The server is a librarian and a referee. It hands out player slots, says who
// else is here, sends the track a race will be run on, and forwards datagrams
// for peers whose routers will not let them talk directly. **It does not
// simulate a race.** A race simulated on a server means every steering input
// waits a round trip, which is the one thing rollback exists to avoid - see the
// platform section of docs/FEATURES.md.
#ifndef GS_PROTO_H
#define GS_PROTO_H

#include "core/gs_common.h"

// Not the netcode's magic and not the peer handshake's, so a rollback session
// that ever saw one of these would reject it rather than read it as input.
#define GS_PROTO_MAGIC   0x56535347u   // "GSSV"
#define GS_PROTO_VERSION 1u

#define GS_PROTO_MAX_PLAYERS 4
#define GS_PROTO_NAME       16
#define GS_PROTO_ADDR       64

// Big enough for the largest message that is not a track chunk, which is what
// the chunk size below is sized against.
#define GS_PROTO_MTU   1200
#define GS_CHUNK_BYTES 1024

typedef enum gs_msg {
    GS_MSG_NONE = 0,

    // Client to server.
    GS_MSG_JOIN,       // "here I am, this is my name"
    GS_MSG_BYE,        // "I am leaving" - courtesy, not required
    GS_MSG_PING,       // keeps the connection alive and measures the trip
    GS_MSG_WANT_TRACK, // "send me the track for this race"
    GS_MSG_RELAY,      // "pass this to the others for me"
    GS_MSG_RESULT,     // "this is what I did" - a time, offered
    GS_MSG_WANT_BEST,  // "what is the record here?"

    // Server to client.
    GS_MSG_WELCOME,    // "you are player N of M, and here is everyone"
    GS_MSG_FULL,       // "there is no room" - with a reason to show a person
    GS_MSG_LOBBY,      // somebody arrived or left; here is the roster again
    GS_MSG_TRACK,      // one chunk of the track
    GS_MSG_START,      // "race, on the track with this hash"
    GS_MSG_PONG,
    GS_MSG_FORWARD,    // a relayed datagram from another player
    GS_MSG_BEST,       // the record on a track, and who holds it

    GS_MSG_COUNT
} gs_msg;

// One player, as the lobby sees them.
typedef struct gs_lobby_player {
    char     name[GS_PROTO_NAME];
    char     addr[GS_PROTO_ADDR];   // as the server sees them, for the mesh
    uint16_t port;
    uint8_t  slot;
    bool     present;
    bool     ready;
} gs_lobby_player;

typedef struct gs_lobby {
    uint8_t         count;
    uint8_t         capacity;
    gs_lobby_player player[GS_PROTO_MAX_PLAYERS];
} gs_lobby;

// --- writing ---------------------------------------------------------------
//
// Every one returns the number of bytes written, or 0 if the buffer is too
// small. Nothing here can fail in any other way.

size_t gs_proto_join(uint8_t *buf, size_t cap, const char *name);
size_t gs_proto_bye(uint8_t *buf, size_t cap);
size_t gs_proto_ping(uint8_t *buf, size_t cap, uint32_t stamp);
size_t gs_proto_pong(uint8_t *buf, size_t cap, uint32_t stamp);
size_t gs_proto_full(uint8_t *buf, size_t cap, const char *why);
size_t gs_proto_welcome(uint8_t *buf, size_t cap, uint8_t slot, const gs_lobby *l);
size_t gs_proto_lobby(uint8_t *buf, size_t cap, const gs_lobby *l);
size_t gs_proto_start(uint8_t *buf, size_t cap, uint64_t track_hash,
                      uint8_t players, uint16_t laps, uint8_t mode);
size_t gs_proto_want_track(uint8_t *buf, size_t cap, uint64_t track_hash);
size_t gs_proto_track_chunk(uint8_t *buf, size_t cap, uint64_t track_hash,
                            uint16_t chunk, uint16_t chunks,
                            const uint8_t *data, uint16_t len);

// --- times ----------------------------------------------------------------
//
// A result is offered rather than asserted. Today the server believes it; the
// item after this one has it re-race the inputs before it does, and the message
// does not change when that happens - only what the server does with it.

size_t gs_proto_result(uint8_t *buf, size_t cap, uint64_t track,
                       uint64_t conditions, uint16_t laps, uint8_t vehicle,
                       uint32_t lap_ticks, uint32_t race_ticks);
bool gs_proto_read_result(const uint8_t *buf, size_t len, uint64_t *track,
                          uint64_t *conditions, uint16_t *laps, uint8_t *vehicle,
                          uint32_t *lap_ticks, uint32_t *race_ticks);

size_t gs_proto_want_best(uint8_t *buf, size_t cap, uint64_t track,
                          uint64_t conditions, uint16_t laps);
bool gs_proto_read_want_best(const uint8_t *buf, size_t len, uint64_t *track,
                             uint64_t *conditions, uint16_t *laps);

size_t gs_proto_best(uint8_t *buf, size_t cap, uint64_t track,
                     uint64_t conditions, uint16_t laps, uint32_t lap_ticks,
                     const char *lap_who, uint32_t race_ticks,
                     const char *race_who);
bool gs_proto_read_best(const uint8_t *buf, size_t len, uint64_t *track,
                        uint64_t *conditions, uint16_t *laps,
                        uint32_t *lap_ticks, char *lap_who, size_t lap_cap,
                        uint32_t *race_ticks, char *race_who, size_t race_cap);

// A datagram to be passed on. `from` is filled in by the server on the way out.
size_t gs_proto_relay(uint8_t *buf, size_t cap, const uint8_t *data, size_t len);
size_t gs_proto_forward(uint8_t *buf, size_t cap, uint8_t from,
                        const uint8_t *data, size_t len);

// --- reading ---------------------------------------------------------------

// What kind of message this is, or GS_MSG_NONE if it is not one of ours. Every
// reader below checks this first, so a stray datagram from anywhere is ignored
// rather than misread.
gs_msg gs_proto_kind(const uint8_t *buf, size_t len);

bool gs_proto_read_join(const uint8_t *buf, size_t len, char *name, size_t cap);
bool gs_proto_read_stamp(const uint8_t *buf, size_t len, uint32_t *stamp);
bool gs_proto_read_full(const uint8_t *buf, size_t len, char *why, size_t cap);
bool gs_proto_read_welcome(const uint8_t *buf, size_t len, uint8_t *slot,
                           gs_lobby *l);
bool gs_proto_read_lobby(const uint8_t *buf, size_t len, gs_lobby *l);
bool gs_proto_read_start(const uint8_t *buf, size_t len, uint64_t *track_hash,
                         uint8_t *players, uint16_t *laps, uint8_t *mode);
bool gs_proto_read_want_track(const uint8_t *buf, size_t len, uint64_t *track_hash);
bool gs_proto_read_track_chunk(const uint8_t *buf, size_t len, uint64_t *track_hash,
                               uint16_t *chunk, uint16_t *chunks,
                               const uint8_t **data, uint16_t *data_len);

// The payload of a relay or a forward. Returns null if it is not one.
const uint8_t *gs_proto_payload(const uint8_t *buf, size_t len, uint8_t *from,
                                size_t *payload_len);

#endif // GS_PROTO_H

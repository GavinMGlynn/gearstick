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
#include "net/gs_noise.h"

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

    // **The two that are not inside the tunnel, because they are the tunnel.**
    // A handshake message cannot be sealed by a session that does not exist
    // yet, and a sealed datagram is the envelope everything else travels in.
    // Every other message below is what comes *out* of one of these, and is
    // refused if it arrives any other way.
    GS_MSG_HANDSHAKE,  // one Noise handshake message, necessarily in the clear
    GS_MSG_SEALED,     // a counter and a sealed datagram: everything else

    // Client to server.
    GS_MSG_JOIN,       // "here I am, this is my name"
    GS_MSG_BYE,        // "I am leaving" - courtesy, not required
    GS_MSG_PING,       // keeps the connection alive and measures the trip
    GS_MSG_WANT_TRACK, // "send me the track for this race"
    GS_MSG_RELAY,      // "pass this to the others for me"
    GS_MSG_RESULT,     // "this is what I did" - a time, offered
    GS_MSG_SESSION,    // "here is a token; spend it when you claim something"
    GS_MSG_WANT_BEST,  // "what is the record here?"
    GS_MSG_PROOF,      // one chunk of the replay behind a claimed time
    GS_MSG_PUBLISH,    // "keep this track, and let people have it"
    GS_MSG_WITHDRAW,   // "take mine down again"
    GS_MSG_WANT_LIST,  // "what is published?"
    GS_MSG_SHARE,      // "let this person have my track" - or stop letting them
    GS_MSG_LOGIN,      // "this name is mine, and here is why"
    GS_MSG_CLAIM,      // "put a password on this name, and make it mine"

    // Server to client.
    GS_MSG_WELCOME,    // "you are player N of M, and here is everyone"
    GS_MSG_FULL,       // "there is no room" - with a reason to show a person
    GS_MSG_LOBBY,      // somebody arrived or left; here is the roster again
    GS_MSG_TRACK,      // one chunk of the track
    GS_MSG_START,      // "race, on the track with this hash"
    GS_MSG_PONG,
    GS_MSG_FORWARD,    // a relayed datagram from another player
    GS_MSG_BEST,       // the record on a track, and who holds it
    GS_MSG_LISTING,    // one published track: its hash, name and author

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

    // **And which key that slot belongs to.** The mesh is peer to peer, so
    // nothing the server does protects it; what the server can do is say who
    // everybody is, having watched each of them prove it during their own
    // handshake. That is what makes a sealed mesh possible at all - without it
    // two clients meeting for the first time have nothing to check each other
    // against, and a key somebody hands you about themselves authenticates
    // nothing.
    uint8_t  key[GS_NOISE_KEY_BYTES];
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

// The replay behind a claimed time, in chunks like a track. **The claim and its
// proof travel separately**: the result says what was done, the proof says what
// was pressed, and the server decides whether the second produces the first.
size_t gs_proto_proof_chunk(uint8_t *buf, size_t cap, uint64_t track,
                            uint16_t chunk, uint16_t chunks,
                            const uint8_t *data, uint16_t len);
bool gs_proto_read_proof_chunk(const uint8_t *buf, size_t len, uint64_t *track,
                               uint16_t *chunk, uint16_t *chunks,
                               const uint8_t **data, uint16_t *data_len);

// **A one-shot token, and the claim that spends it.**
//
// The server hands one out when it places a client and again after each claim
// is resolved. A claim carries the token it was given; one that was never
// issued, was issued to somebody else, has been spent or has expired buys
// nothing. Records are keyed, so a resubmitted time was already harmless - but
// harmless by accident of the schema, and a thing that is safe by accident stops
// being safe when the schema changes.
size_t gs_proto_session(uint8_t *buf, size_t cap, uint64_t nonce);
bool   gs_proto_read_session(const uint8_t *buf, size_t len, uint64_t *nonce);

// --- the envelope -----------------------------------------------------------
//
// A handshake message and a sealed datagram, which are the only two things that
// ever appear on the wire between a client and a server once this is switched
// on. `gs_proto_read_sealed` and `gs_proto_read_handshake` hand back a pointer
// into the caller's buffer rather than copying, because what follows is handed
// straight to the tunnel.
size_t gs_proto_handshake(uint8_t *buf, size_t cap, const uint8_t *msg, size_t len);
bool   gs_proto_read_handshake(const uint8_t *buf, size_t len,
                               const uint8_t **msg, size_t *msg_len);

size_t gs_proto_sealed(uint8_t *buf, size_t cap, const uint8_t *body, size_t len);
bool   gs_proto_read_sealed(const uint8_t *buf, size_t len,
                            const uint8_t **body, size_t *body_len);

size_t gs_proto_result(uint8_t *buf, size_t cap, uint64_t track,
                       uint64_t conditions, uint16_t laps, uint8_t vehicle,
                       uint32_t lap_ticks, uint32_t race_ticks, uint64_t nonce);
bool gs_proto_read_result(const uint8_t *buf, size_t len, uint64_t *track,
                          uint64_t *conditions, uint16_t *laps, uint8_t *vehicle,
                          uint32_t *lap_ticks, uint32_t *race_ticks,
                          uint64_t *nonce);

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

// --- publishing -----------------------------------------------------------
//
// A published track is a track the server keeps and hands to anybody who asks.
// The track itself travels the way it always does - in chunks, checked against
// its own hash - so publishing is a *claim about a track the server already
// has*, and the name that goes with it.

size_t gs_proto_publish(uint8_t *buf, size_t cap, uint64_t track,
                        const char *name);
bool gs_proto_read_publish(const uint8_t *buf, size_t len, uint64_t *track,
                           char *name, size_t cap);

size_t gs_proto_withdraw(uint8_t *buf, size_t cap, uint64_t track);

// **Handing a track to a named few.** The person is named by their public key,
// which is how everybody is named once there is a tunnel - a client learns the
// others' keys from the lobby, and the server checks the asker owns the track
// before it writes anything down.
// **Proving a name is yours.** The password goes across as itself, which is
// only sane because this is inside the tunnel - before that it would have
// needed a challenge-response construction, and inventing one is exactly what
// this project refuses to do. A code of zero means "no second factor offered".
#define GS_PROTO_SECRET 64

size_t gs_proto_login(uint8_t *buf, size_t cap, const char *name,
                      const char *password, uint32_t code);
bool   gs_proto_read_login(const uint8_t *buf, size_t len, char *name,
                           size_t name_cap, char *password, size_t pw_cap,
                           uint32_t *code);

// **Putting a password on a name.** Allowed on a name that has none - which is
// claiming it - and on one you have already proved is yours, which is changing
// it. `secret` is a shared secret for a one-time code, or empty for none; the
// client generates it, because a second factor whose secret the server chose is
// one the server could use.
size_t gs_proto_claim(uint8_t *buf, size_t cap, const char *name,
                      const char *password, const uint8_t *secret,
                      size_t secret_len);
bool   gs_proto_read_claim(const uint8_t *buf, size_t len, char *name,
                           size_t name_cap, char *password, size_t pw_cap,
                           uint8_t *secret, size_t secret_cap,
                           size_t *secret_len);

size_t gs_proto_share(uint8_t *buf, size_t cap, uint64_t track,
                      const uint8_t *with, bool on);
bool   gs_proto_read_share(const uint8_t *buf, size_t len, uint64_t *track,
                           uint8_t *with, bool *on);
bool gs_proto_read_withdraw(const uint8_t *buf, size_t len, uint64_t *track);

size_t gs_proto_want_list(uint8_t *buf, size_t cap);

size_t gs_proto_listing(uint8_t *buf, size_t cap, uint16_t index, uint16_t total,
                        uint64_t track, const char *name, const char *author);
bool gs_proto_read_listing(const uint8_t *buf, size_t len, uint16_t *index,
                           uint16_t *total, uint64_t *track, char *name,
                           size_t name_cap, char *author, size_t author_cap);

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

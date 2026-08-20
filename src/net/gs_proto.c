#include "net/gs_proto.h"

#include <string.h>

// Every message begins the same way, so an unknown one can be identified and
// discarded without knowing anything else about it.
//
//   u32  magic
//   u8   version
//   u8   kind
//
// Little-endian throughout, like every other format in this project, because a
// server and a client are not guaranteed to be the same machine and "it worked
// on mine" is not a wire format.

#define GS_HEAD 6

static void gs_put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static uint16_t gs_get16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

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

static size_t gs_head(uint8_t *buf, size_t cap, gs_msg kind) {
    if (cap < GS_HEAD) return 0;
    gs_put32(buf, GS_PROTO_MAGIC);
    buf[4] = (uint8_t)GS_PROTO_VERSION;
    buf[5] = (uint8_t)kind;
    return GS_HEAD;
}

// A fixed-width string field. Fixed rather than length-prefixed because these
// are names and addresses with known ceilings, and a length that can lie is a
// length somebody has to validate.
static size_t gs_put_str(uint8_t *p, const char *s, size_t width) {
    size_t n = 0;
    while (n < width - 1 && s != nullptr && s[n] != '\0') {
        p[n] = (uint8_t)s[n];
        n++;
    }
    memset(p + n, 0, width - n);
    return width;
}

static void gs_get_str(const uint8_t *p, size_t width, char *out, size_t cap) {
    size_t n = 0;
    while (n < width && n + 1 < cap && p[n] != '\0') {
        out[n] = (char)p[n];
        n++;
    }
    out[n] = '\0';
}

gs_msg gs_proto_kind(const uint8_t *buf, size_t len) {
    if (buf == nullptr || len < GS_HEAD) return GS_MSG_NONE;
    if (gs_get32(buf) != GS_PROTO_MAGIC) return GS_MSG_NONE;
    if (buf[4] != GS_PROTO_VERSION) return GS_MSG_NONE;
    if (buf[5] == 0 || buf[5] >= (uint8_t)GS_MSG_COUNT) return GS_MSG_NONE;
    return (gs_msg)buf[5];
}

// Every reader starts here: the right kind, and long enough to hold what it
// claims. A message that fails either is not read at all.
static bool gs_expect(const uint8_t *buf, size_t len, gs_msg kind, size_t need) {
    return gs_proto_kind(buf, len) == kind && len >= need;
}

// --- the simple ones -------------------------------------------------------

size_t gs_proto_join(uint8_t *buf, size_t cap, const char *name) {
    if (cap < GS_HEAD + GS_PROTO_NAME) return 0;
    size_t n = gs_head(buf, cap, GS_MSG_JOIN);
    n += gs_put_str(buf + n, name, GS_PROTO_NAME);
    return n;
}

bool gs_proto_read_join(const uint8_t *buf, size_t len, char *name, size_t cap) {
    if (!gs_expect(buf, len, GS_MSG_JOIN, GS_HEAD + GS_PROTO_NAME)) return false;
    gs_get_str(buf + GS_HEAD, GS_PROTO_NAME, name, cap);
    return true;
}

size_t gs_proto_bye(uint8_t *buf, size_t cap) {
    return gs_head(buf, cap, GS_MSG_BYE);
}

static size_t gs_stamped(uint8_t *buf, size_t cap, gs_msg kind, uint32_t stamp) {
    if (cap < GS_HEAD + 4) return 0;
    size_t n = gs_head(buf, cap, kind);
    gs_put32(buf + n, stamp);
    return n + 4;
}

size_t gs_proto_ping(uint8_t *buf, size_t cap, uint32_t stamp) {
    return gs_stamped(buf, cap, GS_MSG_PING, stamp);
}

size_t gs_proto_pong(uint8_t *buf, size_t cap, uint32_t stamp) {
    return gs_stamped(buf, cap, GS_MSG_PONG, stamp);
}

bool gs_proto_read_stamp(const uint8_t *buf, size_t len, uint32_t *stamp) {
    gs_msg k = gs_proto_kind(buf, len);
    if ((k != GS_MSG_PING && k != GS_MSG_PONG) || len < GS_HEAD + 4) return false;
    *stamp = gs_get32(buf + GS_HEAD);
    return true;
}

// **A refusal carries a reason.** A client that is turned away has to be able
// to tell its user why, and "connection failed" is not a reason.
size_t gs_proto_full(uint8_t *buf, size_t cap, const char *why) {
    if (cap < GS_HEAD + 64) return 0;
    size_t n = gs_head(buf, cap, GS_MSG_FULL);
    n += gs_put_str(buf + n, why, 64);
    return n;
}

bool gs_proto_read_full(const uint8_t *buf, size_t len, char *why, size_t cap) {
    if (!gs_expect(buf, len, GS_MSG_FULL, GS_HEAD + 64)) return false;
    gs_get_str(buf + GS_HEAD, 64, why, cap);
    return true;
}

// --- the roster ------------------------------------------------------------

#define GS_PLAYER_BYTES \
    (GS_PROTO_NAME + GS_PROTO_ADDR + 2 + 1 + 1 + 1 + GS_NOISE_KEY_BYTES)
#define GS_LOBBY_BYTES  (2 + GS_PROTO_MAX_PLAYERS * GS_PLAYER_BYTES)

static size_t gs_put_lobby(uint8_t *p, const gs_lobby *l) {
    size_t n = 0;
    p[n++] = l->count;
    p[n++] = l->capacity;

    for (uint8_t i = 0; i < GS_PROTO_MAX_PLAYERS; i++) {
        const gs_lobby_player *pl = &l->player[i];
        n += gs_put_str(p + n, pl->name, GS_PROTO_NAME);
        n += gs_put_str(p + n, pl->addr, GS_PROTO_ADDR);
        gs_put16(p + n, pl->port); n += 2;
        p[n++] = pl->slot;
        p[n++] = pl->present ? 1u : 0u;
        p[n++] = pl->ready ? 1u : 0u;
        memcpy(p + n, pl->key, GS_NOISE_KEY_BYTES); n += GS_NOISE_KEY_BYTES;
    }
    return n;
}

static bool gs_get_lobby(const uint8_t *p, gs_lobby *l) {
    size_t n = 0;
    memset(l, 0, sizeof *l);
    l->count = p[n++];
    l->capacity = p[n++];
    if (l->count > GS_PROTO_MAX_PLAYERS || l->capacity > GS_PROTO_MAX_PLAYERS) {
        return false;
    }

    for (uint8_t i = 0; i < GS_PROTO_MAX_PLAYERS; i++) {
        gs_lobby_player *pl = &l->player[i];
        gs_get_str(p + n, GS_PROTO_NAME, pl->name, sizeof pl->name);
        n += GS_PROTO_NAME;
        gs_get_str(p + n, GS_PROTO_ADDR, pl->addr, sizeof pl->addr);
        n += GS_PROTO_ADDR;
        pl->port = gs_get16(p + n); n += 2;
        pl->slot = p[n++];
        pl->present = p[n++] != 0u;
        pl->ready = p[n++] != 0u;
        memcpy(pl->key, p + n, GS_NOISE_KEY_BYTES); n += GS_NOISE_KEY_BYTES;

        if (pl->slot >= GS_PROTO_MAX_PLAYERS) return false;
    }
    return true;
}

size_t gs_proto_welcome(uint8_t *buf, size_t cap, uint8_t slot, const gs_lobby *l) {
    if (cap < GS_HEAD + 1 + GS_LOBBY_BYTES) return 0;
    size_t n = gs_head(buf, cap, GS_MSG_WELCOME);
    buf[n++] = slot;
    n += gs_put_lobby(buf + n, l);
    return n;
}

bool gs_proto_read_welcome(const uint8_t *buf, size_t len, uint8_t *slot,
                           gs_lobby *l) {
    if (!gs_expect(buf, len, GS_MSG_WELCOME, GS_HEAD + 1 + GS_LOBBY_BYTES)) {
        return false;
    }
    *slot = buf[GS_HEAD];
    if (*slot >= GS_PROTO_MAX_PLAYERS) return false;
    return gs_get_lobby(buf + GS_HEAD + 1, l);
}

size_t gs_proto_lobby(uint8_t *buf, size_t cap, const gs_lobby *l) {
    if (cap < GS_HEAD + GS_LOBBY_BYTES) return 0;
    size_t n = gs_head(buf, cap, GS_MSG_LOBBY);
    n += gs_put_lobby(buf + n, l);
    return n;
}

bool gs_proto_read_lobby(const uint8_t *buf, size_t len, gs_lobby *l) {
    if (!gs_expect(buf, len, GS_MSG_LOBBY, GS_HEAD + GS_LOBBY_BYTES)) return false;
    return gs_get_lobby(buf + GS_HEAD, l);
}

// --- starting a race -------------------------------------------------------

size_t gs_proto_start(uint8_t *buf, size_t cap, uint64_t track_hash,
                      uint8_t players, uint16_t laps, uint8_t mode) {
    if (cap < GS_HEAD + 8 + 1 + 2 + 1) return 0;
    size_t n = gs_head(buf, cap, GS_MSG_START);
    gs_put64(buf + n, track_hash); n += 8;
    buf[n++] = players;
    gs_put16(buf + n, laps); n += 2;
    buf[n++] = mode;
    return n;
}

bool gs_proto_read_start(const uint8_t *buf, size_t len, uint64_t *track_hash,
                         uint8_t *players, uint16_t *laps, uint8_t *mode) {
    if (!gs_expect(buf, len, GS_MSG_START, GS_HEAD + 12)) return false;
    size_t n = GS_HEAD;
    *track_hash = gs_get64(buf + n); n += 8;
    *players = buf[n++];
    *laps = gs_get16(buf + n); n += 2;
    *mode = buf[n];
    return *players > 0 && *players <= GS_PROTO_MAX_PLAYERS;
}

// --- the track ------------------------------------------------------------
//
// In chunks, because a track is a few kilobytes and a datagram is not. The hash
// is in every chunk so a client reassembling one track cannot be confused by a
// stray chunk of another.

size_t gs_proto_want_track(uint8_t *buf, size_t cap, uint64_t track_hash) {
    if (cap < GS_HEAD + 8) return 0;
    size_t n = gs_head(buf, cap, GS_MSG_WANT_TRACK);
    gs_put64(buf + n, track_hash);
    return n + 8;
}

bool gs_proto_read_want_track(const uint8_t *buf, size_t len, uint64_t *track_hash) {
    if (!gs_expect(buf, len, GS_MSG_WANT_TRACK, GS_HEAD + 8)) return false;
    *track_hash = gs_get64(buf + GS_HEAD);
    return true;
}

size_t gs_proto_track_chunk(uint8_t *buf, size_t cap, uint64_t track_hash,
                            uint16_t chunk, uint16_t chunks,
                            const uint8_t *data, uint16_t len) {
    if (len > GS_CHUNK_BYTES) return 0;
    if (cap < GS_HEAD + 8 + 2 + 2 + 2 + (size_t)len) return 0;

    size_t n = gs_head(buf, cap, GS_MSG_TRACK);
    gs_put64(buf + n, track_hash); n += 8;
    gs_put16(buf + n, chunk); n += 2;
    gs_put16(buf + n, chunks); n += 2;
    gs_put16(buf + n, len); n += 2;
    memcpy(buf + n, data, len);
    return n + len;
}

bool gs_proto_read_track_chunk(const uint8_t *buf, size_t len, uint64_t *track_hash,
                               uint16_t *chunk, uint16_t *chunks,
                               const uint8_t **data, uint16_t *data_len) {
    if (!gs_expect(buf, len, GS_MSG_TRACK, GS_HEAD + 14)) return false;

    size_t n = GS_HEAD;
    *track_hash = gs_get64(buf + n); n += 8;
    *chunk = gs_get16(buf + n); n += 2;
    *chunks = gs_get16(buf + n); n += 2;
    *data_len = gs_get16(buf + n); n += 2;

    // The length it claims has to be the length that is there. Believing it
    // over the datagram is how a reassembler reads past the end of a packet
    // somebody else sent it.
    if (*data_len > GS_CHUNK_BYTES) return false;
    if (n + *data_len > len) return false;
    if (*chunks == 0 || *chunk >= *chunks) return false;

    *data = buf + n;
    return true;
}

// --- times ----------------------------------------------------------------
//
// A proof chunk is shaped exactly like a track chunk, and deliberately so: the
// same reassembly, the same "the key is in every piece", the same refusal to
// believe a length a datagram does not contain.

size_t gs_proto_proof_chunk(uint8_t *buf, size_t cap, uint64_t track,
                            uint16_t chunk, uint16_t chunks,
                            const uint8_t *data, uint16_t len) {
    if (len > GS_CHUNK_BYTES) return 0;
    if (cap < GS_HEAD + 8 + 2 + 2 + 2 + (size_t)len) return 0;

    size_t n = gs_head(buf, cap, GS_MSG_PROOF);
    gs_put64(buf + n, track); n += 8;
    gs_put16(buf + n, chunk); n += 2;
    gs_put16(buf + n, chunks); n += 2;
    gs_put16(buf + n, len); n += 2;
    memcpy(buf + n, data, len);
    return n + len;
}

bool gs_proto_read_proof_chunk(const uint8_t *buf, size_t len, uint64_t *track,
                               uint16_t *chunk, uint16_t *chunks,
                               const uint8_t **data, uint16_t *data_len) {
    if (!gs_expect(buf, len, GS_MSG_PROOF, GS_HEAD + 14)) return false;

    size_t n = GS_HEAD;
    *track = gs_get64(buf + n); n += 8;
    *chunk = gs_get16(buf + n); n += 2;
    *chunks = gs_get16(buf + n); n += 2;
    *data_len = gs_get16(buf + n); n += 2;

    if (*data_len > GS_CHUNK_BYTES) return false;
    if (n + *data_len > len) return false;
    if (*chunks == 0 || *chunk >= *chunks) return false;

    *data = buf + n;
    return true;
}

// --- the envelope -----------------------------------------------------------

static size_t gs_proto_wrap(uint8_t *buf, size_t cap, gs_msg kind,
                            const uint8_t *body, size_t len) {
    if (len == 0 || cap < GS_HEAD + len) return 0;
    size_t n = gs_head(buf, cap, kind);
    memcpy(buf + n, body, len);
    return n + len;
}

static bool gs_proto_unwrap(const uint8_t *buf, size_t len, gs_msg kind,
                            const uint8_t **body, size_t *body_len) {
    if (!gs_expect(buf, len, kind, GS_HEAD + 1)) return false;
    *body = buf + GS_HEAD;
    *body_len = len - GS_HEAD;
    return true;
}

size_t gs_proto_handshake(uint8_t *buf, size_t cap, const uint8_t *msg, size_t len) {
    return gs_proto_wrap(buf, cap, GS_MSG_HANDSHAKE, msg, len);
}

bool gs_proto_read_handshake(const uint8_t *buf, size_t len,
                             const uint8_t **msg, size_t *msg_len) {
    return gs_proto_unwrap(buf, len, GS_MSG_HANDSHAKE, msg, msg_len);
}

size_t gs_proto_sealed(uint8_t *buf, size_t cap, const uint8_t *body, size_t len) {
    return gs_proto_wrap(buf, cap, GS_MSG_SEALED, body, len);
}

bool gs_proto_read_sealed(const uint8_t *buf, size_t len,
                          const uint8_t **body, size_t *body_len) {
    return gs_proto_unwrap(buf, len, GS_MSG_SEALED, body, body_len);
}

size_t gs_proto_session(uint8_t *buf, size_t cap, uint64_t nonce) {
    if (cap < GS_HEAD + 8) return 0;
    size_t n = gs_head(buf, cap, GS_MSG_SESSION);
    gs_put64(buf + n, nonce); n += 8;
    return n;
}

bool gs_proto_read_session(const uint8_t *buf, size_t len, uint64_t *nonce) {
    if (!gs_expect(buf, len, GS_MSG_SESSION, GS_HEAD + 8)) return false;
    *nonce = gs_get64(buf + GS_HEAD);
    return true;
}

size_t gs_proto_result(uint8_t *buf, size_t cap, uint64_t track,
                       uint64_t conditions, uint16_t laps, uint8_t vehicle,
                       uint32_t lap_ticks, uint32_t race_ticks, uint64_t nonce) {
    if (cap < GS_HEAD + 8 + 8 + 2 + 1 + 4 + 4 + 8) return 0;
    size_t n = gs_head(buf, cap, GS_MSG_RESULT);
    gs_put64(buf + n, track);      n += 8;
    gs_put64(buf + n, conditions); n += 8;
    gs_put16(buf + n, laps);       n += 2;
    buf[n++] = vehicle;
    gs_put32(buf + n, lap_ticks);  n += 4;
    gs_put32(buf + n, race_ticks); n += 4;
    gs_put64(buf + n, nonce);      n += 8;
    return n;
}

bool gs_proto_read_result(const uint8_t *buf, size_t len, uint64_t *track,
                          uint64_t *conditions, uint16_t *laps, uint8_t *vehicle,
                          uint32_t *lap_ticks, uint32_t *race_ticks,
                          uint64_t *nonce) {
    if (!gs_expect(buf, len, GS_MSG_RESULT, GS_HEAD + 35)) return false;
    size_t n = GS_HEAD;
    *track = gs_get64(buf + n);      n += 8;
    *conditions = gs_get64(buf + n); n += 8;
    *laps = gs_get16(buf + n);       n += 2;
    *vehicle = buf[n++];
    *lap_ticks = gs_get32(buf + n);  n += 4;
    *race_ticks = gs_get32(buf + n); n += 4;
    *nonce = gs_get64(buf + n);
    return true;
}

size_t gs_proto_want_best(uint8_t *buf, size_t cap, uint64_t track,
                          uint64_t conditions, uint16_t laps) {
    if (cap < GS_HEAD + 18) return 0;
    size_t n = gs_head(buf, cap, GS_MSG_WANT_BEST);
    gs_put64(buf + n, track);      n += 8;
    gs_put64(buf + n, conditions); n += 8;
    gs_put16(buf + n, laps);       n += 2;
    return n;
}

bool gs_proto_read_want_best(const uint8_t *buf, size_t len, uint64_t *track,
                             uint64_t *conditions, uint16_t *laps) {
    if (!gs_expect(buf, len, GS_MSG_WANT_BEST, GS_HEAD + 18)) return false;
    size_t n = GS_HEAD;
    *track = gs_get64(buf + n);      n += 8;
    *conditions = gs_get64(buf + n); n += 8;
    *laps = gs_get16(buf + n);
    return true;
}

size_t gs_proto_best(uint8_t *buf, size_t cap, uint64_t track,
                     uint64_t conditions, uint16_t laps, uint32_t lap_ticks,
                     const char *lap_who, uint32_t race_ticks,
                     const char *race_who) {
    if (cap < GS_HEAD + 18 + 4 + GS_PROTO_NAME + 4 + GS_PROTO_NAME) return 0;
    size_t n = gs_head(buf, cap, GS_MSG_BEST);
    gs_put64(buf + n, track);      n += 8;
    gs_put64(buf + n, conditions); n += 8;
    gs_put16(buf + n, laps);       n += 2;
    gs_put32(buf + n, lap_ticks);  n += 4;
    n += gs_put_str(buf + n, lap_who, GS_PROTO_NAME);
    gs_put32(buf + n, race_ticks); n += 4;
    n += gs_put_str(buf + n, race_who, GS_PROTO_NAME);
    return n;
}

bool gs_proto_read_best(const uint8_t *buf, size_t len, uint64_t *track,
                        uint64_t *conditions, uint16_t *laps,
                        uint32_t *lap_ticks, char *lap_who, size_t lap_cap,
                        uint32_t *race_ticks, char *race_who, size_t race_cap) {
    size_t need = GS_HEAD + 18 + 4 + GS_PROTO_NAME + 4 + GS_PROTO_NAME;
    if (!gs_expect(buf, len, GS_MSG_BEST, need)) return false;

    size_t n = GS_HEAD;
    *track = gs_get64(buf + n);      n += 8;
    *conditions = gs_get64(buf + n); n += 8;
    *laps = gs_get16(buf + n);       n += 2;
    *lap_ticks = gs_get32(buf + n);  n += 4;
    gs_get_str(buf + n, GS_PROTO_NAME, lap_who, lap_cap); n += GS_PROTO_NAME;
    *race_ticks = gs_get32(buf + n); n += 4;
    gs_get_str(buf + n, GS_PROTO_NAME, race_who, race_cap);
    return true;
}

// --- publishing -----------------------------------------------------------

size_t gs_proto_publish(uint8_t *buf, size_t cap, uint64_t track,
                        const char *name) {
    if (cap < GS_HEAD + 8 + 48) return 0;
    size_t n = gs_head(buf, cap, GS_MSG_PUBLISH);
    gs_put64(buf + n, track); n += 8;
    n += gs_put_str(buf + n, name, 48);
    return n;
}

bool gs_proto_read_publish(const uint8_t *buf, size_t len, uint64_t *track,
                           char *name, size_t cap) {
    if (!gs_expect(buf, len, GS_MSG_PUBLISH, GS_HEAD + 56)) return false;
    *track = gs_get64(buf + GS_HEAD);
    gs_get_str(buf + GS_HEAD + 8, 48, name, cap);
    return true;
}

size_t gs_proto_withdraw(uint8_t *buf, size_t cap, uint64_t track) {
    if (cap < GS_HEAD + 8) return 0;
    size_t n = gs_head(buf, cap, GS_MSG_WITHDRAW);
    gs_put64(buf + n, track);
    return n + 8;
}

bool gs_proto_read_withdraw(const uint8_t *buf, size_t len, uint64_t *track) {
    if (!gs_expect(buf, len, GS_MSG_WITHDRAW, GS_HEAD + 8)) return false;
    *track = gs_get64(buf + GS_HEAD);
    return true;
}

size_t gs_proto_login(uint8_t *buf, size_t cap, const char *name,
                      const char *password, uint32_t code) {
    if (cap < GS_HEAD + GS_PROTO_NAME + GS_PROTO_SECRET + 4) return 0;
    size_t n = gs_head(buf, cap, GS_MSG_LOGIN);
    n += gs_put_str(buf + n, name, GS_PROTO_NAME);
    n += gs_put_str(buf + n, password, GS_PROTO_SECRET);
    gs_put32(buf + n, code); n += 4;
    return n;
}

bool gs_proto_read_login(const uint8_t *buf, size_t len, char *name,
                         size_t name_cap, char *password, size_t pw_cap,
                         uint32_t *code) {
    if (!gs_expect(buf, len, GS_MSG_LOGIN,
                   GS_HEAD + GS_PROTO_NAME + GS_PROTO_SECRET + 4)) {
        return false;
    }
    size_t n = GS_HEAD;
    gs_get_str(buf + n, GS_PROTO_NAME, name, name_cap);       n += GS_PROTO_NAME;
    gs_get_str(buf + n, GS_PROTO_SECRET, password, pw_cap);   n += GS_PROTO_SECRET;
    *code = gs_get32(buf + n);
    return true;
}

size_t gs_proto_share(uint8_t *buf, size_t cap, uint64_t track,
                      const uint8_t *with, bool on) {
    if (with == nullptr || cap < GS_HEAD + 8 + GS_NOISE_KEY_BYTES + 1) return 0;
    size_t n = gs_head(buf, cap, GS_MSG_SHARE);
    gs_put64(buf + n, track); n += 8;
    memcpy(buf + n, with, GS_NOISE_KEY_BYTES); n += GS_NOISE_KEY_BYTES;
    buf[n++] = on ? 1u : 0u;
    return n;
}

bool gs_proto_read_share(const uint8_t *buf, size_t len, uint64_t *track,
                         uint8_t *with, bool *on) {
    if (!gs_expect(buf, len, GS_MSG_SHARE,
                   GS_HEAD + 8 + GS_NOISE_KEY_BYTES + 1)) {
        return false;
    }
    size_t n = GS_HEAD;
    *track = gs_get64(buf + n); n += 8;
    memcpy(with, buf + n, GS_NOISE_KEY_BYTES); n += GS_NOISE_KEY_BYTES;
    *on = buf[n] != 0u;
    return true;
}

size_t gs_proto_want_list(uint8_t *buf, size_t cap) {
    return gs_head(buf, cap, GS_MSG_WANT_LIST);
}

// One track per datagram. A listing that packed as many as would fit would
// need a size nobody can exceed, and the library is meant to grow.
size_t gs_proto_listing(uint8_t *buf, size_t cap, uint16_t index, uint16_t total,
                        uint64_t track, const char *name, const char *author) {
    if (cap < GS_HEAD + 2 + 2 + 8 + 48 + GS_PROTO_NAME) return 0;
    size_t n = gs_head(buf, cap, GS_MSG_LISTING);
    gs_put16(buf + n, index); n += 2;
    gs_put16(buf + n, total); n += 2;
    gs_put64(buf + n, track); n += 8;
    n += gs_put_str(buf + n, name, 48);
    n += gs_put_str(buf + n, author, GS_PROTO_NAME);
    return n;
}

bool gs_proto_read_listing(const uint8_t *buf, size_t len, uint16_t *index,
                           uint16_t *total, uint64_t *track, char *name,
                           size_t name_cap, char *author, size_t author_cap) {
    size_t need = GS_HEAD + 2 + 2 + 8 + 48 + GS_PROTO_NAME;
    if (!gs_expect(buf, len, GS_MSG_LISTING, need)) return false;

    size_t n = GS_HEAD;
    *index = gs_get16(buf + n); n += 2;
    *total = gs_get16(buf + n); n += 2;
    *track = gs_get64(buf + n); n += 8;
    gs_get_str(buf + n, 48, name, name_cap); n += 48;
    gs_get_str(buf + n, GS_PROTO_NAME, author, author_cap);
    return true;
}

// --- relaying --------------------------------------------------------------

size_t gs_proto_relay(uint8_t *buf, size_t cap, const uint8_t *data, size_t len) {
    if (cap < GS_HEAD + 2 + len) return 0;
    size_t n = gs_head(buf, cap, GS_MSG_RELAY);
    gs_put16(buf + n, (uint16_t)len); n += 2;
    memcpy(buf + n, data, len);
    return n + len;
}

size_t gs_proto_forward(uint8_t *buf, size_t cap, uint8_t from,
                        const uint8_t *data, size_t len) {
    if (cap < GS_HEAD + 1 + 2 + len) return 0;
    size_t n = gs_head(buf, cap, GS_MSG_FORWARD);
    buf[n++] = from;
    gs_put16(buf + n, (uint16_t)len); n += 2;
    memcpy(buf + n, data, len);
    return n + len;
}

const uint8_t *gs_proto_payload(const uint8_t *buf, size_t len, uint8_t *from,
                                size_t *payload_len) {
    gs_msg k = gs_proto_kind(buf, len);
    size_t n = GS_HEAD;

    if (k == GS_MSG_FORWARD) {
        if (len < GS_HEAD + 3) return nullptr;
        if (from != nullptr) *from = buf[n];
        n += 1;
    } else if (k == GS_MSG_RELAY) {
        if (len < GS_HEAD + 2) return nullptr;
        if (from != nullptr) *from = 0xffu;   // the server fills this in
    } else {
        return nullptr;
    }

    uint16_t claimed = gs_get16(buf + n);
    n += 2;
    if (n + claimed > len) return nullptr;

    *payload_len = claimed;
    return buf + n;
}

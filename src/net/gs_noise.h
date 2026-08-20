// gs_noise.h - Noise_IK_25519_ChaChaPoly_BLAKE2s, so that nothing on the wire
// is in the clear.
//
// **Nothing here is invented.** The pattern is IK from the Noise Protocol
// Framework, revision 34, spelled exactly as the specification spells it. The
// primitives are somebody else's: X25519 and ChaCha20-Poly1305 from libsodium,
// which is audited and widely deployed. BLAKE2s is RFC 7693 and ours only
// because libsodium ships BLAKE2b and this suite names BLAKE2s.
//
// **IK rather than NK**, because the client already knows the server's static
// public key: one round trip then gets server authentication, client
// authentication, and a client identity that a passive observer cannot read.
// **One suite and no negotiation**, because a protocol that cannot negotiate
// cannot be talked down to something weaker.
//
// The evidence that this is right is not that it looks right. It is that the
// framework's published test vectors pass, and that a handshake completes
// against an implementation written by somebody else - see
// `tests/test_noise.c` and `tools/noise_interop.py`. A protocol whose only
// support is its author's confidence is the thing that fails review.
//
// This file has no sockets in it. It turns bytes into bytes, so it can be
// tested without a network, fuzzed, and driven from a script.
#ifndef GS_NOISE_H
#define GS_NOISE_H

// For `bool` and for the `nullptr` shim on a toolchain whose C23 is partial.
// Rolling these includes by hand is what broke the Windows build twice.
#include "core/gs_common.h"

#define GS_NOISE_KEY_BYTES  32
#define GS_NOISE_TAG_BYTES  16
#define GS_NOISE_HASH_BYTES 32

// The name goes into the handshake hash before anything else does, so two
// endpoints that disagree about it cannot complete a handshake. That is the
// whole of "no negotiation": there is one name, and it is this one.
#define GS_NOISE_PROTOCOL "Noise_IK_25519_ChaChaPoly_BLAKE2s"

// **How many datagrams a session may send before it must be replaced.** The
// nonce is a counter and a repeated nonce with the same key destroys the
// confidentiality of both messages, so the counter is not allowed to reach a
// point where anybody has to think about it. Far below 2^64; a race at 120 Hz
// sends about half a million datagrams an hour.
#define GS_NOISE_MESSAGE_LIMIT (1ull << 40)

typedef struct gs_noise_keypair {
    uint8_t pub[GS_NOISE_KEY_BYTES];
    uint8_t sec[GS_NOISE_KEY_BYTES];
} gs_noise_keypair;

// One direction's cipher: a key and the counter that must never repeat under it.
typedef struct gs_noise_cipher {
    uint8_t  k[GS_NOISE_KEY_BYTES];
    uint64_t n;
    bool     has_key;
} gs_noise_cipher;

typedef struct gs_noise_handshake {
    uint8_t ck[GS_NOISE_HASH_BYTES];   // chaining key
    uint8_t h[GS_NOISE_HASH_BYTES];    // handshake hash
    gs_noise_cipher cs;

    gs_noise_keypair s;                // ours, static
    gs_noise_keypair e;                // ours, ephemeral
    uint8_t rs[GS_NOISE_KEY_BYTES];    // theirs, static
    uint8_t re[GS_NOISE_KEY_BYTES];    // theirs, ephemeral

    bool has_e, has_rs, has_re;
    bool initiator;
    int  step;                         // how many messages have been processed
    bool failed;                       // and it does not recover
} gs_noise_handshake;

// **A datagram channel, so the counter travels with the message.**
//
// Noise's transport phase assumes messages arrive in order, which is true of a
// stream and false of everything this project sends. So each datagram carries
// its own counter and the receiver keeps a window: anything older than the
// window, and anything already seen inside it, is refused. Out of order is not
// the same thing as replayed, and a naive "must be greater than the last one"
// counter cannot tell them apart - it throws away the reordering a lossy link
// produces constantly and calls it an attack.
#define GS_NOISE_WINDOW 64

typedef struct gs_noise_session {
    gs_noise_cipher send;
    gs_noise_cipher recv;

    uint64_t newest;                   // highest counter accepted so far
    uint64_t seen;                     // bitmap of the 64 counters below it
    bool     started;                  // has anything been accepted at all

    bool     established;
    uint8_t  handshake_hash[GS_NOISE_HASH_BYTES];
} gs_noise_session;

// --- keys -------------------------------------------------------------------

// A fresh static key. From libsodium's generator, which is the operating
// system's, because this is the one place in the project where a predictable
// number is a broken defence rather than a shaped one.
void gs_noise_keygen(gs_noise_keypair *k);

// The public key that goes with a secret, for a key read from a file.
void gs_noise_key_from_secret(gs_noise_keypair *k, const uint8_t *secret);

// --- the handshake ----------------------------------------------------------
//
// IK, written out, because reading it here is easier than reading it twice in
// the implementation:
//
//     <- s                         (the initiator already knows this)
//     ...
//     -> e, es, s, ss              message one
//     <- e, ee, se                 message two
//
// The prologue is authenticated but not sent: both ends have to already agree
// on it, and anything that differs makes the handshake fail. It is where a
// version goes.
void gs_noise_init_initiator(gs_noise_handshake *hs, const gs_noise_keypair *s,
                             const uint8_t *rs,
                             const uint8_t *prologue, size_t prologue_len);
void gs_noise_init_responder(gs_noise_handshake *hs, const gs_noise_keypair *s,
                             const uint8_t *prologue, size_t prologue_len);

// **For test vectors, and for nothing else.** The published vectors fix the
// ephemeral keys so that a handshake is reproducible; a real one must not, and
// a real one does not - `gs_noise_write_message` generates its own unless this
// has been called. It is in the header rather than hidden because a test that
// has to reach inside the implementation is a test that stops compiling for the
// wrong reasons.
void gs_noise_set_ephemeral(gs_noise_handshake *hs, const uint8_t *secret);

// Write the next handshake message. Returns its length, or 0.
size_t gs_noise_write_message(gs_noise_handshake *hs, const uint8_t *payload,
                              size_t payload_len, uint8_t *out, size_t cap);

// Read the next one. False on anything at all wrong, and the handshake is then
// dead rather than retryable - a handshake that can be retried after a failure
// is a handshake somebody can grind against.
bool gs_noise_read_message(gs_noise_handshake *hs, const uint8_t *in, size_t len,
                           uint8_t *payload, size_t cap, size_t *payload_len);

// Both messages have been processed.
bool gs_noise_done(const gs_noise_handshake *hs);

// Turn a finished handshake into a session and forget the handshake's keys.
bool gs_noise_split(gs_noise_handshake *hs, gs_noise_session *out);

// Who the far end turned out to be. Meaningful only after the handshake has
// finished; for a responder this is the identity the initiator proved, and for
// an initiator it is the key it already knew and has now confirmed is live.
const uint8_t *gs_noise_remote_static(const gs_noise_handshake *hs);

// --- transport --------------------------------------------------------------

// The bytes a sealed datagram costs on top of what it carries: eight for the
// counter in front of it, sixteen for the tag. What follows the counter is
// exactly what the framework calls a transport message, unchanged, which is
// what lets the published vectors and an independent implementation check this
// half too rather than only the handshake.
#define GS_NOISE_OVERHEAD (8 + GS_NOISE_TAG_BYTES)

// Seal one datagram. Returns its length, or 0 - including when the session has
// sent as many as it is allowed to.
size_t gs_noise_seal(gs_noise_session *s, const uint8_t *plain, size_t len,
                     uint8_t *out, size_t cap);

// Open one. False if it was not sealed by the far end of this session, if a bit
// of it has changed, or if it has been seen before or is too old to tell.
bool gs_noise_open(gs_noise_session *s, const uint8_t *in, size_t len,
                   uint8_t *plain, size_t cap, size_t *plain_len);

// Wipe a session's keys. Called on the way out; `sodium_memzero` rather than a
// memset the compiler is entitled to delete.
void gs_noise_close(gs_noise_session *s);

#endif // GS_NOISE_H

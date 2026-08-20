#include "net/gs_noise.h"

#include "core/gs_blake2s.h"

#include <sodium.h>
#include <string.h>

// --- HMAC and HKDF, exactly as the framework specifies them ------------------
//
// Noise defines HKDF over HMAC-HASH, and HMAC over the hash's *block* size,
// which for BLAKE2s is 64 bytes and not the 32 of its digest. Getting that
// wrong produces a key schedule that is self-consistent and matches nobody -
// which is the argument for checking against the published vectors rather than
// against a second copy of this file.

#define GS_HMAC_BLOCK 64

static void gs_hmac(const uint8_t *key, size_t key_len,
                    const uint8_t *data, size_t data_len,
                    uint8_t out[GS_NOISE_HASH_BYTES]) {
    uint8_t block[GS_HMAC_BLOCK];
    memset(block, 0, sizeof block);

    if (key_len > GS_HMAC_BLOCK) {
        gs_blake2s_hash(block, GS_NOISE_HASH_BYTES, key, key_len);
    } else {
        memcpy(block, key, key_len);
    }

    uint8_t pad[GS_HMAC_BLOCK];
    uint8_t inner[GS_NOISE_HASH_BYTES];

    for (size_t i = 0; i < GS_HMAC_BLOCK; i++) pad[i] = block[i] ^ 0x36u;
    gs_blake2s s;
    gs_blake2s_init(&s, GS_NOISE_HASH_BYTES);
    gs_blake2s_update(&s, pad, sizeof pad);
    gs_blake2s_update(&s, data, data_len);
    gs_blake2s_final(&s, inner);

    for (size_t i = 0; i < GS_HMAC_BLOCK; i++) pad[i] = block[i] ^ 0x5cu;
    gs_blake2s_init(&s, GS_NOISE_HASH_BYTES);
    gs_blake2s_update(&s, pad, sizeof pad);
    gs_blake2s_update(&s, inner, sizeof inner);
    gs_blake2s_final(&s, out);

    sodium_memzero(block, sizeof block);
    sodium_memzero(pad, sizeof pad);
    sodium_memzero(inner, sizeof inner);
}

// HKDF(chaining_key, input) -> two or three outputs. Section 4.3.
static void gs_hkdf(const uint8_t ck[GS_NOISE_HASH_BYTES],
                    const uint8_t *ikm, size_t ikm_len,
                    uint8_t *out1, uint8_t *out2, uint8_t *out3) {
    uint8_t temp[GS_NOISE_HASH_BYTES];
    gs_hmac(ck, GS_NOISE_HASH_BYTES, ikm, ikm_len, temp);

    uint8_t one = 0x01;
    gs_hmac(temp, sizeof temp, &one, 1, out1);

    uint8_t feed[GS_NOISE_HASH_BYTES + 1];
    memcpy(feed, out1, GS_NOISE_HASH_BYTES);
    feed[GS_NOISE_HASH_BYTES] = 0x02;
    gs_hmac(temp, sizeof temp, feed, sizeof feed, out2);

    if (out3 != nullptr) {
        memcpy(feed, out2, GS_NOISE_HASH_BYTES);
        feed[GS_NOISE_HASH_BYTES] = 0x03;
        gs_hmac(temp, sizeof temp, feed, sizeof feed, out3);
    }

    sodium_memzero(temp, sizeof temp);
    sodium_memzero(feed, sizeof feed);
}

// --- the cipher state -------------------------------------------------------
//
// The nonce on the wire for this suite is four zero bytes followed by the
// counter, little-endian - which is exactly the layout libsodium's IETF variant
// takes, so there is no repacking and no chance of packing it differently here
// than the far end does.
static void gs_nonce(uint64_t n, uint8_t out[12]) {
    memset(out, 0, 4);
    for (int i = 0; i < 8; i++) out[4 + i] = (uint8_t)((n >> (8 * i)) & 0xffu);
}

static void gs_cipher_init(gs_noise_cipher *c, const uint8_t *k) {
    if (k == nullptr) {
        memset(c, 0, sizeof *c);
        return;
    }
    memcpy(c->k, k, GS_NOISE_KEY_BYTES);
    c->n = 0;
    c->has_key = true;
}

// EncryptWithAd. With no key this is the identity, which is what the framework
// says and is why the first handshake message can carry a public key in the
// clear without a special case anywhere.
static size_t gs_encrypt_ad(gs_noise_cipher *c, const uint8_t *ad, size_t ad_len,
                            const uint8_t *plain, size_t len,
                            uint8_t *out, size_t cap) {
    if (!c->has_key) {
        if (cap < len) return 0;
        memmove(out, plain, len);
        return len;
    }
    if (cap < len + GS_NOISE_TAG_BYTES) return 0;

    uint8_t nonce[12];
    gs_nonce(c->n, nonce);

    unsigned long long out_len = 0;
    if (crypto_aead_chacha20poly1305_ietf_encrypt(out, &out_len, plain,
                                                  (unsigned long long)len,
                                                  ad, (unsigned long long)ad_len,
                                                  nullptr, nonce, c->k) != 0) {
        return 0;
    }
    c->n++;
    return (size_t)out_len;
}

static bool gs_decrypt_ad(gs_noise_cipher *c, const uint8_t *ad, size_t ad_len,
                          const uint8_t *cipher, size_t len,
                          uint8_t *out, size_t cap, size_t *out_len) {
    if (!c->has_key) {
        if (cap < len) return false;
        memmove(out, cipher, len);
        *out_len = len;
        return true;
    }
    if (len < GS_NOISE_TAG_BYTES) return false;
    if (cap < len - GS_NOISE_TAG_BYTES) return false;

    uint8_t nonce[12];
    gs_nonce(c->n, nonce);

    unsigned long long got = 0;
    if (crypto_aead_chacha20poly1305_ietf_decrypt(out, &got, nullptr, cipher,
                                                  (unsigned long long)len,
                                                  ad, (unsigned long long)ad_len,
                                                  nonce, c->k) != 0) {
        return false;
    }
    c->n++;
    *out_len = (size_t)got;
    return true;
}

// --- the symmetric state ----------------------------------------------------

static void gs_mix_hash(gs_noise_handshake *hs, const uint8_t *data, size_t len) {
    gs_blake2s s;
    gs_blake2s_init(&s, GS_NOISE_HASH_BYTES);
    gs_blake2s_update(&s, hs->h, sizeof hs->h);
    gs_blake2s_update(&s, data, len);
    gs_blake2s_final(&s, hs->h);
}

static void gs_mix_key(gs_noise_handshake *hs, const uint8_t *ikm, size_t len) {
    uint8_t ck[GS_NOISE_HASH_BYTES], temp_k[GS_NOISE_HASH_BYTES];
    gs_hkdf(hs->ck, ikm, len, ck, temp_k, nullptr);
    memcpy(hs->ck, ck, sizeof ck);
    gs_cipher_init(&hs->cs, temp_k);
    sodium_memzero(ck, sizeof ck);
    sodium_memzero(temp_k, sizeof temp_k);
}

static size_t gs_encrypt_and_hash(gs_noise_handshake *hs, const uint8_t *plain,
                                  size_t len, uint8_t *out, size_t cap) {
    size_t n = gs_encrypt_ad(&hs->cs, hs->h, sizeof hs->h, plain, len, out, cap);
    if (n == 0 && len > 0) return 0;
    gs_mix_hash(hs, out, n);
    return n;
}

static bool gs_decrypt_and_hash(gs_noise_handshake *hs, const uint8_t *cipher,
                                size_t len, uint8_t *out, size_t cap,
                                size_t *out_len) {
    // **The hash is mixed with the ciphertext, and only if it was genuine.**
    // Mixing first would be simpler and would let a forged message move this
    // end's hash somewhere the other end's never goes, so a later honest
    // message would fail and the failure would look like a bug in the peer.
    uint8_t before[GS_NOISE_HASH_BYTES];
    memcpy(before, hs->h, sizeof before);

    if (!gs_decrypt_ad(&hs->cs, hs->h, sizeof hs->h, cipher, len, out, cap, out_len)) {
        memcpy(hs->h, before, sizeof before);
        return false;
    }
    gs_mix_hash(hs, cipher, len);
    return true;
}

// --- Diffie-Hellman ---------------------------------------------------------
//
// A zero shared secret means the peer sent a low-order point, and libsodium
// says so rather than carrying on. Refusing it is the framework's advice and
// costs nothing.
static bool gs_dh(const uint8_t sec[32], const uint8_t pub[32], uint8_t out[32]) {
    return crypto_scalarmult(out, sec, pub) == 0;
}

// --- setting up -------------------------------------------------------------

void gs_noise_keygen(gs_noise_keypair *k) {
    crypto_box_keypair(k->pub, k->sec);
}

void gs_noise_key_from_secret(gs_noise_keypair *k, const uint8_t *secret) {
    memcpy(k->sec, secret, GS_NOISE_KEY_BYTES);
    crypto_scalarmult_base(k->pub, k->sec);
}

static void gs_init_symmetric(gs_noise_handshake *hs,
                              const uint8_t *prologue, size_t prologue_len) {
    const char *name = GS_NOISE_PROTOCOL;
    size_t name_len = strlen(name);

    // Section 5.2: a name of at most HASHLEN bytes is used directly, padded
    // with zeros; a longer one is hashed. This one is 32 bytes exactly, so it
    // takes the first branch - and the branch that is not taken is written out
    // anyway, because the day somebody renames the suite is the day it matters.
    memset(hs->h, 0, sizeof hs->h);
    if (name_len <= GS_NOISE_HASH_BYTES) {
        memcpy(hs->h, name, name_len);
    } else {
        gs_blake2s_hash(hs->h, GS_NOISE_HASH_BYTES, name, name_len);
    }
    memcpy(hs->ck, hs->h, sizeof hs->ck);
    gs_cipher_init(&hs->cs, nullptr);

    gs_mix_hash(hs, prologue, prologue_len);
}

void gs_noise_init_initiator(gs_noise_handshake *hs, const gs_noise_keypair *s,
                             const uint8_t *rs,
                             const uint8_t *prologue, size_t prologue_len) {
    memset(hs, 0, sizeof *hs);
    hs->initiator = true;
    hs->s = *s;
    memcpy(hs->rs, rs, GS_NOISE_KEY_BYTES);
    hs->has_rs = true;

    gs_init_symmetric(hs, prologue, prologue_len);

    // The pre-message: IK's `<- s`. The initiator already has the responder's
    // static key, so it goes into the hash without being sent.
    gs_mix_hash(hs, hs->rs, GS_NOISE_KEY_BYTES);
}

void gs_noise_init_responder(gs_noise_handshake *hs, const gs_noise_keypair *s,
                             const uint8_t *prologue, size_t prologue_len) {
    memset(hs, 0, sizeof *hs);
    hs->initiator = false;
    hs->s = *s;

    gs_init_symmetric(hs, prologue, prologue_len);
    gs_mix_hash(hs, hs->s.pub, GS_NOISE_KEY_BYTES);
}

void gs_noise_set_ephemeral(gs_noise_handshake *hs, const uint8_t *secret) {
    gs_noise_key_from_secret(&hs->e, secret);
    hs->has_e = true;
}

bool gs_noise_done(const gs_noise_handshake *hs) {
    return !hs->failed && hs->step >= 2;
}

const uint8_t *gs_noise_remote_static(const gs_noise_handshake *hs) {
    return hs->has_rs ? hs->rs : nullptr;
}

// --- the messages -----------------------------------------------------------

static void gs_take_ephemeral(gs_noise_handshake *hs) {
    if (hs->has_e) return;
    gs_noise_keygen(&hs->e);
    hs->has_e = true;
}

size_t gs_noise_write_message(gs_noise_handshake *hs, const uint8_t *payload,
                              size_t payload_len, uint8_t *out, size_t cap) {
    if (hs->failed) return 0;

    size_t at = 0;
    bool mine = (hs->step == 0) == hs->initiator;
    if (!mine || hs->step >= 2) { hs->failed = true; return 0; }

    // `e` - the same first token in both messages.
    gs_take_ephemeral(hs);
    if (cap < at + GS_NOISE_KEY_BYTES) { hs->failed = true; return 0; }
    memcpy(out + at, hs->e.pub, GS_NOISE_KEY_BYTES);
    gs_mix_hash(hs, hs->e.pub, GS_NOISE_KEY_BYTES);
    at += GS_NOISE_KEY_BYTES;

    uint8_t dh[32];

    if (hs->step == 0) {
        // -> e, es, s, ss
        if (!gs_dh(hs->e.sec, hs->rs, dh)) { hs->failed = true; return 0; }
        gs_mix_key(hs, dh, sizeof dh);

        size_t n = gs_encrypt_and_hash(hs, hs->s.pub, GS_NOISE_KEY_BYTES,
                                       out + at, cap - at);
        if (n == 0) { hs->failed = true; return 0; }
        at += n;

        if (!gs_dh(hs->s.sec, hs->rs, dh)) { hs->failed = true; return 0; }
        gs_mix_key(hs, dh, sizeof dh);
    } else {
        // <- e, ee, se
        if (!gs_dh(hs->e.sec, hs->re, dh)) { hs->failed = true; return 0; }
        gs_mix_key(hs, dh, sizeof dh);

        if (!gs_dh(hs->e.sec, hs->rs, dh)) { hs->failed = true; return 0; }
        gs_mix_key(hs, dh, sizeof dh);
    }

    size_t n = gs_encrypt_and_hash(hs, payload, payload_len, out + at, cap - at);
    if (n == 0 && payload_len > 0) { hs->failed = true; return 0; }
    at += n;

    sodium_memzero(dh, sizeof dh);
    hs->step++;
    return at;
}

bool gs_noise_read_message(gs_noise_handshake *hs, const uint8_t *in, size_t len,
                           uint8_t *payload, size_t cap, size_t *payload_len) {
    if (hs->failed) return false;

    bool mine = (hs->step == 0) == hs->initiator;
    if (mine || hs->step >= 2) { hs->failed = true; return false; }
    if (len < GS_NOISE_KEY_BYTES) { hs->failed = true; return false; }

    size_t at = 0;

    // `e`
    memcpy(hs->re, in, GS_NOISE_KEY_BYTES);
    hs->has_re = true;
    gs_mix_hash(hs, hs->re, GS_NOISE_KEY_BYTES);
    at += GS_NOISE_KEY_BYTES;

    uint8_t dh[32];

    if (hs->step == 0) {
        // <- e, es, s, ss, read by the responder
        if (!gs_dh(hs->s.sec, hs->re, dh)) { hs->failed = true; return false; }
        gs_mix_key(hs, dh, sizeof dh);

        size_t sealed = GS_NOISE_KEY_BYTES + GS_NOISE_TAG_BYTES;
        if (len < at + sealed) { hs->failed = true; return false; }

        size_t got = 0;
        if (!gs_decrypt_and_hash(hs, in + at, sealed, hs->rs, sizeof hs->rs, &got) ||
            got != GS_NOISE_KEY_BYTES) {
            hs->failed = true;
            return false;
        }
        hs->has_rs = true;
        at += sealed;

        if (!gs_dh(hs->s.sec, hs->rs, dh)) { hs->failed = true; return false; }
        gs_mix_key(hs, dh, sizeof dh);
    } else {
        // -> e, ee, se, read by the initiator
        if (!gs_dh(hs->e.sec, hs->re, dh)) { hs->failed = true; return false; }
        gs_mix_key(hs, dh, sizeof dh);

        if (!gs_dh(hs->s.sec, hs->re, dh)) { hs->failed = true; return false; }
        gs_mix_key(hs, dh, sizeof dh);
    }

    size_t got = 0;
    if (!gs_decrypt_and_hash(hs, in + at, len - at, payload, cap, &got)) {
        hs->failed = true;
        return false;
    }
    if (payload_len != nullptr) *payload_len = got;

    sodium_memzero(dh, sizeof dh);
    hs->step++;
    return true;
}

bool gs_noise_split(gs_noise_handshake *hs, gs_noise_session *out) {
    if (!gs_noise_done(hs)) return false;

    uint8_t k1[GS_NOISE_HASH_BYTES], k2[GS_NOISE_HASH_BYTES];
    gs_hkdf(hs->ck, nullptr, 0, k1, k2, nullptr);

    memset(out, 0, sizeof *out);

    // The initiator sends under the first key and receives under the second;
    // the responder does the opposite. One of the two ends has to be the mirror
    // of the other and the framework says which.
    gs_cipher_init(&out->send, hs->initiator ? k1 : k2);
    gs_cipher_init(&out->recv, hs->initiator ? k2 : k1);

    memcpy(out->handshake_hash, hs->h, sizeof out->handshake_hash);
    out->established = true;

    sodium_memzero(k1, sizeof k1);
    sodium_memzero(k2, sizeof k2);

    // The handshake's own secrets are finished with. Everything after this
    // point uses the two transport keys and nothing else.
    sodium_memzero(hs->ck, sizeof hs->ck);
    sodium_memzero(&hs->cs, sizeof hs->cs);
    sodium_memzero(hs->e.sec, sizeof hs->e.sec);
    return true;
}

// --- transport --------------------------------------------------------------

size_t gs_noise_seal(gs_noise_session *s, const uint8_t *plain, size_t len,
                     uint8_t *out, size_t cap) {
    if (!s->established) return 0;
    if (s->send.n >= GS_NOISE_MESSAGE_LIMIT) return 0;
    if (cap < len + GS_NOISE_OVERHEAD) return 0;

    uint64_t counter = s->send.n;
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)((counter >> (8 * i)) & 0xffu);

    // **The counter travels in front, and nothing is mixed into the AEAD.**
    //
    // It looks like it ought to be authenticated data. It does not need to be:
    // the counter *is* the nonce, the nonce is an input to the tag, and a
    // counter changed in flight therefore produces a tag that does not verify.
    // Passing it as associated data as well would add nothing and would make
    // every sealed message differ from what the framework specifies - which
    // would cost the published test vectors and the interoperability check,
    // because both of those cover the transport phase and not only the
    // handshake. Matching the specification exactly is worth more than a
    // redundant belt.
    size_t n = gs_encrypt_ad(&s->send, nullptr, 0, plain, len, out + 8, cap - 8);
    if (n == 0 && len > 0) return 0;
    return 8 + n;
}

// The window, and the two questions it answers: is this too old to judge, and
// have I seen it before.
static bool gs_window_allows(const gs_noise_session *s, uint64_t counter) {
    if (!s->started) return true;
    if (counter > s->newest) return true;
    if (s->newest - counter >= GS_NOISE_WINDOW) return false;
    return (s->seen & (1ull << (s->newest - counter))) == 0;
}

static void gs_window_record(gs_noise_session *s, uint64_t counter) {
    if (!s->started) {
        s->started = true;
        s->newest = counter;
        s->seen = 1ull;
        return;
    }
    if (counter > s->newest) {
        uint64_t shift = counter - s->newest;
        s->seen = shift >= GS_NOISE_WINDOW ? 0ull : (s->seen << shift);
        s->seen |= 1ull;
        s->newest = counter;
        return;
    }
    s->seen |= 1ull << (s->newest - counter);
}

bool gs_noise_open(gs_noise_session *s, const uint8_t *in, size_t len,
                   uint8_t *plain, size_t cap, size_t *plain_len) {
    if (!s->established) return false;
    if (len < GS_NOISE_OVERHEAD) return false;

    uint64_t counter = 0;
    for (int i = 0; i < 8; i++) counter |= (uint64_t)in[i] << (8 * i);

    if (counter >= GS_NOISE_MESSAGE_LIMIT) return false;
    if (!gs_window_allows(s, counter)) return false;

    // The counter off the wire, not the receiver's own idea of what should
    // come next - which is the whole point of sending it.
    uint64_t was = s->recv.n;
    s->recv.n = counter;

    size_t got = 0;
    bool ok = gs_decrypt_ad(&s->recv, nullptr, 0, in + 8, len - 8, plain, cap, &got);

    // **The window is only moved by a datagram that turned out to be genuine.**
    // Recording the counter first would let anybody who can send a packet mark
    // a sequence number as seen and have the real one refused when it arrived.
    s->recv.n = was;
    if (!ok) return false;

    gs_window_record(s, counter);
    if (plain_len != nullptr) *plain_len = got;
    return true;
}

void gs_noise_close(gs_noise_session *s) {
    sodium_memzero(s, sizeof *s);
}

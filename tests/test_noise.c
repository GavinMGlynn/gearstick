// test_noise.c - is the tunnel the tunnel it says it is?
//
// **The evidence here is deliberately not our own opinion of our own code.**
// The first test drives the Noise Protocol Framework's published test vectors,
// which were produced by somebody else's implementation and say what every byte
// of a handshake and of the traffic after it must be. The rest check the
// properties a datagram tunnel needs on top of the framework - a replay
// refused, a reordering accepted, a flipped bit caught - which the vectors do
// not cover because the framework assumes a stream.
//
// The interoperability check lives in tools/noise_interop.py, because an
// independent implementation has to be independent.
#include "core/gs_blake2s.h"
#include "net/gs_noise.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gs_failures = 0;
static int gs_checks = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        gs_checks++;                                                           \
        if (!(cond)) {                                                         \
            gs_failures++;                                                     \
            printf("  FAIL %s\n    %s:%d: %s\n", gs_test, __FILE__, __LINE__,  \
                   #cond);                                                     \
        }                                                                      \
    } while (0)

#define TEST(name)                                                             \
    static const char *gs_test_##name = #name;                                 \
    static void run_##name(void);                                              \
    static void name##_body(const char *gs_test);                              \
    static void run_##name(void) { name##_body(gs_test_##name); }              \
    static void name##_body(const char *gs_test)

// --- reading the published vector -------------------------------------------
//
// A purpose-built scanner rather than a JSON library, because the fixture is
// fixed and a dependency for one file of test data is a dependency. It finds
// `"key": "value"` in order, which is all this shape of document needs.

static char gs_vector[65536];
static size_t gs_vector_len;

static bool gs_load_vector(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == nullptr) return false;
    gs_vector_len = fread(gs_vector, 1, sizeof gs_vector - 1, f);
    fclose(f);
    gs_vector[gs_vector_len] = '\0';
    return gs_vector_len > 0;
}

// The next `"key": "..."` at or after `from`, copied into the caller's buffer.
// `from` moves past it, so the caller can walk the message list in order.
//
// **The buffer belongs to the caller deliberately.** The first version of this
// returned a pointer to one static buffer, and the first thing the test does is
// read a payload and then a ciphertext - so the second call overwrote the
// first, every message was compared against itself, and the whole vector
// appeared to fail against an implementation that was in fact byte-for-byte
// correct. An hour went into the tunnel before the fault turned out to be here.
static bool gs_field(const char *key, size_t *from, char *out, size_t cap) {
    char pattern[64];
    snprintf(pattern, sizeof pattern, "\"%s\"", key);

    const char *at = strstr(gs_vector + *from, pattern);
    if (at == nullptr) return false;

    const char *colon = strchr(at, ':');
    if (colon == nullptr) return false;
    const char *open = strchr(colon, '"');
    if (open == nullptr) return false;
    const char *close = strchr(open + 1, '"');
    if (close == nullptr) return false;

    size_t n = (size_t)(close - open - 1);
    if (n >= cap) return false;
    memcpy(out, open + 1, n);
    out[n] = '\0';

    *from = (size_t)(close - gs_vector) + 1;
    return true;
}

// **Does this buffer contain that run of bytes anywhere?** `memmem` would do
// it and is a GNU extension, so it is not there on Windows and the tests are
// built on Windows. Small, obvious, and portable beats clever here.
static bool gs_contains(const uint8_t *hay, size_t hay_len,
                        const void *needle, size_t needle_len) {
    if (needle_len == 0 || hay_len < needle_len) return false;
    for (size_t i = 0; i + needle_len <= hay_len; i++) {
        if (memcmp(hay + i, needle, needle_len) == 0) return true;
    }
    return false;
}

static size_t gs_unhex(const char *hex, uint8_t *out, size_t cap) {
    size_t n = strlen(hex) / 2;
    if (n > cap) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned byte = 0;
        for (int k = 0; k < 2; k++) {
            char c = hex[i * 2 + (size_t)k];
            unsigned d = (c >= '0' && c <= '9') ? (unsigned)(c - '0')
                       : (c >= 'a' && c <= 'f') ? (unsigned)(c - 'a' + 10)
                       : (unsigned)(c - 'A' + 10);
            byte = byte * 16u + d;
        }
        out[i] = (uint8_t)byte;
    }
    return n;
}

TEST(the_frameworks_published_vectors_pass) {
    // **The one piece of evidence here that does not rest on our own opinion of
    // our own code.** These bytes come from the Noise Protocol Framework's
    // published vectors, generated by an implementation nobody here wrote. Every
    // byte of both handshake messages, the handshake hash, and the traffic
    // afterwards has to come out the same.
    if (!gs_load_vector(GS_NOISE_VECTORS)) {
        printf("  FAIL %s\n    cannot read %s\n", gs_test, GS_NOISE_VECTORS);
        gs_failures++;
        return;
    }

    size_t at = 0;
    uint8_t init_prologue[64], resp_prologue[64];
    uint8_t init_static[32], init_ephemeral[32], init_remote_static[32];
    uint8_t resp_static[32], resp_ephemeral[32], want_hash[32];

    static char field[8192];
    CHECK(gs_field("protocol_name", &at, field, sizeof field));
    CHECK(strcmp(field, GS_NOISE_PROTOCOL) == 0);

    size_t init_prologue_len = 0, resp_prologue_len = 0;
    CHECK(gs_field("init_prologue", &at, field, sizeof field));
    init_prologue_len = gs_unhex(field, init_prologue, sizeof init_prologue);
    CHECK(gs_field("init_static", &at, field, sizeof field));
    CHECK(gs_unhex(field, init_static, sizeof init_static) == 32);
    CHECK(gs_field("init_ephemeral", &at, field, sizeof field));
    CHECK(gs_unhex(field, init_ephemeral, sizeof init_ephemeral) == 32);
    CHECK(gs_field("init_remote_static", &at, field, sizeof field));
    CHECK(gs_unhex(field, init_remote_static, sizeof init_remote_static) == 32);
    CHECK(gs_field("resp_prologue", &at, field, sizeof field));
    resp_prologue_len = gs_unhex(field, resp_prologue, sizeof resp_prologue);
    CHECK(gs_field("resp_static", &at, field, sizeof field));
    CHECK(gs_unhex(field, resp_static, sizeof resp_static) == 32);
    CHECK(gs_field("resp_ephemeral", &at, field, sizeof field));
    CHECK(gs_unhex(field, resp_ephemeral, sizeof resp_ephemeral) == 32);
    CHECK(gs_field("handshake_hash", &at, field, sizeof field));
    CHECK(gs_unhex(field, want_hash, sizeof want_hash) == 32);

    if (gs_failures > 0) return;

    gs_noise_keypair is, rs;
    gs_noise_key_from_secret(&is, init_static);
    gs_noise_key_from_secret(&rs, resp_static);

    // The vector says what the initiator believes the responder's key is. It
    // should be the responder's actual key, and if it is not then the fixture
    // has been mangled and everything below would fail for the wrong reason.
    CHECK(memcmp(init_remote_static, rs.pub, 32) == 0);

    static gs_noise_handshake initiator, responder;
    gs_noise_init_initiator(&initiator, &is, init_remote_static,
                            init_prologue, init_prologue_len);
    gs_noise_init_responder(&responder, &rs, resp_prologue, resp_prologue_len);
    gs_noise_set_ephemeral(&initiator, init_ephemeral);
    gs_noise_set_ephemeral(&responder, resp_ephemeral);

    static uint8_t payload[4096], want_cipher[4096], got_cipher[4096], back[4096];
    static gs_noise_session a, b;
    bool split = false;

    for (int m = 0; m < 6; m++) {
        static char p_hex[8192], c_hex[8192];
        CHECK(gs_field("payload", &at, p_hex, sizeof p_hex));
        CHECK(gs_field("ciphertext", &at, c_hex, sizeof c_hex));

        size_t p_len = gs_unhex(p_hex, payload, sizeof payload);
        size_t c_len = gs_unhex(c_hex, want_cipher, sizeof want_cipher);
        CHECK(c_len > 0);

        // The initiator speaks first and they alternate.
        bool from_initiator = (m % 2) == 0;

        if (m < 2) {
            gs_noise_handshake *writer = from_initiator ? &initiator : &responder;
            gs_noise_handshake *reader = from_initiator ? &responder : &initiator;

            size_t n = gs_noise_write_message(writer, payload, p_len,
                                              got_cipher, sizeof got_cipher);
            CHECK(n == c_len);
            CHECK(n == c_len && memcmp(got_cipher, want_cipher, n) == 0);

            size_t got = 0;
            CHECK(gs_noise_read_message(reader, want_cipher, c_len, back,
                                        sizeof back, &got));
            CHECK(got == p_len);
            CHECK(got == p_len && memcmp(back, payload, got) == 0);

            if (m == 1) {
                CHECK(gs_noise_done(&initiator));
                CHECK(gs_noise_done(&responder));

                // The handshake hash both ends arrived at, which is what a
                // channel binding would be built on.
                CHECK(memcmp(initiator.h, want_hash, 32) == 0);
                CHECK(memcmp(responder.h, want_hash, 32) == 0);

                CHECK(gs_noise_split(&initiator, &a));
                CHECK(gs_noise_split(&responder, &b));
                split = true;
            }
            continue;
        }

        // **The transport phase, and the reason the counter sits outside the
        // AEAD.** What a sealed datagram carries after its eight-byte counter
        // is exactly a framework transport message, so the published vectors
        // check this half too instead of stopping at the handshake.
        CHECK(split);
        if (!split) return;

        gs_noise_session *sender = from_initiator ? &a : &b;
        gs_noise_session *receiver = from_initiator ? &b : &a;

        size_t n = gs_noise_seal(sender, payload, p_len, got_cipher,
                                 sizeof got_cipher);
        CHECK(n == c_len + 8);
        CHECK(n == c_len + 8 && memcmp(got_cipher + 8, want_cipher, c_len) == 0);

        size_t got = 0;
        CHECK(gs_noise_open(receiver, got_cipher, n, back, sizeof back, &got));
        CHECK(got == p_len);
        CHECK(got == p_len && memcmp(back, payload, got) == 0);
    }
}

// --- a whole conversation, with keys nobody wrote down ----------------------

static void gs_connect(gs_noise_session *client, gs_noise_session *server,
                       gs_noise_keypair *client_key, gs_noise_keypair *server_key) {
    static gs_noise_handshake i, r;
    static uint8_t m1[512], m2[512], payload[512];
    size_t got = 0;

    gs_noise_keygen(client_key);
    gs_noise_keygen(server_key);

    gs_noise_init_initiator(&i, client_key, server_key->pub, nullptr, 0);
    gs_noise_init_responder(&r, server_key, nullptr, 0);

    size_t n1 = gs_noise_write_message(&i, nullptr, 0, m1, sizeof m1);
    gs_noise_read_message(&r, m1, n1, payload, sizeof payload, &got);
    size_t n2 = gs_noise_write_message(&r, nullptr, 0, m2, sizeof m2);
    gs_noise_read_message(&i, m2, n2, payload, sizeof payload, &got);

    gs_noise_split(&i, client);
    gs_noise_split(&r, server);
}

TEST(a_handshake_leaves_both_ends_able_to_talk_and_sure_who_to) {
    static gs_noise_handshake i, r;
    static gs_noise_session client, server;
    gs_noise_keypair ck, sk;
    static uint8_t m1[512], m2[512], out[512];
    size_t got = 0;

    gs_noise_keygen(&ck);
    gs_noise_keygen(&sk);

    gs_noise_init_initiator(&i, &ck, sk.pub, (const uint8_t *)"gearstick/1", 11);
    gs_noise_init_responder(&r, &sk, (const uint8_t *)"gearstick/1", 11);

    size_t n1 = gs_noise_write_message(&i, (const uint8_t *)"hello", 5, m1, sizeof m1);
    CHECK(n1 > 0);
    CHECK(gs_noise_read_message(&r, m1, n1, out, sizeof out, &got));
    CHECK(got == 5 && memcmp(out, "hello", 5) == 0);

    // **The responder learns who the initiator is, and it is not something the
    // initiator merely asserted** - the key it named is one it had to prove it
    // holds, because the handshake would not have completed otherwise.
    CHECK(gs_noise_remote_static(&r) != nullptr);
    CHECK(memcmp(gs_noise_remote_static(&r), ck.pub, 32) == 0);

    size_t n2 = gs_noise_write_message(&r, (const uint8_t *)"welcome", 7, m2, sizeof m2);
    CHECK(n2 > 0);
    CHECK(gs_noise_read_message(&i, m2, n2, out, sizeof out, &got));
    CHECK(got == 7 && memcmp(out, "welcome", 7) == 0);

    CHECK(gs_noise_done(&i));
    CHECK(gs_noise_done(&r));
    CHECK(memcmp(i.h, r.h, 32) == 0);

    CHECK(gs_noise_split(&i, &client));
    CHECK(gs_noise_split(&r, &server));

    // Both directions carry traffic.
    static uint8_t sealed[512];
    size_t n = gs_noise_seal(&client, (const uint8_t *)"up", 2, sealed, sizeof sealed);
    CHECK(n == 2 + GS_NOISE_OVERHEAD);
    CHECK(gs_noise_open(&server, sealed, n, out, sizeof out, &got));
    CHECK(got == 2 && memcmp(out, "up", 2) == 0);

    n = gs_noise_seal(&server, (const uint8_t *)"down", 4, sealed, sizeof sealed);
    CHECK(gs_noise_open(&client, sealed, n, out, sizeof out, &got));
    CHECK(got == 4 && memcmp(out, "down", 4) == 0);
}

TEST(a_handshake_to_the_wrong_key_does_not_complete) {
    // IK means the client already knows the server's key. Somebody standing in
    // the middle with a key of their own cannot become the server, and the
    // client finds out on the first message rather than after telling it
    // anything.
    static gs_noise_handshake i, impostor;
    gs_noise_keypair ck, sk, fake;
    static uint8_t m1[512], out[512];
    size_t got = 0;

    gs_noise_keygen(&ck);
    gs_noise_keygen(&sk);
    gs_noise_keygen(&fake);

    gs_noise_init_initiator(&i, &ck, sk.pub, nullptr, 0);
    gs_noise_init_responder(&impostor, &fake, nullptr, 0);

    size_t n1 = gs_noise_write_message(&i, (const uint8_t *)"secret", 6, m1, sizeof m1);
    CHECK(n1 > 0);

    // The impostor cannot read it, and cannot answer it.
    CHECK(!gs_noise_read_message(&impostor, m1, n1, out, sizeof out, &got));
    CHECK(!gs_noise_done(&impostor));

    // And the client's identity was not handed over on the way: the static key
    // in message one is encrypted, which is the reason for IK over NK.
    CHECK(!gs_contains(m1, n1, ck.pub, 32));
}

TEST(a_prologue_both_ends_do_not_share_stops_the_handshake) {
    // The prologue is where a version goes. It is authenticated and not sent,
    // so two ends that disagree about what they are speaking find out at once
    // rather than halfway through a race.
    static gs_noise_handshake i, r;
    gs_noise_keypair ck, sk;
    static uint8_t m1[512], out[512];
    size_t got = 0;

    gs_noise_keygen(&ck);
    gs_noise_keygen(&sk);

    gs_noise_init_initiator(&i, &ck, sk.pub, (const uint8_t *)"gearstick/1", 11);
    gs_noise_init_responder(&r, &sk, (const uint8_t *)"gearstick/2", 11);

    size_t n1 = gs_noise_write_message(&i, nullptr, 0, m1, sizeof m1);
    CHECK(n1 > 0);
    CHECK(!gs_noise_read_message(&r, m1, n1, out, sizeof out, &got));
}

TEST(a_capture_carries_none_of_the_plaintext_it_was_given) {
    static gs_noise_session client, server;
    gs_noise_keypair ck, sk;
    gs_connect(&client, &server, &ck, &sk);

    // Something distinctive, long enough that a partial leak would show.
    static const char secret[] =
        "ada set a lap of 1:23.45 on the oval at one sixth gravity";
    static uint8_t sealed[512], out[512];
    size_t got = 0;

    size_t n = gs_noise_seal(&client, (const uint8_t *)secret, sizeof secret - 1,
                             sealed, sizeof sealed);
    CHECK(n == sizeof secret - 1 + GS_NOISE_OVERHEAD);

    // Not the whole thing, and not any eight bytes of it either - the second is
    // the check that would catch a cipher that had quietly become a no-op for
    // part of the message.
    CHECK(!gs_contains(sealed, n, secret, sizeof secret - 1));
    for (size_t i = 0; i + 8 <= sizeof secret - 1; i++) {
        CHECK(!gs_contains(sealed, n, secret + i, 8));
    }

    CHECK(gs_noise_open(&server, sealed, n, out, sizeof out, &got));
    CHECK(got == sizeof secret - 1);
    CHECK(memcmp(out, secret, got) == 0);
}

TEST(a_datagram_with_one_bit_changed_is_refused_rather_than_acted_on) {
    static gs_noise_session client, server;
    gs_noise_keypair ck, sk;
    gs_connect(&client, &server, &ck, &sk);

    static uint8_t sealed[512], spoiled[512], out[512];
    size_t got = 0;
    const char *msg = "the inputs for tick 4711";

    size_t n = gs_noise_seal(&client, (const uint8_t *)msg, strlen(msg),
                             sealed, sizeof sealed);
    CHECK(n > 0);

    // Every bit of it, one at a time. **Including the counter in front**, which
    // is not passed as associated data and is authenticated all the same,
    // because the counter is the nonce and the nonce is an input to the tag.
    for (size_t byte = 0; byte < n; byte++) {
        for (int bit = 0; bit < 8; bit++) {
            memcpy(spoiled, sealed, n);
            spoiled[byte] = (uint8_t)(spoiled[byte] ^ (1u << bit));

            gs_noise_session copy = server;
            CHECK(!gs_noise_open(&copy, spoiled, n, out, sizeof out, &got));
        }
    }

    // And the untouched one still opens, so the sweep above was refusing
    // damage rather than refusing everything.
    CHECK(gs_noise_open(&server, sealed, n, out, sizeof out, &got));
    CHECK(got == strlen(msg));
}

TEST(a_replayed_datagram_is_refused_and_a_reordered_one_is_not) {
    // **These are different things and a counter alone cannot tell them
    // apart.** A rule of "must be newer than the last one accepted" throws away
    // the reordering every lossy link produces and calls it an attack, which on
    // a race means discarding inputs that were never resent.
    static gs_noise_session client, server;
    gs_noise_keypair ck, sk;
    gs_connect(&client, &server, &ck, &sk);

    enum { COUNT = 200 };
    static uint8_t sealed[COUNT][64];
    static size_t len[COUNT];
    static uint8_t out[128];
    size_t got = 0;

    for (int i = 0; i < COUNT; i++) {
        uint8_t body[8];
        for (int k = 0; k < 8; k++) body[k] = (uint8_t)(i * 8 + k);
        len[i] = gs_noise_seal(&client, body, sizeof body, sealed[i], sizeof sealed[i]);
        CHECK(len[i] > 0);
    }

    // In order first, so there is a window to be inside.
    for (int i = 0; i < 32; i++) {
        CHECK(gs_noise_open(&server, sealed[i], len[i], out, sizeof out, &got));
    }

    // Every one of those again: seen before, refused.
    for (int i = 0; i < 32; i++) {
        CHECK(!gs_noise_open(&server, sealed[i], len[i], out, sizeof out, &got));
    }

    // A jump forwards, then the ones it skipped - which is reordering, and is
    // accepted, and each of them only once.
    CHECK(gs_noise_open(&server, sealed[60], len[60], out, sizeof out, &got));
    for (int i = 59; i >= 40; i--) {
        CHECK(gs_noise_open(&server, sealed[i], len[i], out, sizeof out, &got));
        CHECK(!gs_noise_open(&server, sealed[i], len[i], out, sizeof out, &got));
    }

    // Far enough back that the window cannot say, which is refused - a window
    // that guessed would be a window that could be walked backwards.
    CHECK(gs_noise_open(&server, sealed[150], len[150], out, sizeof out, &got));
    CHECK(!gs_noise_open(&server, sealed[32], len[32], out, sizeof out, &got));
}

TEST(a_session_stops_sending_before_its_counter_could_repeat) {
    // A repeated nonce under one key destroys the confidentiality of both
    // messages it was used for. The counter is therefore not allowed to reach a
    // point where anybody has to reason about it.
    static gs_noise_session client, server;
    gs_noise_keypair ck, sk;
    gs_connect(&client, &server, &ck, &sk);

    static uint8_t sealed[128];
    CHECK(gs_noise_seal(&client, (const uint8_t *)"x", 1, sealed, sizeof sealed) > 0);

    client.send.n = GS_NOISE_MESSAGE_LIMIT - 1;
    CHECK(gs_noise_seal(&client, (const uint8_t *)"x", 1, sealed, sizeof sealed) > 0);
    CHECK(gs_noise_seal(&client, (const uint8_t *)"x", 1, sealed, sizeof sealed) == 0);
}

TEST(rubbish_where_a_handshake_should_be_is_refused_without_reading_past_it) {
    // Every length up to a real message, and a real message with every single
    // byte changed. None of it may be accepted and none of it may be read
    // beyond - which is what the sanitizers this runs under are for.
    static gs_noise_handshake i, r;
    gs_noise_keypair ck, sk;
    static uint8_t m1[512], spoiled[512], out[512];
    size_t got = 0;

    gs_noise_keygen(&ck);
    gs_noise_keygen(&sk);
    gs_noise_init_initiator(&i, &ck, sk.pub, nullptr, 0);
    size_t n1 = gs_noise_write_message(&i, (const uint8_t *)"payload", 7, m1, sizeof m1);
    CHECK(n1 > 0);

    for (size_t cut = 0; cut < n1; cut++) {
        gs_noise_init_responder(&r, &sk, nullptr, 0);
        CHECK(!gs_noise_read_message(&r, m1, cut, out, sizeof out, &got));
    }

    for (size_t byte = 0; byte < n1; byte++) {
        memcpy(spoiled, m1, n1);
        spoiled[byte] = (uint8_t)(spoiled[byte] ^ 0xffu);
        gs_noise_init_responder(&r, &sk, nullptr, 0);
        CHECK(!gs_noise_read_message(&r, spoiled, n1, out, sizeof out, &got));
    }

    // And the real one is still accepted.
    gs_noise_init_responder(&r, &sk, nullptr, 0);
    CHECK(gs_noise_read_message(&r, m1, n1, out, sizeof out, &got));
    CHECK(got == 7);
}

TEST(a_failed_handshake_stays_failed) {
    // A handshake that could be retried after a failure is one somebody can
    // grind against, and one whose state after a refusal is anybody's guess.
    static gs_noise_handshake i, r;
    gs_noise_keypair ck, sk;
    static uint8_t m1[512], out[512];
    size_t got = 0;

    gs_noise_keygen(&ck);
    gs_noise_keygen(&sk);
    gs_noise_init_initiator(&i, &ck, sk.pub, nullptr, 0);
    gs_noise_init_responder(&r, &sk, nullptr, 0);

    size_t n1 = gs_noise_write_message(&i, nullptr, 0, m1, sizeof m1);
    uint8_t spoiled[512];
    memcpy(spoiled, m1, n1);
    spoiled[40] = (uint8_t)(spoiled[40] ^ 0x01u);

    CHECK(!gs_noise_read_message(&r, spoiled, n1, out, sizeof out, &got));
    // The honest message now, which would have worked a moment ago.
    CHECK(!gs_noise_read_message(&r, m1, n1, out, sizeof out, &got));
    CHECK(!gs_noise_done(&r));
}


// --- a whole race, relayed, through the tunnel ------------------------------
//
// **The claim this makes is not about the cipher.** It is that putting a tunnel
// under rollback does not break rollback: the sealed channel has to tolerate
// exactly what the game already tolerates - loss, reordering, and the fact that
// a race sends a datagram every tick - and end with four machines that agree.
//
// The relayed shape is modelled faithfully, because it is the shape the tunnel
// applies to. Every peer holds one tunnel to the server; a datagram is sealed
// to the server, opened there, and re-sealed to each of the others. Two
// encryptions per hop and two sessions per peer, which is what a relay costs.

#include "core/gs_net.h"
#include "core/gs_track.h"

#define GS_RELAY_PEERS 4
#define GS_RELAY_SLOTS 512

// A link that loses and reorders, deterministically, so a failure is one
// somebody else can reproduce rather than a bad afternoon.
typedef struct gs_relay_link {
    struct {
        uint8_t  bytes[GS_NOISE_OVERHEAD + 1024];
        size_t   len;
        uint32_t due;
        bool     live;
    } slot[GS_RELAY_SLOTS];
    uint32_t seed;
    uint32_t latency, jitter, loss_pct;
    uint32_t sent, dropped, delivered;
} gs_relay_link;

static uint32_t gs_relay_rand(gs_relay_link *l) {
    l->seed = l->seed * 1103515245u + 12345u;
    return (l->seed >> 16) & 0x7fffu;
}

static void gs_relay_send(gs_relay_link *l, uint32_t now, const uint8_t *b,
                          size_t n) {
    l->sent++;
    if (n == 0 || n > sizeof l->slot[0].bytes) return;
    if (l->loss_pct > 0 && gs_relay_rand(l) % 100u < l->loss_pct) {
        l->dropped++;
        return;
    }
    for (int i = 0; i < GS_RELAY_SLOTS; i++) {
        if (l->slot[i].live) continue;
        memcpy(l->slot[i].bytes, b, n);
        l->slot[i].len = n;
        // The jitter is what makes this a reordering link and not merely a
        // late one: two datagrams sent in order can arrive the other way round.
        l->slot[i].due = now + l->latency +
                         (l->jitter ? gs_relay_rand(l) % (l->jitter + 1u) : 0u);
        l->slot[i].live = true;
        return;
    }
}


// Four drivers on four rhythms, so no two ever change their minds together.
static gs_input gs_relay_drive(uint8_t player, uint32_t tick) {
    uint32_t turn = 23u + player * 14u;
    uint32_t brake = 53u + player * 19u;

    gs_input in = GS_IN_ACCEL;
    if ((tick / turn) % 3u == 0u) in |= GS_IN_LEFT;
    else if ((tick / turn) % 3u == 1u) in |= GS_IN_RIGHT;
    if ((tick / brake) % 4u == 0u) in |= GS_IN_BRAKE;
    return in;
}

TEST(a_relayed_four_player_race_agrees_tick_for_tick_through_the_tunnel) {
    static gs_noise_session client[GS_RELAY_PEERS];   // each peer's end
    static gs_noise_session server[GS_RELAY_PEERS];   // the server's end of each
    static gs_relay_link up[GS_RELAY_PEERS];          // peer -> server
    static gs_relay_link down[GS_RELAY_PEERS];        // server -> peer
    static gs_net net[GS_RELAY_PEERS];
    static gs_track t;

    gs_noise_keypair server_key;
    gs_noise_keygen(&server_key);

    for (int i = 0; i < GS_RELAY_PEERS; i++) {
        static gs_noise_handshake hs_c, hs_s;
        gs_noise_keypair peer_key;
        gs_noise_keygen(&peer_key);

        static uint8_t m1[512], m2[512], payload[512];
        size_t got = 0;

        gs_noise_init_initiator(&hs_c, &peer_key, server_key.pub, nullptr, 0);
        gs_noise_init_responder(&hs_s, &server_key, nullptr, 0);
        size_t n1 = gs_noise_write_message(&hs_c, nullptr, 0, m1, sizeof m1);
        CHECK(gs_noise_read_message(&hs_s, m1, n1, payload, sizeof payload, &got));
        size_t n2 = gs_noise_write_message(&hs_s, nullptr, 0, m2, sizeof m2);
        CHECK(gs_noise_read_message(&hs_c, m2, n2, payload, sizeof payload, &got));
        CHECK(gs_noise_split(&hs_c, &client[i]));
        CHECK(gs_noise_split(&hs_s, &server[i]));

        up[i] = (gs_relay_link){ 0 };
        down[i] = (gs_relay_link){ 0 };
        up[i].seed = 0x1000u + (uint32_t)i * 7u;
        down[i].seed = 0x5000u + (uint32_t)i * 11u;
        up[i].latency = down[i].latency = 8u + (uint32_t)i;
        up[i].jitter = down[i].jitter = 4;
        // One in twenty, on every hop, which is two chances to lose each
        // datagram rather than one.
        up[i].loss_pct = down[i].loss_pct = 5;
    }

    // A track and a grid, the same on every machine.
    gs_track_init(&t, 48, 20, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= t.h; y++) {
        for (uint8_t x = 0; x <= t.w; x++) {
            gs_track_set_corner(&t, x, y, x > 20 && x < 26 ? GS_INT(1) : 0);
        }
    }
    gs_world w;
    gs_world_init(&w, GS_ONE);
    for (uint8_t i = 0; i < GS_RELAY_PEERS; i++) {
        gs_world_add_car(&w, &t, (uint8_t)GS_VEH_SPRINT_CAR,
                         GS_INT(4), GS_INT(5) + GS_INT(3) * i, 0);
    }
    for (int i = 0; i < GS_RELAY_PEERS; i++) {
        uint8_t secret[GS_NET_SECRET_BYTES];
        for (int k = 0; k < GS_NET_SECRET_BYTES; k++) {
            secret[k] = (uint8_t)((0x33u + (unsigned)i * 29u + (unsigned)k) & 0xffu);
        }
        gs_net_begin(&net[i], &w, GS_RELAY_PEERS, (uint8_t)i, secret);
    }

    // Deliver everything due, opening it on the way in - which is the receiving
    // half of the relay and the half where a tunnel would break a race if it
    // were going to.
    static uint8_t plain[2048];

    const uint32_t ticks = GS_TICK_HZ * 6;
    for (uint32_t tick = 0; tick < ticks + 400u; tick++) {
        for (int i = 0; i < GS_RELAY_PEERS; i++) {
            for (int k = 0; k < GS_RELAY_SLOTS; k++) {
                if (!down[i].slot[k].live || down[i].slot[k].due > tick) continue;
                size_t got = 0;
                if (gs_noise_open(&client[i], down[i].slot[k].bytes,
                                  down[i].slot[k].len, plain, sizeof plain, &got)) {
                    gs_net_receive(&net[i], &t, plain, got);
                    down[i].delivered++;
                }
                down[i].slot[k].live = false;
            }
        }

        // The server: open what came up, and seal it back down to everybody
        // else. Its own losses are on the way down.
        for (int i = 0; i < GS_RELAY_PEERS; i++) {
            for (int k = 0; k < GS_RELAY_SLOTS; k++) {
                if (!up[i].slot[k].live || up[i].slot[k].due > tick) continue;
                size_t got = 0;
                if (gs_noise_open(&server[i], up[i].slot[k].bytes,
                                  up[i].slot[k].len, plain, sizeof plain, &got)) {
                    up[i].delivered++;
                    for (int j = 0; j < GS_RELAY_PEERS; j++) {
                        if (j == i) continue;
                        uint8_t sealed[2048];
                        size_t n = gs_noise_seal(&server[j], plain, got, sealed,
                                                 sizeof sealed);
                        gs_relay_send(&down[j], tick, sealed, n);
                    }
                }
                up[i].slot[k].live = false;
            }
        }

        if (tick < ticks) {
            for (int i = 0; i < GS_RELAY_PEERS; i++) {
                gs_net_local_input(&net[i], gs_relay_drive((uint8_t)i, tick));

                uint8_t buf[GS_NET_MTU], sealed[2048];
                size_t n = gs_net_packet(&net[i], buf, sizeof buf);
                CHECK(n > 0);
                size_t k = gs_noise_seal(&client[i], buf, n, sealed, sizeof sealed);
                CHECK(k == n + GS_NOISE_OVERHEAD);
                gs_relay_send(&up[i], tick, sealed, k);

                gs_net_step(&net[i], &t);
            }

            // The last tick of the race: what is still owed goes out, the same
            // as a real client reaching its results screen.
            if (tick + 1 == ticks) {
                for (int i = 0; i < GS_RELAY_PEERS; i++) gs_net_finish(&net[i]);
            }
            continue;
        }

        // **The tail is part of the race, not after it.** The reveals trail the
        // commitments, so the last dozen ticks are still owed when the driving
        // stops - and they have to cross two lossy hops like everything else.
        // One loop rather than two, because two loops over overlapping
        // simulated time deliver each other's datagrams into the past.
        if (tick < ticks + 200u) {
            for (int i = 0; i < GS_RELAY_PEERS; i++) {
                uint8_t buf[GS_NET_MTU], sealed[2048];
                size_t n = gs_net_packet(&net[i], buf, sizeof buf);
                size_t k = gs_noise_seal(&client[i], buf, n, sealed, sizeof sealed);
                gs_relay_send(&up[i], tick, sealed, k);
            }
        }
    }

    // **The claim.** Four machines, every datagram sealed, one in twenty lost on
    // each of two hops and the rest reordered - and the same race at the end.
    for (int i = 0; i < GS_RELAY_PEERS; i++) {
        CHECK(!net[i].desynced);
        CHECK(!net[i].cheated);
        CHECK(net[i].confirmed_tick == ticks);
        CHECK(gs_world_hash(&net[i].confirmed) == gs_world_hash(&net[0].confirmed));
    }

    // And it is the race one machine with no network at all would have run,
    // which is the stronger statement and the one that says the tunnel changed
    // nothing about the driving.
    gs_world solo;
    gs_world_init(&solo, GS_ONE);
    for (uint8_t i = 0; i < GS_RELAY_PEERS; i++) {
        gs_world_add_car(&solo, &t, (uint8_t)GS_VEH_SPRINT_CAR,
                         GS_INT(4), GS_INT(5) + GS_INT(3) * i, 0);
    }
    for (uint32_t tick = 0; tick < ticks; tick++) {
        gs_input in[GS_MAX_CARS] = { 0 };
        for (uint8_t i = 0; i < GS_RELAY_PEERS; i++) in[i] = gs_relay_drive(i, tick);
        gs_world_step(&solo, &t, in);
    }
    CHECK(gs_world_hash(&solo) == gs_world_hash(&net[0].confirmed));

    // The links really were bad, so none of the above passed by having nothing
    // to survive.
    uint32_t lost = 0;
    for (int i = 0; i < GS_RELAY_PEERS; i++) lost += up[i].dropped + down[i].dropped;
    CHECK(lost > 100);
    for (int i = 0; i < GS_RELAY_PEERS; i++) CHECK(net[i].rollbacks > 0);
}

TEST(the_handshake_is_the_size_the_specification_says) {
    // **docs/TRANSPORT.md gives byte offsets so somebody can implement a client
    // from it.** A document that drifts from the code is worse than no document
    // - it is a wrong one that reads as authoritative - so the numbers in it are
    // pinned here and a change to the format has to come past this test.
    static gs_noise_handshake i, r;
    gs_noise_keypair ck, sk;
    static uint8_t m1[512], m2[512], out[512];
    size_t got = 0;

    gs_noise_keygen(&ck);
    gs_noise_keygen(&sk);
    gs_noise_init_initiator(&i, &ck, sk.pub, (const uint8_t *)"gearstick/1", 11);
    gs_noise_init_responder(&r, &sk, (const uint8_t *)"gearstick/1", 11);

    // Message one: ephemeral, encrypted static, encrypted payload.
    size_t n1 = gs_noise_write_message(&i, nullptr, 0, m1, sizeof m1);
    CHECK(n1 == GS_NOISE_KEY_BYTES +
                (GS_NOISE_KEY_BYTES + GS_NOISE_TAG_BYTES) +
                GS_NOISE_TAG_BYTES);
    CHECK(n1 == 96);

    CHECK(gs_noise_read_message(&r, m1, n1, out, sizeof out, &got));

    // Message two: ephemeral and encrypted payload.
    size_t n2 = gs_noise_write_message(&r, nullptr, 0, m2, sizeof m2);
    CHECK(n2 == GS_NOISE_KEY_BYTES + GS_NOISE_TAG_BYTES);
    CHECK(n2 == 48);

    CHECK(gs_noise_read_message(&i, m2, n2, out, sizeof out, &got));

    // A sealed datagram: eight bytes of counter, then the ciphertext and tag.
    static gs_noise_session a, b;
    CHECK(gs_noise_split(&i, &a));
    CHECK(gs_noise_split(&r, &b));

    static uint8_t sealed[512];
    size_t n = gs_noise_seal(&a, (const uint8_t *)"0123456789", 10, sealed,
                             sizeof sealed);
    CHECK(n == 10 + 8 + GS_NOISE_TAG_BYTES);
    CHECK(GS_NOISE_OVERHEAD == 24);

    // The counter is little-endian and starts at zero, which is what a reader
    // of the document would implement.
    CHECK(sealed[0] == 0 && sealed[1] == 0 && sealed[7] == 0);
    n = gs_noise_seal(&a, (const uint8_t *)"x", 1, sealed, sizeof sealed);
    CHECK(sealed[0] == 1);

    // And the protocol name is longer than the hash, so InitializeSymmetric
    // hashes it rather than padding - the one place an implementer is most
    // likely to go wrong, and the document says so.
    CHECK(strlen(GS_NOISE_PROTOCOL) == 33);
    CHECK(strlen(GS_NOISE_PROTOCOL) > GS_NOISE_HASH_BYTES);
}

int main(void) {
    printf("gearstick noise tests\n");

    run_the_frameworks_published_vectors_pass();
    run_a_handshake_leaves_both_ends_able_to_talk_and_sure_who_to();
    run_a_handshake_to_the_wrong_key_does_not_complete();
    run_a_prologue_both_ends_do_not_share_stops_the_handshake();
    run_a_capture_carries_none_of_the_plaintext_it_was_given();
    run_a_datagram_with_one_bit_changed_is_refused_rather_than_acted_on();
    run_a_replayed_datagram_is_refused_and_a_reordered_one_is_not();
    run_a_session_stops_sending_before_its_counter_could_repeat();
    run_rubbish_where_a_handshake_should_be_is_refused_without_reading_past_it();
    run_a_failed_handshake_stays_failed();
    run_the_handshake_is_the_size_the_specification_says();
    run_a_relayed_four_player_race_agrees_tick_for_tick_through_the_tunnel();

    if (gs_failures == 0) {
        printf("all %d noise checks passed\n", gs_checks);
        return 0;
    }
    printf("%d of %d checks failed\n", gs_failures, gs_checks);
    return 1;
}

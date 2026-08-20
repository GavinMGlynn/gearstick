// fuzz_noise.c - the handshake and the framing, fed rubbish.
//
// **This parser runs before anybody has been authenticated**, which makes it
// the most exposed one in the program: a server has to look at the first
// datagram from a stranger in order to find out whether it is a stranger. The
// framework's vectors say the honest path is right and say nothing about the
// dishonest one, and the dishonest one is where memory-safety bugs live.
//
// One input drives both halves: a handshake message first, and then, if a
// session comes out of it, a stream of framed datagrams into `gs_noise_open`.
#include "net/gs_noise.h"

#include <sodium.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

// Fixed keys, so a crash is reproducible from the input alone. A real endpoint
// must not do this and does not: `gs_noise_keygen` is what the game calls.
static const gs_noise_keypair *gs_fuzz_server(void) {
    static gs_noise_keypair k;
    static bool built = false;
    if (!built) {
        uint8_t secret[32];
        for (int i = 0; i < 32; i++) secret[i] = (uint8_t)(0x40u + (unsigned)i);
        gs_noise_key_from_secret(&k, secret);
        built = true;
    }
    return &k;
}

static const gs_noise_keypair *gs_fuzz_client(void) {
    static gs_noise_keypair k;
    static bool built = false;
    if (!built) {
        uint8_t secret[32];
        for (int i = 0; i < 32; i++) secret[i] = (uint8_t)(0x90u + (unsigned)i);
        gs_noise_key_from_secret(&k, secret);
        built = true;
    }
    return &k;
}

// An established session to throw framed rubbish at. Built once from a real
// handshake between the two fixed keys, and copied for each input so that one
// input cannot exhaust another's replay window.
static const gs_noise_session *gs_fuzz_session(void) {
    static gs_noise_session server;
    static bool built = false;
    if (built) return &server;

    static gs_noise_handshake i, r;
    static gs_noise_session client;
    uint8_t m1[512], m2[512], payload[512];
    size_t got = 0;

    gs_noise_init_initiator(&i, gs_fuzz_client(), gs_fuzz_server()->pub, nullptr, 0);
    gs_noise_init_responder(&r, gs_fuzz_server(), nullptr, 0);

    uint8_t eph[32];
    for (int k = 0; k < 32; k++) eph[k] = (uint8_t)(0x10u + (unsigned)k);
    gs_noise_set_ephemeral(&i, eph);
    for (int k = 0; k < 32; k++) eph[k] = (uint8_t)(0xd0u + (unsigned)k);
    gs_noise_set_ephemeral(&r, eph);

    size_t n1 = gs_noise_write_message(&i, nullptr, 0, m1, sizeof m1);
    gs_noise_read_message(&r, m1, n1, payload, sizeof payload, &got);
    size_t n2 = gs_noise_write_message(&r, nullptr, 0, m2, sizeof m2);
    gs_noise_read_message(&i, m2, n2, payload, sizeof payload, &got);

    gs_noise_split(&i, &client);
    gs_noise_split(&r, &server);
    built = true;
    return &server;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static bool ready = false;
    if (!ready) {
        if (sodium_init() < 0) return 0;
        ready = true;
    }
    if (size < 2) return 0;

    static uint8_t out[65536];
    size_t got = 0;

    // --- the handshake, as the side that has to look at a stranger's bytes.
    static gs_noise_handshake responder;
    gs_noise_init_responder(&responder, gs_fuzz_server(), data, 1);
    if (gs_noise_read_message(&responder, data + 1, size - 1, out, sizeof out, &got)) {
        // It parsed. Answering is part of the surface too - a responder that
        // accepted something malformed then builds a reply from it.
        static uint8_t reply[65536];
        gs_noise_write_message(&responder, out, got, reply, sizeof reply);

        static gs_noise_session session;
        if (gs_noise_split(&responder, &session)) gs_noise_close(&session);
    }

    // --- the framing, on a session that really is established. Length-prefixed
    // records, so one input is a stream and the replay window is exercised by
    // the relationship between datagrams rather than one at a time.
    gs_noise_session session = *gs_fuzz_session();
    size_t at = 0;
    while (at + 2 <= size) {
        size_t len = (size_t)data[at] | ((size_t)data[at + 1] << 8);
        at += 2;
        if (len > size - at) len = size - at;

        gs_noise_open(&session, data + at, len, out, sizeof out, &got);
        at += len;
    }
    gs_noise_close(&session);
    return 0;
}

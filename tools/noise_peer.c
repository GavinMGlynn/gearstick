// noise_peer.c - one end of a handshake, driven from outside.
//
// **This exists so that somebody else's implementation can talk to ours.** The
// published test vectors say our bytes are right; they cannot say that we can
// hold a conversation, because both sides of a vector are the same recording.
// An independent implementation completing a handshake with this is the one
// piece of evidence in the whole transport that does not rest on our own
// opinion of our own code.
//
// Line-oriented hex on stdin and stdout, so the far end can be anything. See
// tools/noise_interop.py, which drives it with the `noiseprotocol` library.
#include "net/gs_noise.h"

#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool gs_read_hex(uint8_t *out, size_t cap, size_t *len) {
    static char line[65536];
    if (fgets(line, sizeof line, stdin) == nullptr) return false;

    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    if (n % 2 != 0 || n / 2 > cap) return false;

    if (sodium_hex2bin(out, cap, line, n, nullptr, len, nullptr) != 0) return false;
    return true;
}

static void gs_write_hex(const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) printf("%02x", b[i]);
    printf("\n");
    fflush(stdout);
}

static bool gs_key_arg(const char *hex, uint8_t out[32]) {
    return strlen(hex) == 64 &&
           sodium_hex2bin(out, 32, hex, 64, nullptr, nullptr, nullptr) == 0;
}

int main(int argc, char **argv) {
    if (sodium_init() < 0) return 1;

    bool initiator = false;
    uint8_t secret[32], peer_static[32];
    bool have_secret = false, have_peer = false;
    const char *prologue = "";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--initiator") == 0) initiator = true;
        else if (strcmp(argv[i], "--responder") == 0) initiator = false;
        else if (strcmp(argv[i], "--static") == 0 && i + 1 < argc) {
            have_secret = gs_key_arg(argv[++i], secret);
        } else if (strcmp(argv[i], "--peer") == 0 && i + 1 < argc) {
            have_peer = gs_key_arg(argv[++i], peer_static);
        } else if (strcmp(argv[i], "--prologue") == 0 && i + 1 < argc) {
            prologue = argv[++i];
        } else {
            fprintf(stderr, "noise_peer: unknown argument '%s'\n", argv[i]);
            return 2;
        }
    }
    if (!have_secret || (initiator && !have_peer)) {
        fprintf(stderr,
                "usage: gearstick_noise_peer --initiator|--responder "
                "--static HEX [--peer HEX] [--prologue TEXT]\n");
        return 2;
    }

    gs_noise_keypair s;
    gs_noise_key_from_secret(&s, secret);

    static gs_noise_handshake hs;
    if (initiator) {
        gs_noise_init_initiator(&hs, &s, peer_static,
                                (const uint8_t *)prologue, strlen(prologue));
    } else {
        gs_noise_init_responder(&hs, &s, (const uint8_t *)prologue, strlen(prologue));
    }

    static uint8_t buf[65536], out[65536];
    size_t len = 0, got = 0;

    // The handshake, in whichever order this end speaks.
    if (initiator) {
        size_t n = gs_noise_write_message(&hs, (const uint8_t *)"gearstick", 9,
                                          out, sizeof out);
        if (n == 0) { fprintf(stderr, "noise_peer: could not write message 1\n"); return 1; }
        gs_write_hex(out, n);

        if (!gs_read_hex(buf, sizeof buf, &len)) return 1;
        if (!gs_noise_read_message(&hs, buf, len, out, sizeof out, &got)) {
            fprintf(stderr, "noise_peer: message 2 refused\n");
            return 1;
        }
    } else {
        if (!gs_read_hex(buf, sizeof buf, &len)) return 1;
        if (!gs_noise_read_message(&hs, buf, len, out, sizeof out, &got)) {
            fprintf(stderr, "noise_peer: message 1 refused\n");
            return 1;
        }
        size_t n = gs_noise_write_message(&hs, (const uint8_t *)"gearstick", 9,
                                          out, sizeof out);
        if (n == 0) { fprintf(stderr, "noise_peer: could not write message 2\n"); return 1; }
        gs_write_hex(out, n);
    }

    if (!gs_noise_done(&hs)) { fprintf(stderr, "noise_peer: handshake unfinished\n"); return 1; }

    static gs_noise_session session;
    if (!gs_noise_split(&hs, &session)) return 1;

    // The handshake hash, so the driver can check both ends arrived at the same
    // one - which is the strongest single statement that the two really did
    // complete the same handshake.
    fprintf(stderr, "handshake_hash ");
    for (size_t i = 0; i < GS_NOISE_HASH_BYTES; i++) {
        fprintf(stderr, "%02x", session.handshake_hash[i]);
    }
    fprintf(stderr, "\n");

    // Then echo: every sealed datagram that arrives is opened, its payload is
    // reported, and it is sealed straight back. A driver that gets its own
    // words returned has watched traffic go both ways.
    while (gs_read_hex(buf, sizeof buf, &len)) {
        if (!gs_noise_open(&session, buf, len, out, sizeof out, &got)) {
            fprintf(stderr, "noise_peer: a datagram was refused\n");
            return 1;
        }
        static uint8_t sealed[65536];
        size_t n = gs_noise_seal(&session, out, got, sealed, sizeof sealed);
        if (n == 0) return 1;
        gs_write_hex(sealed, n);
    }

    gs_noise_close(&session);
    return 0;
}

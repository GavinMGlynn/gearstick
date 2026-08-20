// fuzz_seeds.c - real captures, for the fuzzers to start from.
//
// **A fuzzer handed nothing finds the length check and stops.** Every parser
// here refuses a datagram whose first bytes are not a message it knows, so a
// random-start corpus spends its whole budget rediscovering the header and
// never reaches the arithmetic behind it. Seeding it with genuine messages -
// built by the same code the game builds them with - puts the fuzzer inside the
// parser on the first input, and mutation takes it from there.
//
// This writes the corpus rather than committing it, so a seed can never drift
// out of step with the format it is a sample of.
#include "core/gs_replay.h"
#include "core/gs_track.h"
#include "net/gs_carrier.h"
#include "net/gs_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char gs_dir[512];
static int  gs_written = 0;

static void gs_seed(const char *name, const uint8_t *bytes, size_t len) {
    if (len == 0) {
        fprintf(stderr, "fuzz_seeds: %s came out empty\n", name);
        exit(1);
    }

    char path[1024];
    snprintf(path, sizeof path, "%s/%s", gs_dir, name);

    FILE *f = fopen(path, "wb");
    if (f == nullptr) {
        fprintf(stderr, "fuzz_seeds: cannot write %s\n", path);
        exit(1);
    }
    fwrite(bytes, 1, len, f);
    fclose(f);
    gs_written++;
}

// The track the rest of this is about. A real one: ramps, gates, two surfaces,
// so the corpus exercises the parts of the format that have structure in them.
static void gs_build_track(gs_track *t) {
    gs_track_init(t, 40, 16, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= t->h; y++) {
        for (uint8_t x = 0; x <= t->w; x++) {
            gs_track_set_corner(t, x, y, x > 18 && x < 24 ? GS_INT(1) : 0);
        }
    }
    for (uint8_t x = 0; x < 8; x++) {
        gs_track_set_surface(t, x, 3, GS_SURF_DIRT);
    }
    gs_track_add_gate(t, GS_INT(6), GS_INT(8), 0, GS_INT(6));
    gs_track_add_gate(t, GS_INT(30), GS_INT(8), 0, GS_INT(6));
}

static void gs_seeds_proto(void) {
    static uint8_t b[GS_PROTO_MTU];
    static uint8_t payload[GS_CHUNK_BYTES];
    for (size_t i = 0; i < sizeof payload; i++) payload[i] = (uint8_t)(i * 31u);

    gs_seed("join",        b, gs_proto_join(b, sizeof b, "ada"));
    gs_seed("bye",         b, gs_proto_bye(b, sizeof b));
    gs_seed("ping",        b, gs_proto_ping(b, sizeof b, 0x01020304u));
    gs_seed("pong",        b, gs_proto_pong(b, sizeof b, 0x01020304u));
    gs_seed("full",        b, gs_proto_full(b, sizeof b, "no room"));
    gs_seed("session",     b, gs_proto_session(b, sizeof b, 0x0123456789abcdefull));
    gs_seed("want_track",  b, gs_proto_want_track(b, sizeof b, 0xfeedfaceull));
    gs_seed("withdraw",    b, gs_proto_withdraw(b, sizeof b, 0xfeedfaceull));
    gs_seed("want_list",   b, gs_proto_want_list(b, sizeof b));
    gs_seed("publish",     b, gs_proto_publish(b, sizeof b, 0xfeedfaceull, "the oval"));
    gs_seed("want_best",   b, gs_proto_want_best(b, sizeof b, 0xfeedfaceull, 7, 3));
    gs_seed("start",       b, gs_proto_start(b, sizeof b, 0xfeedfaceull, 4, 3, 1));
    gs_seed("listing",     b, gs_proto_listing(b, sizeof b, 2, 9, 0xfeedfaceull,
                                               "head on", "ada"));
    gs_seed("best",        b, gs_proto_best(b, sizeof b, 0xfeedfaceull, 7, 3,
                                            1234, "ada", 5678, "bez"));
    gs_seed("result",      b, gs_proto_result(b, sizeof b, 0xfeedfaceull, 7, 3, 1,
                                              1234, 5678, 0xdeadbeefull));
    gs_seed("track_chunk", b, gs_proto_track_chunk(b, sizeof b, 0xfeedfaceull,
                                                   1, 4, payload,
                                                   (uint16_t)GS_CHUNK_BYTES));
    gs_seed("track_chunk_last",
            b, gs_proto_track_chunk(b, sizeof b, 0xfeedfaceull, 3, 4, payload, 17));
    gs_seed("proof_chunk", b, gs_proto_proof_chunk(b, sizeof b, 0xfeedfaceull,
                                                   0, 60, payload,
                                                   (uint16_t)GS_CHUNK_BYTES));
    gs_seed("relay",       b, gs_proto_relay(b, sizeof b, payload, 64));
    gs_seed("forward",     b, gs_proto_forward(b, sizeof b, 2, payload, 64));

    // A full lobby, which is the widest of these and the one with an array in
    // it. A half-full one too, because "count" and "capacity" disagreeing is
    // where a reader gets a loop bound from the wrong field.
    static gs_lobby l;
    l.capacity = GS_PROTO_MAX_PLAYERS;
    l.count = GS_PROTO_MAX_PLAYERS;
    for (uint8_t i = 0; i < GS_PROTO_MAX_PLAYERS; i++) {
        snprintf(l.player[i].name, sizeof l.player[i].name, "driver%u", i);
        snprintf(l.player[i].addr, sizeof l.player[i].addr, "10.0.0.%u", i + 2u);
        l.player[i].port = (uint16_t)(47800u + i);
        l.player[i].slot = i;
        l.player[i].present = true;
        l.player[i].ready = (i % 2) == 0;
    }
    gs_seed("lobby_full", b, gs_proto_lobby(b, sizeof b, &l));
    gs_seed("welcome",    b, gs_proto_welcome(b, sizeof b, 1, &l));

    l.count = 1;
    for (uint8_t i = 1; i < GS_PROTO_MAX_PLAYERS; i++) l.player[i].present = false;
    gs_seed("lobby_one",  b, gs_proto_lobby(b, sizeof b, &l));
}

static void gs_seeds_track(void) {
    static gs_track t;
    static uint8_t bytes[GS_CARRIER_MAX_BYTES];

    gs_build_track(&t);
    gs_seed("track_real", bytes, gs_track_serialize(&t, bytes, sizeof bytes));

    // The smallest track the format allows, so the corpus has both ends of the
    // dimension fields rather than one point in the middle.
    gs_track_init(&t, 1, 1, GS_SURF_DIRT);
    gs_seed("track_tiny", bytes, gs_track_serialize(&t, bytes, sizeof bytes));

    gs_track_init(&t, GS_TRACK_MAX, GS_TRACK_MAX, GS_SURF_ICE);
    for (uint8_t y = 0; y <= t.h; y++) {
        for (uint8_t x = 0; x <= t.w; x++) {
            gs_track_set_corner(&t, x, y, (gs_fix)(GS_INT(1) * ((x ^ y) & 3)));
        }
    }
    for (int i = 0; i < GS_TRACK_MAX_GATES; i++) {
        gs_track_add_gate(&t, GS_INT(2 + i), GS_INT(4), 0, GS_INT(3));
    }
    gs_seed("track_biggest", bytes, gs_track_serialize(&t, bytes, sizeof bytes));
}

static void gs_seeds_replay(void) {
    static gs_track t;
    static gs_replay r;
    static uint8_t bytes[GS_CARRIER_MAX_BYTES];

    gs_build_track(&t);

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_set_mode(&w, GS_MODE_RACE);
    gs_world_set_laps(&w, 3);
    for (uint8_t i = 0; i < GS_MAX_CARS; i++) {
        gs_world_add_car(&w, &t, (uint8_t)GS_VEH_SPRINT_CAR,
                         GS_INT(4), GS_INT(5) + GS_INT(2) * i, 0);
    }

    gs_replay_begin(&r, &w, &t);
    gs_replay_set_driver(&r, 0, "ada");
    gs_replay_set_driver(&r, 1, "bez");
    for (uint32_t i = 0; i < 900; i++) {
        gs_input in[GS_MAX_CARS];
        for (uint8_t c = 0; c < GS_MAX_CARS; c++) {
            in[c] = (gs_input)(GS_IN_ACCEL |
                               ((i / (17u + c * 5u)) % 3u == 0u ? GS_IN_LEFT : 0u));
        }
        gs_replay_record(&r, in);
    }
    gs_seed("replay_race", bytes, gs_replay_serialize(&r, bytes, sizeof bytes));

    // And one carrying an agreed ending, which is the version five field.
    gs_replay_set_agreed(&r, 0x0f1e2d3c4b5a6978ull);
    gs_seed("replay_agreed", bytes, gs_replay_serialize(&r, bytes, sizeof bytes));

    // An empty one: no ticks at all, which is the boundary the tick loop turns
    // on and the shape a truncating attacker reaches first.
    gs_replay_begin(&r, &w, &t);
    gs_seed("replay_empty", bytes, gs_replay_serialize(&r, bytes, sizeof bytes));
}

// The carrier's fuzzer reads a sequence of length-prefixed datagrams and takes
// the hash it should expect from the first one, the way the server takes it from
// the claim. A seed is therefore a whole transfer rather than one packet.
static void gs_seeds_carrier(void) {
    static gs_track t;
    static uint8_t track_bytes[GS_CARRIER_MAX_BYTES];
    static uint8_t out[GS_CARRIER_MAX_BYTES * 2];

    gs_build_track(&t);
    size_t len = gs_track_serialize(&t, track_bytes, sizeof track_bytes);
    uint64_t hash = gs_track_hash(&t);
    uint16_t chunks = gs_carrier_chunks(len);

    for (int order = 0; order < 2; order++) {
        size_t at = 0;
        for (uint16_t k = 0; k < chunks; k++) {
            // Backwards as well as forwards: the last chunk arriving first is
            // what sets `len` before the middle exists, and it is a real
            // ordering on a real network.
            uint16_t c = order == 0 ? k : (uint16_t)(chunks - 1u - k);

            uint8_t dg[GS_PROTO_MTU];
            size_t n = gs_carrier_chunk(dg, sizeof dg, hash, track_bytes, len, c);
            if (n == 0 || at + 2 + n > sizeof out) break;

            out[at++] = (uint8_t)(n & 0xffu);
            out[at++] = (uint8_t)((n >> 8) & 0xffu);
            memcpy(out + at, dg, n);
            at += n;
        }
        gs_seed(order == 0 ? "transfer_in_order" : "transfer_backwards", out, at);
    }
}

// **A dictionary, so the fuzzer can find the front door.**
//
// Every one of these formats begins with a four-byte magic word and refuses
// anything else on the first comparison. Mutation reaches four specific bytes
// by luck alone, so without this the fuzzer spends most of its budget being
// turned away at the door - which shows up as a campaign that runs twenty
// million inputs and explores almost nothing.
//
// Written from the constants rather than typed out, so it cannot drift out of
// step with them the way a committed dictionary would.
static void gs_write_dict(const char *corpus) {
    char path[1024];
    snprintf(path, sizeof path, "%s.dict", gs_dir);

    FILE *f = fopen(path, "wb");
    if (f == nullptr) {
        fprintf(stderr, "fuzz_seeds: cannot write %s\n", path);
        exit(1);
    }

    // The magic words, little-endian on the wire.
    uint32_t magics[] = { GS_PROTO_MAGIC, GS_REPLAY_MAGIC, GS_TRACK_MAGIC };
    const char *names[] = { "proto_magic", "replay_magic", "track_magic" };
    for (size_t i = 0; i < sizeof magics / sizeof magics[0]; i++) {
        fprintf(f, "%s=\"", names[i]);
        for (int k = 0; k < 4; k++) {
            fprintf(f, "\\x%02x", (unsigned)((magics[i] >> (8 * k)) & 0xffu));
        }
        fprintf(f, "\"\n");
    }

    // Every message type, because the byte after the header decides which
    // reader's arithmetic runs.
    for (unsigned m = 0; m < (unsigned)GS_MSG_COUNT; m++) {
        fprintf(f, "msg_%u=\"\\x%02x\"\n", m, m);
    }

    // Lengths and counts that sit on a boundary somewhere.
    fprintf(f, "chunk_bytes=\"\\x%02x\\x%02x\"\n",
            (unsigned)(GS_CHUNK_BYTES & 0xffu),
            (unsigned)((GS_CHUNK_BYTES >> 8) & 0xffu));
    fprintf(f, "u16_max=\"\\xff\\xff\"\n");
    fprintf(f, "zero32=\"\\x00\\x00\\x00\\x00\"\n");

    fclose(f);
    printf("fuzz_seeds: dictionary for %s in %s.dict\n", corpus, gs_dir);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr,
                "usage: gearstick_fuzz_seeds <proto|track|replay|carrier> <dir>\n");
        return 2;
    }
    snprintf(gs_dir, sizeof gs_dir, "%s", argv[2]);

    if (strcmp(argv[1], "proto") == 0)        gs_seeds_proto();
    else if (strcmp(argv[1], "track") == 0)   gs_seeds_track();
    else if (strcmp(argv[1], "replay") == 0)  gs_seeds_replay();
    else if (strcmp(argv[1], "carrier") == 0) gs_seeds_carrier();
    else {
        fprintf(stderr, "fuzz_seeds: no such corpus '%s'\n", argv[1]);
        return 2;
    }

    gs_write_dict(argv[1]);
    printf("fuzz_seeds: %d seeds for %s in %s\n", gs_written, argv[1], gs_dir);
    return 0;
}

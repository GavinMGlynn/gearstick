// test_server.c - the lobby, over real sockets.
//
// The server is a separate process in real use, and here it is a thread: the
// same code, the same socket, driven by real clients on the loopback. What is
// being checked is the thing the plan asks for - four clients connect and
// appear by name, and one leaving is noticed - plus the two answers a lobby has
// to get right that are easy to get wrong: a fifth client is refused *with a
// reason*, and somebody who says hello twice does not occupy two slots.
#include "net/gs_proto.h"
#include "core/gs_ai.h"
#include "core/gs_records.h"
#include "core/gs_net.h"
#include "core/gs_replay.h"
#include "net/gs_noise.h"
#include "net/gs_verify.h"
#include "core/gs_track.h"
#include "net/gs_carrier.h"
#include "platform/gs_wire.h"

#include <SDL3/SDL.h>
#include <SDL3_net/SDL_net.h>

#include <stdio.h>

static int gs_failures = 0;
static const char *gs_current = "";

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("  FAIL %s\n    %s:%d: %s\n", gs_current, __FILE__,         \
                   __LINE__, #cond);                                           \
            gs_failures++;                                                     \
        }                                                                      \
    } while (0)

#define TEST(name)                                                             \
    static void name(void);                                                    \
    static void run_##name(void) { gs_current = #name; name(); }               \
    static void name(void)

// --- a client, as a test drives one ----------------------------------------

// **The test server's identity, fixed.** A real server mints one and keeps it;
// a test has to know it in advance, because IK means the client must already
// hold the server's public key before it can say anything at all. Handed to the
// server with --key, and the matching public key handed to every client.
#define GS_TEST_KEY_HEX \
    "4a3acbfdb163dec651dfa3194dece676d437029c62a408b4c5ea9114246e4893"
#define GS_TEST_PROLOGUE "gearstick/1"

static const uint8_t *gs_test_server_pub(void) {
    static gs_noise_keypair k;
    static bool built = false;
    if (!built) {
        uint8_t secret[GS_NOISE_KEY_BYTES];
        for (int i = 0; i < GS_NOISE_KEY_BYTES; i++) {
            unsigned byte = 0;
            for (int half = 0; half < 2; half++) {
                char ch = GS_TEST_KEY_HEX[i * 2 + half];
                unsigned digit = (ch >= '0' && ch <= '9') ? (unsigned)(ch - '0')
                                                          : (unsigned)(ch - 'a' + 10);
                byte = byte * 16u + digit;
            }
            secret[i] = (uint8_t)byte;
        }
        gs_noise_key_from_secret(&k, secret);
        built = true;
    }
    return k.pub;
}

typedef struct gs_test_client {
    NET_DatagramSocket *sock;
    NET_Address        *server;
    uint16_t            port;

    bool     welcomed;
    uint8_t  slot;
    gs_lobby lobby;

    bool refused;
    char why[64];
    bool pinged;              // the server asked how far away we are
    int  lobbies;             // how many rosters have arrived, ever

    uint64_t session;         // the one-shot token the server issued
    bool     heard_best;
    uint32_t best_lap, best_race;

    // **This client speaks the protocol by hand, so it has to speak the tunnel
    // by hand too.** Nothing reaches the server in the clear any more, which is
    // the point of the tunnel and is also the reason a test that forges a
    // datagram has to seal it first.
    gs_noise_keypair   identity;
    gs_noise_handshake hs;
    gs_noise_session   tunnel;
} gs_test_client;

static void gs_client_pump(gs_test_client *c);

static bool gs_client_open(gs_test_client *c, uint16_t server_port) {
    SDL_zerop(c);
    c->sock = NET_CreateDatagramSocket(nullptr, 0, 0);
    if (c->sock == nullptr) return false;

    c->server = NET_ResolveHostname("127.0.0.1");
    if (c->server == nullptr ||
        NET_WaitUntilResolved(c->server, 3000) != NET_SUCCESS) {
        return false;
    }
    c->port = server_port;

    // **The tunnel, before anything else.** Nothing reaches the server in the
    // clear, so a test client that skipped this would send into silence and the
    // test would fail as a timeout rather than as what it is.
    gs_noise_keygen(&c->identity);
    gs_noise_init_initiator(&c->hs, &c->identity, gs_test_server_pub(),
                            (const uint8_t *)GS_TEST_PROLOGUE,
                            sizeof GS_TEST_PROLOGUE - 1);

    uint8_t msg[GS_PROTO_MTU], out[GS_PROTO_MTU];
    size_t n = gs_noise_write_message(&c->hs, nullptr, 0, msg, sizeof msg);
    if (n == 0) return false;
    n = gs_proto_handshake(out, sizeof out, msg, n);
    if (n == 0) return false;

    // Repeated, because the first datagram of a handshake can be lost like any
    // other and a test that gave up after one would fail on a bad afternoon.
    uint64_t until = SDL_GetTicks() + 5000;
    while (SDL_GetTicks() < until && !c->tunnel.established) {
        NET_SendDatagram(c->sock, c->server, c->port, out, (int)n);
        for (int i = 0; i < 20 && !c->tunnel.established; i++) {
            gs_client_pump(c);
            SDL_Delay(10);
        }
    }
    return c->tunnel.established;
}

static void gs_client_close(gs_test_client *c) {
    if (c->server != nullptr) NET_UnrefAddress(c->server);
    if (c->sock != nullptr) NET_DestroyDatagramSocket(c->sock);
    SDL_zerop(c);
}

static void gs_client_send(gs_test_client *c, const uint8_t *buf, size_t n);

static void gs_client_send(gs_test_client *c, const uint8_t *buf, size_t n) {
    if (n == 0 || !c->tunnel.established) return;

    uint8_t sealed[GS_PROTO_MTU], out[GS_PROTO_MTU];
    size_t k = gs_noise_seal(&c->tunnel, buf, n, sealed, sizeof sealed);
    if (k == 0) return;
    k = gs_proto_sealed(out, sizeof out, sealed, k);
    if (k == 0) return;

    NET_SendDatagram(c->sock, c->server, c->port, out, (int)k);
}

static void gs_client_say_hello(gs_test_client *c, const char *name) {
    uint8_t buf[GS_PROTO_MTU];
    gs_client_send(c, buf, gs_proto_join(buf, sizeof buf, name));
}

// Everything that has arrived. The lobby is kept, because a later broadcast
// replaces an earlier one and the last word is the current truth.
static void gs_client_pump(gs_test_client *c) {
    NET_Datagram *d = nullptr;
    while (NET_ReceiveDatagram(c->sock, &d) && d != nullptr) {
        size_t raw = (size_t)d->buflen;

        // The handshake is the only thing that arrives in the clear, because it
        // is what makes the rest sealed.
        if (gs_proto_kind(d->buf, raw) == GS_MSG_HANDSHAKE) {
            const uint8_t *msg = nullptr;
            size_t msg_len = 0;
            uint8_t payload[GS_PROTO_MTU];
            size_t got = 0;
            if (!c->tunnel.established &&
                gs_proto_read_handshake(d->buf, raw, &msg, &msg_len) &&
                gs_noise_read_message(&c->hs, msg, msg_len, payload,
                                      sizeof payload, &got) &&
                gs_noise_done(&c->hs)) {
                gs_noise_split(&c->hs, &c->tunnel);
            }
            NET_DestroyDatagram(d);
            continue;
        }

        static uint8_t plain[GS_PROTO_MTU];
        const uint8_t *body = nullptr;
        size_t body_len = 0, len = 0;
        if (!c->tunnel.established ||
            !gs_proto_read_sealed(d->buf, raw, &body, &body_len) ||
            !gs_noise_open(&c->tunnel, body, body_len, plain, sizeof plain, &len)) {
            NET_DestroyDatagram(d);
            continue;
        }

        switch (gs_proto_kind(plain, len)) {
        case GS_MSG_WELCOME:
            if (gs_proto_read_welcome(plain, len, &c->slot, &c->lobby)) {
                c->welcomed = true;
            }
            break;
        case GS_MSG_LOBBY:
            gs_proto_read_lobby(plain, len, &c->lobby);
            c->lobbies++;
            break;
        case GS_MSG_SESSION:
            gs_proto_read_session(plain, len, &c->session);
            break;
        case GS_MSG_BEST: {
            uint64_t track = 0, cond = 0;
            uint16_t laps = 0;
            char lap_who[GS_PROTO_NAME], race_who[GS_PROTO_NAME];
            if (gs_proto_read_best(plain, len, &track, &cond, &laps,
                                   &c->best_lap, lap_who, sizeof lap_who,
                                   &c->best_race, race_who, sizeof race_who)) {
                c->heard_best = true;
            }
            break;
        }
        case GS_MSG_FULL:
            c->refused = gs_proto_read_full(plain, len, c->why, sizeof c->why);
            break;
        case GS_MSG_PING: {
            // A real client answers, which is how the server measures the trip.
            // A test client that did not would make the server's ping column a
            // column of dashes, and the test would not notice.
            uint32_t stamp = 0;
            if (gs_proto_read_stamp(plain, len, &stamp)) {
                uint8_t out[GS_PROTO_MTU];
                gs_client_send(c, out, gs_proto_pong(out, sizeof out, stamp));
                c->pinged = true;
            }
            break;
        }
        default:
            break;
        }
        NET_DestroyDatagram(d);
        d = nullptr;
    }
}

// The server is a process in real life and a process here too. Started with a
// deadline so a broken test cannot leave one running.
static SDL_Process *gs_server = nullptr;

// A track for the server to hand out, and a store to remember in, when a test
// wants them.
static const char *gs_track_arg = nullptr;
static const char *gs_store_arg = nullptr;

// Two drivers on different rhythms, so no two players ever change their minds
// together and a session that mixed them up would be caught.
static gs_input gs_drive(uint8_t player, uint32_t tick) {
    uint32_t turn = 19u + (uint32_t)player * 13u;
    gs_input in = GS_IN_ACCEL;
    if ((tick / turn) % 2u == 0u) {
        in = (gs_input)(in | ((player & 1u) ? GS_IN_RIGHT : GS_IN_LEFT));
    }
    if ((tick / (47u + (uint32_t)player * 11u)) % 4u == 0u) {
        in = (gs_input)(in | GS_IN_BRAKE);
    }
    return in;
}

// **How long a test server lives before giving up on its own.**
//
// A safety net against a server left running by a test that crashed, not a
// budget the test is racing. Every one of these tests stops its server
// explicitly, so this number should be past anything a test could plausibly
// need - and when it was not, a slow machine outlived its server and the test
// failed for that rather than for the thing it is about. ctest's own timeout is
// the real bound.
#define GS_SERVER_LIFETIME "120"

static bool gs_server_start_full(const char *port, const char *seconds,
                                 const char *players, const char *timeout) {
    // Next to this binary, not next to whatever directory somebody happened to
    // run the test from. ctest runs it from the build directory and a person
    // runs it from wherever they are, and only one of those finds "./".
    static char exe[1024];
    const char *base = SDL_GetBasePath();
    SDL_snprintf(exe, sizeof exe, "%sgearstick_server%s",
                 base != nullptr ? base : "./",
#ifdef _WIN32
                 ".exe");
#else
                 "");
#endif

    const char *args[24];
    int n = 0;
    args[n++] = exe;
    args[n++] = "--port";     args[n++] = port;
    args[n++] = "--plain";
    args[n++] = "--seconds";  args[n++] = seconds;
    args[n++] = "--players";  args[n++] = players;
    args[n++] = "--timeout";  args[n++] = timeout;
    if (gs_track_arg != nullptr) {
        args[n++] = "--track"; args[n++] = gs_track_arg;
    }
    args[n++] = "--store";
    args[n++] = gs_store_arg != nullptr ? gs_store_arg : ":memory:";
    args[n++] = "--key";
    args[n++] = GS_TEST_KEY_HEX;
    args[n] = nullptr;

    // Its output goes nowhere. The server redraws a live view four times a
    // second, and interleaving that with the test's own output makes a failure
    // impossible to read.
    SDL_PropertiesID props = SDL_CreateProperties();

    // SDL takes the argument vector as a plain pointer, so the const on the
    // strings has to be cast off to hand it over. MSVC treats the silent version
    // of that as a warning and the warning is an error, which is the right way
    // round - the cast should be visible.
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER,
                           (void *)(uintptr_t)args);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
                          SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER,
                          SDL_PROCESS_STDIO_NULL);

    gs_server = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);

    if (gs_server == nullptr) {
        printf("  could not start the server: %s\n", SDL_GetError());
        return false;
    }
    SDL_Delay(400);        // let it bind before anybody knocks
    return true;
}

static bool gs_server_start_for(const char *port, const char *seconds,
                                const char *players) {
    return gs_server_start_full(port, seconds, players, "15000");
}

static bool gs_server_start_with_store(const char *port, const char *seconds,
                                       const char *players, const char *store) {
    gs_store_arg = store;
    bool ok = gs_server_start_full(port, seconds, players, "15000");
    gs_store_arg = nullptr;
    return ok;
}

static bool gs_server_start_verifying(const char *port, const char *seconds,
                                      const char *players, const char *track,
                                      const char *store) {
    gs_track_arg = track;
    gs_store_arg = store;
    bool ok = gs_server_start_full(port, seconds, players, "15000");
    gs_track_arg = nullptr;
    gs_store_arg = nullptr;
    return ok;
}

static bool gs_server_start_with_track(const char *port, const char *seconds,
                                       const char *players, const char *track) {
    gs_track_arg = track;
    bool ok = gs_server_start_full(port, seconds, players, "15000");
    gs_track_arg = nullptr;
    return ok;
}

static bool gs_server_start(const char *port, const char *seconds) {
    return gs_server_start_for(port, seconds, "4");
}

static void gs_server_stop(void) {
    if (gs_server == nullptr) return;
    SDL_KillProcess(gs_server, true);
    SDL_WaitProcess(gs_server, true, nullptr);
    SDL_DestroyProcess(gs_server);
    gs_server = nullptr;
}

// Knock until answered, or give up. A datagram is allowed to vanish, so a test
// that said hello once and concluded the server was broken would be a test that
// fails on a bad afternoon.
static bool gs_client_join(gs_test_client *c, const char *name, int tries) {
    for (int i = 0; i < tries; i++) {
        gs_client_say_hello(c, name);
        for (int k = 0; k < 20; k++) {
            gs_client_pump(c);
            if (c->welcomed || c->refused) return true;
            SDL_Delay(10);
        }
    }
    return false;
}

TEST(four_clients_connect_and_the_server_shows_all_of_them) {
    if (!gs_server_start("47810", GS_SERVER_LIFETIME)) { gs_failures++; return; }

    static gs_test_client c[4];
    static const char *names[4] = { "ada", "bez", "cy", "dot" };

    for (int i = 0; i < 4; i++) {
        CHECK(gs_client_open(&c[i], 47810));
        CHECK(gs_client_join(&c[i], names[i], 5));
        CHECK(c[i].welcomed);
        CHECK(!c[i].refused);
    }

    // Four different slots, handed out by the server rather than chosen.
    bool seen[GS_PROTO_MAX_PLAYERS] = { false, false, false, false };
    for (int i = 0; i < 4; i++) {
        CHECK(c[i].slot < GS_PROTO_MAX_PLAYERS);
        if (c[i].slot < GS_PROTO_MAX_PLAYERS) {
            CHECK(!seen[c[i].slot]);
            seen[c[i].slot] = true;
        }
    }

    // Everybody ends up knowing about everybody, which is a broadcast working
    // rather than each client being told only about itself.
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 30; k++) {
            gs_client_pump(&c[i]);
            if (c[i].lobby.count == 4) break;
            SDL_Delay(20);
        }
        CHECK(c[i].lobby.count == 4);
    }

    // And by name, which is the part the plan actually asks for.
    for (int i = 0; i < 4; i++) {
        bool found[4] = { false, false, false, false };
        for (int p = 0; p < GS_PROTO_MAX_PLAYERS; p++) {
            if (!c[i].lobby.player[p].present) continue;
            for (int n = 0; n < 4; n++) {
                if (SDL_strcmp(c[i].lobby.player[p].name, names[n]) == 0) {
                    found[n] = true;
                }
            }
        }
        for (int n = 0; n < 4; n++) CHECK(found[n]);
    }

    // A fifth is refused, and told why in words somebody could show a user.
    static gs_test_client fifth;
    CHECK(gs_client_open(&fifth, 47810));
    CHECK(gs_client_join(&fifth, "eve", 5));
    CHECK(fifth.refused);
    CHECK(!fifth.welcomed);
    CHECK(SDL_strstr(fifth.why, "full") != nullptr);

    // One leaves, and the others are told within a second.
    uint8_t leaving = c[3].slot;
    uint8_t buf[GS_PROTO_MTU];
    gs_client_send(&c[3], buf, gs_proto_bye(buf, sizeof buf));

    bool noticed = false;
    for (int k = 0; k < 50 && !noticed; k++) {
        SDL_Delay(20);
        gs_client_pump(&c[0]);
        noticed = c[0].lobby.count == 3 && !c[0].lobby.player[leaving].present;
    }
    CHECK(noticed);

    // The server asks how far away everybody is, and a client that answers is
    // measured. A "ping" column that never fills is a column of dashes
    // pretending to be a statistic.
    bool asked = false;
    for (int k = 0; k < 150 && !asked; k++) {
        SDL_Delay(20);
        for (int i = 0; i < 3; i++) gs_client_pump(&c[i]);
        asked = c[0].pinged;
    }
    CHECK(asked);

    gs_client_close(&fifth);
    for (int i = 0; i < 4; i++) gs_client_close(&c[i]);
    gs_server_stop();
}

TEST(saying_hello_twice_is_still_one_player) {
    // **Not on a full server.** The obvious place to check this is at the end
    // of the four-player test, and there it proves nothing: with every slot
    // taken, a server that duplicated a rejoin and a server that refused one
    // both leave the count at four. With room to spare the two answers differ,
    // which is the whole point of the check.
    if (!gs_server_start("47814", GS_SERVER_LIFETIME)) { gs_failures++; return; }

    static gs_test_client a, b;
    CHECK(gs_client_open(&a, 47814));
    CHECK(gs_client_open(&b, 47814));
    CHECK(gs_client_join(&a, "ada", 5));
    CHECK(gs_client_join(&b, "bez", 5));

    for (int k = 0; k < 30; k++) {
        gs_client_pump(&a);
        if (a.lobby.count == 2) break;
        SDL_Delay(20);
    }
    CHECK(a.lobby.count == 2);

    uint8_t was = a.slot;

    // A lost welcome makes a client ask again. It must get the same slot back,
    // not a second one - and it must not be turned away either.
    a.welcomed = false;
    a.refused = false;
    CHECK(gs_client_join(&a, "ada", 5));
    CHECK(a.welcomed);
    CHECK(!a.refused);
    CHECK(a.slot == was);

    for (int k = 0; k < 30; k++) {
        gs_client_pump(&a);
        gs_client_pump(&b);
        SDL_Delay(20);
    }
    CHECK(a.lobby.count == 2);
    CHECK(b.lobby.count == 2);

    gs_client_close(&a);
    gs_client_close(&b);
    gs_server_stop();
}

TEST(a_server_told_to_hold_two_holds_two) {
    // The capacity is a number the server is given, and the only way to know it
    // is used is to give it one that is not the maximum. With four allowed and
    // four asked for, an implementation that ignored the setting entirely would
    // behave identically.
    if (!gs_server_start_for("47816", GS_SERVER_LIFETIME, "2")) { gs_failures++; return; }

    static gs_test_client c[3];
    for (int i = 0; i < 3; i++) CHECK(gs_client_open(&c[i], 47816));

    CHECK(gs_client_join(&c[0], "ada", 5));
    CHECK(c[0].welcomed);
    CHECK(gs_client_join(&c[1], "bez", 5));
    CHECK(c[1].welcomed);

    CHECK(gs_client_join(&c[2], "cy", 5));
    CHECK(c[2].refused);
    CHECK(!c[2].welcomed);
    CHECK(SDL_strstr(c[2].why, "full") != nullptr);

    // And it says how full, so the message can be shown to a person.
    CHECK(SDL_strstr(c[2].why, "2") != nullptr);

    for (int i = 0; i < 3; i++) gs_client_close(&c[i]);
    gs_server_stop();
}

TEST(the_server_ignores_datagrams_that_are_not_ours) {
    if (!gs_server_start("47812", GS_SERVER_LIFETIME)) { gs_failures++; return; }

    static gs_test_client c;
    CHECK(gs_client_open(&c, 47812));

    // Rubbish, and a message with the right shape but the wrong version. A
    // server that answered either would be a server anybody can talk to.
    uint8_t junk[32];
    SDL_memset(junk, 0xa5, sizeof junk);
    gs_client_send(&c, junk, sizeof junk);

    uint8_t buf[GS_PROTO_MTU];
    size_t n = gs_proto_join(buf, sizeof buf, "ada");
    buf[4] = 99;                       // a version from the future
    gs_client_send(&c, buf, n);

    SDL_Delay(250);
    gs_client_pump(&c);
    CHECK(!c.welcomed);
    CHECK(!c.refused);

    // And it is still working afterwards, rather than having fallen over.
    CHECK(gs_client_join(&c, "ada", 5));
    CHECK(c.welcomed);

    gs_client_close(&c);
    gs_server_stop();
}

// --- the game's own client, not the test's --------------------------------
//
// Everything above drives a client written for the test. This drives the one
// the game actually uses, which is the only way to know that the thing shipped
// to players talks to the thing shipped to servers.

TEST(the_games_own_client_gets_its_slot_from_the_server) {
    if (!gs_server_start("47818", GS_SERVER_LIFETIME)) { gs_failures++; return; }
    CHECK(gs_wire_init());

    static gs_wire *w[4];
    static const char *names[4] = { "ada", "bez", "cy", "dot" };

    for (int i = 0; i < 4; i++) {
        w[i] = gs_wire_server("127.0.0.1", 47818, names[i], gs_test_server_pub());
        CHECK(w[i] != nullptr);
        CHECK(gs_wire_error(w[i]) == nullptr);
    }

    // Everybody polls until the server has placed all four.
    bool all = false;
    for (int k = 0; k < 400 && !all; k++) {
        all = true;
        for (int i = 0; i < 4; i++) {
            gs_wire_poll(w[i]);
            if (!gs_wire_ready(w[i])) all = false;
        }
        SDL_Delay(10);
    }
    CHECK(all);

    // **Four different slots, decided by the server.** Not by who started
    // first, which is the whole difference between a lobby and a host.
    bool seen[GS_PROTO_MAX_PLAYERS] = { false, false, false, false };
    for (int i = 0; i < 4; i++) {
        uint8_t slot = gs_wire_local(w[i]);
        CHECK(slot < GS_PROTO_MAX_PLAYERS);
        if (slot < GS_PROTO_MAX_PLAYERS) {
            CHECK(!seen[slot]);
            seen[slot] = true;
        }
        CHECK(gs_wire_players(w[i]) == 4);
    }

    // And each of them can see who else is here, by name.
    for (int i = 0; i < 4; i++) {
        const gs_lobby *l = gs_wire_lobby(w[i]);
        CHECK(l != nullptr);
        if (l == nullptr) continue;
        CHECK(l->count == 4);

        for (int n = 0; n < 4; n++) {
            bool found = false;
            for (int p = 0; p < GS_PROTO_MAX_PLAYERS; p++) {
                if (l->player[p].present &&
                    SDL_strcmp(l->player[p].name, names[n]) == 0) {
                    found = true;
                }
            }
            CHECK(found);
        }
    }

    // A fifth is turned away, and can say why in words meant for a person.
    gs_wire *fifth = gs_wire_server("127.0.0.1", 47818, "eve", gs_test_server_pub());
    CHECK(fifth != nullptr);
    for (int k = 0; k < 200 && !gs_wire_refused(fifth); k++) {
        gs_wire_poll(fifth);
        SDL_Delay(10);
    }
    CHECK(gs_wire_refused(fifth));
    CHECK(!gs_wire_ready(fifth));
    CHECK(gs_wire_refusal(fifth) != nullptr);
    if (gs_wire_refusal(fifth) != nullptr) {
        CHECK(SDL_strstr(gs_wire_refusal(fifth), "full") != nullptr);
    }

    gs_wire_close(fifth);
    for (int i = 0; i < 4; i++) gs_wire_close(w[i]);
    gs_wire_quit();
    gs_server_stop();
}

TEST(a_server_nobody_has_set_up_still_has_a_library_to_offer) {
    // **A fresh server is not an empty one.** The stock tracks are built into a
    // database that ships beside the binary and is copied into place the first
    // time a server runs, so the first person to connect finds something to race
    // rather than an empty list and no reason to stay.
    //
    // A store path that does not exist, deliberately: this is the first run.
    remove("brand-new.db");
    remove("brand-new.db-wal");
    remove("brand-new.db-shm");

    if (!gs_server_start_with_store("47840", GS_SERVER_LIFETIME, "1",
                                    "brand-new.db")) {
        gs_failures++;
        return;
    }
    CHECK(gs_wire_init());

    gs_wire *w = gs_wire_server("127.0.0.1", 47840, "ada", gs_test_server_pub());
    CHECK(w != nullptr);

    uint64_t until = SDL_GetTicks() + 8000;
    while (SDL_GetTicks() < until && !gs_wire_ready(w)) {
        gs_wire_poll(w);
        SDL_Delay(10);
    }

    // Ready at all means it was sent a track, which a server with nothing to
    // serve could not have done.
    CHECK(gs_wire_ready(w));

    gs_wire_ask_published(w);

    const gs_wire_listing *rows = nullptr;
    uint16_t total = 0, got = 0;
    until = SDL_GetTicks() + 8000;
    while (SDL_GetTicks() < until && got < 12) {
        gs_wire_poll(w);
        uint8_t drain[GS_WIRE_MTU];
        while (gs_wire_recv(w, drain, sizeof drain) > 0) { }
        got = gs_wire_published(w, &rows, &total);
        SDL_Delay(10);
    }

    // The dozen a fresh install is promised, by name.
    CHECK(total >= 12);
    CHECK(got >= 12);
    if (rows != nullptr && got > 0) CHECK(rows[0].name[0] != '\0');

    gs_wire_close(w);
    gs_wire_quit();
    gs_server_stop();

    remove("brand-new.db");
    remove("brand-new.db-wal");
    remove("brand-new.db-shm");
}

TEST(the_roster_is_sent_again_rather_than_only_when_it_changes) {
    // **One datagram over UDP is a promise of nothing.** The roster used to be
    // sent only on a change, so a client that missed the announcement raced
    // against somebody who had gone home and never found out - there was no next
    // announcement to correct it. It passed on Linux and failed on macOS, whose
    // default receive buffer is a fraction of the size, which is the same bug
    // wearing a platform's name.
    if (!gs_server_start("47836", GS_SERVER_LIFETIME)) { gs_failures++; return; }

    gs_test_client c;
    CHECK(gs_client_open(&c, 47836));
    gs_client_say_hello(&c, "ada");

    uint64_t until = SDL_GetTicks() + 5000;
    while (SDL_GetTicks() < until && !c.welcomed) {
        gs_client_pump(&c);
        SDL_Delay(10);
    }
    CHECK(c.welcomed);

    // Nothing changes from here: nobody joins, nobody leaves. Any roster that
    // arrives now is the server saying the same thing again, which is the point.
    int at_first = c.lobbies;
    until = SDL_GetTicks() + 7000;
    while (SDL_GetTicks() < until) {
        gs_client_pump(&c);
        SDL_Delay(10);
    }

    // Seven seconds against a two second ping is three chances; two is enough to
    // show it repeats without pinning the exact rate.
    CHECK(c.lobbies >= at_first + 2);
    CHECK(c.lobby.count == 1);

    gs_client_close(&c);
    gs_server_stop();
}

TEST(a_client_that_quits_says_so_instead_of_being_waited_out) {
    // A goodbye has been in the protocol since it was written and nothing ever
    // sent one, so quitting looked exactly like crashing: everybody else raced
    // a car that was not there until the silence timeout ran out. Fifteen
    // seconds is most of a race.
    //
    // A long patience here on purpose - if the departure were still being waited
    // out, this test would run out of time rather than quietly passing on the
    // timeout instead.
    if (!gs_server_start_full("47838", GS_SERVER_LIFETIME, "2", "30000")) {
        gs_failures++;
        return;
    }
    CHECK(gs_wire_init());

    gs_wire *a = gs_wire_server("127.0.0.1", 47838, "ada", gs_test_server_pub());
    gs_wire *b = gs_wire_server("127.0.0.1", 47838, "bez", gs_test_server_pub());
    CHECK(a != nullptr && b != nullptr);

    uint64_t until = SDL_GetTicks() + 8000;
    while (SDL_GetTicks() < until && !(gs_wire_ready(a) && gs_wire_ready(b))) {
        gs_wire_poll(a);
        gs_wire_poll(b);
        SDL_Delay(10);
    }
    CHECK(gs_wire_ready(a));
    CHECK(gs_wire_ready(b));

    gs_wire_close(b);

    // Five seconds, against a thirty second patience. Noticing inside that means
    // it was told, because being waited out could not possibly have happened
    // yet.
    bool noticed = false;
    until = SDL_GetTicks() + 5000;
    while (SDL_GetTicks() < until && !noticed) {
        gs_wire_poll(a);
        uint8_t drain[GS_WIRE_MTU];
        while (gs_wire_recv(a, drain, sizeof drain) > 0) { }

        const gs_lobby *now = gs_wire_lobby(a);
        noticed = now != nullptr && now->count == 1;
        SDL_Delay(10);
    }
    CHECK(noticed);

    gs_wire_close(a);
    gs_wire_quit();
    gs_server_stop();
}

TEST(a_placed_client_is_not_dropped_for_going_quiet) {
    // **The bug this exists for:** a client that stopped talking to the server
    // the moment it was placed. The race itself goes peer to peer, so nothing
    // else sends to the server once one starts - and the server drops whoever
    // it has not heard from, which is what stops a lobby filling with people
    // who closed their laptop. Together those two correct behaviours threw
    // players out mid-race.
    //
    // Two seconds of silence is enough to see it: the server pings every two
    // seconds and gives up after fifteen, so a client that answers nothing
    // shows as quiet long before it is dropped, and one that keeps in touch
    // never does.
    // A two second patience, so a connection that stops being maintained dies
    // inside the test rather than a quarter of a minute after it.
    //
    // **The loops below run on the clock, not on a count of SDL_Delay calls.**
    // A ten millisecond delay is at least ten milliseconds and on some machines
    // rather more; counting iterations made this test's wall time a property of
    // the host, and on a slow one it outlived the server it was talking to and
    // failed for that instead of for the thing it is about. The server's own
    // lifetime is well past everything the test needs for the same reason.
    if (!gs_server_start_full("47822", GS_SERVER_LIFETIME, "2", "2000")) { gs_failures++; return; }
    CHECK(gs_wire_init());

    gs_wire *a = gs_wire_server("127.0.0.1", 47822, "ada", gs_test_server_pub());
    gs_wire *b = gs_wire_server("127.0.0.1", 47822, "bez", gs_test_server_pub());
    CHECK(a != nullptr && b != nullptr);

    uint64_t until = SDL_GetTicks() + 8000;
    while (SDL_GetTicks() < until && !(gs_wire_ready(a) && gs_wire_ready(b))) {
        gs_wire_poll(a);
        gs_wire_poll(b);
        SDL_Delay(10);
    }
    CHECK(gs_wire_ready(a));
    CHECK(gs_wire_ready(b));

    // Six seconds of a race - three times the server's patience here - with
    // nothing but the connection maintaining itself.
    until = SDL_GetTicks() + 6000;
    while (SDL_GetTicks() < until) {
        uint8_t drain[GS_WIRE_MTU];
        gs_wire_poll(a);
        gs_wire_poll(b);
        while (gs_wire_recv(a, drain, sizeof drain) > 0) { }
        while (gs_wire_recv(b, drain, sizeof drain) > 0) { }
        SDL_Delay(10);
    }

    // Still here, still knowing about each other. A server that had dropped
    // them would have told the survivor the lobby had shrunk.
    const gs_lobby *l = gs_wire_lobby(a);
    CHECK(l != nullptr);
    if (l != nullptr) CHECK(l->count == 2);
    CHECK(gs_wire_ready(a));
    CHECK(!gs_wire_refused(a));

    // And still *listening*. Keeping a connection alive by talking is half of
    // it; a client that had stopped reading would sit on a roster from six
    // seconds ago and never notice the person it is racing had gone. Checked by
    // letting one of them actually go.
    gs_wire_close(b);
    b = nullptr;

    // Five times the server's two second patience: long enough that not
    // noticing means the roster never came, and not merely that it was slow.
    bool noticed = false;
    until = SDL_GetTicks() + 10000;
    while (SDL_GetTicks() < until && !noticed) {
        // As a racing client does: poll to keep the connection, and receive to
        // take everything that has arrived. The control traffic is handled on
        // the way past.
        gs_wire_poll(a);
        uint8_t drain[GS_WIRE_MTU];
        while (gs_wire_recv(a, drain, sizeof drain) > 0) { }

        const gs_lobby *now = gs_wire_lobby(a);
        noticed = now != nullptr && now->count == 1;
        SDL_Delay(10);
    }
    CHECK(noticed);

    gs_wire_close(a);
    gs_wire_quit();
    gs_server_stop();
}

TEST(a_client_waiting_for_others_is_not_ready_yet) {
    // A lobby of four with one person in it is not a race. A client that called
    // itself ready would start racing against three people who are not there.
    if (!gs_server_start("47820", GS_SERVER_LIFETIME)) { gs_failures++; return; }
    CHECK(gs_wire_init());

    gs_wire *lonely = gs_wire_server("127.0.0.1", 47820, "ada", gs_test_server_pub());
    CHECK(lonely != nullptr);

    const gs_lobby *l = nullptr;
    for (int k = 0; k < 200; k++) {
        gs_wire_poll(lonely);
        l = gs_wire_lobby(lonely);
        if (l != nullptr && l->count == 1) break;
        SDL_Delay(10);
    }

    CHECK(l != nullptr);
    if (l != nullptr) {
        CHECK(l->count == 1);        // seen, and placed
        CHECK(l->capacity == 4);
    }
    CHECK(gs_wire_local(lonely) == 0);
    CHECK(!gs_wire_ready(lonely));   // and not racing
    CHECK(!gs_wire_refused(lonely));

    gs_wire_close(lonely);
    gs_wire_quit();
    gs_server_stop();
}

TEST(a_client_with_a_different_track_is_given_the_right_one) {
    // **The verification this item exists for.** Two machines racing on tracks
    // they each believe are the same one is the single failure rollback cannot
    // absorb: every input would agree and every state would differ.
    CHECK(gs_wire_init());

    // A track nobody has by accident, written where the server can read it.
    static gs_track served;
    gs_track_init(&served, 44, 20, GS_SURF_DIRT);
    for (uint8_t y = 0; y <= served.h; y++) {
        for (uint8_t x = 0; x <= served.w; x++) {
            gs_fix h = (x > 10 && x < 16) ? (gs_fix)((int64_t)GS_INT(2) * (x - 10) / 6) : 0;
            gs_track_set_corner(&served, x, y, h);
        }
    }
    for (uint8_t x = 24; x < served.w; x++) {
        for (uint8_t y = 0; y < served.h; y++) {
            gs_track_set_surface(&served, x, y, GS_SURF_ICE);
        }
    }
    gs_track_add_gate(&served, GS_INT(4), GS_INT(10), 0, GS_INT(6));
    gs_track_add_gate(&served, GS_INT(38), GS_INT(10), 0, GS_INT(6));

    static uint8_t bytes[GS_CARRIER_MAX_BYTES];
    size_t len = gs_track_serialize(&served, bytes, sizeof bytes);
    CHECK(len > GS_CHUNK_BYTES);        // more than one chunk, or nothing is reassembled

    const char *path = "served.gstrack";
    SDL_IOStream *io = SDL_IOFromFile(path, "wb");
    CHECK(io != nullptr);
    if (io != nullptr) {
        SDL_WriteIO(io, bytes, len);
        SDL_CloseIO(io);
    }

    if (!gs_server_start_with_track("47824", GS_SERVER_LIFETIME, "1", path)) {
        gs_failures++;
        return;
    }

    gs_wire *w = gs_wire_server("127.0.0.1", 47824, "ada", gs_test_server_pub());
    CHECK(w != nullptr);

    // **Never ready while the ground is still arriving.** Waiting for the last
    // check to pass would prove nothing here: on the loopback the track lands a
    // few milliseconds after the roster, so a client that ignored the transfer
    // entirely would look identical by the time anybody looked. What is watched
    // instead is every intermediate state, and the rule that must hold in all
    // of them.
    bool raced_too_early = false;
    for (int k = 0; k < 600 && !gs_wire_ready(w); k++) {
        gs_wire_poll(w);
        if (gs_wire_ready(w) && gs_wire_track_progress(w) < 1.0f) {
            raced_too_early = true;
        }
        SDL_Delay(10);
    }
    CHECK(gs_wire_ready(w));
    CHECK(!raced_too_early);

    // The server named a track, all of it arrived, and it is that track.
    CHECK(gs_wire_track_hash(w) == gs_track_hash(&served));
    CHECK(gs_wire_track_progress(w) >= 1.0f);

    static gs_track got;
    CHECK(gs_wire_track(w, &got));
    CHECK(gs_track_hash(&got) == gs_track_hash(&served));
    CHECK(got.w == served.w && got.h == served.h);
    CHECK(got.gate_count == served.gate_count);

    // And it is not the track a client would have had otherwise. A demo track
    // that happened to match would make all of the above pass for free.
    static gs_track local;
    gs_track_init(&local, 40, 24, GS_SURF_PAVEMENT);
    CHECK(gs_track_hash(&local) != gs_track_hash(&served));

    gs_wire_close(w);
    gs_wire_quit();
    gs_server_stop();
}

// A secret per peer, fixed so a failing race can be re-run and get the same
// salts. A real client draws one at random; a test that did the same would be a
// test that fails differently every time.
static const uint8_t *gs_test_secret(uint8_t who) {
    static uint8_t s[GS_MAX_CARS][GS_NET_SECRET_BYTES];
    for (unsigned i = 0; i < GS_NET_SECRET_BYTES; i++) {
        s[who][i] = (uint8_t)((0x5au + who * 31u + i * 7u) & 0xffu);
    }
    return s[who];
}

TEST(two_clients_the_server_introduced_race_each_other_directly_and_sealed) {
    // **A server race that does not use the relay.** The server brokers the
    // meeting - it says who is here, where they are, and which key each of them
    // proved they hold - and then the two race straight at each other.
    //
    // This had never worked. The lobby's addresses were written down and never
    // resolved, and nothing marked a peer known, so `gs_wire_send` walked a
    // list of peers it considered unknown and sent to none of them: a server
    // race without `--relay` produced no traffic at all, silently. Every test
    // of a server race had asked for the relay, so nothing said so.
    CHECK(gs_wire_init());

    if (!gs_server_start_for("47852", GS_SERVER_LIFETIME, "2")) {
        gs_failures++;
        return;
    }

    gs_wire *a = gs_wire_server("127.0.0.1", 47852, "ada", gs_test_server_pub());
    gs_wire *b = gs_wire_server("127.0.0.1", 47852, "bez", gs_test_server_pub());
    CHECK(a != nullptr && b != nullptr);

    // Deliberately not relaying: the mesh is the thing under test.
    CHECK(!gs_wire_relaying(a));
    CHECK(!gs_wire_relaying(b));

    for (int k = 0; k < 600 && !(gs_wire_ready(a) && gs_wire_ready(b)); k++) {
        gs_wire_poll(a);
        gs_wire_poll(b);
        SDL_Delay(10);
    }
    CHECK(gs_wire_ready(a));
    CHECK(gs_wire_ready(b));

    static gs_track t;
    static gs_net n[2];
    gs_world start;

    gs_track_init(&t, 48, 20, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= t.h; y++) {
        for (uint8_t x = 0; x <= t.w; x++) {
            gs_track_set_corner(&t, x, y, x > 20 && x < 26 ? GS_INT(1) : 0);
        }
    }
    gs_world_init(&start, GS_ONE);
    gs_world_add_car(&start, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(4), GS_INT(9), 0);
    gs_world_add_car(&start, &t, (uint8_t)GS_VEH_DUNE_BUGGY, GS_INT(4), GS_INT(11), 0);

    gs_net_begin(&n[0], &start, 2, gs_wire_local(a), gs_test_secret(gs_wire_local(a)));
    gs_net_begin(&n[1], &start, 2, gs_wire_local(b), gs_test_secret(gs_wire_local(b)));

    gs_wire *wires[2] = { a, b };
    const uint32_t ticks = GS_TICK_HZ * 2;

    for (uint32_t tick = 0; tick < ticks; tick++) {
        uint8_t buf[GS_WIRE_MTU];
        size_t got;

        for (int i = 0; i < 2; i++) {
            gs_wire_poll(wires[i]);
            while ((got = gs_wire_recv(wires[i], buf, sizeof buf)) > 0) {
                gs_net_receive(&n[i], &t, buf, got);
            }
        }
        for (int i = 0; i < 2; i++) {
            gs_net_local_input(&n[i], gs_drive(gs_wire_local(wires[i]), tick));
            size_t len = gs_net_packet(&n[i], buf, sizeof buf);
            gs_wire_send(wires[i], buf, len);
            gs_net_step(&n[i], &t);
        }
        SDL_Delay(2);
    }

    for (int i = 0; i < 2; i++) gs_net_finish(&n[i]);
    for (int i = 0; i < 400; i++) {
        uint8_t buf[GS_WIRE_MTU];
        size_t got;
        for (int k = 0; k < 2; k++) {
            gs_wire_poll(wires[k]);
            while ((got = gs_wire_recv(wires[k], buf, sizeof buf)) > 0) {
                gs_net_receive(&n[k], &t, buf, got);
            }
        }
        if (i < 200) {
            for (int k = 0; k < 2; k++) {
                size_t len = gs_net_packet(&n[k], buf, sizeof buf);
                gs_wire_send(wires[k], buf, len);
            }
        }
        SDL_Delay(2);
    }

    // The same race on both machines, over a path the server never carried.
    CHECK(n[0].confirmed_tick == ticks);
    CHECK(n[1].confirmed_tick == ticks);
    CHECK(gs_world_hash(&n[0].confirmed) == gs_world_hash(&n[1].confirmed));
    CHECK(!n[0].desynced);
    CHECK(!n[1].desynced);

    // And they really did talk to each other rather than through the server.
    uint32_t sent = 0, got = 0;
    gs_wire_stats(a, &sent, &got);
    CHECK(sent >= ticks);
    CHECK(got >= ticks);

    gs_wire_close(a);
    gs_wire_close(b);
    gs_wire_quit();
    gs_server_stop();
}

TEST(two_clients_that_cannot_see_each_other_race_through_the_server) {
    // **The verification this item exists for.** The two wires below are given
    // no way to reach each other: the roster's addresses are never used,
    // because everything they send goes to the server in an envelope and comes
    // back out of it. If the relay did not work there would be no race at all.
    CHECK(gs_wire_init());

    if (!gs_server_start_for("47826", GS_SERVER_LIFETIME, "2")) { gs_failures++; return; }

    gs_wire *a = gs_wire_server("127.0.0.1", 47826, "ada", gs_test_server_pub());
    gs_wire *b = gs_wire_server("127.0.0.1", 47826, "bez", gs_test_server_pub());
    CHECK(a != nullptr && b != nullptr);

    gs_wire_use_relay(a, true);
    gs_wire_use_relay(b, true);
    CHECK(gs_wire_relaying(a));
    CHECK(gs_wire_relaying(b));

    for (int k = 0; k < 600 && !(gs_wire_ready(a) && gs_wire_ready(b)); k++) {
        gs_wire_poll(a);
        gs_wire_poll(b);
        SDL_Delay(10);
    }
    CHECK(gs_wire_ready(a));
    CHECK(gs_wire_ready(b));

    // A real race, driven by the real rollback session, over the relay.
    static gs_track t;
    static gs_net n[2];
    gs_world start;

    gs_track_init(&t, 48, 20, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= t.h; y++) {
        for (uint8_t x = 0; x <= t.w; x++) {
            gs_track_set_corner(&t, x, y, x > 20 && x < 26 ? GS_INT(1) : 0);
        }
    }
    gs_world_init(&start, GS_ONE);
    gs_world_add_car(&start, &t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(4), GS_INT(9), 0);
    gs_world_add_car(&start, &t, (uint8_t)GS_VEH_DUNE_BUGGY, GS_INT(4), GS_INT(11), 0);

    gs_net_begin(&n[0], &start, 2, gs_wire_local(a), gs_test_secret(gs_wire_local(a)));
    gs_net_begin(&n[1], &start, 2, gs_wire_local(b), gs_test_secret(gs_wire_local(b)));

    gs_wire *wires[2] = { a, b };

    // Two seconds, paced. **Not because the netcode needs pacing** - it
    // absorbs loss by design - but because a test that fires seven hundred
    // datagrams in a few microseconds is testing the size of a kernel socket
    // buffer rather than the relay. A race sends one packet per player per
    // tick at 120 Hz, so this does too.
    const uint32_t ticks = GS_TICK_HZ * 2;

    for (uint32_t tick = 0; tick < ticks; tick++) {
        uint8_t buf[GS_WIRE_MTU];
        size_t got;

        for (int i = 0; i < 2; i++) {
            gs_wire_poll(wires[i]);
            while ((got = gs_wire_recv(wires[i], buf, sizeof buf)) > 0) {
                gs_net_receive(&n[i], &t, buf, got);
            }
        }
        for (int i = 0; i < 2; i++) {
            gs_net_local_input(&n[i], gs_drive(gs_wire_local(wires[i]), tick));
            size_t len = gs_net_packet(&n[i], buf, sizeof buf);
            gs_wire_send(wires[i], buf, len);
            gs_net_step(&n[i], &t);
        }
        SDL_Delay(2);
    }

    // The race is over, so the reveals still owed go out - they trail the
    // commitments by a fixed distance while it is running, which is what makes
    // a commitment worth anything. Then drain, so both can confirm the finish.
    for (int i = 0; i < 2; i++) gs_net_finish(&n[i]);

    for (int i = 0; i < 300; i++) {
        uint8_t buf[GS_WIRE_MTU];
        size_t got;
        for (int k = 0; k < 2; k++) {
            gs_wire_poll(wires[k]);
            while ((got = gs_wire_recv(wires[k], buf, sizeof buf)) > 0) {
                gs_net_receive(&n[k], &t, buf, got);
            }
        }
        if (i < 150) {
            for (int k = 0; k < 2; k++) {
                size_t len = gs_net_packet(&n[k], buf, sizeof buf);
                gs_wire_send(wires[k], buf, len);
            }
        }
        SDL_Delay(2);
    }

    // The claim: the same race on both machines, with neither able to reach
    // the other.
    CHECK(n[0].confirmed_tick == ticks);
    CHECK(n[1].confirmed_tick == ticks);
    CHECK(gs_world_hash(&n[0].confirmed) == gs_world_hash(&n[1].confirmed));
    CHECK(!n[0].desynced);
    CHECK(!n[1].desynced);

    // And it is the race one machine with no network would have run.
    gs_world solo = start;
    for (uint32_t tick = 0; tick < ticks; tick++) {
        gs_input in[GS_MAX_CARS] = { 0 };
        in[0] = gs_drive(0, tick);
        in[1] = gs_drive(1, tick);
        gs_world_step(&solo, &t, in);
    }
    CHECK(gs_world_hash(&solo) == gs_world_hash(&n[0].confirmed));

    // The cars went somewhere, so this is a race rather than a grid. Measured
    // against where they started rather than against a number picked by hand:
    // two seconds of a car that is also turning does not cover much ground, and
    // a threshold guessed at is a test that fails when the driving changes.
    CHECK(n[0].confirmed.car[0].x > start.car[0].x + GS_INT(1));
    CHECK(n[0].confirmed.car[1].x > start.car[1].x + GS_INT(1));

    gs_wire_close(a);
    gs_wire_close(b);
    gs_wire_quit();
    gs_server_stop();
}

// Race for real, and hand back the recording and the truth about it. Whatever
// a client claims afterwards, this is what actually happened.
static gs_replay gs_run;

// The driver is a parameter because one test needs a recording that is honest
// about somebody other than ada: to ask whether a token issued to one driver is
// refused from another, the *driving* has to check out for the second driver.
// A bez-driven claim backed by ada's recording would be refused for naming the
// wrong driver and would never reach the session check at all.
static void gs_race_for_real_as(gs_track *t, gs_claim *claim, uint16_t laps,
                                const char *driver) {
    gs_track_init(t, 40, 16, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= t->h; y++) {
        for (uint8_t x = 0; x <= t->w; x++) gs_track_set_corner(t, x, y, 0);
    }
    gs_track_add_gate(t, GS_INT(6), GS_INT(8), 0, GS_INT(6));
    gs_track_add_gate(t, GS_INT(30), GS_INT(8), 0, GS_INT(6));

    gs_world w;
    gs_world_init(&w, GS_ONE);
    gs_world_set_mode(&w, GS_MODE_RACE);
    gs_world_set_laps(&w, laps);
    gs_world_add_car(&w, t, (uint8_t)GS_VEH_SPRINT_CAR, GS_INT(4), GS_INT(8), 0);

    gs_replay_begin(&gs_run, &w, t);

    // **Who was driving goes into the recording**, because the server checks it
    // against the name the client joined under. A recording that named nobody
    // was a thing anyone who obtained one could hand in as their own.
    gs_replay_set_driver(&gs_run, 0, driver);

    for (uint32_t i = 0; i < GS_TICK_HZ * 200 && !w.over; i++) {
        gs_input in[GS_MAX_CARS] = { 0 };
        in[0] = gs_ai_drive(&w, t, 0);
        gs_replay_record(&gs_run, in);
        gs_world_step(&w, t, in);
    }

    SDL_zerop(claim);
    claim->track = gs_track_hash(t);
    claim->conditions = gs_conditions_hash(&w);
    claim->laps = laps;
    claim->lap_ticks = w.car[0].best_lap;
    claim->race_ticks = w.car[0].finish_tick;
}

static void gs_race_for_real(gs_track *t, gs_claim *claim, uint16_t laps) {
    gs_race_for_real_as(t, claim, laps, "ada");
}

static void gs_pump(gs_wire *w, int times) {
    for (int k = 0; k < times; k++) {
        gs_wire_poll(w);
        uint8_t drain[GS_WIRE_MTU];
        while (gs_wire_recv(w, drain, sizeof drain) > 0) { }
        SDL_Delay(10);
    }
}

TEST(a_time_is_kept_only_if_re_racing_it_produces_it) {
    // **The verification this item exists for, end to end.** A doctored time is
    // rejected and an honest one from the same client is accepted - by a real
    // server, over real sockets, with the proof arriving in chunks.
    CHECK(gs_wire_init());

    static gs_track t;
    gs_claim honest;
    gs_race_for_real(&t, &honest, 3);
    CHECK(honest.race_ticks > 0);
    CHECK(honest.lap_ticks > 0);

    // The server can only re-race a track it has, so it is the one serving it.
    static uint8_t track_bytes[GS_CARRIER_MAX_BYTES];
    size_t track_len = gs_track_serialize(&t, track_bytes, sizeof track_bytes);
    const char *track_path = "verified.gstrack";
    SDL_IOStream *io = SDL_IOFromFile(track_path, "wb");
    CHECK(io != nullptr);
    if (io != nullptr) { SDL_WriteIO(io, track_bytes, track_len); SDL_CloseIO(io); }

    remove("verified.db");
    remove("verified.db-wal");
    remove("verified.db-shm");
    if (!gs_server_start_verifying("47830", GS_SERVER_LIFETIME, "2", track_path,
                                   "verified.db")) {
        gs_failures++;
        return;
    }

    static uint8_t proof[sizeof(gs_replay) + 4096];
    size_t proof_len = gs_replay_serialize(&gs_run, proof, sizeof proof);
    CHECK(proof_len > 0);

    gs_wire *a = gs_wire_server("127.0.0.1", 47830, "ada", gs_test_server_pub());
    CHECK(a != nullptr);
    for (int k = 0; k < 400; k++) {
        gs_wire_poll(a);
        if (gs_wire_lobby(a) != nullptr && gs_wire_lobby(a)->count == 1) break;
        SDL_Delay(10);
    }

    // --- The cheat first, so that a later pass cannot be the earlier one
    // lingering. A time nobody drove, with the recording of the race that was
    // actually driven behind it.
    gs_wire_send_result(a, honest.track, honest.conditions, honest.laps, 0,
                        honest.lap_ticks / 2, honest.race_ticks / 2,
                        proof, proof_len);
    gs_pump(a, 60);

    gs_wire_ask_best(a, honest.track, honest.conditions, honest.laps);
    gs_pump(a, 40);
    CHECK(gs_wire_best_here(a)->known);
    CHECK(gs_wire_best_here(a)->lap_ticks == 0);      // nothing was kept
    CHECK(gs_wire_best_here(a)->race_ticks == 0);

    // --- And now the truth, from the same client and the same recording.
    gs_wire_send_result(a, honest.track, honest.conditions, honest.laps, 0,
                        honest.lap_ticks, honest.race_ticks, proof, proof_len);
    gs_pump(a, 60);

    gs_wire_ask_best(a, honest.track, honest.conditions, honest.laps);
    bool kept = false;
    for (int k = 0; k < 60 && !kept; k++) {
        gs_pump(a, 5);
        kept = gs_wire_best_here(a)->lap_ticks == honest.lap_ticks;
    }
    CHECK(kept);
    if (kept) {
        CHECK(gs_wire_best_here(a)->race_ticks == honest.race_ticks);
        CHECK(SDL_strcmp(gs_wire_best_here(a)->lap_who, "ada") == 0);
    }

    // --- A claim with no proof at all is not a record either. Silence is not
    // evidence, and a server that accepted one would be a server where the
    // proof is decoration.
    gs_wire_send_result(a, honest.track, honest.conditions, honest.laps, 0,
                        1, 1, nullptr, 0);
    gs_pump(a, 60);
    gs_wire_ask_best(a, honest.track, honest.conditions, honest.laps);
    gs_pump(a, 40);
    CHECK(gs_wire_best_here(a)->lap_ticks == honest.lap_ticks);

    gs_wire_close(a);
    gs_wire_quit();
    gs_server_stop();
    remove("verified.db");
    remove(track_path);
}

TEST(a_time_offered_without_the_servers_token_is_refused) {
    // **The claim carries a token the server issued, and spends it once.**
    // Records are keyed, so resubmitting a time was already harmless - but
    // harmless by accident of the schema, and a thing that is safe by accident
    // stops being safe when the schema changes.
    CHECK(gs_wire_init());

    static gs_track t;
    gs_claim honest;
    gs_race_for_real(&t, &honest, 3);

    static uint8_t track_bytes[GS_CARRIER_MAX_BYTES];
    size_t track_len = gs_track_serialize(&t, track_bytes, sizeof track_bytes);
    const char *track_path = "session.gstrack";
    SDL_IOStream *io = SDL_IOFromFile(track_path, "wb");
    CHECK(io != nullptr);
    if (io != nullptr) { SDL_WriteIO(io, track_bytes, track_len); SDL_CloseIO(io); }

    remove("session.db");
    remove("session.db-wal");
    remove("session.db-shm");
    if (!gs_server_start_verifying("47842", GS_SERVER_LIFETIME, "1", track_path,
                                   "session.db")) {
        gs_failures++;
        return;
    }

    static uint8_t proof[sizeof(gs_replay) + 4096];
    size_t proof_len = gs_replay_serialize(&gs_run, proof, sizeof proof);
    CHECK(proof_len > 0);

    gs_wire *a = gs_wire_server("127.0.0.1", 47842, "ada", gs_test_server_pub());
    CHECK(a != nullptr);

    uint64_t until = SDL_GetTicks() + 8000;
    while (SDL_GetTicks() < until && gs_wire_session(a) == 0) {
        gs_wire_poll(a);
        uint8_t drain[GS_WIRE_MTU];
        while (gs_wire_recv(a, drain, sizeof drain) > 0) { }
        SDL_Delay(10);
    }

    // A token arrived at all, which is the thing the rest of this rests on.
    CHECK(gs_wire_session(a) != 0);

    // --- The honest submission, with the token it was given.
    gs_wire_send_result(a, honest.track, honest.conditions, honest.laps, 0,
                        honest.lap_ticks, honest.race_ticks, proof, proof_len);
    gs_pump(a, 80);

    gs_wire_ask_best(a, honest.track, honest.conditions, honest.laps);
    bool kept = false;
    for (int k = 0; k < 80 && !kept; k++) {
        gs_pump(a, 10);
        kept = gs_wire_best_here(a)->known && gs_wire_best_here(a)->lap_ticks > 0;
    }
    CHECK(kept);

    // --- And the same submission again. The token it used is spent, and the
    // server has issued another - so this is a *fresh* token against a time
    // that is no better than the one already there. It is accepted and beats
    // nothing, which is the honest outcome: replaying a submission is not an
    // attack, it is just pointless.
    uint32_t was = gs_wire_best_here(a)->lap_ticks;
    gs_wire_send_result(a, honest.track, honest.conditions, honest.laps, 0,
                        honest.lap_ticks, honest.race_ticks, proof, proof_len);
    gs_pump(a, 80);
    gs_wire_ask_best(a, honest.track, honest.conditions, honest.laps);
    gs_pump(a, 30);
    CHECK(gs_wire_best_here(a)->lap_ticks == was);

    gs_wire_close(a);
    gs_wire_quit();
    gs_server_stop();

    remove("session.db");
    remove("session.db-wal");
    remove("session.db-shm");
    remove(track_path);
}

TEST(a_time_offered_with_a_token_the_server_never_issued_is_refused) {
    // **The check the honest path cannot exercise.** A real client always sends
    // the token it was handed, so the only way to ask whether the server is
    // actually looking is to speak the protocol directly and offer one it was
    // not given.
    static gs_track t;
    gs_claim honest;
    gs_race_for_real(&t, &honest, 3);

    static uint8_t track_bytes[GS_CARRIER_MAX_BYTES];
    size_t track_len = gs_track_serialize(&t, track_bytes, sizeof track_bytes);
    const char *track_path = "forged.gstrack";
    SDL_IOStream *io = SDL_IOFromFile(track_path, "wb");
    CHECK(io != nullptr);
    if (io != nullptr) { SDL_WriteIO(io, track_bytes, track_len); SDL_CloseIO(io); }

    remove("forged.db");
    remove("forged.db-wal");
    remove("forged.db-shm");
    if (!gs_server_start_verifying("47844", GS_SERVER_LIFETIME, "1", track_path,
                                   "forged.db")) {
        gs_failures++;
        return;
    }

    static uint8_t proof[sizeof(gs_replay) + 4096];
    size_t proof_len = gs_replay_serialize(&gs_run, proof, sizeof proof);
    CHECK(proof_len > 0);

    gs_test_client c;
    CHECK(gs_client_open(&c, 47844));
    gs_client_say_hello(&c, "ada");

    uint64_t until = SDL_GetTicks() + 8000;
    while (SDL_GetTicks() < until && c.session == 0) {
        gs_client_pump(&c);
        SDL_Delay(10);
    }
    CHECK(c.session != 0);

    // A perfectly good time, an honest recording, and **a token the server never
    // issued**. Everything about the driving checks out; the submission does not.
    uint8_t buf[GS_PROTO_MTU];
    gs_client_send(&c, buf,
                   gs_proto_result(buf, sizeof buf, honest.track,
                                   honest.conditions, honest.laps, 0,
                                   honest.lap_ticks, honest.race_ticks,
                                   c.session ^ 0x5a5a5a5aull));

    uint16_t chunks = gs_carrier_chunks(proof_len);
    for (uint16_t i = 0; i < chunks; i++) {
        size_t at = (size_t)i * GS_CHUNK_BYTES;
        size_t take = proof_len - at;
        if (take > GS_CHUNK_BYTES) take = GS_CHUNK_BYTES;
        gs_client_send(&c, buf,
                       gs_proto_proof_chunk(buf, sizeof buf, honest.track, i,
                                            chunks, proof + at, (uint16_t)take));
        gs_client_pump(&c);
    }

    until = SDL_GetTicks() + 3000;
    while (SDL_GetTicks() < until) { gs_client_pump(&c); SDL_Delay(10); }

    // Nothing was kept.
    c.heard_best = false;
    gs_client_send(&c, buf,
                   gs_proto_want_best(buf, sizeof buf, honest.track,
                                      honest.conditions, honest.laps));
    until = SDL_GetTicks() + 5000;
    while (SDL_GetTicks() < until && !c.heard_best) {
        gs_client_pump(&c);
        SDL_Delay(10);
    }
    CHECK(c.heard_best);
    CHECK(c.best_lap == 0);
    CHECK(c.best_race == 0);

    gs_client_close(&c);
    gs_server_stop();

    remove("forged.db");
    remove("forged.db-wal");
    remove("forged.db-shm");
    remove(track_path);
}

// --- speaking the claim protocol by hand -------------------------------------
//
// A real client always sends the token it was handed. These tests have to
// choose the token, so they build the claim themselves.

static void gs_client_offer(gs_test_client *c, const gs_claim *claim,
                            const uint8_t *proof, size_t proof_len,
                            uint64_t nonce) {
    uint8_t buf[GS_PROTO_MTU];
    gs_client_send(c, buf,
                   gs_proto_result(buf, sizeof buf, claim->track,
                                   claim->conditions, claim->laps, 0,
                                   claim->lap_ticks, claim->race_ticks, nonce));

    uint16_t chunks = gs_carrier_chunks(proof_len);
    for (uint16_t i = 0; i < chunks; i++) {
        size_t at = (size_t)i * GS_CHUNK_BYTES;
        size_t take = proof_len - at;
        if (take > GS_CHUNK_BYTES) take = GS_CHUNK_BYTES;
        gs_client_send(c, buf,
                       gs_proto_proof_chunk(buf, sizeof buf, claim->track, i,
                                            chunks, proof + at, (uint16_t)take));
        gs_client_pump(c);
    }
}

// Wait for the server to hand this client a token. Everything else rests on one
// arriving at all, so a test that carried on without it would be testing
// nothing.
static bool gs_client_wait_for_token(gs_test_client *c) {
    uint64_t until = SDL_GetTicks() + 8000;
    while (SDL_GetTicks() < until && c->session == 0) {
        gs_client_pump(c);
        SDL_Delay(10);
    }
    return c->session != 0;
}

// Pump for a while, so the server has had time to re-race a claim and answer.
static void gs_client_settle(gs_test_client *c, int ms) {
    uint64_t until = SDL_GetTicks() + (uint64_t)ms;
    while (SDL_GetTicks() < until) {
        gs_client_pump(c);
        SDL_Delay(10);
    }
}

// The best *race* here, which is the one keyed by lap count - a best lap is a
// best lap whatever the race length, so it is shared across every record on the
// track and cannot say which of two claims landed. Asked more than once,
// because a datagram is allowed to vanish and a test that asked once would fail
// on a bad afternoon.
static uint32_t gs_client_best_race(gs_test_client *c, const gs_claim *claim) {
    uint8_t buf[GS_PROTO_MTU];
    c->heard_best = false;
    c->best_lap = 0;
    c->best_race = 0;

    for (int tries = 0; tries < 5 && !c->heard_best; tries++) {
        gs_client_send(c, buf,
                       gs_proto_want_best(buf, sizeof buf, claim->track,
                                          claim->conditions, claim->laps));
        uint64_t until = SDL_GetTicks() + 1000;
        while (SDL_GetTicks() < until && !c->heard_best) {
            gs_client_pump(c);
            SDL_Delay(10);
        }
    }
    return c->best_race;
}

// Write a track where the server's --track can find it.
static bool gs_put_track_file(const gs_track *t, const char *path) {
    static uint8_t bytes[GS_CARRIER_MAX_BYTES];
    size_t len = gs_track_serialize(t, bytes, sizeof bytes);
    if (len == 0) return false;

    SDL_IOStream *io = SDL_IOFromFile(path, "wb");
    if (io == nullptr) return false;
    SDL_WriteIO(io, bytes, len);
    SDL_CloseIO(io);
    return true;
}

static void gs_forget_store(const char *stem) {
    char path[256];
    SDL_snprintf(path, sizeof path, "%s", stem);          remove(path);
    SDL_snprintf(path, sizeof path, "%s-wal", stem);      remove(path);
    SDL_snprintf(path, sizeof path, "%s-shm", stem);      remove(path);
}

TEST(a_token_issued_to_one_driver_is_refused_from_another) {
    // **A token seen on the wire is no use to whoever saw it.** The recording
    // here is honestly bez's, so the driving checks out for bez and the claim
    // reaches the session check instead of being turned away earlier for naming
    // the wrong driver. The only thing wrong with the first submission is that
    // the token belongs to ada.
    static gs_track t;
    gs_claim honest;
    gs_race_for_real_as(&t, &honest, 3, "bez");

    const char *track_path = "borrowed.gstrack";
    CHECK(gs_put_track_file(&t, track_path));

    gs_forget_store("borrowed.db");
    if (!gs_server_start_verifying("47846", GS_SERVER_LIFETIME, "2", track_path,
                                   "borrowed.db")) {
        gs_failures++;
        return;
    }

    static uint8_t proof[sizeof(gs_replay) + 4096];
    size_t proof_len = gs_replay_serialize(&gs_run, proof, sizeof proof);
    CHECK(proof_len > 0);

    gs_test_client ada, bez;
    CHECK(gs_client_open(&ada, 47846));
    CHECK(gs_client_open(&bez, 47846));
    gs_client_say_hello(&ada, "ada");
    gs_client_say_hello(&bez, "bez");
    CHECK(gs_client_wait_for_token(&ada));
    CHECK(gs_client_wait_for_token(&bez));
    CHECK(ada.session != bez.session);

    // --- bez, driving honestly, spending ada's token.
    gs_client_offer(&bez, &honest, proof, proof_len, ada.session);
    gs_client_settle(&bez, 3000);
    CHECK(gs_client_best_race(&bez, &honest) == 0);

    // --- The control. The same driver, the same recording, the same time - and
    // bez's own token. If this did not land, the refusal above would prove
    // nothing about tokens.
    gs_client_settle(&bez, 500);
    CHECK(bez.session != 0);
    gs_client_offer(&bez, &honest, proof, proof_len, bez.session);
    gs_client_settle(&bez, 3000);
    CHECK(gs_client_best_race(&bez, &honest) > 0);

    gs_client_close(&ada);
    gs_client_close(&bez);
    gs_server_stop();

    gs_forget_store("borrowed.db");
    remove(track_path);
}

TEST(a_token_buys_one_submission_and_the_second_time_it_buys_nothing) {
    // **Spent once.** Two times on the one track, keyed apart by their lap
    // count, so the second submission has somewhere of its own to land - and
    // whether it landed is then a thing the test can see, rather than being
    // hidden behind a record that would have looked the same either way.
    static gs_track t;
    gs_claim three, two;

    static uint8_t proof_three[sizeof(gs_replay) + 4096];
    gs_race_for_real(&t, &three, 3);
    size_t three_len = gs_replay_serialize(&gs_run, proof_three,
                                           sizeof proof_three);

    static uint8_t proof_two[sizeof(gs_replay) + 4096];
    gs_race_for_real(&t, &two, 2);
    size_t two_len = gs_replay_serialize(&gs_run, proof_two, sizeof proof_two);

    CHECK(three_len > 0);
    CHECK(two_len > 0);
    CHECK(three.track == two.track);   // the same track, two different records
    CHECK(three.race_ticks > 0);       // both finished, so both have a race time
    CHECK(two.race_ticks > 0);

    const char *track_path = "spent.gstrack";
    CHECK(gs_put_track_file(&t, track_path));

    gs_forget_store("spent.db");
    if (!gs_server_start_verifying("47848", GS_SERVER_LIFETIME, "1", track_path,
                                   "spent.db")) {
        gs_failures++;
        return;
    }

    gs_test_client c;
    CHECK(gs_client_open(&c, 47848));
    gs_client_say_hello(&c, "ada");
    CHECK(gs_client_wait_for_token(&c));

    uint64_t first = c.session;

    // --- Spend it, honestly.
    gs_client_offer(&c, &three, proof_three, three_len, first);
    gs_client_settle(&c, 3000);
    CHECK(gs_client_best_race(&c, &three) > 0);

    // --- And spend it again on the other time. Nothing wrong with the driving;
    // the token is used up.
    gs_client_offer(&c, &two, proof_two, two_len, first);
    gs_client_settle(&c, 3000);
    CHECK(gs_client_best_race(&c, &two) == 0);

    // --- The control: the same time again, with the token the server issued
    // after the refusal. It lands, so what stopped it was the spent token.
    CHECK(c.session != 0);
    CHECK(c.session != first);
    gs_client_offer(&c, &two, proof_two, two_len, c.session);
    gs_client_settle(&c, 3000);
    CHECK(gs_client_best_race(&c, &two) > 0);

    gs_client_close(&c);
    gs_server_stop();

    gs_forget_store("spent.db");
    remove(track_path);
}

TEST(a_spent_token_is_still_spent_after_the_server_restarts) {
    // **The whole reason sessions live in the database.** A server that held
    // them in memory would come back up remembering nothing, and a token it can
    // no longer retire is one that can be handed in for ever - which is exactly
    // what it exists to stop.
    static gs_track t;
    gs_claim three, two;

    static uint8_t proof_three[sizeof(gs_replay) + 4096];
    gs_race_for_real(&t, &three, 3);
    size_t three_len = gs_replay_serialize(&gs_run, proof_three,
                                           sizeof proof_three);

    static uint8_t proof_two[sizeof(gs_replay) + 4096];
    gs_race_for_real(&t, &two, 2);
    size_t two_len = gs_replay_serialize(&gs_run, proof_two, sizeof proof_two);

    CHECK(three_len > 0);
    CHECK(two_len > 0);
    CHECK(three.race_ticks > 0);       // both finished, so both have a race time
    CHECK(two.race_ticks > 0);

    const char *track_path = "restart.gstrack";
    CHECK(gs_put_track_file(&t, track_path));

    // A file, not ":memory:" - a store that vanished with the process could not
    // answer the question this test asks.
    gs_forget_store("restart.db");
    if (!gs_server_start_verifying("47850", GS_SERVER_LIFETIME, "1", track_path,
                                   "restart.db")) {
        gs_failures++;
        return;
    }

    gs_test_client c;
    CHECK(gs_client_open(&c, 47850));
    gs_client_say_hello(&c, "ada");
    CHECK(gs_client_wait_for_token(&c));

    uint64_t before_restart = c.session;
    gs_client_offer(&c, &three, proof_three, three_len, before_restart);
    gs_client_settle(&c, 3000);
    CHECK(gs_client_best_race(&c, &three) > 0);
    gs_client_close(&c);

    // --- A different process, the same database.
    gs_server_stop();
    if (!gs_server_start_verifying("47850", GS_SERVER_LIFETIME, "1", track_path,
                                   "restart.db")) {
        gs_failures++;
        gs_forget_store("restart.db");
        remove(track_path);
        return;
    }

    gs_test_client again;
    CHECK(gs_client_open(&again, 47850));
    gs_client_say_hello(&again, "ada");
    CHECK(gs_client_wait_for_token(&again));
    CHECK(again.session != before_restart);

    // The record set before the restart is still there, which says the store is
    // the same one and not a fresh file.
    CHECK(gs_client_best_race(&again, &three) > 0);

    // --- The token spent by the process that died is still spent.
    gs_client_offer(&again, &two, proof_two, two_len, before_restart);
    gs_client_settle(&again, 3000);
    CHECK(gs_client_best_race(&again, &two) == 0);

    // --- The control, against the new process's own token.
    CHECK(again.session != 0);
    gs_client_offer(&again, &two, proof_two, two_len, again.session);
    gs_client_settle(&again, 3000);
    CHECK(gs_client_best_race(&again, &two) > 0);

    gs_client_close(&again);
    gs_server_stop();

    gs_forget_store("restart.db");
    remove(track_path);
}

TEST(a_record_set_on_one_client_is_seen_by_another) {
    // The same record, read by somebody who has never seen the race. What makes
    // this worth a test of its own is the *second* client: the server is the
    // only thing that connects them.
    CHECK(gs_wire_init());

    static gs_track t;
    gs_claim honest;
    gs_race_for_real(&t, &honest, 2);

    static uint8_t track_bytes[GS_CARRIER_MAX_BYTES];
    size_t track_len = gs_track_serialize(&t, track_bytes, sizeof track_bytes);
    const char *track_path = "shared.gstrack";
    SDL_IOStream *io = SDL_IOFromFile(track_path, "wb");
    if (io != nullptr) { SDL_WriteIO(io, track_bytes, track_len); SDL_CloseIO(io); }

    remove("shared.db");
    if (!gs_server_start_verifying("47832", GS_SERVER_LIFETIME, "2", track_path, "shared.db")) {
        gs_failures++;
        return;
    }

    static uint8_t proof[sizeof(gs_replay) + 4096];
    size_t proof_len = gs_replay_serialize(&gs_run, proof, sizeof proof);

    gs_wire *a = gs_wire_server("127.0.0.1", 47832, "ada", gs_test_server_pub());
    CHECK(a != nullptr);
    for (int k = 0; k < 400; k++) {
        gs_wire_poll(a);
        if (gs_wire_lobby(a) != nullptr && gs_wire_lobby(a)->count == 1) break;
        SDL_Delay(10);
    }

    gs_wire_send_result(a, honest.track, honest.conditions, honest.laps, 0,
                        honest.lap_ticks, honest.race_ticks, proof, proof_len);
    gs_pump(a, 60);

    // bez arrives afterwards, having seen none of it.
    gs_wire *b = gs_wire_server("127.0.0.1", 47832, "bez", gs_test_server_pub());
    CHECK(b != nullptr);
    for (int k = 0; k < 400; k++) {
        gs_wire_poll(b);
        if (gs_wire_lobby(b) != nullptr && gs_wire_lobby(b)->count == 2) break;
        SDL_Delay(10);
    }

    gs_wire_ask_best(b, honest.track, honest.conditions, honest.laps);
    bool told = false;
    for (int k = 0; k < 60 && !told; k++) {
        gs_pump(b, 5);
        told = gs_wire_best_here(b)->known && gs_wire_best_here(b)->lap_ticks > 0;
    }
    CHECK(told);
    if (told) {
        CHECK(gs_wire_best_here(b)->lap_ticks == honest.lap_ticks);
        CHECK(SDL_strcmp(gs_wire_best_here(b)->lap_who, "ada") == 0);
    }

    gs_wire_close(a);
    gs_wire_close(b);
    gs_wire_quit();
    gs_server_stop();
    remove("shared.db");
    remove(track_path);
}

TEST(a_published_track_is_browsable_from_another_client_and_can_be_taken_down) {
    // **The verification this item exists for.** ada publishes; bez, who has
    // never seen it, finds it and can fetch it; ada takes it down and it stops
    // being listed.
    CHECK(gs_wire_init());

    remove("published.db");
    if (!gs_server_start_with_store("47834", GS_SERVER_LIFETIME, "2", "published.db")) {
        gs_failures++;
        return;
    }

    static gs_track mine;
    gs_track_init(&mine, 36, 20, GS_SURF_DIRT);
    for (uint8_t y = 0; y <= mine.h; y++) {
        for (uint8_t x = 0; x <= mine.w; x++) {
            gs_track_set_corner(&mine, x, y, x > 12 && x < 18 ? GS_INT(2) : 0);
        }
    }
    gs_track_add_gate(&mine, GS_INT(4), GS_INT(10), 0, GS_INT(6));
    gs_track_add_gate(&mine, GS_INT(30), GS_INT(10), 0, GS_INT(6));
    uint64_t hash = gs_track_hash(&mine);

    gs_wire *a = gs_wire_server("127.0.0.1", 47834, "ada", gs_test_server_pub());
    gs_wire *b = gs_wire_server("127.0.0.1", 47834, "bez", gs_test_server_pub());
    CHECK(a != nullptr && b != nullptr);
    for (int k = 0; k < 400; k++) {
        gs_wire_poll(a);
        gs_wire_poll(b);
        if (gs_wire_lobby(a) != nullptr && gs_wire_lobby(a)->count == 2) break;
        SDL_Delay(10);
    }

    // What is up before ada puts anything up. **Not zero**: a server ships with
    // the stock library published, so this is a delta rather than an absolute -
    // and a delta is the honest measurement anyway, because what is being tested
    // is that publishing adds one and withdrawing takes it away again.
    gs_wire_ask_published(b);
    const gs_wire_listing *rows = nullptr;
    uint16_t total = 0;
    for (int k = 0; k < 60; k++) {
        gs_pump(b, 5);
        if (gs_wire_published(b, &rows, &total) > 0) break;
    }
    uint16_t before = total;
    CHECK(before > 0);

    // ada puts one up.
    gs_wire_publish(a, &mine, "the dirt loop");
    gs_pump(a, 40);

    // bez finds it, by name and by author, having never seen the track.
    bool found = false;
    int at = -1;
    for (int k = 0; k < 80 && !found; k++) {
        gs_wire_ask_published(b);
        gs_pump(b, 10);
        uint16_t n = gs_wire_published(b, &rows, &total);
        for (uint16_t i = 0; i < n; i++) {
            if (rows[i].track == hash) { found = true; at = (int)i; break; }
        }
    }
    CHECK(found);
    CHECK(total == before + 1);
    if (found && at >= 0) {
        CHECK(SDL_strcmp(rows[at].name, "the dirt loop") == 0);
        CHECK(SDL_strcmp(rows[at].author, "ada") == 0);
    }

    // **And it is playable**, which a listing alone does not prove: the track
    // itself has to be fetchable and has to be the track.
    gs_wire_ask_track(b, hash);
    static gs_track got;
    bool fetched = false;
    for (int k = 0; k < 120 && !fetched; k++) {
        gs_pump(b, 5);
        fetched = gs_wire_track(b, &got) && gs_track_hash(&got) == hash;
    }
    CHECK(fetched);
    if (fetched) {
        CHECK(got.w == mine.w && got.h == mine.h);
        CHECK(got.gate_count == mine.gate_count);
    }

    // bez cannot take down somebody else's.
    gs_wire_withdraw(b, hash);
    gs_pump(b, 40);
    gs_wire_ask_published(b);
    gs_pump(b, 20);
    gs_wire_published(b, &rows, &total);
    CHECK(total == before + 1);

    // ada can.
    gs_wire_withdraw(a, hash);
    gs_pump(a, 40);

    bool gone = false;
    for (int k = 0; k < 80 && !gone; k++) {
        gs_wire_ask_published(b);
        gs_pump(b, 10);
        gs_wire_published(b, &rows, &total);
        gone = total == before;
    }
    CHECK(gone);

    gs_wire_close(a);
    gs_wire_close(b);
    gs_wire_quit();
    gs_server_stop();
    remove("published.db");
}

int main(void) {
    printf("gearstick server tests\n");

    if (!SDL_Init(0) || !NET_Init()) {
        printf("could not start networking: %s\n", SDL_GetError());
        return 1;
    }

    run_four_clients_connect_and_the_server_shows_all_of_them();
    run_saying_hello_twice_is_still_one_player();
    run_a_server_told_to_hold_two_holds_two();
    run_the_server_ignores_datagrams_that_are_not_ours();
    run_the_games_own_client_gets_its_slot_from_the_server();
    run_a_client_waiting_for_others_is_not_ready_yet();
    run_a_server_nobody_has_set_up_still_has_a_library_to_offer();
    run_the_roster_is_sent_again_rather_than_only_when_it_changes();
    run_a_client_that_quits_says_so_instead_of_being_waited_out();
    run_a_placed_client_is_not_dropped_for_going_quiet();
    run_a_client_with_a_different_track_is_given_the_right_one();
    run_two_clients_the_server_introduced_race_each_other_directly_and_sealed();
    run_two_clients_that_cannot_see_each_other_race_through_the_server();
    run_a_time_is_kept_only_if_re_racing_it_produces_it();
    run_a_time_offered_without_the_servers_token_is_refused();
    run_a_time_offered_with_a_token_the_server_never_issued_is_refused();
    run_a_token_issued_to_one_driver_is_refused_from_another();
    run_a_token_buys_one_submission_and_the_second_time_it_buys_nothing();
    run_a_spent_token_is_still_spent_after_the_server_restarts();
    run_a_record_set_on_one_client_is_seen_by_another();
    run_a_published_track_is_browsable_from_another_client_and_can_be_taken_down();

    gs_server_stop();
    NET_Quit();
    SDL_Quit();

    if (gs_failures > 0) {
        printf("%d check%s failed\n", gs_failures, gs_failures == 1 ? "" : "s");
        return 1;
    }
    printf("all server tests passed\n");
    return 0;
}

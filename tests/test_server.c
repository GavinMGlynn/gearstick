// test_server.c - the lobby, over real sockets.
//
// The server is a separate process in real use, and here it is a thread: the
// same code, the same socket, driven by real clients on the loopback. What is
// being checked is the thing the plan asks for - four clients connect and
// appear by name, and one leaving is noticed - plus the two answers a lobby has
// to get right that are easy to get wrong: a fifth client is refused *with a
// reason*, and somebody who says hello twice does not occupy two slots.
#include "net/gs_proto.h"

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
} gs_test_client;

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
    return true;
}

static void gs_client_close(gs_test_client *c) {
    if (c->server != nullptr) NET_UnrefAddress(c->server);
    if (c->sock != nullptr) NET_DestroyDatagramSocket(c->sock);
    SDL_zerop(c);
}

static void gs_client_send(gs_test_client *c, const uint8_t *buf, size_t n);

static void gs_client_send(gs_test_client *c, const uint8_t *buf, size_t n) {
    NET_SendDatagram(c->sock, c->server, c->port, buf, (int)n);
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
        size_t len = (size_t)d->buflen;

        switch (gs_proto_kind(d->buf, len)) {
        case GS_MSG_WELCOME:
            if (gs_proto_read_welcome(d->buf, len, &c->slot, &c->lobby)) {
                c->welcomed = true;
            }
            break;
        case GS_MSG_LOBBY:
            gs_proto_read_lobby(d->buf, len, &c->lobby);
            break;
        case GS_MSG_FULL:
            c->refused = gs_proto_read_full(d->buf, len, c->why, sizeof c->why);
            break;
        case GS_MSG_PING: {
            // A real client answers, which is how the server measures the trip.
            // A test client that did not would make the server's ping column a
            // column of dashes, and the test would not notice.
            uint32_t stamp = 0;
            if (gs_proto_read_stamp(d->buf, len, &stamp)) {
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

// The server is a process in real life and a thread here. Started with a
// deadline so a broken test cannot leave one running.
static SDL_Process *gs_server = nullptr;

static bool gs_server_start_for(const char *port, const char *seconds,
                                const char *players) {
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

    const char *args[] = {
        exe, "--port", port, "--plain", "--seconds", seconds,
        "--players", players, nullptr,
    };

    // Its output goes nowhere. The server redraws a live view four times a
    // second, and interleaving that with the test's own output makes a failure
    // impossible to read.
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, args);
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
    if (!gs_server_start("47810", "20")) { gs_failures++; return; }

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
    if (!gs_server_start("47814", "15")) { gs_failures++; return; }

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
    if (!gs_server_start_for("47816", "15", "2")) { gs_failures++; return; }

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
    if (!gs_server_start("47812", "12")) { gs_failures++; return; }

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

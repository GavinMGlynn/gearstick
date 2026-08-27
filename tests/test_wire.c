// test_wire.c - a race over two real sockets.
//
// Everything else about the netcode is checked against a simulated link, which
// is the right way to test rollback: the latency and the loss are chosen, so a
// failure reproduces. What that cannot check is whether the *socket* works -
// whether a host finds a peer it has never been told about, whether datagrams
// survive the round trip, whether the two ends agree once real packets carry
// them. This does, over the loopback, which is a real network stack and the
// only one a test is entitled to assume exists.
#include "core/gs_net.h"
#include "platform/gs_wire.h"

#include <SDL3/SDL.h>
#include <SDL3_net/SDL_net.h>

#include <stdio.h>
#include "gs_sandbox.h"

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

static gs_track gs_t;


static void gs_scene(gs_world *w) {
    gs_track_init(&gs_t, 48, 20, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= gs_t.h; y++)
        for (uint8_t x = 0; x <= gs_t.w; x++)
            gs_track_set_corner(&gs_t, x, y, x > 20 && x < 26 ? GS_INT(1) : 0);

    static const uint8_t grid[GS_MAX_CARS] = {
        (uint8_t)GS_VEH_STOCK_CAR, (uint8_t)GS_VEH_DUNE_BUGGY,
        (uint8_t)GS_VEH_SPRINT_CAR, (uint8_t)GS_VEH_BAJA_BUG,
    };
    gs_world_init(w, GS_ONE);
    for (uint8_t i = 0; i < GS_MAX_CARS; i++) {
        gs_world_add_car(w, &gs_t, grid[i], GS_INT(4), GS_INT(5) + GS_INT(2) * i, 0);
    }
}

// Four different drivers, so no two players' inputs are the same and a session
// that mixed two of them up would be caught.
static gs_input gs_drive(uint8_t player, uint32_t tick) {
    uint32_t period = 17u + player * 11u;
    gs_input in = GS_IN_ACCEL;
    if ((tick / period) % 2u == 0u) {
        in |= (player & 1u) ? GS_IN_RIGHT : GS_IN_LEFT;
    }
    if ((tick / (53u + player * 7u)) % 4u == 0u) in |= GS_IN_BRAKE;
    return in;
}

// Bring `count` machines up on the loopback and let them find each other. The
// host is index 0. Returns false if the handshake never completed.
static bool gs_meet(gs_wire **w, int count, uint16_t port) {
    w[0] = gs_wire_host(port, (uint8_t)count);
    if (w[0] == nullptr || gs_wire_error(w[0]) != nullptr) return false;

    for (int i = 1; i < count; i++) {
        // The host's key, which in a real game a person passes on and here
        // is simply asked for. A joiner given none refuses to connect.
        w[i] = gs_wire_join("127.0.0.1", port, gs_wire_public_key(w[0]));
        if (w[i] == nullptr || gs_wire_error(w[i]) != nullptr) return false;
    }

    // Everybody polls until everybody is ready. A second of tries at 60 fps is
    // far longer than a loopback handshake needs and short enough that a broken
    // one fails the test rather than hanging CI.
    for (int tick = 0; tick < 600; tick++) {
        bool all = true;
        for (int i = 0; i < count; i++) {
            gs_wire_poll(w[i]);
            if (!gs_wire_ready(w[i])) all = false;
        }
        if (all) return true;
        SDL_Delay(1);
    }
    return false;
}

// Run a race across `count` sessions over real sockets, and hand back whether
// they all agreed.
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

static bool gs_race(gs_wire **w, gs_net *n, int count, uint32_t ticks) {
    gs_world start;
    gs_scene(&start);

    for (int i = 0; i < count; i++) {
        gs_net_begin(&n[i], &start, (uint8_t)count, gs_wire_local(w[i]),
                     gs_test_secret(gs_wire_local(w[i])));
    }

    for (uint32_t tick = 0; tick < ticks; tick++) {
        uint8_t buf[GS_WIRE_MTU];
        size_t got;

        for (int i = 0; i < count; i++) {
            while ((got = gs_wire_recv(w[i], buf, sizeof buf)) > 0) {
                gs_net_receive(&n[i], &gs_t, buf, got);
            }
        }
        for (int i = 0; i < count; i++) {
            gs_net_local_input(&n[i], gs_drive(gs_wire_local(w[i]), tick));
            size_t len = gs_net_packet(&n[i], buf, sizeof buf);
            gs_wire_send(w[i], buf, len);
            gs_net_step(&n[i], &gs_t);
        }
    }

    // **The race is over, so what is still promised gets shown.** The reveals
    // trail the commitments by a fixed distance while a race is running, which
    // is what makes a commitment worth anything; the last dozen ticks are
    // therefore still owed when the loop ends.
    for (int i = 0; i < count; i++) gs_net_finish(&n[i]);

    // Keep talking while that goes out, then drain whatever is still in flight,
    // so everybody can confirm the finish.
    for (int i = 0; i < 200; i++) {
        uint8_t buf[GS_WIRE_MTU];
        size_t got;
        for (int k = 0; k < count; k++) {
            while ((got = gs_wire_recv(w[k], buf, sizeof buf)) > 0) {
                gs_net_receive(&n[k], &gs_t, buf, got);
            }
        }
        if (i < 100) {
            for (int k = 0; k < count; k++) {
                size_t len = gs_net_packet(&n[k], buf, sizeof buf);
                gs_wire_send(w[k], buf, len);
            }
        }
        SDL_Delay(1);
    }

    for (int i = 1; i < count; i++) {
        if (gs_world_hash(&n[i].confirmed) != gs_world_hash(&n[0].confirmed)) {
            return false;
        }
    }
    return true;
}

// The race that would have happened on one machine, for comparing against.
static uint64_t gs_solo(int count, uint32_t ticks) {
    gs_world w;
    gs_scene(&w);
    for (uint32_t tick = 0; tick < ticks; tick++) {
        gs_input in[GS_MAX_CARS] = { 0 };
        for (int i = 0; i < count; i++) in[i] = gs_drive((uint8_t)i, tick);
        gs_world_step(&w, &gs_t, in);
    }
    return gs_world_hash(&w);
}

TEST(two_processes_on_one_machine_race_over_real_sockets) {
    CHECK(gs_wire_init());

    static gs_wire *w[2];
    static gs_net n[2];
    const uint32_t ticks = GS_TICK_HZ * 4;

    CHECK(gs_meet(w, 2, 47823));
    if (gs_wire_ready(w[0])) {
        CHECK(gs_wire_local(w[0]) == 0);
        CHECK(gs_wire_local(w[1]) == 1);
        CHECK(gs_wire_players(w[0]) == 2);

        CHECK(gs_race(w, n, 2, ticks));
        CHECK(n[0].confirmed_tick == ticks);
        CHECK(n[1].confirmed_tick == ticks);
        CHECK(!n[0].desynced);
        CHECK(!n[1].desynced);

        // And it is the race that would have happened on one machine.
        CHECK(gs_world_hash(&n[0].confirmed) == gs_solo(2, ticks));
    }

    for (int i = 0; i < 2; i++) gs_wire_close(w[i]);
    gs_wire_quit();
}

TEST(four_machines_mesh_up_and_race_the_same_race) {
    CHECK(gs_wire_init());

    static gs_wire *w[4];
    static gs_net n[4];
    const uint32_t ticks = GS_TICK_HZ * 4;

    // Four is the whole point of the couch this is replacing. The host is told
    // to expect four and nobody starts until all four have arrived.
    CHECK(gs_meet(w, 4, 47825));

    if (gs_wire_ready(w[0])) {
        CHECK(gs_wire_players(w[0]) == 4);
        CHECK(gs_wire_present(w[0]) == 4);

        // Everybody got a different slot, and the host kept zero.
        CHECK(gs_wire_local(w[0]) == 0);
        bool seen[4] = { false, false, false, false };
        for (int i = 0; i < 4; i++) {
            uint8_t slot = gs_wire_local(w[i]);
            CHECK(slot < 4);
            if (slot < 4) {
                CHECK(!seen[slot]);
                seen[slot] = true;
            }
        }

        CHECK(gs_race(w, n, 4, ticks));
        for (int i = 0; i < 4; i++) {
            CHECK(n[i].confirmed_tick == ticks);
            CHECK(!n[i].desynced);
        }

        // **A mesh, not a relay.** Every machine heard from all three others
        // directly, so each one received about three datagrams for every one it
        // sent. A star topology would have the clients hearing from one.
        for (int i = 0; i < 4; i++) {
            uint32_t sent = 0, got = 0;
            gs_wire_stats(w[i], &sent, &got);
            CHECK(sent >= ticks);
            CHECK(got > sent * 2);
        }

        CHECK(gs_world_hash(&n[0].confirmed) == gs_solo(4, ticks));
    }

    for (int i = 0; i < 4; i++) gs_wire_close(w[i]);
    gs_wire_quit();
}

TEST(a_datagram_nobody_sealed_is_not_taken_for_race_traffic) {
    // **Before the mesh was sealed, this was an open door.** Anybody who knew a
    // player's address and port could send a rollback datagram and have it
    // handed straight to the race as somebody's inputs - there was nothing in
    // the format that said who wrote it, because there was nothing to say it
    // with.
    CHECK(gs_wire_init());

    static gs_wire *w[2];
    CHECK(gs_meet(w, 2, 47829));
    if (!gs_wire_ready(w[0])) {
        for (int i = 0; i < 2; i++) gs_wire_close(w[i]);
        gs_wire_quit();
        return;
    }

    // A stranger with a socket and the host's port.
    NET_DatagramSocket *sock = NET_CreateDatagramSocket(nullptr, 0, 0);
    CHECK(sock != nullptr);
    NET_Address *host = NET_ResolveHostname("127.0.0.1");
    CHECK(host != nullptr);
    CHECK(NET_WaitUntilResolved(host, 3000) == NET_SUCCESS);

    // Shaped exactly like the real thing: the rollback magic, a player number,
    // and a plausible amount of what looks like input.
    uint8_t forged[128];
    SDL_memset(forged, 0, sizeof forged);
    forged[0] = (uint8_t)(GS_NET_MAGIC & 0xffu);
    forged[1] = (uint8_t)((GS_NET_MAGIC >> 8) & 0xffu);
    forged[2] = (uint8_t)((GS_NET_MAGIC >> 16) & 0xffu);
    forged[3] = (uint8_t)((GS_NET_MAGIC >> 24) & 0xffu);
    forged[4] = 1;                      // "I am player one"

    for (int i = 0; i < 20; i++) {
        CHECK(NET_SendDatagram(sock, host, 47829, forged, (int)sizeof forged));
    }

    // The host is given every chance to accept it and does not.
    bool took = false;
    for (int i = 0; i < 200; i++) {
        uint8_t buf[GS_WIRE_MTU];
        gs_wire_poll(w[0]);
        if (gs_wire_recv(w[0], buf, sizeof buf) > 0) took = true;
        SDL_Delay(1);
    }
    CHECK(!took);

    // And the control: the real peer's traffic, sealed, still gets through.
    // Without this the test would pass just as well if the host had stopped
    // listening altogether.
    uint8_t real[64];
    SDL_memset(real, 0x5a, sizeof real);
    CHECK(gs_wire_send(w[1], real, sizeof real));

    bool heard = false;
    for (int i = 0; i < 200 && !heard; i++) {
        uint8_t buf[GS_WIRE_MTU];
        gs_wire_poll(w[0]);
        size_t got = gs_wire_recv(w[0], buf, sizeof buf);
        if (got == sizeof real && SDL_memcmp(buf, real, got) == 0) heard = true;
        SDL_Delay(1);
    }
    CHECK(heard);

    NET_UnrefAddress(host);
    NET_DestroyDatagramSocket(sock);
    for (int i = 0; i < 2; i++) gs_wire_close(w[i]);
    gs_wire_quit();
}

TEST(a_fifth_machine_is_not_let_into_a_four_player_race) {
    CHECK(gs_wire_init());

    static gs_wire *w[4];
    CHECK(gs_meet(w, 4, 47827));

    // One more knocks. There is no room, and it is given silence rather than a
    // reply telling it there is a game here.
    gs_wire *gatecrasher = gs_wire_join("127.0.0.1", 47827,
                                        gs_wire_public_key(w[0]));
    CHECK(gatecrasher != nullptr);

    for (int tick = 0; tick < 120; tick++) {
        for (int i = 0; i < 4; i++) gs_wire_poll(w[i]);
        gs_wire_poll(gatecrasher);
        SDL_Delay(1);
    }

    CHECK(!gs_wire_ready(gatecrasher));
    CHECK(gs_wire_players(w[0]) == 4);
    CHECK(gs_wire_present(w[0]) == 4);
    CHECK(gs_wire_ready(w[0]));      // and the race in progress is undisturbed

    gs_wire_close(gatecrasher);
    for (int i = 0; i < 4; i++) gs_wire_close(w[i]);
    gs_wire_quit();
}

TEST(a_name_that_does_not_resolve_says_so_rather_than_hanging) {
    CHECK(gs_wire_init());

        // A key that is real, so the refusal below is about the name and not
    // about a missing key.
    static const uint8_t any_key[GS_NOISE_KEY_BYTES] = { 1 };
    gs_wire *w = gs_wire_join("no-such-host.invalid", 47824, any_key);
    CHECK(w != nullptr);
    CHECK(gs_wire_error(w) != nullptr);
    CHECK(!gs_wire_ready(w));

    // And it refuses to pretend it sent anything.
    uint8_t buf[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    CHECK(!gs_wire_send(w, buf, sizeof buf));

    gs_wire_close(w);
    gs_wire_quit();
}

int main(void) {
    gs_sandbox();
    printf("gearstick wire tests\n");

    run_two_processes_on_one_machine_race_over_real_sockets();
    run_four_machines_mesh_up_and_race_the_same_race();
    run_a_datagram_nobody_sealed_is_not_taken_for_race_traffic();
    run_a_fifth_machine_is_not_let_into_a_four_player_race();
    run_a_name_that_does_not_resolve_says_so_rather_than_hanging();

    if (gs_failures > 0) {
        printf("%d check%s failed\n", gs_failures, gs_failures == 1 ? "" : "s");
        return 1;
    }
    printf("all wire tests passed\n");
    return 0;
}

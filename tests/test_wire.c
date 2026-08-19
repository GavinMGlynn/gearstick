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

static gs_track gs_t;
static gs_net   gs_a, gs_b;

static void gs_scene(gs_world *w) {
    gs_track_init(&gs_t, 48, 20, GS_SURF_PAVEMENT);
    for (uint8_t y = 0; y <= gs_t.h; y++)
        for (uint8_t x = 0; x <= gs_t.w; x++)
            gs_track_set_corner(&gs_t, x, y, x > 20 && x < 26 ? GS_INT(1) : 0);

    gs_world_init(w, GS_ONE);
    gs_world_add_car(w, &gs_t, (uint8_t)GS_VEH_STOCK_CAR, GS_INT(4), GS_INT(9), 0);
    gs_world_add_car(w, &gs_t, (uint8_t)GS_VEH_DUNE_BUGGY, GS_INT(4), GS_INT(11), 0);
}

static gs_input gs_drive(uint8_t player, uint32_t tick) {
    gs_input in = GS_IN_ACCEL;
    if (player == 0) {
        if ((tick / 31u) % 2u == 0u) in |= GS_IN_LEFT;
        if ((tick / 77u) % 3u == 0u) in |= GS_IN_BRAKE;
    } else {
        if ((tick / 19u) % 2u == 0u) in |= GS_IN_RIGHT;
    }
    return in;
}

TEST(two_processes_on_one_machine_race_over_real_sockets) {
    CHECK(gs_wire_init());

    // A port nobody is likely to be using. The host does not know where the
    // other player is until they say something, which is the thing being
    // tested as much as the datagrams are.
    const uint16_t port = 47823;
    gs_wire *host = gs_wire_host(port);
    CHECK(host != nullptr);
    CHECK(gs_wire_error(host) == nullptr);
    CHECK(!gs_wire_connected(host));      // nobody has spoken yet

    gs_wire *join = gs_wire_join("127.0.0.1", port);
    CHECK(join != nullptr);
    CHECK(gs_wire_error(join) == nullptr);
    CHECK(gs_wire_connected(join));       // it knows where it is going

    gs_world w;
    gs_scene(&w);
    gs_net_begin(&gs_a, &w, 2, 0);
    gs_net_begin(&gs_b, &w, 2, 1);

    const uint32_t ticks = GS_TICK_HZ * 5;
    for (uint32_t tick = 0; tick < ticks; tick++) {
        uint8_t buf[GS_WIRE_MTU];
        size_t n;

        while ((n = gs_wire_recv(host, buf, sizeof buf)) > 0) {
            gs_net_receive(&gs_a, &gs_t, buf, n);
        }
        while ((n = gs_wire_recv(join, buf, sizeof buf)) > 0) {
            gs_net_receive(&gs_b, &gs_t, buf, n);
        }

        gs_net_local_input(&gs_a, gs_drive(0, tick));
        gs_net_local_input(&gs_b, gs_drive(1, tick));

        n = gs_net_packet(&gs_a, buf, sizeof buf);
        gs_wire_send(host, buf, n);       // silently does nothing until joined
        n = gs_net_packet(&gs_b, buf, sizeof buf);
        CHECK(gs_wire_send(join, buf, n));

        gs_net_step(&gs_a, &gs_t);
        gs_net_step(&gs_b, &gs_t);
    }

    // The host learned where to reply from the first datagram that arrived.
    CHECK(gs_wire_connected(host));

    // Drain whatever is still in flight, so both ends can confirm the finish.
    for (int i = 0; i < 64; i++) {
        uint8_t buf[GS_WIRE_MTU];
        size_t n;
        while ((n = gs_wire_recv(host, buf, sizeof buf)) > 0) {
            gs_net_receive(&gs_a, &gs_t, buf, n);
        }
        while ((n = gs_wire_recv(join, buf, sizeof buf)) > 0) {
            gs_net_receive(&gs_b, &gs_t, buf, n);
        }
        SDL_Delay(1);
    }

    // The claim: two sockets, no shared authority, the same race.
    CHECK(gs_a.confirmed_tick == ticks);
    CHECK(gs_b.confirmed_tick == ticks);
    CHECK(gs_world_hash(&gs_a.confirmed) == gs_world_hash(&gs_b.confirmed));
    CHECK(!gs_a.desynced);
    CHECK(!gs_b.desynced);

    // And it is the race that would have happened on one machine.
    gs_world solo;
    gs_scene(&solo);
    for (uint32_t tick = 0; tick < ticks; tick++) {
        gs_input in[GS_MAX_CARS] = { 0 };
        in[0] = gs_drive(0, tick);
        in[1] = gs_drive(1, tick);
        gs_world_step(&solo, &gs_t, in);
    }
    CHECK(gs_world_hash(&solo) == gs_world_hash(&gs_a.confirmed));

    uint32_t sent = 0, got = 0;
    gs_wire_stats(join, &sent, nullptr);
    gs_wire_stats(host, nullptr, &got);
    CHECK(sent >= ticks);
    CHECK(got > 0);

    gs_wire_close(host);
    gs_wire_close(join);
    gs_wire_quit();
}

TEST(a_name_that_does_not_resolve_says_so_rather_than_hanging) {
    CHECK(gs_wire_init());

    gs_wire *w = gs_wire_join("no-such-host.invalid", 47824);
    CHECK(w != nullptr);
    CHECK(gs_wire_error(w) != nullptr);
    CHECK(!gs_wire_connected(w));

    // And it refuses to pretend it sent anything.
    uint8_t buf[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    CHECK(!gs_wire_send(w, buf, sizeof buf));

    gs_wire_close(w);
    gs_wire_quit();
}

int main(void) {
    printf("gearstick wire tests\n");

    run_two_processes_on_one_machine_race_over_real_sockets();
    run_a_name_that_does_not_resolve_says_so_rather_than_hanging();

    if (gs_failures > 0) {
        printf("%d check%s failed\n", gs_failures, gs_failures == 1 ? "" : "s");
        return 1;
    }
    printf("all wire tests passed\n");
    return 0;
}

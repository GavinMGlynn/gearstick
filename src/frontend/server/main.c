// gearstick_server - the meeting point.
//
// **A librarian and a referee, never a player.** It holds the lobby, hands out
// player slots, will send the track and forward datagrams for peers whose
// routers will not let them talk directly - and it does not simulate a race.
// A race simulated on a server means every steering input waits a round trip,
// which is precisely what the rollback netcode exists to avoid. See the
// platform section of docs/FEATURES.md, where that line is drawn on purpose.
//
// It runs headless. SDL is initialised with no subsystems at all - SDL_net
// needs SDL, not a display - so this is something you can leave running on a
// machine with no screen, which is what a server is.
#include "net/gs_proto.h"

#include <SDL3/SDL.h>
#include <SDL3_net/SDL_net.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GS_DEFAULT_PORT 47800

// Silence for this long and a client is gone. Generous, because a client that
// is merely having a bad thirty seconds should not lose its slot - and short
// enough that a lobby of ghosts is not what somebody walks back to.
#define GS_TIMEOUT_MS 15000

// Overridable so a test can watch a connection die in seconds rather than in a
// quarter of a minute. A timeout nothing can reach is a timeout nothing tests.
static uint32_t gs_timeout_ms = GS_TIMEOUT_MS;

// How often the view is redrawn. Four times a second is fast enough to feel
// live and slow enough that a terminal over ssh is not the bottleneck.
#define GS_DRAW_MS 250

// How often the server asks each client how far away it is. The client cannot
// tell the server this - only the end that sent a ping can time its own reply -
// so a server that wants to show a round trip has to ask for one.
#define GS_PING_MS 2000

typedef struct gs_client {
    bool         used;
    NET_Address *addr;
    uint16_t     port;
    char         text[GS_PROTO_ADDR];
    char         name[GS_PROTO_NAME];

    uint64_t joined_ms;
    uint64_t last_seen_ms;
    uint32_t ping_ms;          // last measured round trip
    bool     ping_known;       // ...and whether one has been measured at all
    uint64_t pinged_ms;        // when we last asked

    uint32_t in, out;          // datagrams
    uint64_t in_bytes, out_bytes;
} gs_client;

static struct {
    NET_DatagramSocket *sock;
    uint16_t            port;
    uint8_t             capacity;

    gs_client client[GS_PROTO_MAX_PLAYERS];

    uint64_t started_ms;
    uint32_t total_in, total_out, relayed, refused;
    uint64_t total_in_bytes, total_out_bytes;
    uint8_t  peak;

    bool quit;
    bool plain;                // no ANSI, for a log file or a dumb terminal
} gs_srv;

// --- the lobby --------------------------------------------------------------

static uint8_t gs_present(void) {
    uint8_t n = 0;
    for (int i = 0; i < GS_PROTO_MAX_PLAYERS; i++) {
        if (gs_srv.client[i].used) n++;
    }
    return n;
}

// Who this datagram came from, or -1 for somebody new. Matched on address *and*
// port: two people behind one router share an address and are not the same
// player.
static int gs_find(NET_Address *addr, uint16_t port) {
    const char *text = NET_GetAddressString(addr);
    if (text == nullptr) return -1;

    for (int i = 0; i < GS_PROTO_MAX_PLAYERS; i++) {
        gs_client *c = &gs_srv.client[i];
        if (c->used && c->port == port && SDL_strcmp(c->text, text) == 0) return i;
    }
    return -1;
}

static void gs_build_lobby(gs_lobby *l) {
    SDL_zerop(l);
    l->capacity = gs_srv.capacity;
    l->count = gs_present();

    for (uint8_t i = 0; i < GS_PROTO_MAX_PLAYERS; i++) {
        const gs_client *c = &gs_srv.client[i];
        gs_lobby_player *p = &l->player[i];
        p->slot = i;
        p->present = c->used;
        if (!c->used) continue;

        SDL_strlcpy(p->name, c->name, sizeof p->name);
        SDL_strlcpy(p->addr, c->text, sizeof p->addr);
        p->port = c->port;
    }
}

static void gs_send(gs_client *c, const uint8_t *buf, size_t len) {
    if (!c->used || len == 0) return;
    if (!NET_SendDatagram(gs_srv.sock, c->addr, c->port, buf, (int)len)) return;

    c->out++;
    c->out_bytes += len;
    gs_srv.total_out++;
    gs_srv.total_out_bytes += len;
}

// Everybody hears about everybody. A lobby that only told the newcomer who was
// there would leave the people already waiting looking at a stale list.
static void gs_broadcast_lobby(void) {
    gs_lobby l;
    gs_build_lobby(&l);

    uint8_t buf[GS_PROTO_MTU];
    size_t n = gs_proto_lobby(buf, sizeof buf, &l);

    for (int i = 0; i < GS_PROTO_MAX_PLAYERS; i++) {
        gs_send(&gs_srv.client[i], buf, n);
    }
}

static void gs_drop(int slot, const char *why) {
    gs_client *c = &gs_srv.client[slot];
    if (!c->used) return;

    SDL_Log("player %d (%s) left: %s", slot, c->name, why);
    if (c->addr != nullptr) NET_UnrefAddress(c->addr);
    SDL_zerop(c);
    gs_broadcast_lobby();
}

static void gs_join(NET_Address *addr, uint16_t port, const char *name,
                    uint64_t now) {
    // Already here? Their welcome was lost, or they restarted. Either way they
    // get the answer again rather than a second slot.
    int at = gs_find(addr, port);
    if (at < 0) {
        for (int i = 0; i < gs_srv.capacity; i++) {
            if (!gs_srv.client[i].used) { at = i; break; }
        }
    }

    if (at < 0) {
        // **A refusal carries a reason.** A client that is turned away has to
        // be able to tell its user why, and "connection failed" is not a
        // reason.
        uint8_t buf[GS_PROTO_MTU];
        char why[64];
        SDL_snprintf(why, sizeof why, "the server is full (%u of %u)",
                     gs_present(), gs_srv.capacity);
        size_t n = gs_proto_full(buf, sizeof buf, why);
        NET_SendDatagram(gs_srv.sock, addr, port, buf, (int)n);

        gs_srv.refused++;
        gs_srv.total_out++;
        return;
    }

    gs_client *c = &gs_srv.client[at];
    bool fresh = !c->used;

    if (fresh) {
        SDL_zerop(c);
        c->used = true;
        c->addr = addr;
        NET_RefAddress(addr);
        c->port = port;
        SDL_strlcpy(c->text, NET_GetAddressString(addr), sizeof c->text);
        c->joined_ms = now;
    }
    SDL_strlcpy(c->name, (name != nullptr && name[0] != '\0') ? name : "driver",
                sizeof c->name);
    c->last_seen_ms = now;

    gs_lobby l;
    gs_build_lobby(&l);
    uint8_t buf[GS_PROTO_MTU];
    size_t n = gs_proto_welcome(buf, sizeof buf, (uint8_t)at, &l);
    gs_send(c, buf, n);

    if (fresh) {
        SDL_Log("player %d (%s) joined from %s:%u", at, c->name, c->text, port);
        if (gs_present() > gs_srv.peak) gs_srv.peak = gs_present();
        gs_broadcast_lobby();
    }
}

// --- the view ---------------------------------------------------------------

static void gs_bytes_text(char *out, size_t cap, uint64_t n) {
    if (n < 1024ull) SDL_snprintf(out, cap, "%llu B", (unsigned long long)n);
    else if (n < 1024ull * 1024ull) SDL_snprintf(out, cap, "%.1f KB", (double)n / 1024.0);
    else SDL_snprintf(out, cap, "%.1f MB", (double)n / (1024.0 * 1024.0));
}

static void gs_draw(uint64_t now) {
    uint64_t up = (now - gs_srv.started_ms) / 1000u;

    if (!gs_srv.plain) {
        // Home and clear-to-end rather than a full clear: a full clear makes
        // the whole view flicker, and this one redraws four times a second.
        printf("\033[H\033[J");
    }

    printf("  gearstick server            port %u        up %llu:%02llu:%02llu\n",
           gs_srv.port, (unsigned long long)(up / 3600u),
           (unsigned long long)((up / 60u) % 60u), (unsigned long long)(up % 60u));
    printf("  ------------------------------------------------------------------\n");
    printf("  %-3s %-16s %-22s %6s %8s %8s\n",
           "", "driver", "from", "ping", "in", "out");

    for (int i = 0; i < GS_PROTO_MAX_PLAYERS; i++) {
        const gs_client *c = &gs_srv.client[i];
        if (i >= gs_srv.capacity) continue;

        if (!c->used) {
            printf("  %-3d %-16s %-22s %6s %8s %8s\n", i, "-", "", "", "", "");
            continue;
        }

        char from[24];
        SDL_snprintf(from, sizeof from, "%s:%u", c->text, c->port);

        char ping[8];
        if (c->ping_known) SDL_snprintf(ping, sizeof ping, "%ums", c->ping_ms);
        else SDL_strlcpy(ping, "-", sizeof ping);

        // Silence is the thing worth seeing before it becomes a disconnection.
        uint64_t idle = (now - c->last_seen_ms) / 1000u;
        printf("  %-3d %-16s %-22s %6s %8u %8u%s\n",
               i, c->name, from, ping, c->in, c->out,
               idle >= 3u ? "  quiet" : "");
    }

    char in_b[24], out_b[24];
    gs_bytes_text(in_b, sizeof in_b, gs_srv.total_in_bytes);
    gs_bytes_text(out_b, sizeof out_b, gs_srv.total_out_bytes);

    printf("  ------------------------------------------------------------------\n");
    printf("  %u of %u here, peak %u        refused %u\n",
           gs_present(), gs_srv.capacity, gs_srv.peak, gs_srv.refused);
    printf("  datagrams  in %u (%s)   out %u (%s)   relayed %u\n",
           gs_srv.total_in, in_b, gs_srv.total_out, out_b, gs_srv.relayed);

    if (up > 0) {
        printf("  rate       %.1f in/s   %.1f out/s\n",
               (double)gs_srv.total_in / (double)up,
               (double)gs_srv.total_out / (double)up);
    }
    printf("\n  ctrl-c to stop\n");
    fflush(stdout);
}

// --- one datagram -----------------------------------------------------------

static void gs_handle(NET_Datagram *d, uint64_t now) {
    size_t len = (size_t)d->buflen;

    gs_srv.total_in++;
    gs_srv.total_in_bytes += len;

    gs_msg kind = gs_proto_kind(d->buf, len);
    if (kind == GS_MSG_NONE) return;      // not ours; say nothing back

    int at = gs_find(d->addr, d->port);
    if (at >= 0) {
        gs_client *c = &gs_srv.client[at];
        c->in++;
        c->in_bytes += len;
        c->last_seen_ms = now;
    }

    switch (kind) {
    case GS_MSG_JOIN: {
        char name[GS_PROTO_NAME];
        if (gs_proto_read_join(d->buf, len, name, sizeof name)) {
            gs_join(d->addr, d->port, name, now);
        }
        break;
    }

    case GS_MSG_BYE:
        if (at >= 0) gs_drop(at, "said goodbye");
        break;

    case GS_MSG_PING: {
        // Answered with the client's own stamp, so the client measures the
        // round trip rather than the server guessing at it.
        uint32_t stamp = 0;
        if (at >= 0 && gs_proto_read_stamp(d->buf, len, &stamp)) {
            uint8_t buf[GS_PROTO_MTU];
            size_t n = gs_proto_pong(buf, sizeof buf, stamp);
            gs_send(&gs_srv.client[at], buf, n);
        }
        break;
    }

    case GS_MSG_PONG: {
        // The other direction: a reply to something we asked, so the stamp is
        // ours and the difference is the round trip.
        uint32_t stamp = 0;
        if (at >= 0 && gs_proto_read_stamp(d->buf, len, &stamp)) {
            gs_client *c = &gs_srv.client[at];
            uint32_t then = stamp;
            c->ping_ms = (uint32_t)(now & 0xffffffffu) - then;
            c->ping_known = true;
        }
        break;
    }

    default:
        // Everything else belongs to items not built yet - the track, the
        // relay, records. Ignored rather than guessed at.
        break;
    }
}

// --- running ----------------------------------------------------------------

static void gs_on_signal(int sig) {
    (void)sig;
    gs_srv.quit = true;
}

static void gs_usage(void) {
    printf("gearstick_server - the meeting point for online races\n\n");
    printf("  --port N       listen on this port (default %u)\n", GS_DEFAULT_PORT);
    printf("  --players N    how many to allow, 1 to %d (default %d)\n",
           GS_PROTO_MAX_PLAYERS, GS_PROTO_MAX_PLAYERS);
    printf("  --plain        no cursor control, for a log file\n");
    printf("  --timeout N    drop a client after N ms of silence (default %u)\n",
           GS_TIMEOUT_MS);
    printf("  --seconds N    stop after N seconds, for tests\n");
    printf("  --help\n");
}

int main(int argc, char **argv) {
    uint16_t port = GS_DEFAULT_PORT;
    uint8_t players = GS_PROTO_MAX_PLAYERS;
    uint32_t seconds = 0;

    for (int i = 1; i < argc; i++) {
        if (SDL_strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = (uint16_t)SDL_atoi(argv[++i]);
        } else if (SDL_strcmp(argv[i], "--players") == 0 && i + 1 < argc) {
            int n = SDL_atoi(argv[++i]);
            players = (uint8_t)SDL_clamp(n, 1, GS_PROTO_MAX_PLAYERS);
        } else if (SDL_strcmp(argv[i], "--plain") == 0) {
            gs_srv.plain = true;
        } else if (SDL_strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            gs_timeout_ms = (uint32_t)SDL_atoi(argv[++i]);
        } else if (SDL_strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = (uint32_t)SDL_atoi(argv[++i]);
        } else if (SDL_strcmp(argv[i], "--help") == 0) {
            gs_usage();
            return 0;
        } else {
            printf("unknown option: %s\n\n", argv[i]);
            gs_usage();
            return 2;
        }
    }

    // No subsystems at all. SDL_net needs SDL initialised; it does not need a
    // display, and a server that demanded one could not run where servers run.
    if (!SDL_Init(0)) {
        printf("could not start SDL: %s\n", SDL_GetError());
        return 1;
    }
    if (!NET_Init()) {
        printf("could not start networking: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    gs_srv.sock = NET_CreateDatagramSocket(nullptr, port, 0);
    if (gs_srv.sock == nullptr) {
        printf("could not listen on port %u: %s\n", port, SDL_GetError());
        NET_Quit();
        SDL_Quit();
        return 1;
    }

    gs_srv.port = port;
    gs_srv.capacity = players;
    gs_srv.started_ms = SDL_GetTicks();

    signal(SIGINT, gs_on_signal);
    signal(SIGTERM, gs_on_signal);

    SDL_Log("gearstick server listening on port %u for up to %u players",
            port, players);

    uint64_t last_draw = 0;
    while (!gs_srv.quit) {
        uint64_t now = SDL_GetTicks();

        NET_Datagram *d = nullptr;
        while (NET_ReceiveDatagram(gs_srv.sock, &d) && d != nullptr) {
            gs_handle(d, now);
            NET_DestroyDatagram(d);
            d = nullptr;
        }

        // Silence for long enough is a departure. Without this a lobby fills
        // with people who closed their laptop, and the fifth person who
        // actually wants to play is told it is full.
        for (int i = 0; i < GS_PROTO_MAX_PLAYERS; i++) {
            gs_client *c = &gs_srv.client[i];
            if (!c->used) continue;

            if (now - c->last_seen_ms > gs_timeout_ms) {
                gs_drop(i, "went quiet");
                continue;
            }

            if (now - c->pinged_ms >= GS_PING_MS) {
                uint8_t buf[GS_PROTO_MTU];
                size_t n = gs_proto_ping(buf, sizeof buf,
                                         (uint32_t)(now & 0xffffffffu));
                gs_send(c, buf, n);
                c->pinged_ms = now;
            }
        }

        if (now - last_draw >= GS_DRAW_MS) {
            gs_draw(now);
            last_draw = now;
        }

        if (seconds > 0 && (now - gs_srv.started_ms) / 1000u >= seconds) break;

        SDL_Delay(5);
    }

    printf("\nstopping\n");
    for (int i = 0; i < GS_PROTO_MAX_PLAYERS; i++) {
        if (gs_srv.client[i].addr != nullptr) NET_UnrefAddress(gs_srv.client[i].addr);
    }
    NET_DestroyDatagramSocket(gs_srv.sock);
    NET_Quit();
    SDL_Quit();
    return 0;
}

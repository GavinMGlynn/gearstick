#include "platform/gs_wire.h"

#include <SDL3/SDL.h>
#include <SDL3_net/SDL_net.h>

// --- the handshake ----------------------------------------------------------
//
// Two control datagrams, both carrying a magic that is *not* the netcode's, so
// a rollback session that ever saw one would reject it rather than read it as
// input.
//
//   HELLO    joiner -> host, repeated until answered. Carries nothing: the host
//            learns where the joiner is from the datagram itself.
//   ROSTER   host -> joiner, personalised. Says how many players there are,
//            which one the recipient is, and where all the others live.
//
// Sent again and again rather than acknowledged, because that is the whole
// vocabulary here: a lost HELLO is followed by another one 250 ms later, and a
// lost ROSTER by another, and neither costs a round trip to discover.

#define GS_CTRL_MAGIC  0x54435347u   // "GSCT"
#define GS_MSG_HELLO   1u
#define GS_MSG_ROSTER  2u

#define GS_ADDR_MAX    64
#define GS_RETRY_TICKS 15            // about a quarter second at 60 fps

typedef struct gs_peer {
    NET_Address *addr;
    uint16_t     port;
    bool         known;
    char         text[GS_ADDR_MAX];   // how it was written down, for the roster
} gs_peer;

struct gs_wire {
    NET_DatagramSocket *sock;

    bool    hosting;
    uint8_t players;      // how many there will be when everybody has arrived
    uint8_t local;        // which one this machine is
    bool    ready;

    gs_peer peer[GS_WIRE_PLAYERS];

    // Joining: where the host is, and how it was written, so the roster's
    // empty slot-zero entry can be filled in from what we already know.
    char     host_text[GS_ADDR_MAX];
    uint16_t host_port;

    uint32_t retry;
    uint32_t sent, received;
    char     error[256];
};

static int gs_wire_users = 0;

bool gs_wire_init(void) {
    if (gs_wire_users++ > 0) return true;
    if (!NET_Init()) {
        gs_wire_users--;
        return false;
    }
    return true;
}

void gs_wire_quit(void) {
    if (gs_wire_users == 0) return;
    if (--gs_wire_users == 0) NET_Quit();
}

// --- little-endian on the wire, like everything else in this project --------

static void gs_put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static uint16_t gs_get16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static void gs_put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xffu);
}

static uint32_t gs_get32(const uint8_t *p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}

// --- opening ----------------------------------------------------------------

static gs_wire *gs_wire_new(uint16_t port) {
    gs_wire *w = (gs_wire *)SDL_calloc(1, sizeof *w);
    if (w == nullptr) return nullptr;

    // Bound to every interface: a host has no idea which one the other players
    // will arrive on, and nothing here is worth restricting.
    w->sock = NET_CreateDatagramSocket(nullptr, port, 0);
    if (w->sock == nullptr) {
        SDL_snprintf(w->error, sizeof w->error, "could not open port %u: %s",
                     port, SDL_GetError());
    }
    return w;
}

gs_wire *gs_wire_host(uint16_t port, uint8_t players) {
    gs_wire *w = gs_wire_new(port);
    if (w == nullptr) return nullptr;

    w->hosting = true;
    w->local = 0;
    w->players = (uint8_t)SDL_clamp(players, 2, GS_WIRE_PLAYERS);
    w->peer[0].known = true;      // ourselves, trivially

    // One player short of nobody: a host waiting for one other person is ready
    // the moment they arrive, and a host waiting for three waits for three.
    return w;
}

gs_wire *gs_wire_join(const char *host, uint16_t port) {
    gs_wire *w = gs_wire_new(0);
    if (w == nullptr || w->sock == nullptr) return w;

    w->peer[0].addr = NET_ResolveHostname(host);
    if (w->peer[0].addr == nullptr ||
        NET_WaitUntilResolved(w->peer[0].addr, 5000) != NET_SUCCESS) {
        SDL_snprintf(w->error, sizeof w->error, "could not look up %s: %s",
                     host, SDL_GetError());
        if (w->peer[0].addr != nullptr) NET_UnrefAddress(w->peer[0].addr);
        w->peer[0].addr = nullptr;
        return w;
    }

    w->peer[0].port = port;
    w->peer[0].known = true;
    w->local = 0xffu;                       // until the host says otherwise
    SDL_strlcpy(w->host_text, host, sizeof w->host_text);
    w->host_port = port;
    return w;
}

void gs_wire_close(gs_wire *w) {
    if (w == nullptr) return;
    for (int i = 0; i < GS_WIRE_PLAYERS; i++) {
        if (w->peer[i].addr != nullptr) NET_UnrefAddress(w->peer[i].addr);
    }
    if (w->sock != nullptr) NET_DestroyDatagramSocket(w->sock);
    SDL_free(w);
}

bool gs_wire_ready(const gs_wire *w) { return w != nullptr && w->ready; }
uint8_t gs_wire_local(const gs_wire *w) { return w != nullptr ? w->local : 0; }
uint8_t gs_wire_players(const gs_wire *w) { return w != nullptr ? w->players : 0; }

uint8_t gs_wire_present(const gs_wire *w) {
    if (w == nullptr) return 0;
    uint8_t n = 0;
    for (int i = 0; i < GS_WIRE_PLAYERS; i++) {
        if (w->peer[i].known) n++;
    }
    return n;
}

// --- the handshake ----------------------------------------------------------

static void gs_send_to(gs_wire *w, const gs_peer *p, const uint8_t *buf, size_t n) {
    if (p->addr == nullptr) return;
    NET_SendDatagram(w->sock, p->addr, p->port, buf, (int)n);
}

static void gs_send_hello(gs_wire *w) {
    uint8_t buf[5];
    gs_put32(buf, GS_CTRL_MAGIC);
    buf[4] = (uint8_t)GS_MSG_HELLO;
    gs_send_to(w, &w->peer[0], buf, sizeof buf);
}

// The roster, personalised: everybody needs to be told which player they are.
//
// Slot zero's address is deliberately sent empty. The host does not know what
// its own address looks like from outside - behind a router it is not the one
// it is bound to - and every joiner already knows it, because they typed it.
// So they fill that entry in themselves.
static void gs_send_roster(gs_wire *w, uint8_t to) {
    uint8_t buf[GS_WIRE_MTU];
    size_t n = 0;

    gs_put32(buf, GS_CTRL_MAGIC); n += 4;
    buf[n++] = (uint8_t)GS_MSG_ROSTER;
    buf[n++] = w->players;
    buf[n++] = to;

    for (uint8_t i = 0; i < w->players; i++) {
        const char *text = (i == 0) ? "" : w->peer[i].text;
        size_t len = SDL_strlen(text);
        if (n + 1 + len + 2 > sizeof buf) return;

        buf[n++] = (uint8_t)len;
        SDL_memcpy(buf + n, text, len); n += len;
        gs_put16(buf + n, w->peer[i].port); n += 2;
    }

    gs_send_to(w, &w->peer[to], buf, n);
}

static void gs_take_roster(gs_wire *w, const uint8_t *buf, size_t len) {
    if (w->hosting || len < 7) return;

    size_t n = 5;
    uint8_t players = buf[n++];
    uint8_t slot = buf[n++];
    if (players < 2 || players > GS_WIRE_PLAYERS || slot >= players) return;

    for (uint8_t i = 0; i < players; i++) {
        if (n >= len) return;
        size_t tlen = buf[n++];
        if (n + tlen + 2 > len || tlen >= GS_ADDR_MAX) return;

        char text[GS_ADDR_MAX];
        SDL_memcpy(text, buf + n, tlen);
        text[tlen] = '\0';
        n += tlen;
        uint16_t port = gs_get16(buf + n); n += 2;

        if (i == slot) continue;                 // no need to reach ourselves

        // Slot zero arrives empty on purpose - see gs_send_roster.
        const char *want = (i == 0) ? w->host_text : text;
        if (i == 0) port = w->host_port;
        if (want[0] == '\0') continue;

        if (w->peer[i].known && w->peer[i].addr != nullptr) continue;

        NET_Address *a = NET_ResolveHostname(want);
        if (a == nullptr || NET_WaitUntilResolved(a, 3000) != NET_SUCCESS) {
            if (a != nullptr) NET_UnrefAddress(a);
            SDL_snprintf(w->error, sizeof w->error,
                         "could not reach player %u at %s", i, want);
            return;
        }
        if (w->peer[i].addr != nullptr) NET_UnrefAddress(w->peer[i].addr);
        w->peer[i].addr = a;
        w->peer[i].port = port;
        w->peer[i].known = true;
        SDL_strlcpy(w->peer[i].text, want, sizeof w->peer[i].text);
    }

    w->players = players;
    w->local = slot;
    w->peer[slot].known = true;

    uint8_t have = 0;
    for (uint8_t i = 0; i < players; i++) {
        if (w->peer[i].known) have++;
    }
    w->ready = have == players;
}

// A joiner the host has not seen before, taken from the datagram it sent.
static void gs_take_hello(gs_wire *w, NET_Address *from, uint16_t port) {
    if (!w->hosting) return;

    const char *text = NET_GetAddressString(from);
    if (text == nullptr) return;

    for (uint8_t i = 1; i < w->players; i++) {
        if (w->peer[i].known && w->peer[i].port == port &&
            SDL_strcmp(w->peer[i].text, text) == 0) {
            // Already known - their hello was lost or ours was. Say it again.
            gs_send_roster(w, i);
            return;
        }
    }

    for (uint8_t i = 1; i < w->players; i++) {
        if (w->peer[i].known) continue;

        w->peer[i].addr = from;
        NET_RefAddress(from);
        w->peer[i].port = port;
        w->peer[i].known = true;
        SDL_strlcpy(w->peer[i].text, text, sizeof w->peer[i].text);

        // Everybody who is already here gets told again, because the roster
        // they were sent was the roster before this person arrived.
        w->ready = gs_wire_present(w) == w->players;
        for (uint8_t k = 1; k < w->players; k++) {
            if (w->peer[k].known) gs_send_roster(w, k);
        }
        return;
    }
    // Full. Nothing is sent back: an uninvited machine gets silence rather than
    // a reply telling it there is a game here.
}

void gs_wire_poll(gs_wire *w) {
    if (w == nullptr || w->sock == nullptr || w->ready) return;

    // Joiners knock until answered. The host has nothing to say until somebody
    // knocks, so it only listens.
    if (!w->hosting && w->retry++ % GS_RETRY_TICKS == 0) gs_send_hello(w);

    uint8_t buf[GS_WIRE_MTU];
    while (gs_wire_recv(w, buf, sizeof buf) > 0) {
        // Data arriving before the handshake finished is dropped: there is
        // nothing to hand it to yet.
    }
}

// --- carrying the race ------------------------------------------------------

bool gs_wire_send(gs_wire *w, const uint8_t *buf, size_t len) {
    if (w == nullptr || w->sock == nullptr || len == 0 || len > GS_WIRE_MTU) {
        return false;
    }

    // To everybody else, directly. The rollback packet is identical for every
    // peer - it is this machine's own inputs - so this is one call rather than
    // one per peer, and it is a mesh rather than a relay because a relay would
    // put the host's latency between two clients who can see each other.
    bool any = false;
    for (uint8_t i = 0; i < w->players; i++) {
        if (i == w->local || !w->peer[i].known || w->peer[i].addr == nullptr) {
            continue;
        }
        if (NET_SendDatagram(w->sock, w->peer[i].addr, w->peer[i].port, buf,
                             (int)len)) {
            any = true;
        } else {
            SDL_snprintf(w->error, sizeof w->error, "send failed: %s",
                         SDL_GetError());
        }
    }
    if (any) w->sent++;
    return any;
}

size_t gs_wire_recv(gs_wire *w, uint8_t *buf, size_t cap) {
    if (w == nullptr || w->sock == nullptr) return 0;

    for (;;) {
        NET_Datagram *d = nullptr;
        if (!NET_ReceiveDatagram(w->sock, &d) || d == nullptr) return 0;

        size_t n = (size_t)d->buflen;

        // The handshake's own traffic never reaches the caller.
        if (n >= 5 && gs_get32(d->buf) == GS_CTRL_MAGIC) {
            if (d->buf[4] == GS_MSG_HELLO) {
                gs_take_hello(w, d->addr, d->port);
            } else if (d->buf[4] == GS_MSG_ROSTER) {
                gs_take_roster(w, d->buf, n);
            }
            NET_DestroyDatagram(d);
            continue;
        }

        if (n > cap) n = cap;
        SDL_memcpy(buf, d->buf, n);
        NET_DestroyDatagram(d);
        w->received++;
        return n;
    }
}

void gs_wire_stats(const gs_wire *w, uint32_t *sent, uint32_t *received) {
    if (sent != nullptr) *sent = w != nullptr ? w->sent : 0;
    if (received != nullptr) *received = w != nullptr ? w->received : 0;
}

const char *gs_wire_error(const gs_wire *w) {
    return (w != nullptr && w->error[0] != '\0') ? w->error : nullptr;
}

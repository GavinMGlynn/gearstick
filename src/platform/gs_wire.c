#include "platform/gs_wire.h"

#include <SDL3/SDL.h>
#include <SDL3_net/SDL_net.h>

// One socket, one peer. Two players is what the game is for and adding a third
// is not a matter of a bigger array here - it is a different rollback problem -
// so the structure says two rather than pretending otherwise.
struct gs_wire {
    NET_DatagramSocket *sock;
    NET_Address        *peer;
    uint16_t            peer_port;
    bool                joined;      // we know where they are
    uint32_t            sent, received;
    char                error[256];
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

static gs_wire *gs_wire_new(uint16_t port) {
    gs_wire *w = (gs_wire *)SDL_calloc(1, sizeof *w);
    if (w == nullptr) return nullptr;

    // Bound to every interface: a host has no idea which one the other player
    // will arrive on, and nothing here is worth restricting.
    w->sock = NET_CreateDatagramSocket(nullptr, port, 0);
    if (w->sock == nullptr) {
        SDL_snprintf(w->error, sizeof w->error, "could not open port %u: %s",
                     port, SDL_GetError());
        return w;
    }
    return w;
}

gs_wire *gs_wire_host(uint16_t port) {
    // The host learns where the other player is from the first datagram that
    // arrives, so there is nothing to type at this end. That also means a host
    // is not "connected" until somebody has actually said something.
    return gs_wire_new(port);
}

gs_wire *gs_wire_join(const char *host, uint16_t port) {
    gs_wire *w = gs_wire_new(0);
    if (w == nullptr || w->sock == nullptr) return w;

    w->peer = NET_ResolveHostname(host);
    if (w->peer == nullptr) {
        SDL_snprintf(w->error, sizeof w->error, "could not look up %s: %s",
                     host, SDL_GetError());
        return w;
    }

    // Resolution is asynchronous. Five seconds is long past the point where a
    // name that is going to resolve has, and short enough that somebody who
    // typed it wrong finds out while they still remember typing it.
    if (NET_WaitUntilResolved(w->peer, 5000) != NET_SUCCESS) {
        SDL_snprintf(w->error, sizeof w->error, "could not look up %s: %s",
                     host, SDL_GetError());
        NET_UnrefAddress(w->peer);
        w->peer = nullptr;
        return w;
    }

    w->peer_port = port;
    w->joined = true;
    return w;
}

void gs_wire_close(gs_wire *w) {
    if (w == nullptr) return;
    if (w->peer != nullptr) NET_UnrefAddress(w->peer);
    if (w->sock != nullptr) NET_DestroyDatagramSocket(w->sock);
    SDL_free(w);
}

bool gs_wire_connected(const gs_wire *w) {
    return w != nullptr && w->sock != nullptr && w->joined;
}

bool gs_wire_send(gs_wire *w, const uint8_t *buf, size_t len) {
    if (!gs_wire_connected(w) || len == 0 || len > GS_WIRE_MTU) return false;

    // A datagram that leaves and never arrives is a success here. Reporting it
    // as a failure would invite somebody to retry it, and a retry is a round
    // trip - the one thing this whole design is arranged to avoid.
    if (!NET_SendDatagram(w->sock, w->peer, w->peer_port, buf, (int)len)) {
        SDL_snprintf(w->error, sizeof w->error, "send failed: %s", SDL_GetError());
        return false;
    }
    w->sent++;
    return true;
}

size_t gs_wire_recv(gs_wire *w, uint8_t *buf, size_t cap) {
    if (w == nullptr || w->sock == nullptr) return 0;

    NET_Datagram *d = nullptr;
    if (!NET_ReceiveDatagram(w->sock, &d) || d == nullptr) return 0;

    size_t n = (size_t)d->buflen;
    if (n > cap) n = cap;
    SDL_memcpy(buf, d->buf, n);

    // A host finds out where to reply by being spoken to. Only the first
    // speaker is adopted: once there is a peer, datagrams from anywhere else
    // are read and answered to the peer, not to whoever sent them, which is
    // what stops a stranger redirecting a race in progress.
    if (!w->joined) {
        w->peer = d->addr;
        NET_RefAddress(w->peer);
        w->peer_port = d->port;
        w->joined = true;
    }

    NET_DestroyDatagram(d);
    w->received++;
    return n;
}

void gs_wire_stats(const gs_wire *w, uint32_t *sent, uint32_t *received) {
    if (sent != nullptr) *sent = w != nullptr ? w->sent : 0;
    if (received != nullptr) *received = w != nullptr ? w->received : 0;
}

const char *gs_wire_error(const gs_wire *w) {
    return (w != nullptr && w->error[0] != '\0') ? w->error : nullptr;
}

#include "platform/gs_wire.h"

#include "net/gs_noise.h"
#include "net/gs_proto.h"

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

    // **The sealed channel to this peer.** A race between two machines that can
    // see each other goes straight between them, so the tunnel to the server
    // protects none of it - the inputs, and with them everything a state hash
    // is computed from, would cross in the clear.
    //
    // The key comes from whoever brokered the meeting: the server's lobby, or
    // the host's roster, or the command line for the host itself. It is never
    // taken from the peer's own say-so, because a key somebody hands you about
    // themselves authenticates nothing.
    uint8_t            key[GS_NOISE_KEY_BYTES];
    bool               has_key;
    bool               greeted;       // a handshake has been started with them
    gs_noise_handshake hs;
    gs_noise_session   tunnel;
} gs_peer;

struct gs_wire {
    NET_DatagramSocket *sock;

    // Meeting at a server rather than at each other. The peer handshake above
    // is untouched by this: a server connection is a different way of finding
    // out who is here, and once everybody is found the two are the same thing.
    bool     via_server;

    // **The server is not a player and does not live in the peer table.** It
    // did, briefly, sharing slot zero - and then the roster arrived and
    // overwrote slot zero with player zero's address, so every client that was
    // not placed first lost the server entirely and was dropped for silence.
    NET_Address *server_addr;
    uint16_t     server_port;

    char     me[GS_PROTO_NAME];

    // The track the server says this lobby races on, and the pieces of it as
    // they turn up.
    uint64_t   want_track;
    bool       heard_start;    // the server has said what the race is on
    bool       relay;          // everything goes through the server
    gs_wire_best best;         // what the server says stands here

    gs_wire_listing listing[GS_WIRE_LISTINGS];
    uint16_t        listings;   // how many have arrived
    uint16_t        listed;     // how many the server said there were
    gs_carrier carrier;
    uint32_t   asked_at;

    bool     refused;
    char     refusal[64];
    gs_lobby lobby;

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

    // When this client last told the server it is still here, in milliseconds.
    // A clock rather than a count of frames: see GS_HEARTBEAT_MS.
    uint64_t last_ping;

    // The one-shot token the server last issued, spent by the next claim. Zero
    // means none has arrived, and a claim carrying zero is refused.
    uint64_t session;

    // **The sealed channel to the server, and this client's own identity.**
    //
    // The identity is generated per connection rather than kept: the server
    // learns it during the handshake and nothing yet depends on it being the
    // same next time. It becomes a profile's long-term key when there are
    // accounts, which is a later item, and the handshake pattern does not
    // change when it does.
    gs_noise_keypair   identity;
    uint8_t            server_key[GS_NOISE_KEY_BYTES];
    bool               has_server_key;
    gs_noise_handshake hs;
    gs_noise_session   tunnel;
    uint32_t           handshakes_sent;
    uint32_t sent, received;
    char     error[256];
};

// Sending to the server, which is not a peer. Declared here because both the
// handshake below and the accessors above it need it.
static void gs_to_server(gs_wire *w, const uint8_t *buf, size_t n);

// Is the ground agreed? **Not knowing counts as no.** A client that has not yet
// been told what the race is on cannot tell that from having been told there is
// nothing to fetch, and one that guessed would race on whatever it had loaded.
static bool gs_wire_settled(const gs_wire *w) {
    if (!w->heard_start) return false;
    return w->want_track == 0 || gs_carrier_done(&w->carrier);
}

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

    // The host is the one everybody else has to already know, so it has an
    // identity for the same reason the server does. `gs_wire_public_key` hands
    // it out for a person to pass on.
    gs_noise_keygen(&w->identity);

    // One player short of nobody: a host waiting for one other person is ready
    // the moment they arrive, and a host waiting for three waits for three.
    return w;
}

gs_wire *gs_wire_server(const char *host, uint16_t port, const char *name,
                        const uint8_t *server_key) {
    gs_wire *w = gs_wire_new(0);
    if (w == nullptr || w->sock == nullptr) return w;

    // **Without the server's key there is no connection at all.** IK means the
    // client already knows who it is talking to, and that is the whole reason
    // nobody can answer in the server's place. A client that would carry on
    // regardless would be a client that connects to whoever replies first.
    if (server_key == nullptr) {
        SDL_snprintf(w->error, sizeof w->error,
                     "no server key: this client cannot tell %s from anybody "
                     "else claiming to be it", host);
        return w;
    }
    SDL_memcpy(w->server_key, server_key, GS_NOISE_KEY_BYTES);
    w->has_server_key = true;
    gs_noise_keygen(&w->identity);

    w->server_addr = NET_ResolveHostname(host);
    if (w->server_addr == nullptr ||
        NET_WaitUntilResolved(w->server_addr, 5000) != NET_SUCCESS) {
        SDL_snprintf(w->error, sizeof w->error, "could not look up %s: %s",
                     host, SDL_GetError());
        if (w->server_addr != nullptr) NET_UnrefAddress(w->server_addr);
        w->server_addr = nullptr;
        return w;
    }

    w->server_port = port;
    w->via_server = true;
    w->local = 0xffu;
    w->players = 0;
    SDL_strlcpy(w->me, (name != nullptr && name[0] != '\0') ? name : "driver",
                sizeof w->me);
    SDL_strlcpy(w->host_text, host, sizeof w->host_text);
    w->host_port = port;
    return w;
}

void gs_wire_send_result(gs_wire *w, uint64_t track, uint64_t conditions,
                         uint16_t laps, uint8_t vehicle, uint32_t lap_ticks,
                         uint32_t race_ticks, const uint8_t *proof,
                         size_t proof_len) {
    if (w == nullptr || !w->via_server) return;

    // **The token the server last handed out.** A claim without one, or with a
    // stale one, buys nothing - so a client that has not been given a session
    // yet sends zero and is refused, which is the honest outcome rather than a
    // quietly accepted claim.
    uint8_t buf[GS_PROTO_MTU];
    gs_to_server(w, buf,
                 gs_proto_result(buf, sizeof buf, track, conditions, laps,
                                 vehicle, lap_ticks, race_ticks, w->session));

    // The claim first, then what backs it. The server holds the one until it
    // has the other, so the order matters and the reverse would be a proof
    // arriving for a claim nobody made.
    if (proof == nullptr || proof_len == 0) return;

    uint16_t chunks = gs_carrier_chunks(proof_len);
    for (uint16_t i = 0; i < chunks; i++) {
        size_t at = (size_t)i * GS_CHUNK_BYTES;
        size_t take = proof_len - at;
        if (take > GS_CHUNK_BYTES) take = GS_CHUNK_BYTES;

        size_t n = gs_proto_proof_chunk(buf, sizeof buf, track, i, chunks,
                                        proof + at, (uint16_t)take);
        gs_to_server(w, buf, n);
    }
}

void gs_wire_ask_best(gs_wire *w, uint64_t track, uint64_t conditions,
                      uint16_t laps) {
    if (w == nullptr || !w->via_server) return;
    uint8_t buf[GS_PROTO_MTU];
    gs_to_server(w, buf,
                 gs_proto_want_best(buf, sizeof buf, track, conditions, laps));
}

const gs_wire_best *gs_wire_best_here(const gs_wire *w) {
    return (w != nullptr && w->via_server) ? &w->best : nullptr;
}

void gs_wire_ask_track(gs_wire *w, uint64_t hash) {
    if (w == nullptr || !w->via_server || hash == 0) return;

    w->want_track = hash;
    gs_carrier_expect(&w->carrier, hash);
    w->asked_at = w->retry;

    uint8_t buf[GS_PROTO_MTU];
    gs_to_server(w, buf, gs_proto_want_track(buf, sizeof buf, hash));
}

void gs_wire_publish(gs_wire *w, const gs_track *t, const char *name) {
    if (w == nullptr || !w->via_server) return;

    static uint8_t bytes[GS_CARRIER_MAX_BYTES];
    size_t len = gs_track_serialize(t, bytes, sizeof bytes);
    if (len == 0) return;

    uint64_t hash = gs_track_hash(t);
    uint16_t chunks = gs_carrier_chunks(len);

    uint8_t buf[GS_PROTO_MTU];
    for (uint16_t i = 0; i < chunks; i++) {
        gs_to_server(w, buf, gs_carrier_chunk(buf, sizeof buf, hash, bytes, len, i));
    }

    // The track first, then the claim about it. The server publishes only
    // things it already has, so the reverse would be a claim about nothing.
    gs_to_server(w, buf, gs_proto_publish(buf, sizeof buf, hash, name));
}

void gs_wire_withdraw(gs_wire *w, uint64_t track) {
    if (w == nullptr || !w->via_server) return;
    uint8_t buf[GS_PROTO_MTU];
    gs_to_server(w, buf, gs_proto_withdraw(buf, sizeof buf, track));
}

void gs_wire_login(gs_wire *w, const char *name, const char *password,
                   uint32_t code) {
    if (w == nullptr || !w->via_server || name == nullptr) return;
    uint8_t buf[GS_PROTO_MTU];
    gs_to_server(w, buf,
                 gs_proto_login(buf, sizeof buf, name,
                                password != nullptr ? password : "", code));

    // The name this client will be known by from here on, so the caller does
    // not have to wait for a lobby to find out what it asked for.
    SDL_strlcpy(w->me, name, sizeof w->me);
}

void gs_wire_share(gs_wire *w, uint64_t track, const uint8_t *with, bool on) {
    if (w == nullptr || !w->via_server || with == nullptr) return;
    uint8_t buf[GS_PROTO_MTU];
    gs_to_server(w, buf, gs_proto_share(buf, sizeof buf, track, with, on));
}

const uint8_t *gs_wire_peer_key(const gs_wire *w, uint8_t slot) {
    if (w == nullptr || slot >= GS_PROTO_MAX_PLAYERS) return nullptr;
    const gs_lobby_player *p = &w->lobby.player[slot];
    return p->present ? p->key : nullptr;
}

void gs_wire_ask_published(gs_wire *w) {
    if (w == nullptr || !w->via_server) return;
    uint8_t buf[GS_PROTO_MTU];
    gs_to_server(w, buf, gs_proto_want_list(buf, sizeof buf));
}

uint16_t gs_wire_published(const gs_wire *w, const gs_wire_listing **out,
                           uint16_t *total) {
    if (w == nullptr || !w->via_server) return 0;
    if (out != nullptr) *out = w->listing;
    if (total != nullptr) *total = w->listed;
    return w->listings;
}

void gs_wire_use_relay(gs_wire *w, bool on) {
    if (w != nullptr && w->via_server) w->relay = on;
}

bool gs_wire_relaying(const gs_wire *w) {
    return w != nullptr && w->relay;
}

uint64_t gs_wire_track_hash(const gs_wire *w) {
    return w != nullptr ? w->want_track : 0;
}

bool gs_wire_track(const gs_wire *w, gs_track *out) {
    if (w == nullptr) return false;
    return gs_carrier_track(&w->carrier, out);
}

float gs_wire_track_progress(const gs_wire *w) {
    return w != nullptr ? gs_carrier_progress(&w->carrier) : 0.0f;
}

void gs_wire_want_track(gs_wire *w) {
    if (w == nullptr || w->want_track == 0) return;
    uint8_t buf[GS_PROTO_MTU];
    gs_to_server(w, buf, gs_proto_want_track(buf, sizeof buf, w->want_track));
    w->asked_at = w->retry;
}

bool gs_wire_refused(const gs_wire *w) { return w != nullptr && w->refused; }

const char *gs_wire_refusal(const gs_wire *w) {
    return (w != nullptr && w->refused) ? w->refusal : nullptr;
}

const gs_lobby *gs_wire_lobby(const gs_wire *w) {
    return (w != nullptr && w->via_server) ? &w->lobby : nullptr;
}

gs_wire *gs_wire_join(const char *host, uint16_t port, const uint8_t *host_key) {
    gs_wire *w = gs_wire_new(0);
    if (w == nullptr || w->sock == nullptr) return w;

    // The same rule as the server: a client with no key for the far end is a
    // client that would race whoever answered first.
    if (host_key == nullptr) {
        SDL_snprintf(w->error, sizeof w->error,
                     "no host key: this client cannot tell %s from anybody "
                     "else claiming to be it", host);
        return w;
    }
    gs_noise_keygen(&w->identity);
    SDL_memcpy(w->peer[0].key, host_key, GS_NOISE_KEY_BYTES);
    w->peer[0].has_key = true;

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

    // **Say goodbye on the way out.** The protocol has had a message for this
    // since it was written and nothing ever sent one, so a player who quit was
    // noticed only when they had been silent long enough to look crashed -
    // fifteen seconds of everybody else racing a car that was not there. It is
    // one datagram and it is not required to arrive: the silence timeout is
    // still what catches a client that was unplugged rather than closed.
    if (w->server_addr != nullptr) {
        // Sealed, like everything else. A goodbye in the clear would be one
        // anybody on the path could forge, and forging one throws somebody out
        // of their own race.
        uint8_t buf[GS_PROTO_MTU];
        gs_to_server(w, buf, gs_proto_bye(buf, sizeof buf));
        NET_UnrefAddress(w->server_addr);
    }
    for (int i = 0; i < GS_WIRE_PLAYERS; i++) {
        if (w->peer[i].addr != nullptr) NET_UnrefAddress(w->peer[i].addr);
    }
    if (w->sock != nullptr) NET_DestroyDatagramSocket(w->sock);
    SDL_free(w);
}

bool gs_wire_ready(const gs_wire *w) { return w != nullptr && w->ready; }

const uint8_t *gs_wire_public_key(const gs_wire *w) {
    return w != nullptr ? w->identity.pub : nullptr;
}
uint64_t gs_wire_session(const gs_wire *w) { return w != nullptr ? w->session : 0; }
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

// Sealed, to a peer whose tunnel is up. Nothing is sent to one whose is not:
// there is no plaintext path here either.
static bool gs_seal_to(gs_wire *w, uint8_t i, const uint8_t *buf, size_t n) {
    gs_peer *p = &w->peer[i];
    if (!p->known || p->addr == nullptr || !p->tunnel.established || n == 0) {
        return false;
    }

    uint8_t sealed[GS_PROTO_MTU], out[GS_PROTO_MTU];
    size_t k = gs_noise_seal(&p->tunnel, buf, n, sealed, sizeof sealed);
    if (k == 0) return false;
    k = gs_proto_sealed(out, sizeof out, sealed, k);
    if (k == 0) return false;

    gs_send_to(w, p, out, k);
    return true;
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
        if (n + 1 + len + 2 + GS_NOISE_KEY_BYTES > sizeof buf) return;

        buf[n++] = (uint8_t)len;
        SDL_memcpy(buf + n, text, len); n += len;
        gs_put16(buf + n, w->peer[i].port); n += 2;

        // **And whose key that slot is.** This is the whole reason the roster
        // is worth sealing: it is how everybody learns who everybody else is,
        // and a roster anybody could rewrite would let them put their own key
        // in somebody else's slot. Slot zero is the host's own.
        const uint8_t *key = (i == 0) ? w->identity.pub : w->peer[i].key;
        SDL_memcpy(buf + n, key, GS_NOISE_KEY_BYTES);
        n += GS_NOISE_KEY_BYTES;
    }

    gs_seal_to(w, to, buf, n);
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
        if (n + tlen + 2 + GS_NOISE_KEY_BYTES > len || tlen >= GS_ADDR_MAX) return;

        char text[GS_ADDR_MAX];
        SDL_memcpy(text, buf + n, tlen);
        text[tlen] = '\0';
        n += tlen;
        uint16_t port = gs_get16(buf + n); n += 2;

        uint8_t key[GS_NOISE_KEY_BYTES];
        SDL_memcpy(key, buf + n, GS_NOISE_KEY_BYTES);
        n += GS_NOISE_KEY_BYTES;

        if (i == slot) continue;                 // no need to reach ourselves

        // Slot zero arrives empty on purpose - see gs_send_roster.
        const char *want = (i == 0) ? w->host_text : text;
        if (i == 0) port = w->host_port;

        // **An empty slot is somebody who has not arrived, not somebody with no
        // key.** The host sends the roster again every time anybody joins, so
        // the early ones carry blank entries for the seats still to be filled -
        // and taking a key from one of those meant remembering thirty-two zero
        // bytes as that player's identity for ever, because a key already known
        // is deliberately not replaced. Every later handshake with them then
        // failed the check, silently, and the race started with three machines
        // that could only hear the host.
        if (want[0] == '\0') continue;

        // The key, before the address: it is what the handshake with them is
        // checked against. Peer zero's is already ours from the command line
        // and must not be quietly replaced by whatever arrived.
        if (!w->peer[i].has_key) {
            SDL_memcpy(w->peer[i].key, key, GS_NOISE_KEY_BYTES);
            w->peer[i].has_key = true;
        }

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


// What the server said. Handled here rather than by the caller, for the same
// reason the peer handshake is: the layer above wants players, not datagrams.
// **Everybody the server named, turned into somebody who can be reached.**
//
// Run for every lobby and not only the first. A client that is welcomed before
// anybody else has arrived is told about nobody, and the lobbies that follow are
// how it learns who turned up - so populating peers only on the welcome left
// the first player in every race unable to see the second.
static void gs_peers_from_lobby(gs_wire *w) {
    if (w->local >= GS_WIRE_PLAYERS) return;

    for (uint8_t i = 0; i < GS_WIRE_PLAYERS; i++) {
        const gs_lobby_player *p = &w->lobby.player[i];
        if (i == w->local || !p->present) continue;

        SDL_strlcpy(w->peer[i].text, p->addr, sizeof w->peer[i].text);
        w->peer[i].port = p->port;

        // The key the server watched them prove during their own
        // handshake. Without it there is nothing to check a peer against
        // and no mesh can be sealed - which is the only reason the lobby
        // carries one.
        if (!w->peer[i].has_key) {
            SDL_memcpy(w->peer[i].key, p->key, GS_NOISE_KEY_BYTES);
            w->peer[i].has_key = true;
        }

        // **And an address that can actually be sent to.**
        //
        // This used to stop at writing down the text. Nothing resolved it
        // and nothing set `known`, so `gs_wire_send` walked a list of peers
        // it considered unknown and sent to none of them: a server race
        // without `--relay` produced no traffic whatsoever, silently, and
        // was never noticed because every test of a server race asked for
        // the relay.
        if (w->peer[i].known && w->peer[i].addr != nullptr) continue;
        if (p->addr[0] == '\0') continue;

        NET_Address *a = NET_ResolveHostname(p->addr);
        if (a == nullptr || NET_WaitUntilResolved(a, 3000) != NET_SUCCESS) {
            if (a != nullptr) NET_UnrefAddress(a);
            continue;
        }
        if (w->peer[i].addr != nullptr) NET_UnrefAddress(w->peer[i].addr);
        w->peer[i].addr = a;
        w->peer[i].known = true;
    }
}

static void gs_take_server(gs_wire *w, const uint8_t *buf, size_t len) {
    switch (gs_proto_kind(buf, len)) {
    case GS_MSG_WELCOME: {
        uint8_t slot = 0;
        if (!gs_proto_read_welcome(buf, len, &slot, &w->lobby)) break;

        w->local = slot;
        w->players = w->lobby.capacity;
        w->refused = false;

        // Everybody the server named, so a race can be meshed with them
        // directly later. Slot zero is a player here, not the server - the
        // server's own address is kept separately in host_text.
        // Where everybody is, for meshing with them directly later. Safe to
        // write over slot zero now: the server keeps its own address.
        gs_peers_from_lobby(w);

        // Ready when the lobby is full *and* the ground is agreed. Racing
        // before the track has arrived would be racing on whatever was loaded
        // locally, which is exactly the bug this item exists to remove.
        w->ready = w->lobby.count >= w->lobby.capacity && gs_wire_settled(w);
        break;
    }

    case GS_MSG_SESSION: {
        uint64_t nonce = 0;
        if (gs_proto_read_session(buf, len, &nonce)) w->session = nonce;
        break;
    }

    case GS_MSG_LOBBY:
        if (gs_proto_read_lobby(buf, len, &w->lobby)) {
            w->players = w->lobby.capacity;
            gs_peers_from_lobby(w);
            w->ready = w->local < GS_WIRE_PLAYERS &&
                       w->lobby.count >= w->lobby.capacity &&
                       gs_wire_settled(w);
        }
        break;

    case GS_MSG_START: {
        uint64_t hash = 0;
        uint8_t players = 0, mode = 0;
        uint16_t laps = 0;
        if (!gs_proto_read_start(buf, len, &hash, &players, &laps, &mode)) break;

        if (hash != w->want_track) {
            w->want_track = hash;
            gs_carrier_expect(&w->carrier, hash);
            w->asked_at = 0;          // ask for it on the next poll
        }
        w->heard_start = true;

        // The roster may have arrived first and left this client thinking it
        // was ready before it knew there was any ground to wait for.
        w->ready = w->lobby.count >= w->lobby.capacity && gs_wire_settled(w);
        break;
    }

    case GS_MSG_TRACK:
        gs_carrier_take(&w->carrier, buf, len);
        if (gs_carrier_done(&w->carrier)) {
            w->ready = w->local < GS_WIRE_PLAYERS &&
                       w->lobby.count >= w->lobby.capacity && gs_wire_settled(w);
        }
        break;

    case GS_MSG_BEST: {
        uint64_t track = 0, conditions = 0;
        uint16_t laps = 0;
        uint32_t lap_ticks = 0, race_ticks = 0;
        char lap_who[GS_PROTO_NAME] = { 0 }, race_who[GS_PROTO_NAME] = { 0 };

        if (gs_proto_read_best(buf, len, &track, &conditions, &laps, &lap_ticks,
                               lap_who, sizeof lap_who, &race_ticks, race_who,
                               sizeof race_who)) {
            w->best.known = true;
            w->best.track = track;
            w->best.lap_ticks = lap_ticks;
            w->best.race_ticks = race_ticks;
            SDL_strlcpy(w->best.lap_who, lap_who, sizeof w->best.lap_who);
            SDL_strlcpy(w->best.race_who, race_who, sizeof w->best.race_who);
        }
        break;
    }

    case GS_MSG_LISTING: {
        uint16_t index = 0, total = 0;
        uint64_t track = 0;
        char name[48] = { 0 }, author[GS_PROTO_NAME] = { 0 };

        if (!gs_proto_read_listing(buf, len, &index, &total, &track, name,
                                   sizeof name, author, sizeof author)) {
            break;
        }

        // A new answer replaces the last one. Merging two would leave a list
        // holding tracks that have since been taken down.
        if (total != w->listed) {
            w->listed = total;
            w->listings = 0;
            SDL_memset(w->listing, 0, sizeof w->listing);
        }
        if (total == 0) break;          // "nothing published" is an answer

        if (index < GS_WIRE_LISTINGS) {
            gs_wire_listing *l = &w->listing[index];
            l->track = track;
            SDL_strlcpy(l->name, name, sizeof l->name);
            SDL_strlcpy(l->author, author, sizeof l->author);
            if (index + 1 > w->listings) w->listings = (uint16_t)(index + 1);
        }
        break;
    }

    case GS_MSG_FULL:
        // **Kept, and shown.** A client that could only say "connection
        // failed" makes a full server and a wrong address look the same.
        w->refused = gs_proto_read_full(buf, len, w->refusal, sizeof w->refusal);
        break;

    case GS_MSG_PING: {
        // Only the end that sent a ping can time the reply, so answering is
        // how the server learns how far away this machine is.
        uint32_t stamp = 0;
        if (gs_proto_read_stamp(buf, len, &stamp)) {
            uint8_t out[GS_PROTO_MTU];
            gs_to_server(w, out, gs_proto_pong(out, sizeof out, stamp));
        }
        break;
    }

    default:
        break;
    }
}

// The server's answer to the handshake. One message and the tunnel is up; IK
// needs no more than that, which is why there is no state here to keep between
// datagrams and nothing for anybody to fill up.
static void gs_take_handshake(gs_wire *w, const uint8_t *in, size_t len) {
    if (w->tunnel.established) return;      // already up; a late duplicate

    const uint8_t *msg = nullptr;
    size_t msg_len = 0;
    if (!gs_proto_read_handshake(in, len, &msg, &msg_len)) return;

    uint8_t payload[GS_PROTO_MTU];
    size_t got = 0;
    if (!gs_noise_read_message(&w->hs, msg, msg_len, payload, sizeof payload,
                               &got)) {
        // A failed handshake stays failed, so the next attempt starts a fresh
        // one rather than grinding against this.
        return;
    }
    if (!gs_noise_done(&w->hs)) return;
    gs_noise_split(&w->hs, &w->tunnel);
}

// Everything else the server says arrives sealed. Returns the plaintext length,
// or zero when the datagram was not sealed by the far end of this tunnel, has
// been changed, or has been seen before.
static size_t gs_unseal(gs_wire *w, const uint8_t *in, size_t len,
                        uint8_t *out, size_t cap) {
    if (!w->tunnel.established) return 0;

    const uint8_t *body = nullptr;
    size_t body_len = 0;
    if (!gs_proto_read_sealed(in, len, &body, &body_len)) return 0;

    size_t got = 0;
    if (!gs_noise_open(&w->tunnel, body, body_len, out, cap, &got)) return 0;
    return got;
}

// Authenticated and never sent, so two ends that disagree about what they are
// speaking fail the handshake rather than a race - which is where a version
// number lives. The same string the server uses, because it is the same
// protocol whichever end of it you are.
#define GS_WIRE_PROLOGUE "gearstick/1"

// --- the peer tunnels -------------------------------------------------------
//
// **Who initiates is decided by slot number, and the higher one does.**
//
// It has to be settled without a negotiation, because a negotiation is a round
// trip and something to get wrong. Higher-initiates is the rule that works
// everywhere this is used: a joiner reaching the host it typed the address of
// has the host's key and a slot above zero, so it initiates; a peer meeting
// another peer has both keys from the roster or the lobby, and the rule breaks
// the tie. The responder needs no key in advance, which is exactly the property
// IK has and the reason the host never needs to know a joiner before it knocks.
static bool gs_we_initiate(const gs_wire *w, uint8_t peer) {
    return w->local > peer;
}

static int gs_peer_at(const gs_wire *w, NET_Address *addr, uint16_t port) {
    const char *text = NET_GetAddressString(addr);
    if (text == nullptr) return -1;

    for (uint8_t i = 0; i < GS_WIRE_PLAYERS; i++) {
        const gs_peer *p = &w->peer[i];
        if (!p->known || p->addr == nullptr || p->port != port) continue;
        const char *theirs = NET_GetAddressString(p->addr);
        if (theirs != nullptr && SDL_strcmp(theirs, text) == 0) return (int)i;
    }
    return -1;
}

static bool gs_from_server(const gs_wire *w, NET_Address *addr, uint16_t port) {
    if (!w->via_server || w->server_addr == nullptr || port != w->server_port) {
        return false;
    }
    const char *text = NET_GetAddressString(addr);
    const char *ours = NET_GetAddressString(w->server_addr);
    return text != nullptr && ours != nullptr && SDL_strcmp(text, ours) == 0;
}

// **Ready means able to race, not merely introduced.**
//
// It used to mean everybody's address was known, which was the same thing while
// the mesh was in the clear and is not any more: a peer whose tunnel is not up
// cannot be sent to at all, so a race that started on addresses alone would
// begin with players who can hear nothing.
//
// **Decided in one place, and last.** The first version worked this out in the
// middle of the poll, and then the roster arrived later in the same poll and
// wrote the old address-only answer over the top. Every joiner then declared
// itself ready with only the host reachable, raced for four seconds hearing one
// machine out of three, and confirmed nothing.
// The server has placed this client and the ground is agreed. Everything the
// server has a say in, and nothing the peers do.
static bool gs_placed(const gs_wire *w) {
    return w->local < GS_WIRE_PLAYERS &&
           w->lobby.count >= w->lobby.capacity &&
           gs_wire_settled(w);
}

static void gs_settle_ready(gs_wire *w) {
    if (w->players == 0 || w->local >= w->players) return;

    if (w->via_server) {
        // Relaying: the path is the tunnel to the server, and it is up long
        // before anything else happens.
        if (w->relay) { w->ready = gs_placed(w); return; }
        if (!gs_placed(w)) { w->ready = false; return; }
    }

    // **And a sealed channel to every one of them.**
    //
    // A race that started on addresses alone began before the peer tunnels
    // existed, so the first ticks of input were never sent to anybody - and
    // they cannot be recovered, because the redundancy in each datagram only
    // reaches back thirty-two ticks. The race then ran to the end and confirmed
    // nothing, which reads as a desync and is a race that started too early.
    for (uint8_t i = 0; i < w->players; i++) {
        if (i == w->local) continue;
        if (!w->peer[i].known || !w->peer[i].tunnel.established) {
            w->ready = false;
            return;
        }
    }
    w->ready = true;
}

// Start, or restart, the handshake with a peer we are supposed to initiate to.
// Repeated on the same cadence as everything else here, because the first
// datagram of a handshake can be lost like any other.
static void gs_greet_peer(gs_wire *w, uint8_t i) {
    gs_peer *p = &w->peer[i];
    if (!p->known || p->addr == nullptr || !p->has_key) return;
    if (p->tunnel.established) return;
    if (!gs_we_initiate(w, i)) return;

    gs_noise_init_initiator(&p->hs, &w->identity, p->key,
                            (const uint8_t *)GS_WIRE_PROLOGUE,
                            sizeof GS_WIRE_PROLOGUE - 1);
    p->greeted = true;

    uint8_t msg[GS_PROTO_MTU], out[GS_PROTO_MTU];
    size_t n = gs_noise_write_message(&p->hs, nullptr, 0, msg, sizeof msg);
    if (n == 0) return;
    n = gs_proto_handshake(out, sizeof out, msg, n);
    if (n == 0) return;
    gs_send_to(w, p, out, n);
}

// **Knocking is now a handshake.** It used to be five bytes of "hello" that
// anybody could send and anybody could forge; it is the first message of an IK
// handshake, which tells the host who is knocking and cannot be produced by
// somebody who is not them.
static void gs_send_hello(gs_wire *w) {
    gs_greet_peer(w, 0);
}

// One handshake message from a peer, in whichever direction we are going.
static void gs_take_peer_handshake(gs_wire *w, uint8_t i, const uint8_t *in,
                                   size_t len) {
    gs_peer *p = &w->peer[i];
    if (p->tunnel.established) return;

    const uint8_t *msg = nullptr;
    size_t msg_len = 0;
    if (!gs_proto_read_handshake(in, len, &msg, &msg_len)) return;

    uint8_t payload[GS_PROTO_MTU];
    size_t got = 0;

    if (gs_we_initiate(w, i)) {
        if (!p->greeted) return;                 // an answer to nothing
        if (!gs_noise_read_message(&p->hs, msg, msg_len, payload, sizeof payload,
                                   &got)) {
            p->greeted = false;                  // a fresh one next time round
            return;
        }
        if (gs_noise_done(&p->hs)) gs_noise_split(&p->hs, &p->tunnel);
        return;
    }

    // Answering. A responder needs no key in advance - it learns the far end's
    // from the message itself - but it still checks it against the one the
    // broker said to expect, because otherwise anybody could take the slot.
    gs_noise_init_responder(&p->hs, &w->identity,
                            (const uint8_t *)GS_WIRE_PROLOGUE,
                            sizeof GS_WIRE_PROLOGUE - 1);
    if (!gs_noise_read_message(&p->hs, msg, msg_len, payload, sizeof payload,
                               &got)) {
        return;
    }

    const uint8_t *theirs = gs_noise_remote_static(&p->hs);
    if (p->has_key &&
        (theirs == nullptr || SDL_memcmp(theirs, p->key, GS_NOISE_KEY_BYTES) != 0)) {
        // **Somebody else answering to this slot.** The handshake completed, so
        // they hold *a* key; it is not the one the broker said this player has.
        return;
    }

    uint8_t reply[GS_PROTO_MTU], out[GS_PROTO_MTU];
    size_t n = gs_noise_write_message(&p->hs, nullptr, 0, reply, sizeof reply);
    if (n == 0) return;
    n = gs_proto_handshake(out, sizeof out, reply, n);
    if (n == 0) return;

    gs_send_to(w, p, out, n);
    if (gs_noise_done(&p->hs)) gs_noise_split(&p->hs, &p->tunnel);
}

// A joiner the host has not seen before. Their first message is a handshake, so
// **the host learns who they are from the message rather than from a claim** -
// which is what makes the roster it then publishes worth anything to everybody
// else in the race.
static void gs_take_knock(gs_wire *w, NET_Address *from, uint16_t port,
                          const uint8_t *in, size_t len) {
    if (!w->hosting) return;

    const char *text = NET_GetAddressString(from);
    if (text == nullptr) return;

    // Already here: their knock or our answer was lost. Say the roster again
    // rather than tearing down a tunnel that works - a replayed knock would
    // otherwise throw a racing player out, which is the same trick the server
    // refuses.
    for (uint8_t i = 1; i < w->players; i++) {
        if (w->peer[i].known && w->peer[i].port == port &&
            SDL_strcmp(w->peer[i].text, text) == 0) {
            if (w->peer[i].tunnel.established) gs_send_roster(w, i);
            return;
        }
    }

    for (uint8_t i = 1; i < w->players; i++) {
        gs_peer *p = &w->peer[i];
        if (p->known) continue;

        const uint8_t *msg = nullptr;
        size_t msg_len = 0;
        if (!gs_proto_read_handshake(in, len, &msg, &msg_len)) return;

        uint8_t payload[GS_PROTO_MTU];
        size_t got = 0;
        gs_noise_init_responder(&p->hs, &w->identity,
                                (const uint8_t *)GS_WIRE_PROLOGUE,
                                sizeof GS_WIRE_PROLOGUE - 1);
        if (!gs_noise_read_message(&p->hs, msg, msg_len, payload, sizeof payload,
                                   &got)) {
            return;
        }

        const uint8_t *theirs = gs_noise_remote_static(&p->hs);
        if (theirs == nullptr) return;
        SDL_memcpy(p->key, theirs, GS_NOISE_KEY_BYTES);
        p->has_key = true;

        p->addr = from;
        NET_RefAddress(from);
        p->port = port;
        p->known = true;
        SDL_strlcpy(p->text, text, sizeof p->text);

        uint8_t reply[GS_PROTO_MTU], out[GS_PROTO_MTU];
        size_t n = gs_noise_write_message(&p->hs, nullptr, 0, reply, sizeof reply);
        if (n > 0) {
            n = gs_proto_handshake(out, sizeof out, reply, n);
            if (n > 0) gs_send_to(w, p, out, n);
        }
        if (!gs_noise_done(&p->hs) || !gs_noise_split(&p->hs, &p->tunnel)) {
            return;
        }

        // Everybody who is already here gets told again, because the roster
        // they were sent was the roster before this person arrived - and it is
        // the roster that carries the keys they need to reach each other.
        w->ready = gs_wire_present(w) == w->players;
        for (uint8_t k = 1; k < w->players; k++) {
            if (w->peer[k].tunnel.established) gs_send_roster(w, k);
        }
        return;
    }
    // Full. Nothing is sent back: an uninvited machine gets silence rather than
    // a reply telling it there is a game here.
}

// To the server, which is not a peer - and sealed, always.
//
// **There is no plaintext path.** A client whose tunnel is not up yet sends
// nothing rather than sending it in the clear: a fallback to plaintext is a
// fallback anybody on the path can force by dropping one datagram. Everything
// here is retried anyway - the join, the track request, the heartbeat - so a
// message dropped for want of a tunnel comes round again a moment later.
static void gs_to_server(gs_wire *w, const uint8_t *buf, size_t n) {
    if (w->server_addr == nullptr || n == 0) return;
    if (!w->tunnel.established) return;

    uint8_t sealed[GS_PROTO_MTU];
    size_t k = gs_noise_seal(&w->tunnel, buf, n, sealed, sizeof sealed);
    if (k == 0) return;

    uint8_t out[GS_PROTO_MTU];
    k = gs_proto_sealed(out, sizeof out, sealed, k);
    if (k == 0) return;

    NET_SendDatagram(w->sock, w->server_addr, w->server_port, out, (int)k);
}

// The first message of the handshake, which is the only thing this client ever
// sends the server in the clear. Repeated until an answer comes back, the same
// way everything else here recovers from loss.
static void gs_send_handshake(gs_wire *w) {
    if (w->server_addr == nullptr || !w->has_server_key) return;

    gs_noise_init_initiator(&w->hs, &w->identity, w->server_key,
                            (const uint8_t *)GS_WIRE_PROLOGUE,
                            sizeof GS_WIRE_PROLOGUE - 1);

    uint8_t msg[GS_PROTO_MTU], out[GS_PROTO_MTU];
    size_t n = gs_noise_write_message(&w->hs, nullptr, 0, msg, sizeof msg);
    if (n == 0) return;

    n = gs_proto_handshake(out, sizeof out, msg, n);
    if (n == 0) return;

    NET_SendDatagram(w->sock, w->server_addr, w->server_port, out, (int)n);
    w->handshakes_sent++;
}

static void gs_send_join(gs_wire *w) {
    uint8_t buf[GS_PROTO_MTU];
    gs_to_server(w, buf, gs_proto_join(buf, sizeof buf, w->me));
}

// **A connection has to keep saying it is there.** The server drops anybody it
// has not heard from, which is what stops a lobby filling with people who
// closed their laptop - so a client that went quiet the moment it was placed
// would be thrown out mid-race. Nothing else sends to the server once a race
// starts: the race itself goes peer to peer.
// **How often a placed client says it is still here, in milliseconds.**
//
// A wall clock and not a frame count. The first version pinged every ninety
// calls to gs_wire_poll - "about a second and a half at 60 fps" - while the
// server's patience has always been in milliseconds, so the two only agreed on
// a machine running at the frame rate somebody assumed. A client at thirty
// frames pings every three seconds; one that hitches pings when it recovers;
// and a slow CI runner gets thrown out of its own test, intermittently, on one
// platform, which is exactly what happened.
//
// Well inside the server's two-second ping cycle and far inside its patience.
#define GS_HEARTBEAT_MS 500u

void gs_wire_poll(gs_wire *w) {
    if (w == nullptr || w->sock == nullptr) return;

    if (w->via_server) {
        w->retry++;

        // **The tunnel first, and nothing else until it is up.** Every message
        // below this is sealed, so knocking before the handshake completes
        // would send nothing at all. Repeated on the same cadence as the join,
        // because the first datagram of a handshake can be lost like any other.
        if (!w->tunnel.established) {
            if (w->has_server_key && w->retry % GS_RETRY_TICKS == 0) {
                gs_send_handshake(w);
            }
        } else if (!w->ready && !w->refused && w->retry % GS_RETRY_TICKS == 0) {
            // Knocking. A refused client stops asking: a full server does not
            // become less full by being asked again, and a client that kept
            // knocking would be a client nobody can turn away.
            gs_send_join(w);
        }

        // Asking for the track until it is here. There is nothing that
        // acknowledges a chunk, so a piece that went missing is recovered by
        // asking again rather than by anybody keeping a list.
        if (w->want_track != 0 && !gs_carrier_done(&w->carrier) &&
            (w->asked_at == 0 || w->retry - w->asked_at > 120u)) {
            uint8_t buf[GS_PROTO_MTU];
            gs_to_server(w, buf,
                         gs_proto_want_track(buf, sizeof buf, w->want_track));
            w->asked_at = w->retry;
        }

        // And once placed, still saying so - on the clock, so how often it
        // happens does not depend on how fast this machine is drawing.
        uint64_t now = SDL_GetTicks();
        if (w->ready && (w->last_ping == 0 || now - w->last_ping >= GS_HEARTBEAT_MS)) {
            uint8_t buf[GS_PROTO_MTU];
            gs_to_server(w, buf, gs_proto_ping(buf, sizeof buf, (uint32_t)w->retry));
            w->last_ping = now;
        }
    } else if (!w->ready && !w->hosting && w->retry++ % GS_RETRY_TICKS == 0) {
        gs_send_hello(w);
    }

    // **Every peer link, until it is up.** A race cannot start before these
    // finish, and they cannot finish before the broker has said who everybody
    // is - so this runs on the same retry clock as everything else and settles
    // as the roster or the lobby fills in.
    if (w->retry % GS_RETRY_TICKS == 0) {
        for (uint8_t i = 0; i < GS_WIRE_PLAYERS; i++) {
            if (i == w->local) continue;
            gs_greet_peer(w, i);
        }
    }



    // **Only while still finding everybody.** Once a race is running the
    // caller's own receive loop is the pump: draining here would swallow race
    // traffic and drop it on the floor, which is exactly what it did the first
    // time - a relayed race delivered nothing at all, because every forwarded
    // datagram was consumed by the poll that was supposed to be idle.
    //
    // Control traffic is still handled during a race, because gs_wire_recv
    // handles it on the way past and returns only what the caller wants.
    if (w->ready) return;

    uint8_t buf[GS_WIRE_MTU];
    while (gs_wire_recv(w, buf, sizeof buf) > 0) {
        // Data arriving before the handshake finished is dropped: there is
        // nothing to hand it to yet.
    }

    // Last, so that nothing later in this poll can write an older answer over
    // the top of it.
    gs_settle_ready(w);
}

// --- carrying the race ------------------------------------------------------

bool gs_wire_send(gs_wire *w, const uint8_t *buf, size_t len) {
    if (w == nullptr || w->sock == nullptr || len == 0 || len > GS_WIRE_MTU) {
        return false;
    }

    // Through the server, when the mesh is not an option. One datagram rather
    // than one per peer, and the server fans it out - which is the whole cost
    // of a relay: an extra hop, paid only by the people who need it.
    if (w->relay) {
        uint8_t wrapped[GS_PROTO_MTU];
        size_t n = gs_proto_relay(wrapped, sizeof wrapped, buf, len);
        if (n == 0) return false;
        gs_to_server(w, wrapped, n);
        w->sent++;
        return true;
    }

    // To everybody else, directly. It is a mesh rather than a relay because a
    // relay would put the host's latency between two clients who can see each
    // other.
    bool any = false;
    for (uint8_t i = 0; i < w->players; i++) {
        if (i == w->local || !w->peer[i].known || w->peer[i].addr == nullptr) {
            continue;
        }
        // **Sealed to each of them separately.** One datagram per peer rather
        // than one for everybody, which is what a tunnel costs on a mesh: the
        // plaintext is identical and every ciphertext is different, because
        // each peer has its own key and its own counter.
        if (gs_seal_to(w, i, buf, len)) {
            any = true;
        } else if (w->peer[i].tunnel.established) {
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

        // **Routed by where it came from, not by what it looks like.**
        //
        // The server and every peer share one socket, so "is this the server
        // talking" is a question about the sender's address and nothing else.
        // Deciding it by the message type instead worked right up until a peer
        // sent a rollback datagram, which is not a protocol message at all and
        // was quietly thrown away.
        if (w->via_server && gs_from_server(w, d->addr, d->port)) {
            gs_msg kind = gs_proto_kind(d->buf, n);

            // The handshake, which is the only thing from the server that is
            // not sealed - because it is what makes sealing possible.
            if (kind == GS_MSG_HANDSHAKE) {
                gs_take_handshake(w, d->buf, n);
                NET_DestroyDatagram(d);
                continue;
            }

            // **Anything else in the clear is dropped without being read.**
            // Not "read it anyway if the tunnel is not up yet": a client that
            // would accept plaintext is a client anybody on the path can talk
            // to by pretending the tunnel failed.
            if (kind != GS_MSG_SEALED) {
                NET_DestroyDatagram(d);
                continue;
            }

            uint8_t plain[GS_PROTO_MTU];
            size_t got = gs_unseal(w, d->buf, n, plain, sizeof plain);
            NET_DestroyDatagram(d);
            if (got == 0) continue;

            // A relayed datagram is somebody's race traffic in an envelope.
            // The envelope is opened here, so nothing above this layer can
            // tell a relayed race from a direct one.
            if (gs_proto_kind(plain, got) == GS_MSG_FORWARD) {
                uint8_t from = 0;
                size_t payload_len = 0;
                const uint8_t *payload = gs_proto_payload(plain, got, &from,
                                                          &payload_len);
                if (payload != nullptr && payload_len > 0 && payload_len <= cap) {
                    SDL_memcpy(buf, payload, payload_len);
                    w->received++;
                    return payload_len;
                }
                continue;
            }

            // The rest of the server's traffic never reaches the caller.
            gs_take_server(w, plain, got);
            continue;
        }

        // --- a peer, on the direct path.
        int from = gs_peer_at(w, d->addr, d->port);
        if (from >= 0) {
            gs_peer *p = &w->peer[from];
            gs_msg kind = gs_proto_kind(d->buf, n);

            if (kind == GS_MSG_HANDSHAKE) {
                gs_take_peer_handshake(w, (uint8_t)from, d->buf, n);
                NET_DestroyDatagram(d);
                continue;
            }

            // **Nothing from a peer is read unsealed.** A race between two
            // machines that can see each other is the traffic a tunnel to the
            // server does nothing for, so it gets its own.
            if (kind != GS_MSG_SEALED || !p->tunnel.established) {
                NET_DestroyDatagram(d);
                continue;
            }

            const uint8_t *body = nullptr;
            size_t body_len = 0;
            uint8_t plain[GS_PROTO_MTU];
            size_t got = 0;
            bool ok = gs_proto_read_sealed(d->buf, n, &body, &body_len) &&
                      gs_noise_open(&p->tunnel, body, body_len, plain,
                                    sizeof plain, &got);
            NET_DestroyDatagram(d);
            if (!ok) continue;

            // The roster is the host telling everybody who is here; everything
            // else a peer says is race traffic and belongs to the caller.
            if (got >= 5 && gs_get32(plain) == GS_CTRL_MAGIC) {
                if (plain[4] == GS_MSG_ROSTER) gs_take_roster(w, plain, got);
                continue;
            }
            if (got > cap) got = cap;
            SDL_memcpy(buf, plain, got);
            w->received++;
            return got;
        }

        // Somebody who is not a peer yet. A joiner knocking is the only thing
        // that can legitimately be in this position, and the knock is a
        // handshake message: the host learns who they are from it, which is
        // what IK's responder gets for free.
        if (w->hosting && gs_proto_kind(d->buf, n) == GS_MSG_HANDSHAKE) {
            gs_take_knock(w, d->addr, d->port, d->buf, n);
            NET_DestroyDatagram(d);
            continue;
        }

        // Anything else is from somebody this client has no business hearing
        // from. Before the tunnel it would have been handed to the caller as
        // race traffic, which is to say anybody who knew the port could inject
        // inputs into somebody else's race.
        NET_DestroyDatagram(d);
    }
}

void gs_wire_stats(const gs_wire *w, uint32_t *sent, uint32_t *received) {
    if (sent != nullptr) *sent = w != nullptr ? w->sent : 0;
    if (received != nullptr) *received = w != nullptr ? w->received : 0;
}

const char *gs_wire_error(const gs_wire *w) {
    return (w != nullptr && w->error[0] != '\0') ? w->error : nullptr;
}

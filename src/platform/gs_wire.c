#include "platform/gs_wire.h"

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

    // One player short of nobody: a host waiting for one other person is ready
    // the moment they arrive, and a host waiting for three waits for three.
    return w;
}

gs_wire *gs_wire_server(const char *host, uint16_t port, const char *name) {
    gs_wire *w = gs_wire_new(0);
    if (w == nullptr || w->sock == nullptr) return w;

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

    uint8_t buf[GS_PROTO_MTU];
    gs_to_server(w, buf,
                 gs_proto_result(buf, sizeof buf, track, conditions, laps,
                                 vehicle, lap_ticks, race_ticks));

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

    // **Say goodbye on the way out.** The protocol has had a message for this
    // since it was written and nothing ever sent one, so a player who quit was
    // noticed only when they had been silent long enough to look crashed -
    // fifteen seconds of everybody else racing a car that was not there. It is
    // one datagram and it is not required to arrive: the silence timeout is
    // still what catches a client that was unplugged rather than closed.
    if (w->server_addr != nullptr) {
        uint8_t buf[GS_PROTO_MTU];
        size_t n = gs_proto_bye(buf, sizeof buf);
        NET_SendDatagram(w->sock, w->server_addr, w->server_port, buf, (int)n);
        NET_UnrefAddress(w->server_addr);
    }
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

// What the server said. Handled here rather than by the caller, for the same
// reason the peer handshake is: the layer above wants players, not datagrams.
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
        for (uint8_t i = 0; i < GS_WIRE_PLAYERS; i++) {
            const gs_lobby_player *p = &w->lobby.player[i];
            if (i == slot || !p->present) continue;
            SDL_strlcpy(w->peer[i].text, p->addr, sizeof w->peer[i].text);
            w->peer[i].port = p->port;
        }

        // Ready when the lobby is full *and* the ground is agreed. Racing
        // before the track has arrived would be racing on whatever was loaded
        // locally, which is exactly the bug this item exists to remove.
        w->ready = w->lobby.count >= w->lobby.capacity && gs_wire_settled(w);
        break;
    }

    case GS_MSG_LOBBY:
        if (gs_proto_read_lobby(buf, len, &w->lobby)) {
            w->players = w->lobby.capacity;
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

// To the server, which is not a peer.
static void gs_to_server(gs_wire *w, const uint8_t *buf, size_t n) {
    if (w->server_addr == nullptr || n == 0) return;
    NET_SendDatagram(w->sock, w->server_addr, w->server_port, buf, (int)n);
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
#define GS_HEARTBEAT_TICKS 90        // about a second and a half at 60 fps

void gs_wire_poll(gs_wire *w) {
    if (w == nullptr || w->sock == nullptr) return;

    if (w->via_server) {
        w->retry++;

        // Knocking. A refused client stops asking: a full server does not
        // become less full by being asked again, and a client that kept
        // knocking would be a client nobody can turn away.
        if (!w->ready && !w->refused && w->retry % GS_RETRY_TICKS == 0) {
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

        // And once placed, still saying so.
        if (w->ready && w->retry % GS_HEARTBEAT_TICKS == 0) {
            uint8_t buf[GS_PROTO_MTU];
            gs_to_server(w, buf, gs_proto_ping(buf, sizeof buf, (uint32_t)w->retry));
        }
    } else if (!w->ready && !w->hosting && w->retry++ % GS_RETRY_TICKS == 0) {
        gs_send_hello(w);
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

        // A relayed datagram is somebody's race traffic in an envelope. The
        // envelope is opened here, so nothing above this layer can tell a
        // relayed race from a direct one.
        if (w->via_server && gs_proto_kind(d->buf, n) == GS_MSG_FORWARD) {
            uint8_t from = 0;
            size_t payload_len = 0;
            const uint8_t *payload = gs_proto_payload(d->buf, n, &from,
                                                      &payload_len);
            if (payload != nullptr && payload_len > 0 && payload_len <= cap) {
                SDL_memcpy(buf, payload, payload_len);
                NET_DestroyDatagram(d);
                w->received++;
                return payload_len;
            }
            NET_DestroyDatagram(d);
            continue;
        }

        // The server's traffic never reaches the caller either.
        if (w->via_server && gs_proto_kind(d->buf, n) != GS_MSG_NONE) {
            gs_take_server(w, d->buf, n);
            NET_DestroyDatagram(d);
            continue;
        }

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

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
#include "core/gs_generate.h"
#include "core/gs_sim.h"
#include "core/gs_track.h"
#include "net/gs_carrier.h"
#include "net/gs_proto.h"
#include "net/gs_store.h"
#include "net/gs_auth.h"
#include "net/gs_noise.h"

#include <sodium.h>
#include "net/gs_verify.h"
#include "platform/gs_paths.h"
#include "window.h"

#include <SDL3/SDL.h>
#include <SDL3_net/SDL_net.h>

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Only to ask whether stdout is a terminal. There is no portable spelling of
// that question, and the answer decides whether this writes a dashboard or a
// log - see gs_stdout_is_terminal.
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

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
// And the window, when there is one: thirty frames a second is a window that
// answers the mouse, on a loop that otherwise sleeps a millisecond at a time.
#define GS_FRAME_MS 33

// And how often a server whose output is not a terminal says it is still here.
// A minute apart, so an hour of running is sixty short lines rather than
// fourteen thousand copies of a table.
#define GS_HEARTBEAT_MS 60000

// How often the server asks each client how far away it is. The client cannot
// tell the server this - only the end that sent a ping can time its own reply -
// so a server that wants to show a round trip has to ask for one.
#define GS_PING_MS 2000

// **A sealed channel to one address, which exists before a client does.**
//
// The handshake arrives before the join - it has to, because the join travels
// inside it - so the tunnel cannot live in the client table. It is keyed by
// address, and a second handshake from the same address replaces the first,
// which is what a client that restarted looks like.
//
// Twice as many as there are player slots, so that somebody knocking cannot
// evict the tunnel of somebody already racing simply by handshaking.
#define GS_SRV_TUNNELS (GS_PROTO_MAX_PLAYERS * 2)

// Authenticated and never sent, so two ends that disagree about what they are
// speaking fail the handshake rather than a race. This is where a version
// number lives.
#define GS_SRV_PROLOGUE "gearstick/1"

typedef struct gs_tunnel {
    bool             used;
    char             text[GS_PROTO_ADDR];
    uint16_t         port;
    uint64_t         last_ms;
    gs_noise_session session;

    // **Who they proved they were.** Not a claim: the handshake does not
    // complete unless they hold the secret for this key. It goes into the
    // lobby so that two clients meeting on the mesh have something to check
    // each other against.
    uint8_t          key[GS_NOISE_KEY_BYTES];
} gs_tunnel;

// How many track chunks go out for one ask. See gs_client::sending.
#define GS_SRV_CHUNK_BURST 32

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

    // **Where the last burst of track chunks got to.** A track was eight
    // kilobytes and went out in one go; it is a hundred and thirty now, and a
    // hundred and twenty-seven datagrams written to a socket in a tight loop
    // overruns the buffer at the other end - most of them are dropped, the
    // client asks again, and the same burst is lost the same way. A track
    // transfer never completed and a client sat at "receiving the track" until
    // it gave up. So a burst is bounded and the next ask carries on from here.
    uint16_t sending;

    // The replay behind whatever time this client last claimed. Per client,
    // because two people can finish a race at the same moment and each one's
    // proof is their own.
    gs_carrier proof;
    bool       claimed;
    gs_claim   claim;

    // The one-shot token this client was last issued, and the one it offered
    // with its claim. The first is what the server believes; the second is what
    // arrived, and they have to be the same thing and still unspent.
    uint64_t   session;
    uint64_t   nonce;

    // **The key this client proved it holds**, copied from its tunnel when it
    // joined. The name beside it is whatever they typed; this is not.
    uint8_t    key[GS_NOISE_KEY_BYTES];
    bool       has_key;

    // Whether this client has proved the name it is using is theirs. Only ever
    // true for a name that has a password: a name with none needs no proving,
    // and a name with one cannot be used without it.
    bool       proved;

    // A track on its way up, for publishing. Separate from the proof carrier
    // because somebody can perfectly well be submitting a time on one track
    // while uploading another.
    gs_carrier upload;
} gs_client;

static struct {
    NET_DatagramSocket *sock;
    uint16_t            port;
    uint8_t             capacity;

    gs_client client[GS_PROTO_MAX_PLAYERS];

    // Who this server is, and the tunnels open to it.
    gs_noise_keypair identity;
    gs_tunnel        tunnel[GS_SRV_TUNNELS];
    uint32_t         handshakes, refused_clear, refused_sealed;

    uint64_t started_ms;
    uint32_t total_in, total_out, relayed, refused;
    uint64_t total_in_bytes, total_out_bytes;
    uint8_t  peak;

    bool quit;
    bool plain;                // no ANSI, for a dumb terminal
    bool tty;                  // stdout is a terminal, so there is a dashboard
    bool headless;             // --headless: no window, whatever the machine has
    bool window_dump;          // --window-dump: print what the window showed, at the end
    const char *window_shot;   // --window-shot FILE: and write its last frame
    gs_srv_log log;            // arrivals and departures, for the window

    // Uploads in progress, one per client, and the track this lobby races on. **The server hands it out**, so
    // everybody races the same ground - which is the one thing rollback cannot
    // recover from being wrong about. Held as bytes rather than as a gs_track:
    // the server never simulates anything, so the only thing it needs to do
    // with a track is send it and hash it.
    uint8_t  track[GS_CARRIER_MAX_BYTES];
    size_t   track_len;
    uint64_t track_hash;
    uint32_t chunks_sent;

    // What is remembered between races, and between runs of the server.
    gs_store *store;
    uint32_t  results, kept, rejected;
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

static gs_tunnel *gs_tunnel_find(const char *text, uint16_t port);

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

        const gs_tunnel *t = gs_tunnel_find(c->text, c->port);
        if (t != nullptr) SDL_memcpy(p->key, t->key, GS_NOISE_KEY_BYTES);
    }
}

// --- the tunnel -------------------------------------------------------------

static gs_tunnel *gs_tunnel_find(const char *text, uint16_t port) {
    for (int i = 0; i < GS_SRV_TUNNELS; i++) {
        gs_tunnel *t = &gs_srv.tunnel[i];
        if (t->used && t->port == port && SDL_strcmp(t->text, text) == 0) return t;
    }
    return nullptr;
}

// A slot for a new tunnel: this address's own if it has one, then any free one,
// then the one that has been quiet longest. **Never one belonging to a client
// that is here**, because otherwise anybody who can send a datagram could
// knock a racing player off by handshaking eight times.
static gs_tunnel *gs_tunnel_slot(const char *text, uint16_t port, uint64_t now) {
    gs_tunnel *mine = gs_tunnel_find(text, port);
    if (mine != nullptr) return mine;

    for (int i = 0; i < GS_SRV_TUNNELS; i++) {
        if (!gs_srv.tunnel[i].used) return &gs_srv.tunnel[i];
    }

    gs_tunnel *oldest = nullptr;
    for (int i = 0; i < GS_SRV_TUNNELS; i++) {
        gs_tunnel *t = &gs_srv.tunnel[i];
        bool racing = false;
        for (int k = 0; k < GS_PROTO_MAX_PLAYERS; k++) {
            const gs_client *c = &gs_srv.client[k];
            if (c->used && c->port == t->port && SDL_strcmp(c->text, t->text) == 0) {
                racing = true;
                break;
            }
        }
        if (racing) continue;
        if (oldest == nullptr || t->last_ms < oldest->last_ms) oldest = t;
    }
    (void)now;
    return oldest;
}

// To an address rather than to a client, for the messages that go to somebody
// who has no slot - a refusal, most of all. Sealed just the same: the tunnel
// exists before the client does, which is what makes this possible at all.
static void gs_send_sealed(NET_Address *addr, uint16_t port,
                           const uint8_t *buf, size_t len) {
    const char *text = NET_GetAddressString(addr);
    if (text == nullptr || len == 0) return;

    gs_tunnel *t = gs_tunnel_find(text, port);
    if (t == nullptr) return;

    uint8_t sealed[GS_PROTO_MTU], out[GS_PROTO_MTU];
    size_t n = gs_noise_seal(&t->session, buf, len, sealed, sizeof sealed);
    if (n == 0) return;
    n = gs_proto_sealed(out, sizeof out, sealed, n);
    if (n == 0) return;

    if (NET_SendDatagram(gs_srv.sock, addr, port, out, (int)n)) {
        gs_srv.total_out++;
        gs_srv.total_out_bytes += n;
    }
}

static void gs_send(gs_client *c, const uint8_t *buf, size_t len) {
    if (!c->used || len == 0) return;

    // **Nothing leaves in the clear.** A client with no tunnel gets nothing
    // rather than getting it unsealed, because a fallback to plaintext is a
    // fallback anybody on the path can force.
    gs_tunnel *t = gs_tunnel_find(c->text, c->port);
    if (t == nullptr) return;

    uint8_t sealed[GS_PROTO_MTU];
    size_t n = gs_noise_seal(&t->session, buf, len, sealed, sizeof sealed);
    if (n == 0) return;

    uint8_t out[GS_PROTO_MTU];
    n = gs_proto_sealed(out, sizeof out, sealed, n);
    if (n == 0) return;

    if (!NET_SendDatagram(gs_srv.sock, c->addr, c->port, out, (int)n)) return;
    len = n;

    c->out++;
    c->out_bytes += len;
    gs_srv.total_out++;
    gs_srv.total_out_bytes += len;
}

// Everybody hears about everybody. A lobby that only told the newcomer who was
// there would leave the people already waiting looking at a stale list.
// The wall clock in Unix seconds, for dating a session. The server links SDL, so
// unlike src/core/ it may ask what time it is.
static int64_t gs_now(void) {
    SDL_Time now = 0;
    if (!SDL_GetCurrentTime(&now)) return 0;
    return (int64_t)(now / 1000000000);
}

// How long a token is good for. Long enough that a race and its proof fit
// comfortably inside one, short enough that a machine which walked away does not
// leave something spendable lying about for a week.
#define GS_SESSION_SECONDS 3600

// Hand a client a fresh one-shot token, and remember it.
//
// **Random, and not a counter.** A predictable token is one somebody else can
// present, which would make the whole exercise decorative. SDL's generator is
// not a cryptographic one and this is not yet a cryptographic defence - it is
// the shape the defence will take, and the note in docs/THREATS.md says so
// rather than leaving a reader to assume otherwise.
static void gs_issue_session(gs_client *c) {
    if (gs_srv.store == nullptr || !c->used) return;

    for (int tries = 0; tries < 8; tries++) {
        uint64_t nonce = ((uint64_t)SDL_rand_bits() << 32) ^ (uint64_t)SDL_rand_bits();
        if (nonce == 0) continue;

        if (gs_store_issue_session(gs_srv.store, nonce, c->name, gs_now(),
                                   GS_SESSION_SECONDS)) {
            c->session = nonce;

            uint8_t buf[GS_PROTO_MTU];
            gs_send(c, buf, gs_proto_session(buf, sizeof buf, nonce));
            return;
        }
    }
}

static void gs_send_lobby(gs_client *c) {
    gs_lobby l;
    gs_build_lobby(&l);

    uint8_t buf[GS_PROTO_MTU];
    size_t n = gs_proto_lobby(buf, sizeof buf, &l);
    gs_send(c, buf, n);
}

// **Sent again on every ping, not only when it changes.**
//
// This is one datagram over UDP, which promises nothing. Announcing a departure
// once means a single lost packet leaves somebody racing against a player who
// went home - permanently, because there is no next announcement to correct it.
// It showed up as a test that passed on Linux and failed on macOS, whose default
// receive buffer for a socket is a fraction of the size, so a busy client drops
// exactly the datagram that mattered.
//
// The roster is a few dozen bytes and the ping is every two seconds. Re-sending
// state that is small and idempotent is how the rest of this protocol already
// works; a roster that is only ever announced was the odd one out.
static void gs_broadcast_lobby(void) {
    for (int i = 0; i < GS_PROTO_MAX_PLAYERS; i++) {
        gs_send_lobby(&gs_srv.client[i]);
    }
}

// **Said, and kept.** A line for the log, and for the window's list of
// arrivals and departures - which is the same line, so the two cannot
// disagree about what happened.
static void gs_note(const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(1);
static void gs_note(const char *fmt, ...) {
    char line[GS_SRV_LOG_WIDTH];
    va_list ap;
    va_start(ap, fmt);
    (void)SDL_vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    SDL_Log("%s", line);
    gs_srv_log_add(&gs_srv.log, line);
}

static void gs_drop(int slot, const char *why) {
    gs_client *c = &gs_srv.client[slot];
    if (!c->used) return;

    gs_note("player %d (%s) left: %s", slot, c->name, why);
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

        // **Through the tunnel, like everything else.** A refusal is still a
        // message from this server, and a client that would read one in the
        // clear is a client anybody can turn away by forging one.
        gs_send_sealed(addr, port, buf, n);

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
    // **A name somebody has put a password on is not one you can just type.**
    // Joining under it without proving it lands them under a name of their own
    // instead of being refused outright, because being thrown off a server for
    // picking a name that happens to be taken is a worse experience than being
    // told the name is spoken for.
    const char *wanted = (name != nullptr && name[0] != '\0') ? name : "driver";
    if (gs_srv.store != nullptr && !c->proved &&
        gs_store_has_password(gs_srv.store, wanted)) {
        SDL_Log("%s is spoken for; log in to use it", wanted);
        char spare[GS_PROTO_NAME];
        SDL_snprintf(spare, sizeof spare, "%.*s-%u",
                     (int)(sizeof spare - 8), wanted, (unsigned)(at + 1));
        SDL_strlcpy(c->name, spare, sizeof c->name);
    } else if (!c->proved) {
        SDL_strlcpy(c->name, wanted, sizeof c->name);
    }

    // **And who they actually are**, from the tunnel they had to complete
    // before this message could arrive at all. The name above is typed; this is
    // proved, and it is what every question of ownership is answered against.
    const gs_tunnel *t = gs_tunnel_find(c->text, c->port);
    if (t != nullptr) {
        SDL_memcpy(c->key, t->key, GS_NOISE_KEY_BYTES);
        c->has_key = true;
    }
    c->last_seen_ms = now;

    gs_lobby l;
    gs_build_lobby(&l);
    uint8_t buf[GS_PROTO_MTU];
    size_t n = gs_proto_welcome(buf, sizeof buf, (uint8_t)at, &l);
    gs_send(c, buf, n);

    // And what the race will be on - **always**, even when the answer is "no
    // track". A client cannot tell "there is nothing to wait for" from "the
    // message has not arrived yet" by looking at silence, and one that guessed
    // would start racing on whatever it had loaded locally.
    n = gs_proto_start(buf, sizeof buf, gs_srv.track_hash, gs_srv.capacity, 3,
                       (uint8_t)GS_MODE_RACE);
    gs_send(c, buf, n);

    if (fresh) {
        gs_note("player %d (%s) joined from %s:%u", at, c->name, c->text, port);
        if (gs_srv.store != nullptr) {
            gs_store_put_driver(gs_srv.store, c->name, 0, 0);
        }
        if (gs_present() > gs_srv.peak) gs_srv.peak = gs_present();
        gs_broadcast_lobby();
    }

    // A token to spend on the next claim. Issued on every join, fresh or
    // repeated, so a client that lost its welcome gets another rather than being
    // left unable to submit anything.
    gs_issue_session(c);
}

// --- the view ---------------------------------------------------------------

static void gs_bytes_text(char *out, size_t cap, uint64_t n) {
    if (n < 1024ull) SDL_snprintf(out, cap, "%llu B", (unsigned long long)n);
    else if (n < 1024ull * 1024ull) SDL_snprintf(out, cap, "%.1f KB", (double)n / 1024.0);
    else SDL_snprintf(out, cap, "%.1f MB", (double)n / (1024.0 * 1024.0));
}

// **Is anybody watching?** The dashboard repaints four times a second, which is
// what a person at a terminal wants and is a trap for everything else: written
// into a pipe nobody is draining it fills the buffer, and then the server
// blocks inside printf and stops answering the network. It stops being a server
// because of its own output. A pipe on Windows holds four kilobytes by default,
// which is about one second of dashboard - which is why a client on Windows
// could never join a server whose output was being captured, and why the same
// server on Linux worked for the sixteen seconds it took to fill a bigger pipe.
//
// So the dashboard is for a terminal, and anything else gets a log: the lines
// that mark events, and one line a minute saying the server is still here.
static bool gs_stdout_is_terminal(void) {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    // STDOUT_FILENO rather than fileno(stdout), which a strict -std=c2x does
    // not declare without asking for POSIX by name.
    return isatty(STDOUT_FILENO) != 0;
#endif
}

// The whole dashboard in one line, for a log. Everything a person scrolling
// back would want to know about a moment: how long it has been up, who is on
// it, and whether anything is moving.
static void gs_heartbeat(uint64_t now) {
    uint64_t up = (now - gs_srv.started_ms) / 1000u;
    SDL_Log("up %llu:%02llu:%02llu, %u of %u here, %u datagram(s) in, %u out",
            (unsigned long long)(up / 3600u),
            (unsigned long long)((up / 60u) % 60u),
            (unsigned long long)(up % 60u), gs_present(), gs_srv.capacity,
            gs_srv.total_in, gs_srv.total_out);
}

// **One set of facts for both views.** The terminal and the window draw
// from this and nothing else, so a number on one is the number on the
// other; the output check reads both back and holds them to it.
static void gs_gather(gs_srv_facts *f, uint64_t now) {
    SDL_zerop(f);
    f->port = gs_srv.port;
    f->up_s = (now - gs_srv.started_ms) / 1000u;
    f->here = gs_present();
    f->capacity = gs_srv.capacity;
    f->peak = gs_srv.peak;
    f->refused = gs_srv.refused;
    f->total_in = gs_srv.total_in;
    f->total_out = gs_srv.total_out;
    f->relayed = gs_srv.relayed;
    gs_bytes_text(f->in_bytes, sizeof f->in_bytes, gs_srv.total_in_bytes);
    gs_bytes_text(f->out_bytes, sizeof f->out_bytes, gs_srv.total_out_bytes);
    if (gs_srv.store != nullptr) {
        f->store = true;
        f->drivers = gs_store_driver_count(gs_srv.store);
        f->records = gs_store_record_count(gs_srv.store);
        f->tracks = gs_store_track_count(gs_srv.store);
        f->results = gs_srv.results;
        f->kept = gs_srv.kept;
        f->rejected = gs_srv.rejected;
    }
    if (gs_srv.track_len > 0) {
        f->track = true;
        f->track_hash = gs_srv.track_hash;
        f->track_len = gs_srv.track_len;
        f->chunks_sent = gs_srv.chunks_sent;
    }
    if (f->up_s > 0) {
        f->in_rate = (double)gs_srv.total_in / (double)f->up_s;
        f->out_rate = (double)gs_srv.total_out / (double)f->up_s;
    }
    for (int i = 0; i < GS_PROTO_MAX_PLAYERS; i++) {
        const gs_client *c = &gs_srv.client[i];
        gs_srv_row *r = &f->row[i];
        if (!c->used) continue;
        r->used = true;
        SDL_strlcpy(r->name, c->name, sizeof r->name);
        SDL_snprintf(r->from, sizeof r->from, "%s:%u", c->text, c->port);
        if (c->ping_known) SDL_snprintf(r->ping, sizeof r->ping, "%ums", c->ping_ms);
        else SDL_strlcpy(r->ping, "-", sizeof r->ping);
        r->in = c->in;
        r->out = c->out;
        // Silence is the thing worth seeing before it becomes a disconnection.
        r->quiet = (now - c->last_seen_ms) / 1000u >= 3u;
    }
}

static void gs_draw_facts(const gs_srv_facts *f) {
    if (!gs_srv.plain) {
        // Home and clear-to-end rather than a full clear: a full clear makes
        // the whole view flicker, and this one redraws four times a second.
        printf("\033[H\033[J");
    }

    printf("  gearstick server            port %u        up %llu:%02llu:%02llu\n",
           f->port, (unsigned long long)(f->up_s / 3600u),
           (unsigned long long)((f->up_s / 60u) % 60u), (unsigned long long)(f->up_s % 60u));
    printf("  ------------------------------------------------------------------\n");
    printf("  %-3s %-16s %-22s %6s %8s %8s\n",
           "", "driver", "from", "ping", "in", "out");

    for (int i = 0; i < GS_PROTO_MAX_PLAYERS; i++) {
        const gs_srv_row *r = &f->row[i];
        if (i >= f->capacity) continue;

        if (!r->used) {
            printf("  %-3d %-16s %-22s %6s %8s %8s\n", i, "-", "", "", "", "");
            continue;
        }
        printf("  %-3d %-16s %-22s %6s %8u %8u%s\n",
               i, r->name, r->from, r->ping, r->in, r->out,
               r->quiet ? "  quiet" : "");
    }

    printf("  ------------------------------------------------------------------\n");
    printf("  %u of %u here, peak %u        refused %u\n",
           f->here, f->capacity, f->peak, f->refused);
    printf("  datagrams  in %u (%s)   out %u (%s)   relayed %u\n",
           f->total_in, f->in_bytes, f->total_out, f->out_bytes, f->relayed);

    if (f->store) {
        printf("  remembered %d driver(s), %d record(s), %d track(s)"
               "   results %u, kept %u\n",
               f->drivers, f->records, f->tracks, f->results, f->kept);
        if (f->rejected > 0) {
            printf("  rejected   %u time(s) that the replay did not produce\n",
                   f->rejected);
        }
    }

    if (f->track) {
        printf("  track      %016llx, %zu bytes, %u chunks sent\n",
               (unsigned long long)f->track_hash, f->track_len, f->chunks_sent);
    }

    if (f->up_s > 0) {
        printf("  rate       %.1f in/s   %.1f out/s\n", f->in_rate, f->out_rate);
    }
    printf("\n  ctrl-c to stop\n");
    fflush(stdout);
}

static void gs_draw(uint64_t now) {
    gs_srv_facts f;
    gs_gather(&f, now);
    gs_draw_facts(&f);
}

// **The track, taken down.** It stops being served - a client that asks for
// it from now on is told there is none - and it is withdrawn from the list
// as well where the store allows. A track that shipped with the game stays
// listed, since taking one of those down is not an operator's to do; it is
// still not served by this lobby.
static void gs_take_down(void) {
    if (gs_srv.track_len == 0) return;
    const uint64_t hash = gs_srv.track_hash;
    const bool withdrawn = gs_srv.store != nullptr &&
                           gs_store_withdraw(gs_srv.store, hash, "");
    gs_srv.track_len = 0;
    gs_srv.track_hash = 0;
    gs_note("track %016llx taken down%s", (unsigned long long)hash,
            withdrawn ? ", and withdrawn from the list" : "");
}

// --- one datagram -----------------------------------------------------------

// One message, already out of its envelope. Everything below this line has
// been through the tunnel: it came from the address it says it came from, it
// has not been changed, and it has not been seen before.
static void gs_handle_plain(NET_Address *addr, uint16_t port,
                            const uint8_t *msg, size_t len, uint64_t now) {
    gs_msg kind = gs_proto_kind(msg, len);
    if (kind == GS_MSG_NONE) return;      // not ours; say nothing back

    int at = gs_find(addr, port);
    if (at >= 0) {
        gs_client *c = &gs_srv.client[at];
        c->in++;
        c->in_bytes += len;
        c->last_seen_ms = now;
    }

    switch (kind) {
    case GS_MSG_JOIN: {
        char name[GS_PROTO_NAME];
        if (gs_proto_read_join(msg, len, name, sizeof name)) {
            gs_join(addr, port, name, now);
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
        if (at >= 0 && gs_proto_read_stamp(msg, len, &stamp)) {
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
        if (at >= 0 && gs_proto_read_stamp(msg, len, &stamp)) {
            gs_client *c = &gs_srv.client[at];
            uint32_t then = stamp;
            c->ping_ms = (uint32_t)(now & 0xffffffffu) - then;
            c->ping_known = true;
        }
        break;
    }

    case GS_MSG_RELAY: {
        // **Forwarded, not understood.** The payload is a rollback datagram
        // and the server has no idea what is in it - it stamps who it came
        // from and passes it on. A server that parsed race traffic would be a
        // server that could disagree with the race.
        uint8_t from = 0;
        size_t payload_len = 0;
        const uint8_t *payload = gs_proto_payload(msg, len, &from,
                                                  &payload_len);
        if (at < 0 || payload == nullptr) break;

        uint8_t out[GS_PROTO_MTU];
        size_t n = gs_proto_forward(out, sizeof out, (uint8_t)at, payload,
                                    payload_len);
        for (int k = 0; k < GS_PROTO_MAX_PLAYERS; k++) {
            if (k == at) continue;
            gs_send(&gs_srv.client[k], out, n);
            if (gs_srv.client[k].used) gs_srv.relayed++;
        }
        break;
    }

    case GS_MSG_RESULT: {
        // A time, offered. **Believed, for now** - the item after this one has
        // the server re-race the inputs before it accepts anything, and this
        // message does not change when that happens. Only what is done with it
        // does.
        uint64_t track = 0, conditions = 0;
        uint16_t laps = 0;
        uint8_t vehicle = 0;
        uint32_t lap_ticks = 0, race_ticks = 0;

        if (at < 0 || gs_srv.store == nullptr) break;
        uint64_t nonce = 0;
        if (!gs_proto_read_result(msg, len, &track, &conditions, &laps,
                                  &vehicle, &lap_ticks, &race_ticks, &nonce)) {
            break;
        }

        gs_srv.results++;

        // **Held, not believed.** The claim waits for the inputs that produced
        // it; nothing goes in the table until the server has re-raced them.
        gs_client *cl = &gs_srv.client[at];
        cl->claim = (gs_claim){ 0 };
        cl->claim.track = track;
        cl->claim.conditions = conditions;
        cl->claim.laps = laps;
        cl->claim.car = 0;
        cl->claim.lap_ticks = lap_ticks;
        cl->claim.race_ticks = race_ticks;

        // **Who the server believes is claiming it**, which is the name they
        // joined under and not one they put in the message. That is still only
        // as good as knowing who is on the other end of the socket - today,
        // nothing - but it means the recording has to name the person the server
        // already thinks it is talking to, so a replay picked up elsewhere
        // cannot be handed in as their own. See docs/THREATS.md.
        for (int k = 0; k < GS_REPLAY_NAME - 1 && cl->name[k] != '\0'; k++) {
            cl->claim.who[k] = cl->name[k];
        }

        cl->nonce = nonce;
        cl->claimed = true;
        gs_carrier_expect(&cl->proof, track);
        break;
    }

    case GS_MSG_PROOF: {
        // The inputs behind a claim, in pieces. Reassembled with the same
        // carrier a track uses - it keys on a hash and refuses a length the
        // datagram does not contain, which is what a proof needs too.
        if (at < 0 || gs_srv.store == nullptr) break;

        gs_client *cl = &gs_srv.client[at];
        if (!cl->claimed) break;

        uint64_t hash = 0;
        uint16_t chunk = 0, chunks = 0, data_len = 0;
        const uint8_t *data = nullptr;
        if (!gs_proto_read_proof_chunk(msg, len, &hash, &chunk, &chunks,
                                       &data, &data_len)) {
            break;
        }
        if (hash != cl->proof.hash) break;

        // The carrier reads track chunks; a proof chunk is the same shape with
        // a different name, so it is handed over as one.
        uint8_t as_track[GS_PROTO_MTU];
        size_t n = gs_proto_track_chunk(as_track, sizeof as_track, hash, chunk,
                                        chunks, data, data_len);
        if (n == 0 || !gs_carrier_take(&cl->proof, as_track, n)) break;
        if (!gs_carrier_done(&cl->proof)) break;

        // Everything is here. Re-race it.
        static gs_track t;
        static uint8_t track_bytes[GS_CARRIER_MAX_BYTES];
        size_t track_len = 0;
        // **A time on today's track is a time on a track this server can
        // build for itself.** The daily is made from the date, the same on
        // every machine, so a server that was never sent it generates today's
        // and yesterday's, and if the hash matches, keeps it as if it had
        // been published - and the time is then re-raced like any other.
        if (!gs_store_has_track(gs_srv.store, cl->claim.track)) {
            static gs_track daily;
            const uint64_t days = (uint64_t)gs_now() / 86400u;
            if (days >= GS_DAILY_EPOCH_DAY &&
                gs_daily_track_for_hash(&daily, (uint32_t)(days - GS_DAILY_EPOCH_DAY),
                                        cl->claim.track)) {
                const size_t daily_len = gs_track_serialize(&daily, track_bytes, sizeof track_bytes);
                if (daily_len > 0 &&
                    gs_store_put_track(gs_srv.store, cl->claim.track, "today's track",
                                       "the calendar", track_bytes, daily_len)) {
                    gs_store_publish(gs_srv.store, cl->claim.track, "today's track",
                                     "the calendar");
                    SDL_Log("%s claimed a time on today's track; built it from the date",
                            cl->name);
                }
            }
        }
        if (!gs_store_get_track(gs_srv.store, cl->claim.track, track_bytes,
                                sizeof track_bytes, &track_len) ||
            !gs_track_deserialize(&t, track_bytes, track_len)) {
            SDL_Log("%s claimed a time on a track this server does not have",
                    cl->name);
            cl->claimed = false;
            gs_srv.rejected++;
            break;
        }

        gs_verdict v = gs_verify_bytes(cl->proof.bytes, cl->proof.len, &t,
                                       &cl->claim, nullptr);
        cl->claimed = false;

        if (v != GS_VERDICT_OK) {
            SDL_Log("%s: time rejected - %s", cl->name, gs_verdict_text(v));
            gs_srv.rejected++;
            gs_issue_session(cl);
            break;
        }

        // **The token, spent once.**
        //
        // After the driving is verified rather than before: re-racing is what
        // says the time is real, and a token burnt on a claim that turned out to
        // be nonsense would cost an honest client its next submission for
        // somebody else's mistake.
        //
        // The store does the checking, in one statement - issued, to this
        // person, unspent, in date - because reading a row and then updating it
        // leaves a gap, and the gap is where one token is spent twice.
        if (!gs_store_spend_session(gs_srv.store, cl->nonce, cl->name, gs_now())) {
            SDL_Log("%s: time rejected - the session token was not one I issued, "
                    "or it has been used already", cl->name);
            gs_srv.rejected++;
            gs_issue_session(cl);
            break;
        }

        // And another for next time, so a client can submit again without
        // rejoining.
        gs_issue_session(cl);

        if (gs_store_put_record(gs_srv.store, cl->claim.track,
                                cl->claim.conditions, cl->claim.laps, cl->name,
                                0, cl->claim.lap_ticks, cl->claim.race_ticks)) {
            gs_srv.kept++;
        }
        SDL_Log("%s: time verified by re-racing it", cl->name);
        break;
    }

    case GS_MSG_TRACK: {
        // A track arriving *from* a client, for the library. The same chunks
        // the server sends, going the other way - and checked the same way,
        // because a track that arrived damaged is not a track.
        if (at < 0 || gs_srv.store == nullptr) break;

        gs_client *up = &gs_srv.client[at];
        uint64_t hash = 0;
        uint16_t chunk = 0, chunks = 0, data_len = 0;
        const uint8_t *data = nullptr;
        if (!gs_proto_read_track_chunk(msg, len, &hash, &chunk, &chunks,
                                       &data, &data_len)) {
            break;
        }

        if (up->upload.hash != hash) gs_carrier_expect(&up->upload, hash);
        if (!gs_carrier_take(&up->upload, msg, len)) break;
        if (!gs_carrier_done(&up->upload)) break;

        static gs_track arrived;
        if (!gs_carrier_track(&up->upload, &arrived)) {
            SDL_Log("%s uploaded a track that did not survive the trip", up->name);
            break;
        }

        gs_store_put_track(gs_srv.store, hash, "", up->name,
                           up->upload.bytes, up->upload.len);

        // **Uploading it is claiming it**, and the first claim wins. A track is
        // content-addressed, so somebody uploading a copy of a track already
        // here has uploaded the same track and does not take it from whoever
        // got there first - and a track that shipped with the game refuses the
        // claim outright.
        if (up->has_key) gs_store_claim_track(gs_srv.store, hash, up->key);

        SDL_Log("%s uploaded %016llx", up->name, (unsigned long long)hash);
        break;
    }

    case GS_MSG_PUBLISH: {
        uint64_t track = 0;
        char name[48] = { 0 };
        if (at < 0 || gs_srv.store == nullptr) break;
        if (!gs_proto_read_publish(msg, len, &track, name, sizeof name)) break;

        // **Only a track the server has.** Publishing is a claim about
        // something already here; the track itself arrives the way tracks
        // always do, checked against its own hash on the way in.
        if (!gs_store_has_track(gs_srv.store, track)) {
            SDL_Log("%s tried to publish a track this server does not have",
                    gs_srv.client[at].name);
            break;
        }
        // **Publishing is something an owner does**, so the first thing that
        // happens is the claim - which the server can only grant to a key the
        // client proved it holds. A track that shipped with the game refuses
        // both, whoever is asking.
        gs_client *pub = &gs_srv.client[at];
        if (!pub->has_key) break;

        if (!gs_store_claim_track(gs_srv.store, track, pub->key)) {
            SDL_Log("%s tried to publish a track that is not theirs", pub->name);
            break;
        }
        gs_store_name_track(gs_srv.store, track, pub->key, name);
        if (gs_store_set_visible(gs_srv.store, track, pub->key, GS_TRACK_PUBLIC)) {
            SDL_Log("%s published %016llx (%s)", pub->name,
                    (unsigned long long)track, name);
        }
        break;
    }

    case GS_MSG_WITHDRAW: {
        uint64_t track = 0;
        if (at < 0 || gs_srv.store == nullptr) break;
        if (!gs_proto_read_withdraw(msg, len, &track)) break;

        gs_client *own = &gs_srv.client[at];
        if (!own->has_key) break;

        if (gs_store_set_visible(gs_srv.store, track, own->key, GS_TRACK_PRIVATE)) {
            SDL_Log("%s withdrew %016llx", own->name,
                    (unsigned long long)track);
        } else {
            SDL_Log("%s tried to withdraw a track that is not theirs", own->name);
        }
        break;
    }

    case GS_MSG_LOGIN: {
        char who[GS_PROTO_NAME] = { 0 };
        char password[GS_PROTO_SECRET] = { 0 };
        uint32_t code = 0;
        if (at < 0 || gs_srv.store == nullptr) break;
        if (!gs_proto_read_login(msg, len, who, sizeof who, password,
                                 sizeof password, &code)) {
            break;
        }

        gs_client *c = &gs_srv.client[at];
        char hash[GS_AUTH_HASH_BYTES];

        // A name with no password is not something anybody logs in to, and
        // saying so is not a leak: whether a name is taken is already visible
        // from the lobby.
        if (!gs_store_password(gs_srv.store, who, hash, sizeof hash)) {
            SDL_Log("%s asked to log in as %s, which has no password", c->name, who);
            break;
        }
        if (!gs_auth_check_password(hash, password)) {
            SDL_Log("%s failed to log in as %s", c->name, who);
            gs_srv.refused++;
            break;
        }

        // **And the second factor, if that name has one.** The code has to be
        // current *and* not already spent - a window without a spend is a
        // window in which a code works more than once.
        uint8_t secret[GS_STORE_TOTP];
        size_t secret_len = 0;
        if (gs_store_totp(gs_srv.store, who, secret, sizeof secret, &secret_len)) {
            int64_t step = 0;
            if (!gs_auth_check_code(secret, secret_len, code, gs_now(), 1, &step) ||
                !gs_store_totp_use(gs_srv.store, who, step)) {
                SDL_Log("%s gave a code for %s that was wrong or already used",
                        c->name, who);
                gs_srv.refused++;
                sodium_memzero(secret, sizeof secret);
                break;
            }
        }
        sodium_memzero(secret, sizeof secret);
        sodium_memzero(password, sizeof password);

        SDL_strlcpy(c->name, who, sizeof c->name);
        c->proved = true;
        SDL_Log("%s proved the name is theirs", c->name);
        gs_broadcast_lobby();
        break;
    }

    case GS_MSG_CLAIM: {
        char who[GS_PROTO_NAME] = { 0 };
        char password[GS_PROTO_SECRET] = { 0 };
        uint8_t secret[GS_STORE_TOTP];
        size_t secret_len = 0;
        if (at < 0 || gs_srv.store == nullptr) break;
        if (!gs_proto_read_claim(msg, len, who, sizeof who, password,
                                 sizeof password, secret, sizeof secret,
                                 &secret_len)) {
            break;
        }

        gs_client *c = &gs_srv.client[at];

        // **A name with a password already on it is only changed by whoever
        // proved it.** A name with none is unclaimed and anybody may take it,
        // which is what claiming means; without that rule there would be no way
        // to get a first password at all.
        bool taken = gs_store_has_password(gs_srv.store, who);
        if (taken && !(c->proved && SDL_strcmp(c->name, who) == 0)) {
            SDL_Log("%s tried to put a password on %s, which is spoken for",
                    c->name, who);
            gs_srv.refused++;
            sodium_memzero(password, sizeof password);
            break;
        }
        if (password[0] == '\0') {
            sodium_memzero(password, sizeof password);
            break;
        }

        char hash[GS_AUTH_HASH_BYTES];
        if (!gs_auth_hash_password(password, hash, sizeof hash) ||
            !gs_store_set_password(gs_srv.store, who, hash)) {
            sodium_memzero(password, sizeof password);
            break;
        }
        sodium_memzero(password, sizeof password);

        // A second factor for anybody who wants one, and none for anybody who
        // does not - an empty secret takes it off again.
        gs_store_set_totp(gs_srv.store, who,
                          secret_len > 0 ? secret : nullptr, secret_len);
        sodium_memzero(secret, sizeof secret);

        SDL_strlcpy(c->name, who, sizeof c->name);
        c->proved = true;
        SDL_Log("%s is now %s's, with%s a second factor", who, who,
                secret_len > 0 ? "" : "out");
        gs_broadcast_lobby();
        break;
    }

    case GS_MSG_SHARE: {
        uint64_t track = 0;
        uint8_t with[GS_NOISE_KEY_BYTES];
        bool on = false;
        if (at < 0 || gs_srv.store == nullptr) break;
        if (!gs_proto_read_share(msg, len, &track, with, &on)) break;

        gs_client *sharer = &gs_srv.client[at];
        if (!sharer->has_key) break;

        // Sharing implies the track is shared rather than private, and the
        // store refuses all of it for a track that is not theirs.
        bool ok = on ? (gs_store_set_visible(gs_srv.store, track, sharer->key,
                                             GS_TRACK_SHARED) &&
                        gs_store_share_track(gs_srv.store, track, sharer->key,
                                             with))
                     : gs_store_unshare_track(gs_srv.store, track, sharer->key,
                                              with);
        SDL_Log("%s %s %016llx %s somebody", sharer->name,
                ok ? (on ? "shared" : "unshared") : "could not share",
                (unsigned long long)track, on ? "with" : "from");
        break;
    }

    case GS_MSG_WANT_LIST: {
        if (at < 0 || gs_srv.store == nullptr) break;

        // **What this client may see**, which is the shipped and the public
        // plus their own and whatever was shared with them by name. Listing
        // what is published would show everybody the same library and make
        // "handed to a named few" a label rather than a rule.
        const gs_client *asker = &gs_srv.client[at];
        static gs_track_row rows[64];
        int n = gs_store_list_visible(gs_srv.store,
                                      asker->has_key ? asker->key : nullptr,
                                      rows, 64);

        // One per datagram, each saying how many there are, so a client knows
        // when it has them all without anybody counting acknowledgements.
        for (int i = 0; i < n; i++) {
            uint8_t out[GS_PROTO_MTU];
            size_t m = gs_proto_listing(out, sizeof out, (uint16_t)i,
                                        (uint16_t)n, rows[i].hash, rows[i].name,
                                        rows[i].author);
            gs_send(&gs_srv.client[at], out, m);
        }

        // Nothing published is still an answer. Without it a client cannot tell
        // an empty library from a server that ignored the question.
        if (n == 0) {
            uint8_t out[GS_PROTO_MTU];
            size_t m = gs_proto_listing(out, sizeof out, 0, 0, 0, "", "");
            gs_send(&gs_srv.client[at], out, m);
        }
        break;
    }

    case GS_MSG_WANT_BEST: {
        uint64_t track = 0, conditions = 0;
        uint16_t laps = 0;
        if (at < 0 || gs_srv.store == nullptr) break;
        if (!gs_proto_read_want_best(msg, len, &track, &conditions, &laps)) {
            break;
        }

        char lap_who[GS_PROTO_NAME] = { 0 }, race_who[GS_PROTO_NAME] = { 0 };
        uint32_t lap = gs_store_best_lap(gs_srv.store, track, conditions,
                                         lap_who, sizeof lap_who);
        uint32_t race = gs_store_best_race(gs_srv.store, track, conditions, laps,
                                           race_who, sizeof race_who);

        uint8_t out[GS_PROTO_MTU];
        size_t n = gs_proto_best(out, sizeof out, track, conditions, laps, lap,
                                 lap_who, race, race_who);
        gs_send(&gs_srv.client[at], out, n);
        break;
    }

    case GS_MSG_WANT_TRACK: {
        // Asked for again rather than acknowledged, which is the vocabulary
        // everywhere else here: a client that is missing a piece asks for the
        // track, and gets all of it. Resending a chunk somebody already has
        // costs one datagram and no bookkeeping at all.
        uint64_t want = 0;
        if (at < 0 || !gs_proto_read_want_track(msg, len, &want)) break;

        // Any track the server has, not only the one this lobby is racing: a
        // client browsing what is published wants to fetch one of those.
        static uint8_t bytes[GS_CARRIER_MAX_BYTES];
        size_t bytes_len = 0;

        if (gs_srv.track_len > 0 && want == gs_srv.track_hash) {
            SDL_memcpy(bytes, gs_srv.track, gs_srv.track_len);
            bytes_len = gs_srv.track_len;
        } else if (gs_srv.store == nullptr ||
                   !gs_store_get_track(gs_srv.store, want, bytes, sizeof bytes,
                                       &bytes_len)) {
            break;
        }

        // A bounded burst, carrying on from where the last one stopped, so a
        // track of any size arrives over a few asks rather than being lost in
        // one. Thirty-two kilobytes at a time is comfortably inside any
        // receive buffer and still fills a track in four or five asks.
        uint16_t chunks = gs_carrier_chunks(bytes_len);
        uint16_t burst = chunks < GS_SRV_CHUNK_BURST ? chunks : GS_SRV_CHUNK_BURST;
        gs_client *to = &gs_srv.client[at];

        for (uint16_t k = 0; k < burst; k++) {
            uint16_t i = (uint16_t)((to->sending + k) % chunks);
            uint8_t out[GS_PROTO_MTU];
            size_t n = gs_carrier_chunk(out, sizeof out, want, bytes, bytes_len, i);
            gs_send(to, out, n);
            gs_srv.chunks_sent++;
        }
        to->sending = (uint16_t)((to->sending + burst) % chunks);
        break;
    }

    default:
        // Everything else belongs to items not built yet - the relay, records.
        // Ignored rather than guessed at.
        break;
    }
}
// --- the envelope, and the two things that come outside it ------------------

static void gs_handle(NET_Datagram *d, uint64_t now) {
    size_t len = (size_t)d->buflen;

    gs_srv.total_in++;
    gs_srv.total_in_bytes += len;

    const char *text = NET_GetAddressString(d->addr);
    if (text == nullptr) return;

    gs_msg kind = gs_proto_kind(d->buf, len);

    // **A handshake, which cannot be sealed because it is what makes sealing
    // possible.** One datagram in, one out: IK's responder can answer
    // immediately, so there is no half-finished handshake to keep anywhere and
    // nothing for somebody to fill a table with.
    if (kind == GS_MSG_HANDSHAKE) {
        const uint8_t *msg = nullptr;
        size_t msg_len = 0;
        if (!gs_proto_read_handshake(d->buf, len, &msg, &msg_len)) return;

        gs_noise_handshake hs;
        uint8_t payload[GS_PROTO_MTU];
        size_t got = 0;

        gs_noise_init_responder(&hs, &gs_srv.identity,
                                (const uint8_t *)GS_SRV_PROLOGUE,
                                sizeof GS_SRV_PROLOGUE - 1);
        if (!gs_noise_read_message(&hs, msg, msg_len, payload, sizeof payload,
                                   &got)) {
            gs_srv.refused_sealed++;
            return;
        }

        uint8_t reply[GS_PROTO_MTU], out[GS_PROTO_MTU];
        size_t n = gs_noise_write_message(&hs, nullptr, 0, reply, sizeof reply);
        if (n == 0) return;

        // **A handshake from an address that is already racing is refused.**
        //
        // Message one is replayable - anybody who captured one can send it
        // again, and the responder cannot tell the difference, because telling
        // the difference is what a session nonce is for and there is no session
        // yet. Accepting it would install a tunnel whose keys the real client
        // does not have, and knock a racing player off with a packet somebody
        // recorded. So a client that genuinely restarted waits out the silence
        // timeout and is dropped first, which costs it a few seconds and costs
        // an attacker the whole trick.
        if (gs_find(d->addr, d->port) >= 0) {
            gs_srv.refused_sealed++;
            return;
        }

        const uint8_t *theirs = gs_noise_remote_static(&hs);
        if (theirs == nullptr) return;

        gs_tunnel *t = gs_tunnel_slot(text, d->port, now);
        if (t == nullptr) return;

        SDL_zerop(t);
        SDL_memcpy(t->key, theirs, GS_NOISE_KEY_BYTES);
        t->used = true;
        t->port = d->port;
        SDL_strlcpy(t->text, text, sizeof t->text);
        t->last_ms = now;
        if (!gs_noise_split(&hs, &t->session)) {
            t->used = false;
            return;
        }

        size_t framed = gs_proto_handshake(out, sizeof out, reply, n);
        if (framed > 0 &&
            NET_SendDatagram(gs_srv.sock, d->addr, d->port, out, (int)framed)) {
            gs_srv.total_out++;
            gs_srv.total_out_bytes += framed;
        }
        gs_srv.handshakes++;
        return;
    }

    // Everything else has to arrive sealed. A message in the clear once the
    // tunnel exists is either somebody who has not handshaked or somebody
    // hoping this end will accept plaintext if asked nicely; both get nothing.
    if (kind != GS_MSG_SEALED) {
        gs_srv.refused_clear++;
        return;
    }

    const uint8_t *body = nullptr;
    size_t body_len = 0;
    if (!gs_proto_read_sealed(d->buf, len, &body, &body_len)) return;

    gs_tunnel *t = gs_tunnel_find(text, d->port);
    if (t == nullptr) {
        gs_srv.refused_sealed++;
        return;
    }

    uint8_t plain[GS_PROTO_MTU];
    size_t got = 0;
    if (!gs_noise_open(&t->session, body, body_len, plain, sizeof plain, &got)) {
        gs_srv.refused_sealed++;
        return;
    }
    t->last_ms = now;

    gs_handle_plain(d->addr, d->port, plain, got, now);
}


// --- running ----------------------------------------------------------------

static void gs_on_signal(int sig) {
    (void)sig;
    gs_srv.quit = true;
}

// The track the server will hand out. Without one it is still a lobby, and
// everybody has to already agree about the ground - which is exactly the
// limitation this removes.
static bool gs_load_track(const char *path) {
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (io == nullptr) return false;

    size_t n = SDL_ReadIO(io, gs_srv.track, sizeof gs_srv.track);
    SDL_CloseIO(io);
    if (n == 0) return false;

    static gs_track probe;
    if (!gs_track_deserialize(&probe, gs_srv.track, n)) return false;

    gs_srv.track_len = n;
    gs_srv.track_hash = gs_track_hash(&probe);
    return true;
}

// **The library a server ships with.**
//
// A server nobody has set up still has to have something to offer, or the first
// person to connect finds an empty list and no reason to stay. The shipped
// database is copied into place rather than opened where it lies: the copy is
// the server's to write to, and the original stays as it was built so the next
// fresh server gets the same start.
//
// Only when there is nothing there. A store that exists is somebody's history
// and is never overwritten, and `:memory:` is a test asking for nothing to be
// remembered at all.
static void gs_seed_store(const char *path) {
    if (SDL_strcmp(path, ":memory:") == 0) return;

    SDL_PathInfo info;
    if (SDL_GetPathInfo(path, &info)) return;          // already there

    // **A journal without its database is not a journal.** SQLite keeps its
    // write-ahead log in files beside the database, and if one is left behind by
    // something that removed only the database itself, the next open replays it
    // onto whatever is there now - which, once a fresh store is a *copy* of a
    // shipped one, means yesterday's rows appearing inside today's library. The
    // database is known not to exist at this point, so anything claiming to be
    // its journal is orphaned.
    char side[1024];
    SDL_snprintf(side, sizeof side, "%s-wal", path);
    remove(side);
    SDL_snprintf(side, sizeof side, "%s-shm", path);
    remove(side);

    char shipped[1024];
    gs_asset_path(shipped, sizeof shipped, "server/gearstick.db");

    size_t len = 0;
    void *bytes = SDL_LoadFile(shipped, &len);
    if (bytes == nullptr) {
        SDL_Log("no shipped library at %s - starting empty", shipped);
        return;
    }

    if (SDL_SaveFile(path, bytes, len)) {
        SDL_Log("started %s from the shipped library (%zu bytes)", path, len);
    } else {
        SDL_Log("could not write %s: %s", path, SDL_GetError());
    }
    SDL_free(bytes);
}

// Take the track out of the store and make it the one this lobby races.
static bool gs_serve_track(uint64_t hash) {
    if (!gs_store_get_track(gs_srv.store, hash, gs_srv.track,
                            sizeof gs_srv.track, &gs_srv.track_len)) {
        return false;
    }

    static gs_track probe;
    if (!gs_track_deserialize(&probe, gs_srv.track, gs_srv.track_len)) {
        gs_srv.track_len = 0;
        return false;
    }

    // Derived from the bytes rather than taken from the row that named them: a
    // track's identity is what it is, not what a column says it is.
    gs_srv.track_hash = gs_track_hash(&probe);
    return true;
}

static void gs_usage(void) {
    printf("gearstick_server - the meeting point for online races\n\n");
    printf("  --port N       listen on this port (default %u)\n", GS_DEFAULT_PORT);
    printf("  --players N    how many to allow, 1 to %d (default %d)\n",
           GS_PROTO_MAX_PLAYERS, GS_PROTO_MAX_PLAYERS);
    printf("  --track FILE   the track this lobby races on\n");
    printf("  --store FILE   where to remember drivers, records and tracks\n");
    printf("  --plain        no cursor control, for a dumb terminal.\n");
    printf("                 The dashboard is only drawn to a terminal at "
           "all; anything\n");
    printf("                 else - a pipe, a file - gets the log instead.\n");
    printf("  --headless     no window, even on a machine with a display\n");
    printf("  --timeout N    drop a client after N ms of silence (default %u)\n",
           GS_TIMEOUT_MS);
    printf("  --key HEX      this server's 32-byte secret, as 64 hex "
           "characters.\n");
    printf("                 Otherwise it is read from the store, or minted "
           "once and kept.\n");
    printf("  --seconds N    stop after N seconds, for tests\n");
    printf("  --window-dump  and then print what the window showed, for tests\n");
    printf("  --window-shot FILE  and then write the window's last frame as a BMP\n");
    printf("  --window-press LABEL  press this button in the window the first time it\n"
           "                 is drawn, for tests\n");
    printf("  --help\n");
}

int main(int argc, char **argv) {
    // **Unbuffered, because somebody is reading this while it runs.** stdout to
    // a pipe or a file is fully buffered by default, so the key line - which
    // nobody can connect without - would sit in a four-kilobyte buffer until
    // the server exited. It was only ever visible because the dashboard used to
    // flood that buffer several times a second, which is the same flood that
    // could block the server; fixing one uncovered the other. Windows makes
    // this the only answer of the three: setvbuf there treats line buffering as
    // full buffering, so _IONBF is what actually reaches a reader.
    (void)setvbuf(stdout, nullptr, _IONBF, 0);

    uint16_t port = GS_DEFAULT_PORT;
    uint8_t players = GS_PROTO_MAX_PLAYERS;
    uint32_t seconds = 0;
    const char *track_path = nullptr;
    const char *store_path = "gearstick.db";
    const char *key_hex = nullptr;

    for (int i = 1; i < argc; i++) {
        if (SDL_strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = (uint16_t)SDL_atoi(argv[++i]);
        } else if (SDL_strcmp(argv[i], "--players") == 0 && i + 1 < argc) {
            int n = SDL_atoi(argv[++i]);
            players = (uint8_t)SDL_clamp(n, 1, GS_PROTO_MAX_PLAYERS);
        } else if (SDL_strcmp(argv[i], "--store") == 0 && i + 1 < argc) {
            store_path = argv[++i];
        } else if (SDL_strcmp(argv[i], "--track") == 0 && i + 1 < argc) {
            track_path = argv[++i];
        } else if (SDL_strcmp(argv[i], "--key") == 0 && i + 1 < argc) {
            key_hex = argv[++i];
        } else if (SDL_strcmp(argv[i], "--plain") == 0) {
            gs_srv.plain = true;
        } else if (SDL_strcmp(argv[i], "--headless") == 0) {
            gs_srv.headless = true;
        } else if (SDL_strcmp(argv[i], "--window-dump") == 0) {
            gs_srv.window_dump = true;
        } else if (SDL_strcmp(argv[i], "--window-shot") == 0 && i + 1 < argc) {
            gs_srv.window_shot = argv[++i];
        } else if (SDL_strcmp(argv[i], "--window-press") == 0 && i + 1 < argc) {
            gs_window_press(argv[++i]);
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

    // A track named on the command line is *imported*, not served from where it
    // lies. Everything this server knows is in the store; a file is a way to put
    // something into it and never a second place it is kept.
    uint64_t wanted = 0;
    if (track_path != nullptr) {
        if (!gs_load_track(track_path)) {
            printf("could not read a track from %s\n", track_path);
            NET_DestroyDatagramSocket(gs_srv.sock);
            NET_Quit();
            SDL_Quit();
            return 1;
        }
        wanted = gs_srv.track_hash;
    }

    // Opened before anybody can knock, so a server that cannot remember
    // anything says so at the start rather than the first time it tries.
    gs_seed_store(store_path);
    gs_srv.store = gs_store_open(store_path);
    if (gs_srv.store == nullptr) {
        printf("could not open the store at %s\n", store_path);
        NET_DestroyDatagramSocket(gs_srv.sock);
        NET_Quit();
        SDL_Quit();
        return 1;
    }
    SDL_Log("store %s: %d driver(s), %d record(s), %d track(s)", store_path,
            gs_store_driver_count(gs_srv.store),
            gs_store_record_count(gs_srv.store),
            gs_store_track_count(gs_srv.store));

    // **Who this server is.** From the command line for a test that needs to
    // know it in advance, otherwise from the store, otherwise minted once and
    // kept - because a server that generated a new identity on every restart
    // would be a different server every time, and every client that had been
    // told which one to trust would be right to refuse it.
    if (sodium_init() < 0) {
        printf("could not start libsodium\n");
        return 1;
    }

    uint8_t secret[GS_STORE_IDENTITY_BYTES];
    if (key_hex != nullptr) {
        if (SDL_strlen(key_hex) != 64 ||
            sodium_hex2bin(secret, sizeof secret, key_hex, 64, nullptr, nullptr,
                           nullptr) != 0) {
            printf("--key wants 64 hex characters, which is a 32-byte secret\n");
            return 2;
        }
        gs_noise_key_from_secret(&gs_srv.identity, secret);
    } else if (gs_store_identity(gs_srv.store, secret)) {
        gs_noise_key_from_secret(&gs_srv.identity, secret);
    } else {
        gs_noise_keygen(&gs_srv.identity);
        if (!gs_store_set_identity(gs_srv.store, gs_srv.identity.sec)) {
            printf("could not write this server's identity to the store\n");
            return 1;
        }
        SDL_Log("a new identity was minted and kept");
    }

    // Printed every time, because a client cannot connect without it: IK means
    // the client already knows the server's key, which is what stops somebody
    // in the middle answering in its place.
    char pub_hex[65];
    sodium_bin2hex(pub_hex, sizeof pub_hex, gs_srv.identity.pub,
                   GS_NOISE_KEY_BYTES);
    SDL_Log("this server's public key is %s", pub_hex);
    printf("  key %s\n", pub_hex);

    // The imported one goes in, and is published: somebody who ran a server with
    // a track meant that track to be raced.
    if (wanted != 0 && gs_srv.track_len > 0) {
        gs_store_put_track(gs_srv.store, wanted, "", "",
                           gs_srv.track, gs_srv.track_len);
        gs_store_publish(gs_srv.store, wanted, "", "");
    }

    // **What this lobby races comes out of the store**, whether it arrived by
    // being imported a moment ago or by having shipped with the server. Nothing
    // else is a track this server can be asked for.
    if (wanted == 0) {
        gs_track_row rows[1];
        if (gs_store_list_published(gs_srv.store, rows, 1) == 1) wanted = rows[0].hash;
    }

    gs_srv.track_len = 0;
    if (wanted != 0 && gs_serve_track(wanted)) {
        SDL_Log("serving track %016llx, %zu bytes in %u chunks",
                (unsigned long long)gs_srv.track_hash, gs_srv.track_len,
                gs_carrier_chunks(gs_srv.track_len));
    } else if (wanted != 0) {
        SDL_Log("the library has no usable track %016llx",
                (unsigned long long)wanted);
    }

    SDL_Log("gearstick server listening on port %u for up to %u players",
            port, players);

    gs_srv.tty = gs_stdout_is_terminal();
    if (!gs_srv.tty) {
        SDL_Log("this output is not a terminal, so there is no dashboard - "
                "a line a minute says the server is still here");
    }

    // **A window as well, where there is a screen.** The terminal view and
    // the log stay exactly as they are; a machine with a display gets the
    // same facts in a window, with a list of who came and went and a button
    // beside each row. No display is the usual case for a server and is not
    // an error - it is said once, and that is all.
    if (!gs_srv.headless) {
        char icon_path[1024];
        gs_asset_path(icon_path, sizeof icon_path, "icon.png");
        if (gs_window_open(icon_path)) {
            SDL_Log("a window is open as well; closing it stops the server");
        } else {
            SDL_Log("no display, so no window - the terminal is the view");
        }
    }

    uint64_t last_draw = 0;
    uint64_t last_frame = 0;
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

                // And who is here, so a roster lost on the way out is corrected
                // two seconds later rather than never.
                gs_send_lobby(c);
                c->pinged_ms = now;
            }
        }

        // **A terminal gets the dashboard; anything else gets a log.** Drawn
        // on a timer into a pipe nobody reads, the dashboard is what stops
        // this server serving - see gs_stdout_is_terminal.
        if (now - last_draw >= (gs_srv.tty ? GS_DRAW_MS : GS_HEARTBEAT_MS)) {
            if (gs_srv.tty) {
                gs_draw(now);
            } else {
                gs_heartbeat(now);
            }
            last_draw = now;
        }

        // The window: its events every time round, a frame thirty times a
        // second, and whatever the operator pressed acted on here, where
        // the clients and the store are.
        if (gs_window_is_open()) {
            if (!gs_window_pump()) {
                gs_note("the window was closed: stopping");
                gs_srv.quit = true;
            } else if (now - last_frame >= GS_FRAME_MS) {
                gs_srv_facts f;
                gs_srv_ask ask;
                gs_gather(&f, now);
                gs_window_draw(&f, &gs_srv.log, &ask, false);
                if (ask.drop_slot >= 0) gs_drop(ask.drop_slot, "dropped by the operator");
                if (ask.take_down) gs_take_down();
                last_frame = now;
            }
        }

        if (seconds > 0 && (now - gs_srv.started_ms) / 1000u >= seconds) break;

        // A relayed race is one datagram per player per tick at 120 Hz coming
        // through here and going straight back out. Sleeping five milliseconds
        // between drains lets a burst pile up in the socket buffer and be
        // dropped; one keeps the queue short without spinning a core.
        SDL_Delay(1);
    }

    // For the test that reads the window: one last frame of both views from
    // one set of facts, and the window's lines printed after the terminal's.
    if ((gs_srv.window_dump || gs_srv.window_shot != nullptr) && gs_window_is_open()) {
        gs_srv_facts f;
        gs_srv_ask ask;
        gs_gather(&f, SDL_GetTicks());
        if (gs_srv.tty) gs_draw_facts(&f);
        gs_window_draw(&f, &gs_srv.log, &ask, gs_srv.window_dump);
        if (gs_srv.window_dump) gs_window_dump_print();
        if (gs_srv.window_shot != nullptr && !gs_window_shot(gs_srv.window_shot)) {
            SDL_Log("could not write %s: %s", gs_srv.window_shot, SDL_GetError());
        }
    }
    gs_window_close();

    printf("\nstopping\n");
    gs_store_close(gs_srv.store);
    for (int i = 0; i < GS_PROTO_MAX_PLAYERS; i++) {
        if (gs_srv.client[i].addr != nullptr) NET_UnrefAddress(gs_srv.client[i].addr);
    }
    NET_DestroyDatagramSocket(gs_srv.sock);
    NET_Quit();
    SDL_Quit();
    return 0;
}

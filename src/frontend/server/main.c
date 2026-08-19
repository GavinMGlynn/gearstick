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
#include "core/gs_sim.h"
#include "core/gs_track.h"
#include "net/gs_carrier.h"
#include "net/gs_proto.h"
#include "net/gs_store.h"
#include "net/gs_verify.h"
#include "platform/gs_paths.h"

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

    // The replay behind whatever time this client last claimed. Per client,
    // because two people can finish a race at the same moment and each one's
    // proof is their own.
    gs_carrier proof;
    bool       claimed;
    gs_claim   claim;

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

    uint64_t started_ms;
    uint32_t total_in, total_out, relayed, refused;
    uint64_t total_in_bytes, total_out_bytes;
    uint8_t  peak;

    bool quit;
    bool plain;                // no ANSI, for a log file or a dumb terminal

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

    // And what the race will be on - **always**, even when the answer is "no
    // track". A client cannot tell "there is nothing to wait for" from "the
    // message has not arrived yet" by looking at silence, and one that guessed
    // would start racing on whatever it had loaded locally.
    n = gs_proto_start(buf, sizeof buf, gs_srv.track_hash, gs_srv.capacity, 3,
                       (uint8_t)GS_MODE_RACE);
    gs_send(c, buf, n);

    if (fresh) {
        SDL_Log("player %d (%s) joined from %s:%u", at, c->name, c->text, port);
        if (gs_srv.store != nullptr) {
            gs_store_put_driver(gs_srv.store, c->name, 0, 0);
        }
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

    if (gs_srv.store != nullptr) {
        printf("  remembered %d driver(s), %d record(s), %d track(s)"
               "   results %u, kept %u\n",
               gs_store_driver_count(gs_srv.store),
               gs_store_record_count(gs_srv.store),
               gs_store_track_count(gs_srv.store), gs_srv.results, gs_srv.kept);
        if (gs_srv.rejected > 0) {
            printf("  rejected   %u time(s) that the replay did not produce\n",
                   gs_srv.rejected);
        }
    }

    if (gs_srv.track_len > 0) {
        printf("  track      %016llx, %zu bytes, %u chunks sent\n",
               (unsigned long long)gs_srv.track_hash, gs_srv.track_len,
               gs_srv.chunks_sent);
    }

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

    case GS_MSG_RELAY: {
        // **Forwarded, not understood.** The payload is a rollback datagram
        // and the server has no idea what is in it - it stamps who it came
        // from and passes it on. A server that parsed race traffic would be a
        // server that could disagree with the race.
        uint8_t from = 0;
        size_t payload_len = 0;
        const uint8_t *payload = gs_proto_payload(d->buf, len, &from,
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
        if (!gs_proto_read_result(d->buf, len, &track, &conditions, &laps,
                                  &vehicle, &lap_ticks, &race_ticks)) {
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
        if (!gs_proto_read_proof_chunk(d->buf, len, &hash, &chunk, &chunks,
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
            break;
        }

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
        if (!gs_proto_read_track_chunk(d->buf, len, &hash, &chunk, &chunks,
                                       &data, &data_len)) {
            break;
        }

        if (up->upload.hash != hash) gs_carrier_expect(&up->upload, hash);
        if (!gs_carrier_take(&up->upload, d->buf, len)) break;
        if (!gs_carrier_done(&up->upload)) break;

        static gs_track arrived;
        if (!gs_carrier_track(&up->upload, &arrived)) {
            SDL_Log("%s uploaded a track that did not survive the trip", up->name);
            break;
        }

        gs_store_put_track(gs_srv.store, hash, "", up->name,
                           up->upload.bytes, up->upload.len);
        SDL_Log("%s uploaded %016llx", up->name, (unsigned long long)hash);
        break;
    }

    case GS_MSG_PUBLISH: {
        uint64_t track = 0;
        char name[48] = { 0 };
        if (at < 0 || gs_srv.store == nullptr) break;
        if (!gs_proto_read_publish(d->buf, len, &track, name, sizeof name)) break;

        // **Only a track the server has.** Publishing is a claim about
        // something already here; the track itself arrives the way tracks
        // always do, checked against its own hash on the way in.
        if (!gs_store_has_track(gs_srv.store, track)) {
            SDL_Log("%s tried to publish a track this server does not have",
                    gs_srv.client[at].name);
            break;
        }
        if (gs_store_publish(gs_srv.store, track, name,
                             gs_srv.client[at].name)) {
            SDL_Log("%s published %016llx (%s)", gs_srv.client[at].name,
                    (unsigned long long)track, name);
        }
        break;
    }

    case GS_MSG_WITHDRAW: {
        uint64_t track = 0;
        if (at < 0 || gs_srv.store == nullptr) break;
        if (!gs_proto_read_withdraw(d->buf, len, &track)) break;

        if (gs_store_withdraw(gs_srv.store, track, gs_srv.client[at].name)) {
            SDL_Log("%s withdrew %016llx", gs_srv.client[at].name,
                    (unsigned long long)track);
        }
        break;
    }

    case GS_MSG_WANT_LIST: {
        if (at < 0 || gs_srv.store == nullptr) break;

        static gs_track_row rows[64];
        int n = gs_store_list_published(gs_srv.store, rows, 64);

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
        if (!gs_proto_read_want_best(d->buf, len, &track, &conditions, &laps)) {
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
        if (at < 0 || !gs_proto_read_want_track(d->buf, len, &want)) break;

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

        uint16_t chunks = gs_carrier_chunks(bytes_len);
        for (uint16_t i = 0; i < chunks; i++) {
            uint8_t out[GS_PROTO_MTU];
            size_t n = gs_carrier_chunk(out, sizeof out, want, bytes, bytes_len, i);
            gs_send(&gs_srv.client[at], out, n);
            gs_srv.chunks_sent++;
        }
        break;
    }

    default:
        // Everything else belongs to items not built yet - the relay, records.
        // Ignored rather than guessed at.
        break;
    }
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
    const char *track_path = nullptr;
    const char *store_path = "gearstick.db";

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

                // And who is here, so a roster lost on the way out is corrected
                // two seconds later rather than never.
                gs_send_lobby(c);
                c->pinged_ms = now;
            }
        }

        if (now - last_draw >= GS_DRAW_MS) {
            gs_draw(now);
            last_draw = now;
        }

        if (seconds > 0 && (now - gs_srv.started_ms) / 1000u >= seconds) break;

        // A relayed race is one datagram per player per tick at 120 Hz coming
        // through here and going straight back out. Sleeping five milliseconds
        // between drains lets a burst pile up in the socket buffer and be
        // dropped; one keeps the queue short without spinning a core.
        SDL_Delay(1);
    }

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

// gs_wire.h - the sockets under the netcode, and nothing more than sockets.
//
// **Datagrams, unordered, unacknowledged, allowed to vanish.** Every comfort a
// stream socket offers - delivery, ordering, retransmission - is a round trip,
// and rollback exists precisely so that nobody waits a round trip to see their
// own car move. Loss is dealt with by sending the last thirty-two ticks of
// input in every packet, so a lost datagram is already replaced by the time
// anybody could have asked for it.
//
// **A mesh, not a star.** Up to four players all send directly to each other
// rather than through the host, because a relay is an extra hop of latency
// between two clients and latency is the whole thing this is arranged to
// minimise. The host is used only to *find* everybody: joiners say hello to it,
// it assigns the player slots, and it hands each of them the list of where the
// others are. After that it is nobody's server.
//
// This file is the only thing in the project that knows what a socket is. The
// rollback session in src/core/gs_net.c produces and consumes byte arrays, and
// that separation is what lets a four-player race under 200 ms of latency and
// twelve percent packet loss run inside a unit test with no network at all.
#ifndef GS_WIRE_H
#define GS_WIRE_H

// For `nullptr` on a toolchain that took -std=c23 without implementing it, as
// well as the fixed-width types. MSVC is that toolchain today.
#include "core/gs_common.h"
#include "net/gs_noise.h"
#include "core/gs_net.h"
#include "core/gs_track.h"
#include "net/gs_carrier.h"
#include "net/gs_proto.h"

#define GS_WIRE_MTU     512

// **The carrier has to be able to carry it.** A rollback datagram that does not
// fit is not an error anybody sees: `gs_net_packet` writes nothing, the wire
// sends nothing, and every machine sits waiting for inputs that were never
// produced. That reads as a network fault and is a constant somebody changed,
// so it is checked here rather than remembered.
static_assert(GS_WIRE_MTU >= GS_NET_MTU,
              "a rollback datagram no longer fits in one wire datagram");
#define GS_WIRE_PLAYERS 4

typedef struct gs_wire gs_wire;

// Bring the networking up. Safe to call more than once; the last quit wins.
bool gs_wire_init(void);
void gs_wire_quit(void);

// Wait for `players` people in total, this machine being one of them and player
// zero. Nothing else is configured at this end: joiners are learned from the
// datagrams they send.
gs_wire *gs_wire_host(uint16_t port, uint8_t players);

// Join somebody who is waiting. `host` is a name or an address. Which player
// this machine turns out to be is decided by the host and arrives with the
// roster.
// **The host's public key is not optional either**, for the same reason the
// server's is not: without it a joiner cannot tell the machine it meant to race
// from anybody else who answers. The host prints it; a person passes it on.
gs_wire *gs_wire_join(const char *host, uint16_t port, const uint8_t *host_key);

// This machine's own public key. Meaningful for a host, which is the end
// everybody else has to already know.
const uint8_t *gs_wire_public_key(const gs_wire *w);

// Meet everybody at a server instead. **Which player you are is the server's
// decision**, not the decision of whoever happened to start the game first -
// which is the difference between a lobby and a host, and the reason a server
// exists at all. `name` is who to appear as.
//
// The same object either way: once ready, sending and receiving work the same,
// so nothing above this layer knows or cares which way the players found each
// other.
// **The server's public key is not optional.** IK means the client already
// knows who it is talking to, which is exactly what stops somebody in the
// middle answering in the server's place; a client handed no key is a client
// that would connect to whoever replied first, so it refuses instead.
gs_wire *gs_wire_server(const char *host, uint16_t port, const char *name,
                        const uint8_t *server_key);

void gs_wire_close(gs_wire *w);

// Work the handshake. Call it every frame until `gs_wire_ready`; it is cheap
// and it does nothing once everybody is present.
void gs_wire_poll(gs_wire *w);

// True once every player is present and this machine knows how to reach all of
// them. Until then there is nobody to race.
bool gs_wire_ready(const gs_wire *w);

// Which player this machine is, and how many there are. Both are only
// meaningful once ready.
uint8_t gs_wire_local(const gs_wire *w);
uint8_t gs_wire_players(const gs_wire *w);

// How many are present so far, for a "waiting for 2 more" line rather than a
// frozen window.
uint8_t gs_wire_present(const gs_wire *w);

// Send to *everybody else*. The rollback packet is the same for every peer, so
// there is one call rather than one per peer. False only if there is nobody to
// send to yet.
bool gs_wire_send(gs_wire *w, const uint8_t *buf, size_t len);

// Take the next datagram from any peer, or 0 if none has arrived. Never blocks.
// The handshake's own traffic is dealt with inside and never handed back.
size_t gs_wire_recv(gs_wire *w, uint8_t *buf, size_t cap);

void gs_wire_stats(const gs_wire *w, uint32_t *sent, uint32_t *received);

// --- meeting at a server ---------------------------------------------------

// Turned away, and why. **The reason is meant to be shown to a person**: a
// client that can only say "connection failed" makes a full server and a wrong
// address look identical.
bool        gs_wire_refused(const gs_wire *w);
const char *gs_wire_refusal(const gs_wire *w);

// Who else is here, as the server last described it. Null when this is not a
// server connection. Waiting for people is a thing with a picture, not a
// frozen window.
const gs_lobby *gs_wire_lobby(const gs_wire *w);

// --- the track the server is running -------------------------------------

// The track this lobby will race on, as the server named it. Zero when it has
// not said, which is a lobby with no track rather than an error.
uint64_t gs_wire_track_hash(const gs_wire *w);

// The track itself, once all of it has arrived and it hashes to what was
// promised. False until then. **Both conditions matter**: the pieces arriving
// is not the same as the track being right, and two machines racing on tracks
// they each believe are the same one is the one thing rollback cannot absorb.
bool gs_wire_track(const gs_wire *w, gs_track *out);

// How much of it is here, for something to show a person.
float gs_wire_track_progress(const gs_wire *w);

// Ask for it. Safe to call repeatedly: asking again is how a missing piece is
// recovered, because there is nothing here that acknowledges anything.
void gs_wire_want_track(gs_wire *w);

// Ask for a particular track - one found in the published list, rather than the
// one the server named for the race.
void gs_wire_ask_track(gs_wire *w, uint64_t hash);

// --- what the server remembers --------------------------------------------

// Offer a time, **with the inputs that produced it**. The server re-races them
// and keeps the time only if they produce it, so a claim without its proof is
// not a record - it is a sentence nobody checked.
void gs_wire_send_result(gs_wire *w, uint64_t track, uint64_t conditions,
                         uint16_t laps, uint8_t vehicle, uint32_t lap_ticks,
                         uint32_t race_ticks, const uint8_t *proof,
                         size_t proof_len);

// Ask what stands on a track, and read the answer when it comes.
void gs_wire_ask_best(gs_wire *w, uint64_t track, uint64_t conditions,
                      uint16_t laps);

typedef struct gs_wire_best {
    bool     known;
    uint64_t track;
    uint32_t lap_ticks;
    char     lap_who[GS_PROTO_NAME];
    uint32_t race_ticks;
    char     race_who[GS_PROTO_NAME];
} gs_wire_best;

const gs_wire_best *gs_wire_best_here(const gs_wire *w);

// --- publishing ------------------------------------------------------------

// Send a track up and ask for it to be listed. The track travels in chunks and
// is checked against its own hash at the far end, like every other track.
void gs_wire_publish(gs_wire *w, const gs_track *t, const char *name);

// Take one of yours down again. The server keeps the track - times set on it
// have to stay checkable - it simply stops being listed.
void gs_wire_withdraw(gs_wire *w, uint64_t track);

// Hand a track to one named person, or stop. They are named by their public
// key, which the lobby carries - so sharing is with somebody you are actually
// in a room with rather than with a string you typed.
void gs_wire_share(gs_wire *w, uint64_t track, const uint8_t *with, bool on);

// Prove a name is yours. The password crosses as itself, which is only sane
// because this is inside the tunnel. `code` is zero when there is no second
// factor to offer.
void gs_wire_login(gs_wire *w, const char *name, const char *password,
                   uint32_t code);

// Put a password on a name - claiming one nobody has taken, or changing your
// own. `secret` is a shared secret for a one-time code, generated here because
// a second factor whose secret the server chose is one the server could use;
// null for none.
void gs_wire_claim_name(gs_wire *w, const char *name, const char *password,
                        const uint8_t *secret, size_t secret_len);

// The public key the server says belongs to a slot, or null. This is what a
// client shares a track *with*, and it is not something that player claimed
// about themselves - the server watched them prove it.
const uint8_t *gs_wire_peer_key(const gs_wire *w, uint8_t slot);

// The one-shot token the server last issued to this client, or zero if none has
// arrived. Exposed so a test can see whether a session exists at all; the claim
// path spends it without being asked.
uint64_t gs_wire_session(const gs_wire *w);

// Ask what is published, and read what comes back.
void gs_wire_ask_published(gs_wire *w);

typedef struct gs_wire_listing {
    uint64_t track;
    char     name[48];
    char     author[GS_PROTO_NAME];
} gs_wire_listing;

#define GS_WIRE_LISTINGS 32

// How many have arrived, and them. `total` is what the server said there were,
// so a caller can tell a short list from a partial one.
uint16_t gs_wire_published(const gs_wire *w, const gs_wire_listing **out,
                           uint16_t *total);

// --- getting through a router that will not cooperate ----------------------

// Send everything through the server rather than to the other players.
//
// **A last resort, and a real one.** Peers that can reach each other should
// race each other directly, because that is the shortest path and this game is
// about response - a relay costs an extra hop each way. But a meaningful number
// of home connections will not accept anything unsolicited, and for those the
// choice is a relay or no game at all.
void gs_wire_use_relay(gs_wire *w, bool on);
bool gs_wire_relaying(const gs_wire *w);

// The last thing that went wrong, for putting in front of the player rather
// than in a log they will never see.
const char *gs_wire_error(const gs_wire *w);

#endif // GS_WIRE_H

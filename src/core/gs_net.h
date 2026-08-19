// gs_net.h - rollback, which is the only honest way to put two people on one
// couch when the couch is two thousand kilometres long.
//
// Nothing is sent but inputs. Each machine runs the whole race locally and
// *guesses* what the other player is doing - they are usually still holding
// what they were holding a moment ago, and when they are not, the truth arrives
// a few ticks later and the machine rewinds to where the guess went wrong and
// replays. The player sees a car twitch. They do not see a car teleport, and
// they never wait for a packet before their own car responds.
//
// Everything about this is downstream of two decisions made long before it:
//
//   - the simulation is deterministic, so replaying the same inputs from the
//     same state lands in the same place on both machines; and
//   - the world has no pointers in it, so taking a snapshot to rewind to is a
//     memcpy of nine kilobytes rather than a graph walk.
//
// There are no sockets here. src/core/ links nothing, so this produces and
// consumes datagrams and something else carries them - which also means the
// verification can be a race under whatever latency and packet loss a test
// feels like inventing, rather than a race somebody watched on a good day.
#ifndef GS_NET_H
#define GS_NET_H

#include "core/gs_sim.h"

#define GS_NET_MAGIC  0x544E5347u   // "GSNT"

// How far ahead of confirmed truth a machine will run before it stops and
// waits. Two seconds at 120 Hz, which is far past any latency a race is
// playable at - the limit exists so that a peer that has *stopped* sending
// stalls the race rather than silently running out of history.
#define GS_NET_WINDOW 256u

// How many ticks of input every packet repeats. Loss is absorbed rather than
// retransmitted: at 120 Hz this is a quarter second of history in every
// datagram, so anything short of a quarter second of total blackout never needs
// asking for again. Retransmission requests are a round trip, which is the one
// thing rollback exists to avoid.
#define GS_NET_REDUNDANCY 32u

typedef struct gs_net {
    // The state at `confirmed_tick`, where every player's input is known for
    // every tick before it. This is the thing rewound to, and it only ever
    // moves forwards.
    gs_world confirmed;
    uint32_t confirmed_tick;

    // The state the player is looking at: `local_tick`, some of it built on
    // guesses about the other player.
    gs_world current;
    uint32_t local_tick;

    uint8_t players;
    uint8_t local;

    // Input history over the window. `known` is a bit per player: whether this
    // is what they actually did, or what we assumed they did.
    gs_input in[GS_NET_WINDOW][GS_MAX_CARS];
    gs_input used[GS_NET_WINDOW][GS_MAX_CARS];
    uint8_t  known[GS_NET_WINDOW];

    // Which tick each slot currently holds. The history is a ring, so slot 0
    // means tick 0 and then tick 256 and then tick 512, and without this a
    // machine reads last-time-round's inputs as this time's known truth. It
    // does not look like a ring bug when it happens: it looks like a desync
    // four seconds into an otherwise perfect race.
    uint32_t stamp[GS_NET_WINDOW];

    // The hash of the confirmed state at each confirmed tick, so that the other
    // machine's claim about the same tick can be checked. A desync that is
    // detected is a bug report; a desync that is not is two people describing
    // different races to each other.
    uint64_t hash[GS_NET_WINDOW];

    bool     desynced;
    uint32_t desync_tick;

    // What it cost, which is the thing worth watching when a connection is bad.
    uint32_t rollbacks;
    uint32_t resimulated;    // ticks re-run, total
    uint32_t deepest;        // the worst single rewind
    uint32_t stalls;
} gs_net;

// Start a session from a world both machines already agree on. `local` is which
// car this machine drives.
void gs_net_begin(gs_net *n, const gs_world *w, uint8_t players, uint8_t local);

// What this machine's player is doing on the tick about to be simulated.
void gs_net_local_input(gs_net *n, gs_input in);

// Advance one tick, guessing for anybody whose input has not arrived. False
// when the window is full - which means the other machine has gone quiet, and
// the race waits rather than running somewhere it can never rewind from.
bool gs_net_step(gs_net *n, const gs_track *t);

// The datagram to send. Carries the local player's recent inputs and this
// machine's claim about the confirmed state, so the other end can check it.
size_t gs_net_packet(const gs_net *n, uint8_t *buf, size_t cap);

// Take one. Corrections are applied, the confirmed state is advanced as far as
// the new truth allows, and the visible state is rewound and replayed if a
// guess turned out to be wrong. False if the datagram is not one of ours.
bool gs_net_receive(gs_net *n, const gs_track *t, const uint8_t *buf, size_t len);

// The state to draw.
const gs_world *gs_net_world(const gs_net *n);

#endif // GS_NET_H

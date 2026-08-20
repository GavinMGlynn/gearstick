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

#include "core/gs_blake2s.h"
#include "core/gs_sim.h"

// "GSN2". The second version of this format: the first carried inputs alone,
// and a peer speaking it cannot be told apart from one that has simply chosen
// not to commit to anything, so it is refused by its magic rather than by
// stalling mysteriously.
#define GS_NET_MAGIC  0x324E5347u   // "GSN2"

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

// --- commit, then reveal ----------------------------------------------------
//
// **Rollback hands every peer the others' inputs for a tick, so a modified
// client can wait and choose.** Nothing about that desyncs: everybody then
// simulates the dishonest input faithfully, and the state-hash check catches a
// changed *simulation* while being completely blind to a changed *decision*.
//
// So an input is promised before it is shown. Each datagram carries, for the
// most recent ticks, a commitment - a truncated BLAKE2s of the input, the tick
// it belongs to and a per-tick salt - and, for ticks further back, the input
// and salt themselves. A peer that waited to see somebody else's input before
// choosing its own would have to produce a reveal that does not match what it
// already promised, and that is caught.
//
// **A commitment only counts if it arrived in a datagram that did not also
// reveal that tick.** Otherwise the promise and the proof travel together and
// the promise is worth nothing - a cheat could choose late and build both at
// once. That rule is what forces the two apart, and it is why the reveal runs
// a fixed distance behind.
#define GS_NET_REVEAL_DELAY 12u

// How many ticks of commitment ride in each datagram. Exactly the reveal delay,
// and not one more: a commitment for a tick this datagram also reveals is
// inadmissible by the rule above, so sending it would be bytes spent on
// something the far end is obliged to ignore. Twelve copies is twelve chances
// for a commitment to survive the network before its reveal comes due.
#define GS_NET_COMMITS GS_NET_REVEAL_DELAY

// The salt, and the promise. Eight bytes of each: finding a second input and
// salt that hash to a given sixty-four-bit commitment is far out of reach, and
// out of reach by an enormous margin inside the tenth of a second a cheat would
// have to do it in. The salt is derived from a per-race secret rather than
// stored, so revealing one tick's salt says nothing about the next one's.
#define GS_NET_SECRET_BYTES 32

// --- how big a datagram gets --------------------------------------------
//
// Worth stating rather than discovering. `gs_net_packet` refuses to write into
// a buffer too small for it and returns zero, and a caller that treats zero as
// "nothing to send" gets a race where no input ever crosses and every machine
// stalls waiting for the others - which looks exactly like a network fault and
// is not one. So whatever carries these has to be big enough, and that is
// asserted where the carrying happens rather than left to whoever changes a
// constant next.
#define GS_NET_HEAD (4u + 1u + 1u + 4u + 1u + 4u)
#define GS_NET_TAIL (4u + 8u)
#define GS_NET_MTU  (GS_NET_HEAD + GS_NET_COMMITS * 8u + \
                     GS_NET_REDUNDANCY * 9u + GS_NET_TAIL)

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

    // What each player promised for each tick, and whether an admissible
    // promise has been heard at all. A reveal for a tick with no promise behind
    // it is not accepted - it is held, and the later copies of the promise are
    // still arriving.
    uint64_t commit[GS_NET_WINDOW][GS_MAX_CARS];
    uint8_t  committed[GS_NET_WINDOW];

    // This machine's own secret for the race, which every salt it publishes is
    // derived from. It never goes on the wire.
    uint8_t  secret[GS_NET_SECRET_BYTES];

    // Set once the race is over locally, after which the remaining reveals go
    // out with no delay. Without it the last twelve ticks of every race are
    // promised and never shown, and the other machine can never confirm the
    // finish.
    bool     flushing;

    // **Packets still to send before the flush actually starts.**
    //
    // A promise only counts when it arrives in a datagram that does not also
    // prove it, so a flushing datagram's commitments are inadmissible - it
    // reveals every tick it commits. That means the commitment for the last
    // tick of a race gets exactly one admissible copy: the datagram sent on
    // that tick. Lose it and the far end can never accept the reveal, and the
    // race is never confirmed to its end.
    //
    // The rest of the protocol survives loss by repetition and this did not, so
    // the flush waits: `gs_net_finish` asks for more ordinary datagrams first,
    // which commit the final ticks without revealing them, and only then do the
    // reveals go out. Found by a four-player race through the tunnel with one
    // datagram in twenty dropped on each of two hops - at which point the last
    // tick failed to confirm on three machines out of four.
    uint32_t flush_wait;

    bool     desynced;
    uint32_t desync_tick;

    // A peer whose reveal did not match its promise, or who promised two
    // different things for one tick. Unlike a desync this is not a bug: it is
    // somebody's client having been modified, and the race stops.
    bool     cheated;
    uint32_t cheat_tick;
    uint8_t  cheat_by;

    // What it cost, which is the thing worth watching when a connection is bad.
    uint32_t rollbacks;
    uint32_t resimulated;    // ticks re-run, total
    uint32_t deepest;        // the worst single rewind
    uint32_t stalls;
} gs_net;

// Start a session from a world both machines already agree on. `local` is which
// car this machine drives. `secret` is this machine's own, thirty-two bytes that
// never leave it; every salt it publishes is derived from it, so an opponent who
// has seen a hundred revealed salts still cannot work out the next one. Where it
// comes from is the caller's business, because core has no source of randomness
// and should not pretend to.
void gs_net_begin(gs_net *n, const gs_world *w, uint8_t players, uint8_t local,
                  const uint8_t *secret);

// What this machine's player is doing on the tick about to be simulated.
void gs_net_local_input(gs_net *n, gs_input in);

// Advance one tick, guessing for anybody whose input has not arrived. False
// when the window is full - which means the other machine has gone quiet, and
// the race waits rather than running somewhere it can never rewind from - and
// false for good once somebody has been caught breaking a promise.
bool gs_net_step(gs_net *n, const gs_track *t);

// Somebody's reveal did not match what they committed to, and which tick it was.
// A race that sees this true does not carry on: there is no honest reading of it.
bool gs_net_cheated(const gs_net *n);

// The race has finished locally, so the reveals still owed can go out at once
// rather than trailing twelve ticks behind for ever.
void gs_net_finish(gs_net *n);

// The datagram to send. Carries the local player's recent inputs and this
// machine's claim about the confirmed state, so the other end can check it.
//
// Not const: a race that has finished spends its first several datagrams
// committing the last ticks before it reveals them, and counting those is the
// only state this keeps.
size_t gs_net_packet(gs_net *n, uint8_t *buf, size_t cap);

// Take one. Corrections are applied, the confirmed state is advanced as far as
// the new truth allows, and the visible state is rewound and replayed if a
// guess turned out to be wrong. False if the datagram is not one of ours.
bool gs_net_receive(gs_net *n, const gs_track *t, const uint8_t *buf, size_t len);

// The state to draw.
const gs_world *gs_net_world(const gs_net *n);

// --- what everybody agreed happened -----------------------------------------
//
// `gs_net_world` is the state the player is looking at, and some of it is
// guesses. **This is the other one**: the state built only from inputs every
// peer has actually sent, which is the race that can be written down and handed
// to a server. A recording made from the visible state would be a recording of
// predictions, most of which were rolled back.
const gs_world *gs_net_confirmed(const gs_net *n);
uint32_t gs_net_confirmed_tick(const gs_net *n);

// The hash of the confirmed state, which every peer computes for itself and
// compares with what the others claim. Two machines that disagree here have
// already stopped racing the same race, and say so.
uint64_t gs_net_agreed_hash(const gs_net *n);

// Every player's input for a confirmed tick, or null when that tick is not
// confirmed yet or has fallen out of the window. The caller is expected to keep
// up: the window is two seconds, and anything older than that is gone.
const gs_input *gs_net_confirmed_input(const gs_net *n, uint32_t tick);

#endif // GS_NET_H

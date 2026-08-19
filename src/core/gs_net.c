#include "core/gs_net.h"

#include <string.h>

#define GS_SLOT(tick) ((tick) % GS_NET_WINDOW)

// The slot a tick lives in, emptied first if it was still holding an older one.
// Whichever of the two writers - local input or an arriving datagram - reaches
// a tick first does the clearing, and the other finds it already stamped.
static uint32_t gs_claim(gs_net *n, uint32_t tick) {
    uint32_t slot = GS_SLOT(tick);
    if (n->stamp[slot] != tick) {
        n->stamp[slot] = tick;
        n->known[slot] = 0;
        n->committed[slot] = 0;
        memset(n->in[slot], 0, sizeof n->in[slot]);
        memset(n->commit[slot], 0, sizeof n->commit[slot]);
    }
    return slot;
}

void gs_net_begin(gs_net *n, const gs_world *w, uint8_t players, uint8_t local,
                  const uint8_t *secret) {
    memset(n, 0, sizeof *n);
    for (uint32_t i = 0; i < GS_NET_WINDOW; i++) n->stamp[i] = UINT32_MAX;
    n->confirmed = *w;
    n->current   = *w;
    n->players   = players;
    n->local     = local;
    n->hash[0]   = gs_world_hash(w);
    if (secret != nullptr) memcpy(n->secret, secret, sizeof n->secret);
}

// What a player is assumed to be doing on a tick nobody has told us about:
// whatever they were last known to be doing. People hold the accelerator down
// for seconds at a time, so this is right far more often than it is wrong, and
// being wrong only costs a rewind.
static gs_input gs_predict(const gs_net *n, uint32_t tick, uint8_t p) {
    // The most recent thing this player is *known* to have done, anywhere in
    // the history still held. Searching only back to the confirmed tick is the
    // obvious version and it is wrong: the confirmed tick is precisely the one
    // whose remote input has not arrived, so the search would find nothing and
    // predict an idle player. On a race where both players simply held the
    // accelerator, that guessed wrong on every single tick and rolled back on
    // every single tick - rollback firing on agreement, which is the one thing
    // it must never do.
    uint32_t back = tick < GS_NET_WINDOW ? tick : GS_NET_WINDOW;

    for (uint32_t i = 0; i < back; i++) {
        uint32_t t = tick - 1u - i;
        uint32_t slot = GS_SLOT(t);
        if (n->stamp[slot] == t && (n->known[slot] & (uint8_t)(1u << p))) {
            return n->in[slot][p];
        }
    }
    return (gs_input)0;
}

// Fill in a whole tick's inputs: the truth where it is known, a guess where it
// is not.
static void gs_inputs_for(const gs_net *n, uint32_t tick, gs_input *out) {
    uint32_t slot = GS_SLOT(tick);
    bool live = n->stamp[slot] == tick;

    for (uint8_t p = 0; p < GS_MAX_CARS; p++) {
        if (p >= n->players) { out[p] = (gs_input)0; continue; }
        out[p] = (live && (n->known[slot] & (uint8_t)(1u << p)))
                     ? n->in[slot][p]
                     : gs_predict(n, tick, p);
    }
}

void gs_net_local_input(gs_net *n, gs_input in) {
    uint32_t slot = gs_claim(n, n->local_tick);
    uint8_t bit = (uint8_t)(1u << n->local);

    // **The first word on a tick is the last one.** Once an input has gone out
    // inside a commitment it cannot be taken back, because a promise that
    // changes is exactly what a peer choosing late would look like and the far
    // end is right to refuse it.
    //
    // This is not hypothetical. A race that stalls sits on one tick while the
    // frame loop keeps running, and a caller polling the pad every frame hands
    // over a different input for that same tick each time - rewriting a promise
    // already made, and getting itself thrown out of an honest race for it.
    if (n->known[slot] & bit) return;

    n->in[slot][n->local] = in;
    n->known[slot] |= bit;
}

bool gs_net_cheated(const gs_net *n) { return n->cheated; }

void gs_net_finish(gs_net *n) { n->flushing = true; }

bool gs_net_step(gs_net *n, const gs_track *t) {
    // Somebody has been caught breaking a promise. There is no honest reading
    // of what follows, so the race does not carry on and find out.
    if (n->cheated) return false;

    // The window is the whole of the machine's ability to be wrong. Running
    // past it would mean the tick that needs rewinding to has been overwritten,
    // so instead the race waits - visibly, and for a reason that can be shown
    // to the player.
    if (n->local_tick - n->confirmed_tick >= GS_NET_WINDOW - 1u) {
        n->stalls++;
        return false;
    }

    gs_input in[GS_MAX_CARS];
    gs_inputs_for(n, n->local_tick, in);
    memcpy(n->used[GS_SLOT(n->local_tick)], in, sizeof in);

    gs_world_step(&n->current, t, in);
    n->local_tick++;
    return true;
}

const gs_world *gs_net_world(const gs_net *n) {
    return &n->current;
}

// --- the wire --------------------------------------------------------------
//
//   u32  magic
//   u8   which player is speaking
//   u8   how many ticks of commitment follow
//   u32  the tick the first of them is for
//   u8   how many ticks of reveal follow
//   u32  the tick the first of them is for
//   u64  one commitment per tick, the promise
//   u8 + u64  one input and salt per tick, the proof
//   u32  the tick this machine has confirmed
//   u64  the hash of its state there
//
// Everything is sent again and again rather than acknowledged, which is what
// makes loss cost nothing: by the time a datagram is missed, the next
// thirty-one carry the same information. Retransmission requests are a round
// trip, which is the one thing rollback exists to avoid.
//
// **The two ranges never overlap.** The reveals run `GS_NET_REVEAL_DELAY` ticks
// behind the commitments, which is what lets the far end insist that a promise
// arrived before its proof did - see the note on `GS_NET_REVEAL_DELAY`. The one
// exception is a race that has finished locally, where the reveals still owed
// go out at once; by then their promises have been in flight for a dozen ticks
// already, and the copies riding along in the flush are ignored as
// inadmissible, exactly like any other late copy.

static void gs_put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xffu);
}

static uint32_t gs_get32(const uint8_t *p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}

static void gs_put64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xffu);
}

static uint64_t gs_get64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

// The salt for one of this machine's own ticks, derived rather than stored.
// Preimage resistance is doing the work: an opponent who has been handed every
// salt so far still cannot say what the next one is, because saying so would
// mean inverting BLAKE2s to recover the secret.
static uint64_t gs_salt_for(const gs_net *n, uint32_t tick) {
    uint8_t msg[GS_NET_SECRET_BYTES + 4];
    memcpy(msg, n->secret, GS_NET_SECRET_BYTES);
    gs_put32(msg + GS_NET_SECRET_BYTES, tick);
    return gs_blake2s_u64(msg, sizeof msg);
}

// The promise itself. **The tick is inside it**, so a commitment cannot be
// lifted from one tick and presented for another - without that, a peer could
// promise once and reuse the promise for every tick it liked.
static uint64_t gs_commit_of(uint64_t salt, uint32_t tick, gs_input in) {
    uint8_t msg[8 + 4 + 1];
    gs_put64(msg, salt);
    gs_put32(msg + 8, tick);
    msg[12] = (uint8_t)in;
    return gs_blake2s_u64(msg, sizeof msg);
}

size_t gs_net_packet(const gs_net *n, uint8_t *buf, size_t cap) {
    // Up to and including the tick whose input has just been recorded but not
    // yet simulated. Stopping one short of it means the last input of a race is
    // never sent at all, and the other machine can never confirm the finish -
    // which shows up as a race that is correct everywhere except the end.
    uint32_t end = n->local_tick;
    uint32_t slot = GS_SLOT(end);
    if (n->stamp[slot] == end && (n->known[slot] & (uint8_t)(1u << n->local))) {
        end++;
    }

    uint32_t commits = end < GS_NET_COMMITS ? end : GS_NET_COMMITS;
    uint32_t commit_base = end - commits;

    // Reveals stop a fixed distance short of the commitments, which is the
    // whole mechanism. Once the race is over locally there is nothing left to
    // choose dishonestly, so what is still owed goes out together.
    uint32_t reveal_end = n->flushing ? end
                        : (end > GS_NET_REVEAL_DELAY ? end - GS_NET_REVEAL_DELAY : 0);
    uint32_t reveals = reveal_end < GS_NET_REDUNDANCY ? reveal_end : GS_NET_REDUNDANCY;
    uint32_t reveal_base = reveal_end - reveals;

    size_t need = GS_NET_HEAD + commits * 8u + reveals * 9u + GS_NET_TAIL;
    if (cap < need) return 0;

    uint8_t *p = buf;
    gs_put32(p, GS_NET_MAGIC);  p += 4;
    *p++ = n->local;
    *p++ = (uint8_t)commits;
    gs_put32(p, commit_base);   p += 4;
    *p++ = (uint8_t)reveals;
    gs_put32(p, reveal_base);   p += 4;

    for (uint32_t i = 0; i < commits; i++) {
        uint32_t tick = commit_base + i;
        gs_input in = n->in[GS_SLOT(tick)][n->local];
        gs_put64(p, gs_commit_of(gs_salt_for(n, tick), tick, in));
        p += 8;
    }
    for (uint32_t i = 0; i < reveals; i++) {
        uint32_t tick = reveal_base + i;
        *p++ = n->in[GS_SLOT(tick)][n->local];
        gs_put64(p, gs_salt_for(n, tick));
        p += 8;
    }

    gs_put32(p, n->confirmed_tick);                     p += 4;
    gs_put64(p, n->hash[GS_SLOT(n->confirmed_tick)]);   p += 8;
    return need;
}

// Everything before `to` is now known for every player, so walk the confirmed
// state up to it. This is the only place a snapshot is taken, and it is the
// state every rewind starts from.
static void gs_advance_confirmed(gs_net *n, const gs_track *t) {
    while (n->confirmed_tick < n->local_tick) {
        uint32_t slot = GS_SLOT(n->confirmed_tick);

        uint8_t all = (uint8_t)((1u << n->players) - 1u);
        if (n->stamp[slot] != n->confirmed_tick) break;
        if ((n->known[slot] & all) != all) break;

        gs_world_step(&n->confirmed, t, n->in[slot]);
        n->confirmed_tick++;
        n->hash[GS_SLOT(n->confirmed_tick)] = gs_world_hash(&n->confirmed);
    }
}

bool gs_net_receive(gs_net *n, const gs_track *t, const uint8_t *buf, size_t len) {
    if (len < GS_NET_HEAD + GS_NET_TAIL) return false;

    const uint8_t *p = buf;
    if (gs_get32(p) != GS_NET_MAGIC) return false;
    p += 4;

    uint8_t  from        = *p++;
    uint8_t  commits     = *p++;
    uint32_t commit_base = gs_get32(p); p += 4;
    uint8_t  reveals     = *p++;
    uint32_t reveal_base = gs_get32(p); p += 4;

    if (from >= n->players || from == n->local) return false;
    if (len < (size_t)GS_NET_HEAD + (size_t)commits * 8u +
              (size_t)reveals * 9u + (size_t)GS_NET_TAIL) {
        return false;
    }

    // A peer already caught is not listened to further.
    if (n->cheated) return false;

    const uint8_t *commit_at = p;
    const uint8_t *reveal_at = p + (size_t)commits * 8u;

    // --- the promises.
    //
    // **A promise is only admissible from a datagram that does not also prove
    // it.** If the two travel together the promise costs nothing to make, and a
    // peer that waited to see everybody else's input could build both at once
    // and be indistinguishable from an honest one. Late copies of a promise
    // ride along with reveals all the time and are simply ignored - by then the
    // admissible copy has already been kept.
    for (uint32_t i = 0; i < commits; i++) {
        uint32_t tick = commit_base + i;
        uint64_t said = gs_get64(commit_at + (size_t)i * 8u);

        if (reveals > 0 && tick >= reveal_base && tick < reveal_base + reveals) {
            continue;
        }
        if (tick < n->confirmed_tick) continue;
        if (tick - n->confirmed_tick >= GS_NET_WINDOW) continue;

        uint32_t slot = gs_claim(n, tick);
        uint8_t bit = (uint8_t)(1u << from);

        if (n->committed[slot] & bit) {
            // **Two different promises for one tick.** An honest peer repeats
            // the same eight bytes a dozen times; a peer whose story changes
            // between copies is choosing after the fact and hoping one of them
            // lands.
            if (n->commit[slot][from] != said) {
                n->cheated = true;
                n->cheat_tick = tick;
                n->cheat_by = from;
                return true;
            }
        } else {
            n->commit[slot][from] = said;
            n->committed[slot] |= bit;
        }
    }

    // --- the proofs.
    bool wrong = false;
    for (uint32_t i = 0; i < reveals; i++) {
        uint32_t tick = reveal_base + i;
        const uint8_t *at = reveal_at + (size_t)i * 9u;
        gs_input said = (gs_input)at[0];
        uint64_t salt = gs_get64(at + 1);

        // Older than anything that can still be rewound to: it has already been
        // folded into the confirmed state, and cannot be changed now.
        if (tick < n->confirmed_tick) continue;
        if (tick - n->confirmed_tick >= GS_NET_WINDOW) continue;

        uint32_t slot = gs_claim(n, tick);
        uint8_t bit = (uint8_t)(1u << from);

        // No admissible promise for this tick yet, so there is nothing to check
        // it against and it is not accepted. The promise is still being
        // repeated in the datagrams either side of this one; it is a wait, not
        // a refusal.
        if (!(n->committed[slot] & bit)) continue;

        if (gs_commit_of(salt, tick, said) != n->commit[slot][from]) {
            n->cheated = true;
            n->cheat_tick = tick;
            n->cheat_by = from;
            return true;
        }

        // Already accepted, and the same as before: an ordinary repeat.
        if ((n->known[slot] & bit) && n->in[slot][from] == said) continue;

        n->in[slot][from] = said;
        n->known[slot] |= bit;

        // A guess that turned out to be wrong, on a tick already simulated.
        if (tick < n->local_tick && n->used[slot][from] != said) wrong = true;
    }

    const uint8_t *tail = reveal_at + (size_t)reveals * 9u;
    uint32_t their_tick = gs_get32(tail);
    uint64_t their_hash = gs_get64(tail + 4);

    gs_advance_confirmed(n, t);

    if (wrong) {
        // Rewind and replay. Everything from the confirmed tick forwards is
        // rebuilt from what is now known, so the correction propagates through
        // every tick that was built on the bad guess - which is the whole
        // point, because a wrong guess about a car three seconds ago is a wrong
        // collision two seconds ago.
        uint32_t depth = n->local_tick - n->confirmed_tick;
        n->current = n->confirmed;
        for (uint32_t tick = n->confirmed_tick; tick < n->local_tick; tick++) {
            gs_input in[GS_MAX_CARS];
            gs_inputs_for(n, tick, in);
            memcpy(n->used[GS_SLOT(tick)], in, sizeof in);
            gs_world_step(&n->current, t, in);
        }

        n->rollbacks++;
        n->resimulated += depth;
        if (depth > n->deepest) n->deepest = depth;
    }

    // **Both machines have to agree about the past, not just the present.**
    // If the other end has confirmed a tick this one has too, the states there
    // must hash the same. Anything else is a desync, and a desync that is
    // noticed can be reported; one that is not is two people describing
    // different races to each other.
    if (their_tick <= n->confirmed_tick &&
        n->confirmed_tick - their_tick < GS_NET_WINDOW && !n->desynced) {
        if (n->hash[GS_SLOT(their_tick)] != their_hash) {
            n->desynced = true;
            n->desync_tick = their_tick;
        }
    }
    return true;
}

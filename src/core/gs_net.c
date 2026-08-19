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
        memset(n->in[slot], 0, sizeof n->in[slot]);
    }
    return slot;
}

void gs_net_begin(gs_net *n, const gs_world *w, uint8_t players, uint8_t local) {
    memset(n, 0, sizeof *n);
    for (uint32_t i = 0; i < GS_NET_WINDOW; i++) n->stamp[i] = UINT32_MAX;
    n->confirmed = *w;
    n->current   = *w;
    n->players   = players;
    n->local     = local;
    n->hash[0]   = gs_world_hash(w);
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
    n->in[slot][n->local] = in;
    n->known[slot] |= (uint8_t)(1u << n->local);
}

bool gs_net_step(gs_net *n, const gs_track *t) {
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
//   u8   how many ticks of input follow
//   u32  the tick the first of them is for
//   u8   one per tick
//   u32  the tick this machine has confirmed
//   u64  the hash of its state there
//
// The inputs are sent again and again rather than acknowledged, which is what
// makes loss cost nothing: by the time a datagram is missed, the next thirty-one
// carry the same information.

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

    uint32_t count = end < GS_NET_REDUNDANCY ? end : GS_NET_REDUNDANCY;
    uint32_t base = end - count;

    size_t need = 4 + 1 + 1 + 4 + count + 4 + 8;
    if (cap < need) return 0;

    uint8_t *p = buf;
    gs_put32(p, GS_NET_MAGIC);  p += 4;
    *p++ = n->local;
    *p++ = (uint8_t)count;
    gs_put32(p, base);          p += 4;
    for (uint32_t i = 0; i < count; i++) *p++ = n->in[GS_SLOT(base + i)][n->local];
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
    if (len < 4 + 1 + 1 + 4 + 4 + 8) return false;

    const uint8_t *p = buf;
    if (gs_get32(p) != GS_NET_MAGIC) return false;
    p += 4;

    uint8_t from  = *p++;
    uint8_t count = *p++;
    uint32_t base = gs_get32(p); p += 4;

    if (from >= n->players || from == n->local) return false;
    if (len < (size_t)(4 + 1 + 1 + 4 + count + 4 + 8)) return false;

    bool wrong = false;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t tick = base + i;
        gs_input said = (gs_input)p[i];

        // Older than anything that can still be rewound to: it has already been
        // folded into the confirmed state, and cannot be changed now.
        if (tick < n->confirmed_tick) continue;
        if (tick - n->confirmed_tick >= GS_NET_WINDOW) continue;

        uint32_t slot = gs_claim(n, tick);
        n->in[slot][from] = said;
        n->known[slot] |= (uint8_t)(1u << from);

        // A guess that turned out to be wrong, on a tick already simulated.
        if (tick < n->local_tick && n->used[slot][from] != said) wrong = true;
    }
    p += count;

    uint32_t their_tick = gs_get32(p); p += 4;
    uint64_t their_hash = gs_get64(p);

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

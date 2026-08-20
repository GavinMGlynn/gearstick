# Threat model

What this system is defending, from whom, and what it deliberately does not
try to stop. Written down because a defence nobody stated is a defence nobody
can review, and because the things ruled *out* are decisions rather than
oversights.

Scope: the client, the server, and the traffic between them. Not the operating
system, not the machine a server runs on, not the player's own hardware.

---

## The one assumption everything else follows from

**A player owns the machine their game runs on.** Any check that runs there can
be removed by whoever is running it, and a client that reports its own results
is a client reporting whatever it likes. So nothing the client says is believed
because it said it; it is believed when it can be independently reproduced.

This is affordable here for a reason that is not true of most games: **the
simulation is exactly reproducible**. Fixed-point integers, a committed
trigonometry table, no floating point anywhere in `src/core/`, and a fixed 120 Hz
step mean the same inputs produce the same race on every machine, at every
optimisation level, on every platform CI builds for. A claimed time therefore
comes with the inputs that produced it, and the server drives them again.

Cheating reduces to *drive that fast*.

---

## Adversaries

| | Who | What they want |
|---|---|---|
| **A1** | A player with a modified client | A time, a place, a win |
| **A2** | A player racing you | To win this race |
| **A3** | Somebody on the network path | To read, alter or replay traffic |
| **A4** | Anybody who can reach the server | To break it, or take somebody's work down |

---

## What is defended, and how

### Fabricated times — A1

**Defence: re-race the submission.** `src/net/gs_verify.c` rebuilds the world
from the replay's own metadata, drives the recorded inputs, and compares the
result with the claim. A claim better than the recording produces is rejected;
a claim *worse* is accepted, because being slower than you proved is not a lie.
The track is the server's copy, fetched by content hash, so a doctored track
cannot come along with the claim.

Verdicts are specific — wrong track, wrong rules, no such car, never finished,
lap too good, race too good — so a rejection says what was wrong with it.

**Closed: the whole race, not just the winning lap.** Every one of those checks
is about one car and one lap, so a log altered anywhere else passed all of them.
A networked recording now carries the state every peer agreed the race ended in,
and re-racing the log has to arrive there — a statement about every tick and
every car at once, and nearly free because the simulation is exactly
reproducible. That is the argument for having built it this way, collected.

Two limits worth stating. A recording with no agreed ending — a race run on one
machine, or any recording made before this landed — is not failed for the
absence: "it does not say" is not "it disagrees", and it is still checked for
the lap it claims. And an alteration that changes no outcome is not caught,
because it is not a different race: by the end of a long race some cars are
wrecked, and a wrecked car is not taking input.

**Closed: a replay says who drove it.** `gs_replay_meta` carries the driver of
each car, the claim carries who is submitting, and the verifier refuses a claim
whose name is not the one in the recording — `GS_VERDICT_WRONG_DRIVER`. A
recording that names nobody, which every version three replay does, backs
nobody's claim: "it does not say" is not "it says you". A caller asserting no
identity at all still gets an answer about the driving, which is what a local
ghost or an offline analysis wants.

**What that is worth, exactly.** It stops a replay somebody *obtained* being
spent as their own, which was a complete break. It does not stop somebody
claiming to *be* that driver, because the name the server checks against is the
name they joined under, and joining under a name proves nothing until there are
accounts. The value goes up when the transport and the accounts land, and not
before — the binding is a precondition for those being worth anything, rather
than a defence that stands alone.

**Closed: a submission is bound to the session that asked for it.** The server
issues a one-shot nonce when it places a client and again after each claim is
resolved; a claim carries the one it was given, and the server spends it. A
nonce it never issued, one it issued to somebody else, one already spent or one
out of date buys nothing. Records were already keyed, so a resubmission was
idempotent — but idempotent by accident of the schema, and a thing that is safe
by accident stops being safe when the schema changes. This makes it deliberate.

The nonce is checked and retired in **one** `UPDATE`, with all four conditions in
the statement, because reading the row and then writing it leaves a gap and the
gap is where one nonce is spent twice. It is spent *after* the re-race rather
than before: re-racing is what says the time is real, and a nonce burnt on a
claim that turned out to be nonsense would cost an honest client its next
submission for somebody else's mistake. Sessions live in the database with
everything else the server knows, because a server that held them in memory
would forget every one on restart, and a nonce nobody can retire is one that can
be handed in for ever — which is the whole thing it exists to stop.

**What *that* is worth, exactly.** Two limits, stated rather than left to be
found. The nonce comes from `SDL_rand_bits`, which is not a cryptographic
generator: this is the shape the defence will take and not yet a defence against
somebody who can predict it. And a nonce travels in clear over the same
unauthenticated channel as everything else, so anybody on the path can read one
— it is refused from a different *name*, and the name is only worth something
once the transport and the accounts below land.

### Cheating inside a race — A2

Races are peer to peer with rollback, so every machine simulates everything and
there is no referee in the loop. Two different problems live here.

**A modified simulation** is already caught: peers exchange state hashes and a
divergence stops the race rather than being lived with. A client that changes
how the physics works stops agreeing with everybody else immediately.

**Closed: dishonestly chosen inputs.** In rollback each peer receives the
others' inputs for a tick, and a modified client could wait to see them before
deciding its own. Nothing about that desyncs — everyone then simulates the same
dishonest input faithfully — so the state-hash check, which catches a modified
*simulation*, is completely blind to it.

So an input is promised before it is shown. Each datagram carries a commitment
for the most recent ticks — `BLAKE2s(salt ‖ tick ‖ input)`, truncated to eight
bytes — and the input and salt themselves for ticks twelve further back. A peer
that waited to see somebody else's input would have to reveal something other
than what it promised, and that is caught; the race stops rather than carrying
on, because there is no honest reading of the rest of it.

**The rule that makes it work is that a promise only counts when it arrives in a
datagram that does not also prove it.** Otherwise the two travel together, the
promise costs nothing to make, and a peer choosing late is indistinguishable
from an honest one. That is what the twelve-tick gap between the commitments and
the reveals is for. A peer that ignores the gap is not accused of anything — it
has not lied — it simply never says anything checkable, and nothing it sends is
ever accepted.

Three further things, stated rather than left to be found:

- The salt for a tick is derived from a per-race secret that never leaves the
  machine, so revealing one tick's salt says nothing about the next one's. That
  secret comes from `SDL_rand_bits`, which is **not** a cryptographic generator:
  as with the session nonce, this is the shape the defence takes and not yet a
  defence against somebody who can predict it.
- Eight bytes of commitment is not enough to resist a determined search for a
  *collision*, and is far more than enough to resist finding a second input and
  salt matching a given one — which is the property this actually rests on, and
  it has to be done inside the tenth of a second before the reveal is due.
- BLAKE2s is RFC 7693 and is implemented here in `src/core/`, because core links
  nothing and cannot reach libsodium. It is checked against the RFC's published
  vector and against Python's `hashlib`, an implementation written by other
  people.

**A peer that stalls rather than lying is still not stopped.** Refusing to send
at all stalls the race, which is visible; choosing *when* to send within the
window is not something a commitment addresses.

### Traffic — A3

Today: nothing. Datagrams are plaintext, unauthenticated, and replayable, and a
source address is whatever the sender wrote.

Planned: `Noise_IK_25519_ChaChaPoly_BLAKE2s` over libsodium. Both halves of that
sentence are the point. **The pattern is named and specified** rather than
invented here, so its properties are somebody else's published analysis and not
this project's opinion; **the primitives are audited and widely deployed** rather
than written here. A handshake a game programmer designed is the thing a reviewer
rejects, and the rejection is correct: the failure modes are subtle, and
confidence without evidence is how they survive to production.

IK rather than NK because the client already holds the server's static key, which
buys client authentication in the first message and hides the client's identity
from a passive observer. One cipher suite and no negotiation, so there is nothing
to be talked down to.

Sealed one datagram at a time, because the racing tolerates loss and reordering
and anything that recovers a stream turns a dropped packet into a stall. Replay
protection is therefore an RFC 6479 sliding bitmap — the construction IPsec and
WireGuard use — and not a counter, because ordinary reordering must be accepted
while a genuine replay is refused.

Decided deliberately, and to be stated in the transport specification rather than
discovered by a reader: nonce construction and the message limit before a rekey;
ephemeral keys per session for forward secrecy; the pattern's known
key-compromise-impersonation properties; constant-time comparison for everything
secret; explicit zeroisation, because a compiler may legally delete a `memset`
on a dying buffer; and a cookie-style reply or rate limit for the handshake,
since an unauthenticated first message otherwise costs the server a scalar
multiplication on demand.

The evidence, which matters as much as the design: the framework's published test
vectors in CI, and **a handshake completed against an independent implementation
of the same pattern** — the one artefact that does not rest on our own reading of
our own code.

Until that exists, **an account password would be worthless**, which is why the
transport work comes first and the accounts work comes second.

### The server itself — A4

The server parses attacker-controlled bytes on every path it has: the protocol
decoder, the chunked reassembler, and the track and replay deserialisers behind
them. These are the parts most likely to contain a memory-safety bug and the
parts a reviewer should look at first.

Current position:

- Every protocol reader validates lengths against the datagram it was given
  rather than believing a field, and rejects a chunk index outside the declared
  count.
- The reassembler bounds every write against the buffer it owns, and refuses a
  chunk count larger than the array it tracks them in.
- All SQL is bound parameters; no statement is built by concatenation.
- Builds run with `-Wconversion` and `-Wsign-conversion` as errors, and the
  sanitized presets run ASan and UBSan.

Open: **none of these parsers is fuzzed**, and the reassembler's bound on its
own `have[]` index is sound only because the parser upstream established it.
Both are cheap to fix and both are listed in `COMPLETION_PLAN.md`.

Also open: a published track can be taken down only by whoever published it, but
there is no authentication behind "whoever", so today that check is a formality.

---

## Deliberately not defended

- **A player who is simply very good, or a bot that is.** Re-racing proves a time
  is *achievable*, not that a person achieved it. Detecting superhuman-but-legal
  driving is heuristics with a poor success rate and a real false-positive cost,
  and the honest position is the original's: the leaderboard says somebody drove
  this, and it is true.
- **Anything on the player's own machine.** Save files, local records and the
  local library are the player's to edit. They mean nothing to anybody else until
  they are submitted, and submission is verified.
- **Denial of service by volume.** A server can be flooded. That is an operations
  problem — rate limits, a firewall, an upstream — and not something the protocol
  can solve for itself.
- **Traffic analysis.** Sealing the contents does not hide that two addresses are
  exchanging datagrams at 120 Hz.

---

## Order of work, and why

**Ordered by how much each one matters**, which is not the same as ordering by
what depends on what. The first four are open holes today and are cheap; the
transport work is larger and defends a channel nobody is currently attacking.

1. ~~**Bind a replay to its driver.**~~ **Done.** The driver is inside the
   recording's metadata and the verifier refuses a claim naming anybody else.
2. ~~**Bind a submission to a session.**~~ **Done.** A server-chosen one-shot
   nonce rides in the claim and is spent once, so a resubmission is refused by
   design rather than being harmless by accident of the schema. Sessions are in
   the database, so a nonce stays spent across a restart.
3. ~~**Commit then reveal for race inputs.**~~ **Done.** A commitment for each
   tick rides twelve ticks ahead of the input it promises, and a reveal that
   does not match it stops the race. The cost is the twelve ticks: the local car
   is unaffected, and a remote car's corrections land that much later.
4. ~~**Verify the whole race, not the winner's lap.**~~ **Done.** A networked
   recording carries the state every peer agreed the race ended in, and the
   server's re-race has to arrive there. The checks before it were about one car
   and one lap; a log altered anywhere else walked past all of them.
5. ~~**Fuzz the parsers.**~~ **Done.** Four libFuzzer targets under ASan and
   UBSan, seeded from generated captures, run in CI. No crash found so far; the
   targets are known to be capable of finding one because a planted bug was
   found in seconds.
6. ~~**Seal the transport.**~~ **Done.** `Noise_IK_25519_ChaChaPoly_BLAKE2s`
   between a client and the server, and again between every pair of peers on the
   mesh — keyed from the static keys the broker watched each of them prove. Only
   the handshake itself is in the clear, which is what a handshake is.
7. ~~**Accounts, and a track's ownership.**~~ **Done.** A track belongs to the
   static key its builder proved during the handshake; stock tracks are outside
   ownership entirely; sharing names people by key. And a name can carry a
   password — Argon2id, with a one-time code for anybody who wants one — so an
   identity survives a reinstall rather than being whatever key that machine
   happened to generate.

**Said out loud rather than left to be discovered: one to four are not complete
defences until six and seven land.** Binding a replay to a driver stops somebody
handing in a recording they found; it does not stop somebody claiming to *be*
that driver, because until there are authenticated accounts the name attached to
a datagram is whatever the sender typed. Each is worth doing on its own account.
None of them finishes the job alone, and the ordering is a judgement about impact
per day of work rather than a claim that the earlier ones are sufficient.

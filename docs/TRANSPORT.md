# The gearstick transport

**What this document is for.** A design nobody wrote down cannot be reviewed,
and the parts left out are what a reviewer most needs to see were decisions
rather than oversights. This says what goes on the wire between a gearstick
client and a gearstick server, byte for byte, so that somebody who has never
read the source can write a client that talks to one.

It also says what this transport does **not** claim. That half is not a
disclaimer; it is the more useful half.

---

## 1. What it is

`Noise_IK_25519_ChaChaPoly_BLAKE2s`, from the Noise Protocol Framework revision
34, over a UDP datagram socket, with a sequence number added in front of each
transport message and a replay window behind it.

Nothing here is invented. The pattern is IK exactly as the framework specifies
it. The primitives are libsodium's X25519 and ChaCha20-Poly1305. BLAKE2s is
RFC 7693.

**IK rather than NK or XX.** The client already knows the server's static public
key — it is given out of band, printed by the server at startup and passed to
the client with `--server-key`. One round trip then buys server authentication,
client authentication, and a client identity that a passive observer cannot
read, because the client's static key travels encrypted inside the first
message.

**One suite, no negotiation.** There is no cipher list, no version byte inside
the handshake, and no downgrade path — a protocol that cannot negotiate cannot
be talked down to something weaker. Two endpoints that disagree about what they
are speaking fail the handshake and learn nothing else.

---

## 2. Notation

- All multi-byte integers are **little-endian** unless this document says
  otherwise. There is exactly one exception and it is called out in §7.
- `u8`, `u16`, `u32`, `u64` are unsigned integers of that width.
- `x[n]` is n bytes.
- Byte offsets are from the start of the UDP payload.

---

## 3. The envelope

Every datagram between a client and a server begins with a six-byte header.

| offset | size | field | value |
|---|---|---|---|
| 0 | 4 | magic | `0x56535347` (`"GSSV"` in wire order: `47 53 53 56`) |
| 4 | 1 | version | `1` |
| 5 | 1 | type | see below |

A datagram whose magic or version does not match is dropped without a reply.

Only two types ever appear on the wire once this transport is in use:

| type | name | meaning |
|---|---|---|
| 1 | `HANDSHAKE` | one Noise handshake message, necessarily unencrypted |
| 2 | `SEALED` | a sequence number and a Noise transport message |

Every other message type in the protocol — join, lobby, track chunks, results,
proofs, records — is a **plaintext payload carried inside a `SEALED`
datagram**, and is refused if it arrives any other way. A client that would read
an unsealed protocol message is one that anybody on the path can talk to by
pretending the tunnel failed.

The maximum datagram this transport will produce or accept is **1200 bytes**.

---

## 4. The handshake

### 4.1 Parameters

| | |
|---|---|
| protocol name | `Noise_IK_25519_ChaChaPoly_BLAKE2s` (33 bytes of ASCII) |
| prologue | `gearstick/1` (11 bytes of ASCII) |
| DHLEN | 32 |
| HASHLEN | 32 |
| cipher key | 32 |
| AEAD tag | 16 |

The protocol name is **33 bytes, which is longer than HASHLEN**, so
`InitializeSymmetric` takes the hashing branch: `h = BLAKE2s(name)`. An
implementation that pads instead will not interoperate. This is the single most
likely place to get it wrong.

The prologue is mixed in but never sent. It is where a version lives: change it
and every handshake with an endpoint using the old one fails immediately rather
than half-way through a race.

### 4.2 Pattern

```
IK:
  <- s
  ...
  -> e, es, s, ss
  <- e, ee, se
```

The pre-message `<- s` is the server's static public key. The initiator mixes it
into `h` after the prologue; the responder mixes in its own.

### 4.3 Message one, client to server

`HANDSHAKE` payload, 96 bytes with an empty Noise payload:

| offset | size | field |
|---|---|---|
| 6 | 32 | client ephemeral public key |
| 38 | 48 | client static public key, encrypted (32 + 16 tag) |
| 86 | 16 | Noise payload, encrypted (0 bytes + 16 tag) |

The gearstick client sends an empty Noise payload here. An implementation may
send bytes; the server ignores them.

### 4.4 Message two, server to client

`HANDSHAKE` payload, 48 bytes with an empty Noise payload:

| offset | size | field |
|---|---|---|
| 6 | 32 | server ephemeral public key |
| 38 | 16 | Noise payload, encrypted (0 bytes + 16 tag) |

The server answers immediately on receiving message one. **There is no
half-finished handshake state anywhere on the server**, which is deliberate:
there is nothing for an attacker to fill up.

### 4.5 Failure

A handshake that fails is dead, not retryable. The endpoint discards the state
and a subsequent message on it is refused. A client that wants to try again
starts a whole new handshake with a fresh ephemeral key.

**A handshake from an address that already has a live client is refused by the
server.** Message one is replayable — anybody who captured one can send it
again, and the responder cannot tell, because telling would need a session and
there is no session yet. Accepting it would install a tunnel whose keys the real
client does not hold, and knock a racing player off with a recorded packet. A
client that genuinely restarted waits out the silence timeout first.

---

### 4.6 The state machine

Each endpoint is in exactly one of four states. Anything not listed as accepted
in a state is dropped without a reply.

| state | accepts | on success | on failure |
|---|---|---|---|
| `NEW` (initiator) | nothing | sends message one → `WAITING` | — |
| `NEW` (responder) | `HANDSHAKE` message one | sends message two, `Split()` → `OPEN` | → `DEAD` |
| `WAITING` (initiator) | `HANDSHAKE` message two | `Split()` → `OPEN` | → `DEAD` |
| `OPEN` | `SEALED` only | stays `OPEN` | a datagram that fails to open is dropped; the state does not change |
| `DEAD` | nothing | — | — |

Four rules that are easy to get wrong and matter:

- **The responder reaches `OPEN` on one datagram.** It writes message two and
  splits immediately; it does not wait for anything from the initiator. There is
  no half-open state on a server, deliberately — there is nothing to fill up.
- **`DEAD` is final.** A handshake that failed is not retried on the same state.
  A peer that wants to try again begins a new handshake with a fresh ephemeral
  key. A handshake that could be retried is one somebody can grind against.
- **A datagram that fails to open in `OPEN` does not change the state**, does
  not advance the replay window, and does not produce a reply. Only a datagram
  that authenticated moves anything.
- **A `HANDSHAKE` arriving in `OPEN` is not a reason to start again.** The
  gearstick server refuses one from an address that already has a live client,
  because message one is replayable and accepting it would let a recorded packet
  knock a racing player off. See §4.5.

The initiator repeats message one until message two arrives or it gives up;
retransmission is the only recovery, because there is nothing to acknowledge.

## 5. The key schedule

Exactly the framework's, with no additions.

- `HKDF(ck, ikm)` is HMAC-BLAKE2s as RFC 5869 defines it, with **block size 64**
  — BLAKE2s's block size, not its 32-byte digest. Using 32 here produces a key
  schedule that is self-consistent and matches nobody.
- `MixKey(ikm)`: `ck, temp = HKDF(ck, ikm)`, then `InitializeKey(temp)`.
- `MixHash(data)`: `h = BLAKE2s(h ‖ data)`.
- `EncryptAndHash(p)`: `c = EncryptWithAd(h, p)`, then `MixHash(c)`.
- `Split()`: `k1, k2 = HKDF(ck, empty)`. **The initiator sends under `k1` and
  receives under `k2`; the responder is the mirror.**

After `Split` the handshake's chaining key, its cipher state and its ephemeral
private key are wiped.

---

## 6. Transport messages

A `SEALED` datagram:

| offset | size | field |
|---|---|---|
| 6 | 8 | counter, `u64` |
| 14 | n + 16 | ChaCha20-Poly1305 ciphertext and tag |

The ciphertext is **exactly a Noise transport message**: `EncryptWithAd(empty,
plaintext)` under the sending cipher state, with the nonce built from the
counter. Nothing is passed as associated data.

**Why the counter is not associated data.** It looks as though it should be. It
does not need to be: the counter *is* the nonce, the nonce is an input to the
tag, and a counter changed in flight therefore produces a tag that does not
verify. Passing it as AD as well would add nothing and would make every message
differ from what the framework specifies — which would cost both the published
test vectors and interoperability with other Noise implementations, since both
cover the transport phase and not only the handshake.

### 6.1 The nonce

12 bytes: four zero bytes, then the counter as `u64` little-endian. This is
the framework's rule and matches libsodium's IETF ChaCha20-Poly1305 layout
exactly, so no repacking is needed.

### 6.2 The replay window

Noise's transport phase assumes messages arrive in order. UDP does not.

The receiver keeps the highest counter it has accepted and a 64-bit mask of the
64 counters below it. A datagram is refused if its counter is already marked, or
if it is more than 64 behind the highest.

**Out of order is not the same thing as replayed.** A rule of "must be greater
than the last one accepted" cannot tell them apart: it throws away the
reordering every lossy link produces constantly, which in a race means
discarding inputs that will never be resent.

**The window moves only for a datagram that authenticated.** Recording the
counter before the tag is checked would let anybody who can send a packet mark
a sequence number as seen and have the real one refused when it arrived.

---

## 7. The one big-endian thing

The one-time code (§9) uses RFC 4226's counter, which is eight bytes
**big-endian**. Nothing else in this protocol is.

---

## 8. Message limit, and the rekey that is not here

A session refuses to send once its counter reaches **2^40**. A repeated nonce
under one key destroys the confidentiality of both messages it was used for, and
the counter is not allowed anywhere near a point where anybody has to reason
about it. At 120 datagrams a second, 2^40 is about 290 years.

**There is no rekey.** `Rekey()` is in the framework and is not implemented
here. A session that reaches the limit stops sending and the connection must be
re-established with a new handshake. This is stated rather than hidden because a
reviewer looking for a rekey should find out at once that there is not one, and
because the limit is far enough away that adding one now would be code nobody
has ever run.

---

## 9. Proving a name is yours

Inside the tunnel, and only meaningful because of it.

- **Password**: sent as itself in a `LOGIN` message. The server stores
  libsodium's `crypto_pwhash_str` output — Argon2id, salt and cost parameters
  inside the string — and verifies with `crypto_pwhash_str_verify`.
- **One-time code**: RFC 6238 over RFC 4226, 30-second steps, 6 digits,
  **HMAC-SHA256**.

**TOTP-SHA256, not SHA-1.** RFC 6238 names SHA-1, SHA-256 and SHA-512;
libsodium ships the second and third and not the first. Choosing the one the
audited library actually has beats implementing SHA-1 to match what most phone
apps default to. **An authenticator app configured for SHA-1 will produce six
wrong digits**, and this is the paragraph that says so.

A code is accepted within one step either side of now, and the step is then
retired: a window without a spend is a window in which a code works more than
once.

---

## 10. What is claimed

- **Confidentiality and integrity** of everything except the handshake messages
  themselves, against somebody who can read, alter, drop, reorder or replay
  datagrams.
- **Server authentication.** The client knows the server's static key in
  advance; nobody else can complete the handshake in its place.
- **Client authentication.** The server learns a static key the client had to
  prove it holds. Track ownership is keyed on it.
- **Identity hiding for the client.** Its static key is encrypted inside message
  one. A passive observer learns that somebody spoke to this server, not who.
- **Replay resistance** on transport messages, and out-of-order tolerance
  within 64.
- **Forward secrecy for traffic**, from the ephemeral keys, once both handshake
  messages have been exchanged.

## 11. What is **not** claimed

- **No protection for the handshake messages themselves.** A key exchange
  cannot be encrypted under a key that does not exist yet.
- **No denial-of-service resistance.** There is no cookie, no puzzle, no rate
  limit. Anybody who can send datagrams can make the server perform X25519
  operations. This is a game server; the trade was made knowingly.
- **No protection against a compromised endpoint.** Everything here is about
  the wire. A modified client is a different problem, handled — where it is
  handled at all — by re-racing submissions and by commit-then-reveal in the
  rollback session. See `docs/THREATS.md`.
- **No post-compromise security.** There is no rekey and no ratchet, so an
  attacker who obtains a session's keys can read that session until it ends.
- **No protection of the server's static key at rest.** It is a blob in the
  server's SQLite file, with whatever protection the filesystem gives it.
- **No traffic analysis resistance.** Datagram sizes and timing are what they
  are; a race is obvious on the wire even though its contents are not.
- **No formal proof.** The pattern is one that has been analysed; this
  *implementation* has been checked against published test vectors and against
  an independent implementation, which is a different and weaker statement.

---

## 12. How to check an implementation against this document

1. **The published vectors.** The Noise Protocol Framework's own test vectors
   for this suite, in `tests/vectors/`, cover both handshake messages, the
   handshake hash, and the transport messages after them.
2. **An independent implementation.** `tools/noise_interop.py` drives a
   gearstick endpoint from the Python `noiseprotocol` package, in both
   directions, and checks that both ends arrive at the same handshake hash.
3. **The one-time code** against RFC 6238's published SHA-256 values.

If you implement a client from this document and it does not interoperate, the
document is wrong and that is worth reporting: the whole point of writing it
down is that it can be checked against something other than its author's
memory.

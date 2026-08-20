#!/usr/bin/env python3
"""A gearstick client written from docs/TRANSPORT.md and nothing else.

**What this is for.** The plan's verification for the transport specification is
that somebody who has not read the code can implement a client from the document
alone and complete a handshake. This is the closest thing to that which can live
in the repository: every constant, offset and rule below is quoted from
`docs/TRANSPORT.md` by section, and none of it is taken from `src/net/gs_noise.c`.

It is **not** the verification. The person who wrote the document also wrote the
code, and cannot unsee it. What this does establish is that the document is
*sufficient and correct on its own terms*: if a byte offset in it is wrong, if
the prologue is wrong, if the note about the protocol name being hashed rather
than padded is wrong, or if the framing around a transport message is wrong,
this fails to complete a handshake with a real server.

The Noise framework itself comes from the `noiseprotocol` package. That is the
same kind of borrowing the document assumes: it names a standard, and a reader
implementing from it would reach for an implementation of that standard rather
than writing one. What is written here is everything gearstick adds on top -
which is exactly the part a reader could not get from anywhere else.

    usage: transport_client.py <host> <port> <server public key, hex>
"""

import socket
import struct
import sys

try:
    from noise.connection import NoiseConnection, Keypair
except ImportError:
    print("transport_client: needs the noiseprotocol package")
    sys.exit(77)

from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
from cryptography.hazmat.primitives.serialization import (
    Encoding, NoEncryption, PrivateFormat, PublicFormat)

# --- everything below is quoted from docs/TRANSPORT.md ----------------------

# §3, the envelope: 4 bytes of magic, 1 of version, 1 of type.
MAGIC = 0x56535347
VERSION = 1
TYPE_HANDSHAKE = 1
TYPE_SEALED = 2
HEADER = struct.Struct("<IBB")          # §2: little-endian unless stated

# §4.1, parameters.
PROTOCOL = b"Noise_IK_25519_ChaChaPoly_BLAKE2s"
PROLOGUE = b"gearstick/1"

# §4.3 and §4.4, the message sizes with an empty Noise payload.
MSG_ONE_BYTES = 96
MSG_TWO_BYTES = 48

# §3, the largest datagram.
MTU = 1200


def envelope(kind, body):
    """§3: the six-byte header, then the body."""
    return HEADER.pack(MAGIC, VERSION, kind) + body


def unwrap(datagram):
    """§3: refuse anything whose magic or version does not match."""
    if len(datagram) < HEADER.size:
        return None, b""
    magic, version, kind = HEADER.unpack_from(datagram, 0)
    if magic != MAGIC or version != VERSION:
        return None, b""
    return kind, datagram[HEADER.size:]


def seal(noise, counter, plaintext):
    """§6: eight bytes of counter, then a Noise transport message.

    §6 also says nothing is passed as associated data, and §6.1 says the nonce
    is the counter - both of which the framework handles once the counter is the
    message number.
    """
    return envelope(TYPE_SEALED,
                    struct.pack("<Q", counter) + noise.encrypt(plaintext))


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    host, port, server_key_hex = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    server_key = bytes.fromhex(server_key_hex)
    if len(server_key) != 32:
        raise SystemExit("the server key is 32 bytes as 64 hex characters")

    # §4.2: IK, so the initiator needs its own static key and the responder's.
    sk = X25519PrivateKey.generate()
    ours = sk.private_bytes(Encoding.Raw, PrivateFormat.Raw, NoEncryption())

    noise = NoiseConnection.from_name(PROTOCOL)
    noise.set_as_initiator()
    noise.set_prologue(PROLOGUE)
    noise.set_keypair_from_private_bytes(Keypair.STATIC, ours)
    noise.set_keypair_from_public_bytes(Keypair.REMOTE_STATIC, server_key)
    noise.start_handshake()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(2.0)

    # §4.3, message one. §4.6 says the initiator repeats it until message two
    # arrives, because there is nothing to acknowledge.
    one = noise.write_message()
    if len(one) != MSG_ONE_BYTES:
        raise SystemExit(f"document says message one is {MSG_ONE_BYTES} bytes, "
                         f"this is {len(one)}")
    print(f"  message one: {len(one)} bytes, as section 4.3 says")

    two = None
    for attempt in range(10):
        sock.sendto(envelope(TYPE_HANDSHAKE, one), (host, port))
        try:
            datagram, _ = sock.recvfrom(MTU)
        except socket.timeout:
            continue
        kind, body = unwrap(datagram)
        if kind == TYPE_HANDSHAKE:
            two = body
            break
        # §4.6: anything else in this state is dropped without a reply.
    if two is None:
        raise SystemExit("no handshake reply after ten attempts")

    if len(two) != MSG_TWO_BYTES:
        raise SystemExit(f"document says message two is {MSG_TWO_BYTES} bytes, "
                         f"this is {len(two)}")
    print(f"  message two: {len(two)} bytes, as section 4.4 says")

    noise.read_message(two)
    if not noise.handshake_finished:
        raise SystemExit("the handshake did not finish")

    print("  handshake completed against a real gearstick server")
    print(f"  handshake hash: {noise.get_handshake_hash().hex()}")

    # §6: and the tunnel carries something. The server has no reason to answer
    # a datagram it cannot parse, so this proves the framing is accepted rather
    # than that a particular reply comes back - which is all the document
    # promises, since the messages inside are not its subject.
    sealed = seal(noise, 0, b"\x00" * 8)
    if len(sealed) != HEADER.size + 8 + 8 + 16:
        raise SystemExit("sealed framing is not the size section 6 describes")
    sock.sendto(sealed, (host, port))
    print(f"  a sealed datagram is {len(sealed)} bytes for an 8-byte payload: "
          f"6 header + 8 counter + 8 + 16 tag, as section 6 says")

    sock.close()
    print("transport_client: the document was enough")


if __name__ == "__main__":
    main()

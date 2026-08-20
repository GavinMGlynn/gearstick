#!/usr/bin/env python3
"""Talk to gearstick's Noise implementation with somebody else's.

**This is the one check in the transport that is not our own opinion of our own
code.** The published test vectors prove our bytes match a recording; they
cannot prove we can hold a conversation, because both sides of a vector came
from the same implementation. This drives `gearstick_noise_peer` from the
`noiseprotocol` package - a separate implementation of the same framework,
written by other people - and completes a handshake in both directions.

It is a script rather than a test in C on purpose: an independent
implementation has to be independent, and linking one into our own build would
make it ours.

    usage: noise_interop.py <path to gearstick_noise_peer>
"""

import os
import subprocess
import sys

try:
    from noise.connection import NoiseConnection, Keypair
except ImportError:
    print("noise_interop: the noiseprotocol package is not installed; skipping.")
    print("               pip install noiseprotocol")
    sys.exit(77)          # ctest reads this as "skipped", not as "passed"

from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
from cryptography.hazmat.primitives.serialization import (
    Encoding, NoEncryption, PrivateFormat, PublicFormat)

PROTOCOL = b"Noise_IK_25519_ChaChaPoly_BLAKE2s"
PROLOGUE = b"gearstick/1"


def keypair():
    sk = X25519PrivateKey.generate()
    secret = sk.private_bytes(Encoding.Raw, PrivateFormat.Raw, NoEncryption())
    public = sk.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    return secret, public


def counter(n):
    return n.to_bytes(8, "little")


def run(peer_path, ours_is_initiator):
    """One conversation. `ours_is_initiator` is about the C program."""
    c_secret, c_public = keypair()
    py_secret, py_public = keypair()

    args = [peer_path,
            "--initiator" if ours_is_initiator else "--responder",
            "--static", c_secret.hex(),
            "--prologue", PROLOGUE.decode()]
    if ours_is_initiator:
        args += ["--peer", py_public.hex()]

    proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, bufsize=1)

    noise = NoiseConnection.from_name(PROTOCOL)
    if ours_is_initiator:
        noise.set_as_responder()
    else:
        noise.set_as_initiator()
        noise.set_keypair_from_public_bytes(Keypair.REMOTE_STATIC, c_public)
    noise.set_prologue(PROLOGUE)
    noise.set_keypair_from_private_bytes(Keypair.STATIC, py_secret)
    noise.start_handshake()

    def send(data):
        proc.stdin.write(data.hex() + "\n")
        proc.stdin.flush()

    def recv():
        line = proc.stdout.readline().strip()
        if not line:
            raise SystemExit("noise_interop: the C peer said nothing"
                             f" (stderr: {proc.stderr.read()})")
        return bytes.fromhex(line)

    if ours_is_initiator:
        payload = noise.read_message(recv())
        assert payload == b"gearstick", payload
        send(noise.write_message(b"python"))
    else:
        send(noise.write_message(b"python"))
        payload = noise.read_message(recv())
        assert payload == b"gearstick", payload

    assert noise.handshake_finished, "the handshake did not finish"

    # **Both ends arrived at the same handshake hash**, which is the strongest
    # single statement that the two of them completed the same handshake and
    # not two handshakes that happened to produce parseable bytes.
    theirs = None
    for line in iter(proc.stderr.readline, ""):
        if line.startswith("handshake_hash "):
            theirs = bytes.fromhex(line.split()[1])
            break
    ours = noise.get_handshake_hash()
    assert theirs == ours, f"handshake hashes differ:\n  C:  {theirs.hex()}\n  py: {ours.hex()}"

    # And then traffic, through the counter-prefixed framing gearstick puts
    # around a framework transport message.
    sent = 0
    received = 0
    for i in range(8):
        body = f"tick {i} inputs {'x' * i}".encode()
        send(counter(sent) + noise.encrypt(body))
        sent += 1

        reply = recv()
        n = int.from_bytes(reply[:8], "little")
        assert n == received, f"counter out of step: got {n}, expected {received}"
        received += 1
        assert noise.decrypt(reply[8:]) == body, "the echo came back wrong"

    proc.stdin.close()
    if proc.wait(timeout=30) != 0:
        raise SystemExit(f"noise_interop: the C peer exited {proc.returncode}\n"
                         f"{proc.stderr.read()}")

    who = "gearstick" if ours_is_initiator else "noiseprotocol"
    print(f"  handshake completed with {who} as initiator, "
          f"8 datagrams echoed, handshake hashes agree")


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    peer = sys.argv[1]
    if not os.path.exists(peer):
        raise SystemExit(f"noise_interop: no such program: {peer}")

    print(f"noise_interop: {PROTOCOL.decode()} against the noiseprotocol package")
    run(peer, ours_is_initiator=True)
    run(peer, ours_is_initiator=False)
    print("noise_interop: both directions agreed")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""A gearstick transport client written from docs/TRANSPORT.md alone.

Kept as evidence and as a second implementation: it was written by a reader
who was given the document and forbidden the source, and it completed the
handshake against the real server on the first attempt. It is not run by the
test suite; tools/noise_interop.py is the interoperability check. Run it by
hand against a server with `--server-key` set to the key the server prints.

Noise_IK_25519_ChaChaPoly_BLAKE2s (Noise revision 34) over UDP, with the
six-byte envelope from section 3 and the SEALED framing from section 6.
Nothing in this file was derived from the gearstick source.
"""
import argparse
import hashlib
import hmac
import os
import socket
import struct
import sys
import time

from cryptography.hazmat.primitives.asymmetric.x25519 import (
    X25519PrivateKey, X25519PublicKey)
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305

PROTOCOL_NAME = b"Noise_IK_25519_ChaChaPoly_BLAKE2s"   # 33 bytes, section 4.1
PROLOGUE = b"gearstick/1"                              # 11 bytes, section 4.1
HASHLEN = 32
DHLEN = 32
TAGLEN = 16

MAGIC = b"GSSV"          # wire order 47 53 53 56, section 3
VERSION = 1
T_HANDSHAKE = 1
T_SEALED = 2
MAX_DATAGRAM = 1200


# --- primitives -------------------------------------------------------------

def HASH(data: bytes) -> bytes:
    return hashlib.blake2s(data, digest_size=32).digest()


def HMAC_HASH(key: bytes, data: bytes) -> bytes:
    # hashlib.blake2s reports block_size 64, which is what section 5 demands.
    return hmac.new(key, data, lambda d=b"": hashlib.blake2s(d, digest_size=32)).digest()


def HKDF(ck: bytes, ikm: bytes):
    temp = HMAC_HASH(ck, ikm)
    out1 = HMAC_HASH(temp, b"\x01")
    out2 = HMAC_HASH(temp, out1 + b"\x02")
    return out1, out2


def DH(priv: X25519PrivateKey, pub_bytes: bytes) -> bytes:
    return priv.exchange(X25519PublicKey.from_public_bytes(pub_bytes))


def nonce_bytes(n: int) -> bytes:
    # Section 6.1: four zero bytes then the counter as u64 little-endian.
    return b"\x00\x00\x00\x00" + struct.pack("<Q", n)


# --- Noise state objects ----------------------------------------------------

class CipherState:
    def __init__(self):
        self.k = None
        self.n = 0

    def initialize_key(self, k):
        self.k = k
        self.n = 0

    def has_key(self):
        return self.k is not None

    def encrypt_with_ad(self, ad: bytes, plaintext: bytes) -> bytes:
        if self.k is None:
            return plaintext
        c = ChaCha20Poly1305(self.k).encrypt(nonce_bytes(self.n), plaintext, ad)
        self.n += 1
        return c

    def decrypt_with_ad(self, ad: bytes, ciphertext: bytes) -> bytes:
        if self.k is None:
            return ciphertext
        p = ChaCha20Poly1305(self.k).decrypt(nonce_bytes(self.n), ciphertext, ad)
        self.n += 1
        return p

    def decrypt_with_nonce(self, n: int, ciphertext: bytes) -> bytes:
        """Transport receive with an explicit counter (section 6)."""
        return ChaCha20Poly1305(self.k).decrypt(nonce_bytes(n), ciphertext, b"")


class SymmetricState:
    def __init__(self):
        if len(PROTOCOL_NAME) <= HASHLEN:
            self.h = PROTOCOL_NAME.ljust(HASHLEN, b"\x00")
        else:
            self.h = HASH(PROTOCOL_NAME)   # the branch section 4.1 warns about
        self.ck = self.h
        self.cs = CipherState()

    def mix_key(self, ikm: bytes):
        self.ck, temp_k = HKDF(self.ck, ikm)
        self.cs.initialize_key(temp_k)

    def mix_hash(self, data: bytes):
        self.h = HASH(self.h + data)

    def encrypt_and_hash(self, plaintext: bytes) -> bytes:
        c = self.cs.encrypt_with_ad(self.h, plaintext)
        self.mix_hash(c)
        return c

    def decrypt_and_hash(self, ciphertext: bytes) -> bytes:
        p = self.cs.decrypt_with_ad(self.h, ciphertext)
        self.mix_hash(ciphertext)
        return p

    def split(self):
        k1, k2 = HKDF(self.ck, b"")
        c1, c2 = CipherState(), CipherState()
        c1.initialize_key(k1)
        c2.initialize_key(k2)
        return c1, c2


class IKInitiator:
    """Noise IK, initiator side:  <- s ... -> e, es, s, ss   <- e, ee, se"""

    def __init__(self, s: X25519PrivateKey, rs: bytes):
        self.s = s
        self.s_pub = s.public_key().public_bytes_raw()
        self.rs = rs
        self.e = None
        self.re = None
        self.ss = SymmetricState()
        self.ss.mix_hash(PROLOGUE)
        self.ss.mix_hash(rs)          # pre-message  <- s

    def write_message_one(self, payload: bytes = b"") -> bytes:
        out = b""
        # e
        self.e = X25519PrivateKey.generate()
        e_pub = self.e.public_key().public_bytes_raw()
        out += e_pub
        self.ss.mix_hash(e_pub)
        # es
        self.ss.mix_key(DH(self.e, self.rs))
        # s
        out += self.ss.encrypt_and_hash(self.s_pub)
        # ss
        self.ss.mix_key(DH(self.s, self.rs))
        # payload
        out += self.ss.encrypt_and_hash(payload)
        return out

    def read_message_two(self, msg: bytes) -> bytes:
        if len(msg) < DHLEN + TAGLEN:
            raise ValueError("message two too short: %d bytes" % len(msg))
        # e
        self.re = msg[:DHLEN]
        self.ss.mix_hash(self.re)
        # ee
        self.ss.mix_key(DH(self.e, self.re))
        # se  (initiator: DH(s, re))
        self.ss.mix_key(DH(self.s, self.re))
        # payload
        return self.ss.decrypt_and_hash(msg[DHLEN:])

    def split(self):
        c1, c2 = self.ss.split()
        return c1, c2, self.ss.h   # initiator sends under k1, receives under k2


# --- envelope ---------------------------------------------------------------

def envelope(msg_type: int) -> bytes:
    return MAGIC + bytes([VERSION, msg_type])


def parse_envelope(dgram: bytes):
    if len(dgram) < 6:
        raise ValueError("datagram shorter than the envelope: %d bytes" % len(dgram))
    if dgram[:4] != MAGIC:
        raise ValueError("bad magic %s" % dgram[:4].hex())
    if dgram[4] != VERSION:
        raise ValueError("bad version %d" % dgram[4])
    return dgram[5], dgram[6:]


def seal(send_cs: CipherState, plaintext: bytes) -> bytes:
    counter = send_cs.n
    body = send_cs.encrypt_with_ad(b"", plaintext)       # AD is empty, section 6
    return envelope(T_SEALED) + struct.pack("<Q", counter) + body


def open_sealed(recv_cs: CipherState, body: bytes) -> tuple:
    if len(body) < 8 + TAGLEN:
        raise ValueError("SEALED body too short: %d" % len(body))
    counter = struct.unpack("<Q", body[:8])[0]
    return counter, recv_cs.decrypt_with_nonce(counter, body[8:])


# --- driver -----------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server-key", required=True, help="server static key, 64 hex chars")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=47123)
    ap.add_argument("--payload", default="", help="hex of plaintext to send inside SEALED")
    ap.add_argument("--payloads", default="", help="comma-separated hex plaintexts, sent one each")
    ap.add_argument("--static-hex", default="", help="client static private key hex (else random)")
    ap.add_argument("--timeout", type=float, default=2.0)
    ap.add_argument("--retries", type=int, default=3)
    args = ap.parse_args()

    rs = bytes.fromhex(args.server_key)
    if len(rs) != 32:
        sys.exit("server key must decode to 32 bytes, got %d" % len(rs))

    if args.static_hex:
        s = X25519PrivateKey.from_private_bytes(bytes.fromhex(args.static_hex))
    else:
        s = X25519PrivateKey.generate()
    print("client static pub", s.public_key().public_bytes_raw().hex())

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(args.timeout)
    addr = (args.host, args.port)

    # --- handshake --------------------------------------------------------
    hs = IKInitiator(s, rs)
    m1 = envelope(T_HANDSHAKE) + hs.write_message_one(b"")
    print("message one: %d bytes datagram (%d handshake bytes)" % (len(m1), len(m1) - 6))
    print("  ", m1.hex())

    reply = None
    for attempt in range(args.retries):
        sock.sendto(m1, addr)
        try:
            reply, from_addr = sock.recvfrom(MAX_DATAGRAM)
            break
        except socket.timeout:
            print("no reply to message one (attempt %d)" % (attempt + 1))
    if reply is None:
        sys.exit("FAIL: server never answered message one")

    print("message two: %d bytes datagram" % len(reply))
    print("  ", reply.hex())
    t, body = parse_envelope(reply)
    if t != T_HANDSHAKE:
        sys.exit("FAIL: expected HANDSHAKE (1) reply, got type %d" % t)
    try:
        payload2 = hs.read_message_two(body)
    except Exception as exc:                        # noqa: BLE001
        sys.exit("FAIL: message two did not authenticate: %r" % (exc,))
    send_cs, recv_cs, hh = hs.split()
    print("handshake OPEN. handshake hash", hh.hex())
    print("  payload two (%d bytes): %s" % (len(payload2), payload2.hex()))

    # --- one or more sealed messages --------------------------------------
    payloads = []
    if args.payloads:
        payloads = [bytes.fromhex(p) for p in args.payloads.split(",")]
    else:
        payloads = [bytes.fromhex(args.payload)]

    for pt in payloads:
        d = seal(send_cs, pt)
        print("SEALED send counter=%d plaintext=%s -> %d bytes" % (send_cs.n - 1, pt.hex(), len(d)))
        print("  ", d.hex())
        sock.sendto(d, addr)
        # Collect anything the server sends back within the timeout window.
        deadline = time.time() + args.timeout
        while time.time() < deadline:
            try:
                r, _ = sock.recvfrom(MAX_DATAGRAM)
            except socket.timeout:
                break
            print("reply: %d bytes  %s" % (len(r), r.hex()))
            try:
                rt, rbody = parse_envelope(r)
            except ValueError as exc:
                print("  (envelope rejected: %s)" % exc)
                continue
            if rt == T_SEALED:
                try:
                    ctr, plain = open_sealed(recv_cs, rbody)
                    print("  SEALED opened OK, counter=%d, plaintext (%d bytes): %s"
                          % (ctr, len(plain), plain.hex()))
                    print("  as text: %r" % plain)
                except Exception as exc:            # noqa: BLE001
                    print("  SEALED FAILED to open: %r" % (exc,))
            else:
                print("  non-SEALED reply type %d" % rt)
    print("done")


if __name__ == "__main__":
    main()

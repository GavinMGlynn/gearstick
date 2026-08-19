// gs_pack.h - run-length packing and a text alphabet, so a track can be a code.
//
// Both halves are here because both have to be **byte-exact in both
// directions** on every machine, which is the same requirement the simulation
// has and the same reason this lives in src/core/ and links nothing.
//
// The packing is LZSS: literals and back-references, a four kilobyte window,
// no entropy coder and no state carried between calls. It is not the best
// compressor available and it is the right one for this data - a track is
// mostly the same few tiles repeated, and what repeats is a *pattern* rather
// than a byte, because surface and gravity are interleaved per tile.
#ifndef GS_PACK_H
#define GS_PACK_H

#include <stddef.h>
#include <stdint.h>

// Worst case is one control byte per eight literals, for data with nothing
// repeated in it at all.
#define GS_PACK_BOUND(n) ((n) + (n) / 8u + 2u)

// Returns the packed length, or 0 if it would not fit.
size_t gs_pack(const uint8_t *in, size_t n, uint8_t *out, size_t cap);

// Returns the unpacked length, or 0 if the input is malformed or does not fit.
size_t gs_unpack(const uint8_t *in, size_t n, uint8_t *out, size_t cap);

// Base64url - the URL-safe alphabet, and no padding. A code goes in a chat
// message, a URL and a text file without anything quoting or escaping it.
#define GS_B64_BOUND(n) (((n) + 2u) / 3u * 4u + 1u)

size_t gs_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap);
size_t gs_b64_decode(const char *in, size_t n, uint8_t *out, size_t cap);

#endif // GS_PACK_H

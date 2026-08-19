#include "core/gs_blake2s.h"

#include <string.h>

// RFC 7693 section 2.6. The same eight words SHA-256 starts from.
static const uint32_t GS_IV[8] = {
    0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
    0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u
};

// RFC 7693 section 2.7. Ten rounds for BLAKE2s, so ten permutations.
static const uint8_t GS_SIGMA[10][16] = {
    {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
    { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
    { 11,  8, 12,  0,  5,  2, 15, 13, 10, 14,  3,  6,  7,  1,  9,  4 },
    {  7,  9,  3,  1, 13, 12, 11, 14,  2,  6,  5, 10,  4,  0, 15,  8 },
    {  9,  0,  5,  7,  2,  4, 10, 15, 14,  1, 11, 12,  6,  8,  3, 13 },
    {  2, 12,  6, 10,  0, 11,  8,  3,  4, 13,  7,  5, 15, 14,  1,  9 },
    { 12,  5,  1, 15, 14, 13,  4, 10,  0,  7,  6,  3,  9,  2,  8, 11 },
    { 13, 11,  7, 14, 12,  1,  3,  9,  5,  0, 15,  4,  8,  6,  2, 10 },
    {  6, 15, 14,  9, 11,  3,  0,  8, 12,  2, 13,  7,  1,  4, 10,  5 },
    { 10,  2,  8,  4,  7,  6,  1,  5, 15, 11,  9, 14,  3, 12, 13,  0 }
};

// The shift is masked so that a rotation by zero does not become a shift by
// thirty-two, which is undefined. No call here rotates by zero; the mask is
// there so that the function is total rather than only correct when used
// carefully.
static uint32_t gs_rotr(uint32_t x, unsigned n) {
    return (uint32_t)((x >> (n & 31u)) | (x << ((32u - n) & 31u)));
}

static uint32_t gs_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// RFC 7693 section 3.1.
#define GS_G(a, b, c, d, x, y)                  \
    do {                                        \
        v[a] = v[a] + v[b] + (x);               \
        v[d] = gs_rotr(v[d] ^ v[a], 16);        \
        v[c] = v[c] + v[d];                     \
        v[b] = gs_rotr(v[b] ^ v[c], 12);        \
        v[a] = v[a] + v[b] + (y);               \
        v[d] = gs_rotr(v[d] ^ v[a], 8);         \
        v[c] = v[c] + v[d];                     \
        v[b] = gs_rotr(v[b] ^ v[c], 7);         \
    } while (0)

// RFC 7693 section 3.2. `last` marks the final block, which is the only thing
// distinguishing a message from a prefix of a longer one.
static void gs_compress(gs_blake2s *s, const uint8_t *block, bool last) {
    uint32_t m[16];
    for (int i = 0; i < 16; i++) m[i] = gs_le32(block + i * 4);

    uint32_t v[16];
    for (int i = 0; i < 8; i++) v[i] = s->h[i];
    for (int i = 0; i < 8; i++) v[8 + i] = GS_IV[i];

    v[12] ^= (uint32_t)(s->counted & 0xffffffffu);
    v[13] ^= (uint32_t)(s->counted >> 32);
    if (last) v[14] = ~v[14];

    for (int r = 0; r < 10; r++) {
        const uint8_t *g = GS_SIGMA[r];
        GS_G(0, 4,  8, 12, m[g[0]],  m[g[1]]);
        GS_G(1, 5,  9, 13, m[g[2]],  m[g[3]]);
        GS_G(2, 6, 10, 14, m[g[4]],  m[g[5]]);
        GS_G(3, 7, 11, 15, m[g[6]],  m[g[7]]);
        GS_G(0, 5, 10, 15, m[g[8]],  m[g[9]]);
        GS_G(1, 6, 11, 12, m[g[10]], m[g[11]]);
        GS_G(2, 7,  8, 13, m[g[12]], m[g[13]]);
        GS_G(3, 4,  9, 14, m[g[14]], m[g[15]]);
    }

    for (int i = 0; i < 8; i++) s->h[i] ^= v[i] ^ v[8 + i];
}

void gs_blake2s_init(gs_blake2s *s, uint8_t out_len) {
    if (out_len == 0 || out_len > GS_BLAKE2S_BYTES) out_len = GS_BLAKE2S_BYTES;

    memset(s, 0, sizeof *s);
    for (int i = 0; i < 8; i++) s->h[i] = GS_IV[i];

    // The parameter block, folded into the first word: digest length, key
    // length (zero - nothing here is keyed), fanout 1, depth 1.
    s->h[0] ^= 0x01010000u ^ (uint32_t)out_len;
    s->out_len = out_len;
}

void gs_blake2s_update(gs_blake2s *s, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;

    while (len > 0) {
        // **A full buffer is not compressed until something follows it.** The
        // last block is the one whose compression flips v[14], so a block can
        // only be compressed once it is known not to be last. Compressing
        // eagerly is the classic way to get a hash that is right for every
        // message except those whose length is an exact multiple of the block.
        if (s->held == GS_BLAKE2S_BLOCK) {
            s->counted += GS_BLAKE2S_BLOCK;
            gs_compress(s, s->buf, false);
            s->held = 0;
        }

        size_t take = GS_BLAKE2S_BLOCK - s->held;
        if (take > len) take = len;
        memcpy(s->buf + s->held, p, take);
        s->held += take;
        p += take;
        len -= take;
    }
}

void gs_blake2s_final(gs_blake2s *s, uint8_t *out) {
    s->counted += s->held;
    memset(s->buf + s->held, 0, GS_BLAKE2S_BLOCK - s->held);
    gs_compress(s, s->buf, true);

    for (uint8_t i = 0; i < s->out_len; i++) {
        out[i] = (uint8_t)((s->h[i >> 2] >> (8u * (i & 3u))) & 0xffu);
    }
}

void gs_blake2s_hash(uint8_t *out, uint8_t out_len, const void *data, size_t len) {
    gs_blake2s s;
    gs_blake2s_init(&s, out_len);
    gs_blake2s_update(&s, data, len);
    gs_blake2s_final(&s, out);
}

uint64_t gs_blake2s_u64(const void *data, size_t len) {
    uint8_t digest[GS_BLAKE2S_BYTES];
    gs_blake2s_hash(digest, GS_BLAKE2S_BYTES, data, len);

    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)digest[i] << (8 * i);
    return v;
}

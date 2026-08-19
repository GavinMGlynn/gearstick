#include "core/gs_pack.h"

// --- LZSS ------------------------------------------------------------------
//
// A control byte carries eight flags, low bit first. A set bit means one
// literal byte follows; a clear bit means two bytes follow describing a match:
// twelve bits of distance back and four bits of length.
//
//   byte 0   distance & 0xff
//   byte 1   (distance >> 8) << 4 | (length - GS_LZ_MIN)
//
// Run-length packing was tried first and was nearly useless here, for a reason
// worth writing down: a track's surface and gravity are interleaved per tile,
// so flat identical ground is not a run of one byte but a *pattern* of two, and
// a run-length coder sees no runs at all in it. It saved an eighth. This sees
// the period and saves nine tenths, which is the difference between a code you
// can paste into a message and one you cannot.
//
// The match search is greedy and the parameters are fixed, so the same track
// packs to the same bytes everywhere - which a shared code has to, or two
// people who built the same thing would not agree they had.

#define GS_LZ_BITS   12u
#define GS_LZ_WINDOW (1u << GS_LZ_BITS)     // 4096 back
#define GS_LZ_MIN    3u
#define GS_LZ_MAX    (GS_LZ_MIN + 15u)      // 18 forward

// How far back to keep looking once something has been found. A track is
// repetitive enough that the first candidates are nearly always the best ones,
// and this bounds the work at a few million comparisons for a whole track
// rather than a few hundred million.
#define GS_LZ_PATIENCE 64u

static size_t gs_match(const uint8_t *in, size_t n, size_t at, size_t *dist_out) {
    size_t best = 0, best_dist = 0;
    size_t start = at > GS_LZ_WINDOW ? at - GS_LZ_WINDOW : 0;
    size_t limit = n - at < GS_LZ_MAX ? n - at : GS_LZ_MAX;
    if (limit < GS_LZ_MIN) return 0;

    size_t tried = 0;
    for (size_t p = at; p-- > start; ) {
        if (in[p] != in[at] || in[p + best] != in[at + best]) continue;

        size_t len = 0;
        while (len < limit && in[p + len] == in[at + len]) len++;

        if (len > best) {
            best = len;
            best_dist = at - p;
            if (best == limit) break;
        }
        if (++tried >= GS_LZ_PATIENCE) break;
    }
    return best >= GS_LZ_MIN ? (*dist_out = best_dist, best) : 0;
}

size_t gs_pack(const uint8_t *in, size_t n, uint8_t *out, size_t cap) {
    size_t r = 0, w = 0;

    while (r < n) {
        // The control byte is written last, once the eight things it describes
        // are known, so its slot is reserved and filled in at the end.
        if (w >= cap) return 0;
        size_t ctl_at = w++;
        uint8_t ctl = 0;

        for (uint8_t bit = 0; bit < 8 && r < n; bit++) {
            size_t dist = 0;
            size_t len = gs_match(in, n, r, &dist);

            if (len >= GS_LZ_MIN) {
                if (w + 2 > cap) return 0;
                uint32_t d = (uint32_t)(dist - 1);
                out[w++] = (uint8_t)(d & 0xffu);
                out[w++] = (uint8_t)(((d >> 8) << 4) | (uint32_t)(len - GS_LZ_MIN));
                r += len;
            } else {
                if (w + 1 > cap) return 0;
                ctl |= (uint8_t)(1u << bit);
                out[w++] = in[r++];
            }
        }
        out[ctl_at] = ctl;
    }
    return w;
}

size_t gs_unpack(const uint8_t *in, size_t n, uint8_t *out, size_t cap) {
    size_t r = 0, w = 0;

    while (r < n) {
        uint8_t ctl = in[r++];

        for (uint8_t bit = 0; bit < 8 && r < n; bit++) {
            if (ctl & (1u << bit)) {
                if (w + 1 > cap) return 0;
                out[w++] = in[r++];
                continue;
            }

            if (r + 2 > n) return 0;
            uint32_t lo = in[r++];
            uint32_t hi = in[r++];
            size_t dist = (size_t)(((hi >> 4) << 8) | lo) + 1u;
            size_t len  = (size_t)(hi & 0x0fu) + GS_LZ_MIN;

            // A distance that reaches before the start of the output is the
            // shape a damaged code arrives in, and is refused rather than read.
            if (dist > w) return 0;
            if (w + len > cap) return 0;

            // Byte at a time on purpose: an overlapping match is how a run is
            // encoded here, so the bytes being written are part of what is
            // being copied.
            for (size_t i = 0; i < len; i++, w++) out[w] = out[w - dist];
        }
    }
    return w;
}

// --- base64url -------------------------------------------------------------

static const char gs_b64_alphabet[65] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static int gs_b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

size_t gs_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap) {
    size_t w = 0;

    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        size_t have = 1;
        if (i + 1 < n) { v |= (uint32_t)in[i + 1] << 8; have = 2; }
        if (i + 2 < n) { v |= (uint32_t)in[i + 2];      have = 3; }

        // No padding: the byte count follows from the character count, so '='
        // would carry nothing and would need escaping in a URL.
        size_t chars = have + 1;
        if (w + chars >= cap) return 0;
        for (size_t k = 0; k < chars; k++) {
            out[w++] = gs_b64_alphabet[(v >> (18u - 6u * k)) & 0x3fu];
        }
    }

    if (w >= cap) return 0;
    out[w] = '\0';
    return w;
}

size_t gs_b64_decode(const char *in, size_t n, uint8_t *out, size_t cap) {
    size_t w = 0, i = 0;

    while (i < n) {
        // A group of four characters is three bytes; a trailing group of two or
        // three is one or two. A trailing group of one is not any number of
        // bytes, so it is malformed rather than rounded off.
        size_t group = n - i < 4 ? n - i : 4;
        if (group == 1) return 0;

        uint32_t v = 0;
        for (size_t k = 0; k < group; k++) {
            int d = gs_b64_value(in[i + k]);
            if (d < 0) return 0;
            v |= (uint32_t)d << (18u - 6u * k);
        }

        size_t bytes = group - 1;
        if (w + bytes > cap) return 0;
        for (size_t k = 0; k < bytes; k++) out[w++] = (uint8_t)((v >> (16u - 8u * k)) & 0xffu);
        i += group;
    }
    return w;
}

#include "core/gs_share.h"

#include "core/gs_pack.h"

#include <string.h>

// The wire behind the text: eight bytes of the track's own content hash, then
// the packed track. The hash is what makes a damaged code fail loudly - the
// track is rebuilt from the bytes and has to hash to what the code said it
// would, which no accidental edit survives.
#define GS_SHARE_HASH_BYTES 8

static void gs_put64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xffu);
}

static uint64_t gs_get64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

size_t gs_track_to_code(const gs_track *t, char *out, size_t cap) {
    static uint8_t raw[GS_TRACK_TILES * 4 + 4096];
    size_t n = gs_track_serialize(t, raw, sizeof raw);
    if (n == 0) return 0;

    static uint8_t body[GS_SHARE_HASH_BYTES + GS_PACK_BOUND(sizeof raw)];
    gs_put64(body, gs_track_hash(t));

    size_t packed = gs_pack(raw, n, body + GS_SHARE_HASH_BYTES,
                            sizeof body - GS_SHARE_HASH_BYTES);
    if (packed == 0) return 0;

    size_t prefix = strlen(GS_SHARE_PREFIX);
    if (cap <= prefix) return 0;
    memcpy(out, GS_SHARE_PREFIX, prefix);

    size_t text = gs_b64_encode(body, GS_SHARE_HASH_BYTES + packed,
                                out + prefix, cap - prefix);
    if (text == 0) return 0;
    return prefix + text;
}

size_t gs_track_to_url(const gs_track *t, char *out, size_t cap) {
    size_t scheme = strlen(GS_SHARE_SCHEME);
    if (cap <= scheme) return 0;
    memcpy(out, GS_SHARE_SCHEME, scheme);

    size_t n = gs_track_to_code(t, out + scheme, cap - scheme);
    if (n == 0) return 0;
    return scheme + n;
}

static bool gs_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool gs_track_from_code(gs_track *t, const char *code) {
    if (code == nullptr) return false;

    // Whatever the other person's chat client did to it. Leading and trailing
    // whitespace, and a URL wrapper, are the two things that always happen.
    while (*code != '\0' && gs_is_space(*code)) code++;

    size_t scheme = strlen(GS_SHARE_SCHEME);
    if (strncmp(code, GS_SHARE_SCHEME, scheme) == 0) code += scheme;

    size_t prefix = strlen(GS_SHARE_PREFIX);
    if (strncmp(code, GS_SHARE_PREFIX, prefix) != 0) return false;
    code += prefix;

    size_t len = 0;
    while (code[len] != '\0' && !gs_is_space(code[len])) len++;
    if (len <= GS_SHARE_HASH_BYTES) return false;

    static uint8_t body[GS_SHARE_HASH_BYTES + GS_PACK_BOUND(GS_TRACK_TILES * 4 + 4096)];
    size_t n = gs_b64_decode(code, len, body, sizeof body);
    if (n <= GS_SHARE_HASH_BYTES) return false;

    static uint8_t raw[GS_TRACK_TILES * 4 + 4096];
    size_t raw_n = gs_unpack(body + GS_SHARE_HASH_BYTES, n - GS_SHARE_HASH_BYTES,
                             raw, sizeof raw);
    if (raw_n == 0) return false;

    if (!gs_track_deserialize(t, raw, raw_n)) return false;

    // **The code says which track it is, and the track has to agree.** A code
    // that lost a character usually still decodes to *something*; this is what
    // stops that something being handed over as a track somebody built.
    //
    // **Or agree with what it used to be.** A track's identity gained the loop
    // or path byte after v0.1.0-beta1 went out, so a code shared by anybody
    // running that build carries the answer this used to give. Refusing it
    // would be telling somebody a working code was damaged; accepting either
    // costs the ability to notice a corruption of the route byte alone, which
    // is one byte in a hundred and would still open as a real track.
    const uint64_t said = gs_get64(body);
    return gs_track_hash(t) == said ||
           gs_track_hash_before_route_kind(t) == said;
}

#include "net/gs_carrier.h"

#include <string.h>

void gs_carrier_expect(gs_carrier *c, uint64_t hash) {
    if (c->hash == hash && !c->complete) return;   // already collecting this one

    memset(c, 0, sizeof *c);
    c->hash = hash;
}

uint16_t gs_carrier_chunks(size_t len) {
    if (len == 0) return 0;
    return (uint16_t)((len + GS_CHUNK_BYTES - 1) / GS_CHUNK_BYTES);
}

size_t gs_carrier_chunk(uint8_t *buf, size_t cap, uint64_t hash,
                        const uint8_t *bytes, size_t len, uint16_t chunk) {
    uint16_t chunks = gs_carrier_chunks(len);
    if (chunk >= chunks) return 0;

    size_t at = (size_t)chunk * GS_CHUNK_BYTES;
    size_t take = len - at;
    if (take > GS_CHUNK_BYTES) take = GS_CHUNK_BYTES;

    return gs_proto_track_chunk(buf, cap, hash, chunk, chunks, bytes + at,
                                (uint16_t)take);
}

bool gs_carrier_take(gs_carrier *c, const uint8_t *buf, size_t len) {
    uint64_t hash = 0;
    uint16_t chunk = 0, chunks = 0, data_len = 0;
    const uint8_t *data = nullptr;

    if (!gs_proto_read_track_chunk(buf, len, &hash, &chunk, &chunks, &data,
                                   &data_len)) {
        return false;
    }

    // Not the track we are waiting for. Somebody else's chunk, or a late one
    // from a transfer that has been replaced.
    if (hash != c->hash) return false;
    if (chunks > GS_CARRIER_MAX_CHUNKS) return false;

    // The count cannot change halfway through a transfer.
    if (c->chunks != 0 && c->chunks != chunks) return false;
    c->chunks = chunks;

    size_t at = (size_t)chunk * GS_CHUNK_BYTES;
    if (at + data_len > GS_CARRIER_MAX_BYTES) return false;

    // Every chunk but the last is full. A short one in the middle would mean
    // the pieces do not fit back together, however plausible each looks.
    if (chunk + 1 < chunks && data_len != GS_CHUNK_BYTES) return false;

    memcpy(c->bytes + at, data, data_len);

    if (!c->have[chunk]) {
        c->have[chunk] = true;
        c->got++;
    }
    if (at + data_len > c->len) c->len = at + data_len;

    c->complete = c->got == c->chunks;
    return true;
}

bool gs_carrier_done(const gs_carrier *c) {
    return c->complete && c->chunks > 0;
}

bool gs_carrier_track(const gs_carrier *c, gs_track *out) {
    if (!gs_carrier_done(c)) return false;
    if (!gs_track_deserialize(out, c->bytes, c->len)) return false;

    // **The bytes arriving is not the track being right.** Everything else in
    // this project identifies a track by its content, and a transfer is no
    // different: what was asked for has to be what turned up, or two machines
    // race on tracks they both believe are the same one.
    return gs_track_hash(out) == c->hash;
}

float gs_carrier_progress(const gs_carrier *c) {
    if (c->chunks == 0) return 0.0f;
    return (float)c->got / (float)c->chunks;
}

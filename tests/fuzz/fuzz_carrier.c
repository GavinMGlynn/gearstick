// fuzz_carrier.c - the chunked reassembler, fed a stream of datagrams.
//
// **One input is a sequence, not a single datagram.** The reassembler is a
// state machine over many packets, so feeding it one at a time from a fresh
// state would never reach the bugs that live in how one chunk interacts with
// the last: a count that changes halfway, a chunk that overlaps another, a
// short chunk in the middle, the same piece twice. The input is carved into
// length-prefixed records and they go in one after another.
#include "net/gs_carrier.h"
#include "net/gs_proto.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // 300 KB, and one per process rather than one per input.
    static gs_carrier c;

    if (size < 3) return 0;

    // **The transfer expects whatever the first datagram says it is.**
    //
    // The first version of this took the expected hash from the front of the
    // input instead, and it was close to useless: every chunk carries the hash
    // of the track it belongs to, `gs_carrier_take` refuses one that does not
    // match, and mutation reaches an eight-byte match by luck alone. So the
    // fuzzer spent millions of inputs being turned away at that one comparison
    // and never touched the reassembly behind it - which was demonstrated by
    // planting an out-of-bounds write past it and watching four and a half
    // million runs sail straight by.
    //
    // Following the first chunk is also what the server does: it calls
    // gs_carrier_expect with the hash from the claim, which is the hash the
    // client is about to send. A later chunk disagreeing is then the
    // stray-chunk case, still reached, and reached on purpose.
    size_t at = 0;
    bool expecting = false;

    while (at + 2 <= size) {
        size_t len = (size_t)data[at] | ((size_t)data[at + 1] << 8);
        at += 2;
        if (len > size - at) len = size - at;

        if (!expecting) {
            uint64_t hash = 0;
            uint16_t chunk = 0, chunks = 0, data_len = 0;
            const uint8_t *payload = nullptr;
            if (gs_proto_read_track_chunk(data + at, len, &hash, &chunk, &chunks,
                                          &payload, &data_len)) {
                gs_carrier_expect(&c, hash);
                expecting = true;
            }
        }

        if (expecting) gs_carrier_take(&c, data + at, len);
        at += len;

        // Asked after every chunk, not only at the end: "is it finished" is
        // itself a thing that reads the state, and a half-built transfer is
        // exactly when it would go wrong.
        if (gs_carrier_done(&c)) {
            static gs_track t;
            (void)gs_carrier_track(&c, &t);
        }
        (void)gs_carrier_progress(&c);
    }
    return 0;
}

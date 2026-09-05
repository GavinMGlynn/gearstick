// fuzz_proto.c - every reader in the protocol, fed whatever libFuzzer invents.
//
// **Each reader is handed the same bytes.** A real server picks one by the
// message byte, and that is exactly the reason to do the opposite here: the
// dispatch is one line and the readers are where the arithmetic is, so feeding
// a WELCOME to the LISTING reader is a cheap way to find a reader that trusts
// the type byte instead of the length in front of it.
//
// The outputs are checked for nothing. A parser's job here is to survive, not
// to be right - and what "survive" means is decided by the sanitizers this is
// built with, not by an assertion in this file.
#include "net/gs_proto.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    uint64_t u64a = 0, u64b = 0, u64c = 0;
    uint32_t u32a = 0, u32b = 0;
    uint16_t u16a = 0, u16b = 0, u16c = 0;
    uint8_t  u8a = 0, u8b = 0, u8c = 0;
    const uint8_t *chunk_data = nullptr;

    // Deliberately one byte short of the widest name the protocol allows, so a
    // reader that writes the full width into what it was told is the capacity
    // trips the sanitizer rather than fitting by luck.
    char name[GS_PROTO_NAME];
    char other[GS_PROTO_NAME];

    static gs_lobby lobby;

    bool manual = false;
    gs_proto_read_join(data, size, name, sizeof name, &manual);
    gs_proto_read_stamp(data, size, &u32a);
    gs_proto_read_full(data, size, name, sizeof name);
    gs_proto_read_welcome(data, size, &u8a, &lobby);
    gs_proto_read_lobby(data, size, &lobby);
    bool reversed = false;
    gs_proto_read_start(data, size, &u64a, &u8a, &u16a, &u8b, &reversed);
    gs_proto_read_want_track(data, size, &u64a);
    gs_proto_read_track_chunk(data, size, &u64a, &u16a, &u16b, &chunk_data, &u16c);
    gs_proto_read_proof_chunk(data, size, &u64a, &u16a, &u16b, &chunk_data, &u16c);
    gs_proto_read_session(data, size, &u64a);
    gs_proto_read_result(data, size, &u64a, &u64b, &u16a, &u8a, &u32a, &u32b, &u64c);
    gs_proto_read_want_best(data, size, &u64a, &u64b, &u16a);
    gs_proto_read_best(data, size, &u64a, &u64b, &u16a, &u32a, name, sizeof name,
                       &u32b, other, sizeof other);
    gs_proto_read_publish(data, size, &u64a, name, sizeof name);
    gs_proto_read_withdraw(data, size, &u64a);
    gs_proto_read_listing(data, size, &u16a, &u16b, &u64a, name, sizeof name,
                          other, sizeof other);

    (void)u8c;
    return 0;
}

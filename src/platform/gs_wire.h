// gs_wire.h - the socket under the netcode, and nothing more than a socket.
//
// **Datagrams, unordered, unacknowledged, allowed to vanish.** Every comfort a
// stream socket offers - delivery, ordering, retransmission - is a round trip,
// and rollback exists precisely so that nobody waits a round trip to see their
// own car move. Loss is dealt with by sending the last thirty-two ticks of
// input in every packet, so a lost datagram is already replaced by the time
// anybody could have asked for it.
//
// This file is the only thing in the project that knows what a socket is. The
// rollback session in src/core/gs_net.c produces and consumes byte arrays, and
// that separation is what lets a twelve-second race under 200 ms of latency and
// twelve percent packet loss run inside a unit test with no network at all.
#ifndef GS_WIRE_H
#define GS_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GS_WIRE_MTU 512

typedef struct gs_wire gs_wire;

// Bring the networking up. Safe to call more than once; the last quit wins.
bool gs_wire_init(void);
void gs_wire_quit(void);

// Listen on a port, waiting to be joined. The peer's address is learned from
// the first datagram that arrives, so there is nothing to configure at this end
// beyond the port.
gs_wire *gs_wire_host(uint16_t port);

// Join somebody who is listening. `host` is a name or an address.
gs_wire *gs_wire_join(const char *host, uint16_t port);

void gs_wire_close(gs_wire *w);

// True once both ends know where the other one is - which for the host means
// the first datagram has arrived.
bool gs_wire_connected(const gs_wire *w);

// Send one. False only if there is nowhere to send it yet; a datagram that is
// sent and then lost is a success as far as this is concerned, which is the
// whole point of choosing datagrams.
bool gs_wire_send(gs_wire *w, const uint8_t *buf, size_t len);

// Take the next one that has arrived, or 0 if none has. Never blocks.
size_t gs_wire_recv(gs_wire *w, uint8_t *buf, size_t cap);

// What it has been doing, for a corner of the screen when a race feels wrong.
void gs_wire_stats(const gs_wire *w, uint32_t *sent, uint32_t *received);

// The last thing that went wrong, for putting in front of the player rather
// than in a log they will never see.
const char *gs_wire_error(const gs_wire *w);

#endif // GS_WIRE_H

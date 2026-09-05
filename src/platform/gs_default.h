// gs_default.h - the server the game points at by default.
//
// **One centralised server, and an address for it that nobody has to type.**
// Today a player joins a server by giving its host, port and public key on
// the command line, which is fine for a friend's machine and wrong for the
// one server everybody is meant to meet at. That server's address lives in a
// file, `server.txt`: one line, host, port and the 64 hex characters of the
// server's public key, with `#` for a comment. The one that ships lives in
// the assets beside the tracks; one in the player's own preference directory
// takes precedence, so a player can point at a different server without
// touching what was installed.
//
// **The file shipped today is empty**, because there is no hosted instance
// yet - see the plan. The day there is, its address goes in that file and
// `gearstick --online` is the whole of joining.
#ifndef GS_DEFAULT_H
#define GS_DEFAULT_H

#include "net/gs_noise.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GS_DEFAULT_SERVER_FILE "server.txt"
#define GS_DEFAULT_HOST 128

typedef struct gs_default_server {
    char     host[GS_DEFAULT_HOST];
    uint16_t port;
    uint8_t  key[GS_NOISE_KEY_BYTES];
} gs_default_server;

// Read the text of a server file. True when it names a server: a host, a
// port from 1 to 65535 and a 64-character hex key, on one line, after any
// number of blank lines and `#` comments. Anything else - no line, a short
// key, a port out of range, a second line - is false, and the game says
// there is no default rather than joining whatever the file half-said.
bool gs_default_server_parse(const char *text, gs_default_server *out);

// The default server, from the player's own file when there is one and
// from the shipped one otherwise. False when neither names a server. `why`
// says which file was read, or that none was, for the log.
bool gs_default_server_load(gs_default_server *out, char *why, size_t why_cap);

#endif // GS_DEFAULT_H

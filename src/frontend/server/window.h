// window.h - a window on the server, for a machine that has a screen.
//
// The server draws its live view in a terminal and is happy with no screen,
// and both stay true. On a machine with a display it opens a window as well,
// showing the same facts - who is here, from where, how long, what is flowing
// - and a log of arrivals and departures a terminal scrolls away, with room
// for what an operator does next: drop a client, take the track down.
//
// **The window and the terminal draw the same facts.** Neither reads the
// server's state directly: main.c gathers one gs_srv_facts from it and hands
// that to both, so a number on the screen is the number in the terminal by
// construction - and the output check reads both back and says so.
#ifndef GS_SERVER_WINDOW_H
#define GS_SERVER_WINDOW_H

#include "net/gs_proto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// One row of the client table, as text where the terminal shows text.
typedef struct gs_srv_row {
    bool     used;
    char     name[32];
    char     from[24];
    char     ping[8];
    uint32_t in, out;
    bool     quiet;             // silent for three seconds, and not yet dropped
} gs_srv_row;

typedef struct gs_srv_facts {
    uint16_t port;
    uint64_t up_s;
    uint8_t  here, capacity, peak;
    uint32_t refused;
    uint32_t total_in, total_out, relayed;
    char     in_bytes[24], out_bytes[24];
    bool     store;
    int      drivers, records, tracks;
    uint32_t results, kept, rejected;
    bool     track;             // a track is being served
    uint64_t track_hash;
    size_t   track_len;
    uint32_t chunks_sent;
    double   in_rate, out_rate; // datagrams a second, once a second has passed
    gs_srv_row row[GS_PROTO_MAX_PLAYERS];
} gs_srv_facts;

// The last so many things that happened: arrivals, departures, the track
// taken down. A terminal scrolls these away under the dashboard; the window
// keeps them.
#define GS_SRV_LOG_LINES 64
#define GS_SRV_LOG_WIDTH 96
typedef struct gs_srv_log {
    char line[GS_SRV_LOG_LINES][GS_SRV_LOG_WIDTH];
    int  count;                 // how many are held, up to GS_SRV_LOG_LINES
    int  next;                  // where the next one goes
} gs_srv_log;

void gs_srv_log_add(gs_srv_log *log, const char *text);
// The i-th oldest line held, for i in [0, count).
const char *gs_srv_log_line(const gs_srv_log *log, int i);

// What the operator pressed this frame. main.c acts on it, because the
// window knows nothing about clients or stores.
typedef struct gs_srv_ask {
    int  drop_slot;             // -1, or a client to drop
    bool take_down;             // stop serving the track
} gs_srv_ask;

// Open the window. False when there is no display, or SDL's video cannot
// start, or a window cannot be made - none of which is an error for a
// server, which is why this says so and returns rather than failing. The
// icon path may be null.
bool gs_window_open(const char *icon_path);
void gs_window_close(void);
bool gs_window_is_open(void);

// Pump the window's events. False once the window has been closed by its
// user, which is the operator saying stop.
bool gs_window_pump(void);

// Draw one frame from the facts and the log, and report what was pressed.
// With `dump` set, every piece of text handed to the window this frame is
// also kept, for gs_window_dump_print - the test's way of reading a window.
void gs_window_draw(const gs_srv_facts *f, const gs_srv_log *log,
                    gs_srv_ask *ask, bool dump);

// Print what the last dumped frame showed, one line per line of the window,
// each prefixed "window: ".
void gs_window_dump_print(void);

// For tests: press the button with this label the first time it is drawn,
// through Dear ImGui's own item hooks, so the button's real path runs. Up to
// four, each once.
void gs_window_press(const char *label);

// Write the window's last frame as a BMP - the way the game's --shot writes
// a frame - so what the window looks like is a file somebody can open.
bool gs_window_shot(const char *path);

#endif // GS_SERVER_WINDOW_H

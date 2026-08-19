// gs_menu.h - the front end: everything before and after a race.
//
// **A full session without touching a command line** is the whole requirement,
// and it is a bigger one than it sounds. It means a title screen, somewhere to
// say who is playing, a way to choose the race and the machines, and somewhere
// afterwards that says what happened - and it means all of that being reachable
// with a pointer from a cold start, on a machine that has never had a terminal
// open on it.
//
// The screens are drawn with the same immediate-mode UI as the construction
// set, for the same reason: what is on the screen is a function of the state
// rather than a tree of widgets somebody has to keep in step with it.
//
// This owns no simulation and no rendering. It decides *what race to run* and
// then reports what the race did, which is exactly the boundary the database
// will sit on when there is one: upstream of a race, and downstream of it,
// never inside it.
#ifndef GS_MENU_H
#define GS_MENU_H

#include "core/gs_library.h"
#include "core/gs_profile.h"
#include "core/gs_records.h"
#include "core/gs_sim.h"
#include "net/gs_proto.h"
#include "core/gs_track.h"

typedef enum gs_screen {
    GS_SCREEN_TITLE = 0,
    GS_SCREEN_PROFILES,
    GS_SCREEN_SETUP,
    GS_SCREEN_RACE,
    GS_SCREEN_RESULTS,
    GS_SCREEN_RECORDS,
    GS_SCREEN_LOBBY,
    GS_SCREEN_TRACKS,
    GS_SCREEN_COUNT
} gs_screen;

// What the setup screen has been told to do. Handed to the frontend, which
// builds the world from it - so the menu never touches gs_world itself.
typedef struct gs_race_setup {
    uint8_t  players;               // 1 to GS_MAX_CARS
    int8_t   profile[GS_MAX_CARS];  // index into the roster, or -1 for a guest
    uint8_t  vehicle[GS_MAX_CARS];
    uint8_t  colour[GS_MAX_CARS];

    uint8_t  mode;                  // gs_mode
    uint16_t laps;
    gs_fix   gravity;
    int      gravity_preset;        // which button is lit, or -1
} gs_race_setup;

// What a finished race did, in the order it finished. Built once when the race
// ends, so the results screen is reading a fact rather than re-deriving one.
typedef struct gs_result_row {
    uint8_t  car;
    uint8_t  place;                 // 1 is first
    uint32_t finish_tick;           // 0 if they never finished
    uint32_t best_lap;
    uint16_t laps;
    uint8_t  damage;
    bool     wrecked;
    bool     beat_lap;              // set a new lap record here
    bool     beat_race;
} gs_result_row;

typedef struct gs_menu {
    gs_screen screen;

    gs_profiles profiles;
    gs_records  records;

    // The tracks you have. Half a megabyte, which is why gs_menu is a static
    // in the frontend rather than a local anywhere.
    gs_library  library;
    int         chosen;             // which track a race would be on, or -1
    bool        store_dirty;        // something changed and wants writing

    gs_race_setup setup;

    gs_result_row result[GS_MAX_CARS];
    uint8_t       result_count;

    // The profile-editing screen's scratch fields.
    char    new_name[GS_PROFILE_NAME];
    uint8_t new_colour;
    uint8_t new_vehicle;
    int     editing;                // profile being edited, or -1
    int     picking_for;            // which player slot the roster is open for

    char status[160];

    // Waiting at a server. The menu owns none of the networking - it is handed
    // what the wire last heard and draws it, so a lobby screen cannot be a
    // reason the connection behaves differently.
    const gs_lobby *lobby;
    const char     *lobby_error;    // a refusal, in words meant for a person
    uint8_t         lobby_slot;
    bool            lobby_ready;
    float           track_progress;   // 1.0 when there is nothing to wait for
    char            server_text[80];

    // The library screen's scratch: what was picked, and what it is being
    // renamed to.
    int  picked;
    int  take;                      // handed to the frontend, then cleared
    char track_name[GS_LIBRARY_NAME];
    int  name_for;                  // which entry track_name is showing
} gs_menu;

void gs_menu_init(gs_menu *m);

// Draw the current screen. Returns the screen to be on next frame, which is the
// same one unless something was clicked. `t` is the track a race would be on,
// for showing its records and its name.
gs_screen gs_menu_frame(gs_menu *m, const gs_track *t);

// Which track the library screen wants raced next, or -1. Cleared by reading
// it, so a choice is acted on once rather than every frame.
int gs_menu_take_choice(gs_menu *m);

// Work out the finishing order and submit anything worth submitting. Called
// once, when a race ends.
void gs_menu_finish(gs_menu *m, const gs_world *w, const gs_track *t);

// A time as people say them: 1:02.35, or "-" for a car that never finished.
void gs_time_text(char *out, size_t cap, uint32_t ticks);

// Load and save. The frontend owns the disk, so these take bytes rather than
// paths - and both of them are how "remembered between executions" happens.
size_t gs_menu_save(const gs_menu *m, uint8_t *buf, size_t cap);
bool   gs_menu_load(gs_menu *m, const uint8_t *buf, size_t len);

#endif // GS_MENU_H

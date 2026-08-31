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

#include "core/gs_ai.h"
#include "core/gs_library.h"
#include "core/gs_share.h"
#include "core/gs_profile.h"
#include "core/gs_records.h"
#include "core/gs_sim.h"
#include "net/gs_proto.h"
#include "core/gs_track.h"

typedef enum gs_screen {
    // **First, because it is the door.** Nothing else in the front end is
    // reachable until somebody has signed in, so this is both the screen the
    // game starts on and the value a zeroed gs_menu already holds.
    GS_SCREEN_LOGIN = 0,
    GS_SCREEN_TITLE,
    GS_SCREEN_PROFILES,
    GS_SCREEN_SETUP,
    GS_SCREEN_RACE,
    GS_SCREEN_RESULTS,
    GS_SCREEN_RECORDS,
    GS_SCREEN_LOBBY,
    GS_SCREEN_TRACKS,
    GS_SCREEN_COUNT
} gs_screen;

// **What a screen is called, in one place.** The client's trace prints these
// and `tools/play_check.py` makes assertions out of them, and the front-end
// walk names screens with them when it reports a trap or a stranding - so a
// second list somewhere else is a second list that drifts. The switch behind
// it has no `default`, which means adding a screen and forgetting to name it
// is a build failure rather than a "?" nobody notices.
const char *gs_screen_name(gs_screen s);

// What the setup screen has been told to do. Handed to the frontend, which
// builds the world from it - so the menu never touches gs_world itself.
// What a screen's panel came out as, measured as it was drawn.
typedef struct gs_panel_report {
    float x, y, w, h;   // the panel on screen, in pixels
    float view_w, view_h;   // and the window it had to fit inside
    float hidden;       // content below the fold: how far it can be scrolled

    // **And content past the right-hand edge**, which is the same fault turned
    // ninety degrees and is not visible in any of the numbers above it. A panel
    // whose contents are wider than it is draws the last thing on a row cut in
    // half - the word ends in the middle and there is nothing to click on the
    // other side of it. Nothing in the item hooks can see this: what they are
    // handed has already been clipped, so a button hanging off the edge arrives
    // looking like a narrower button that fits.
    float wider;
} gs_panel_report;

typedef struct gs_race_setup {
    uint8_t  players;               // 1 to GS_MAX_CARS
    int8_t   profile[GS_MAX_CARS];  // index into the roster, or -1 for a guest
    uint8_t  vehicle[GS_MAX_CARS];
    uint8_t  colour[GS_MAX_CARS];

    // **Whose car is this, if it is nobody's.** A race set up for four with one
    // person at the keyboard used to be three cars nobody was driving, sitting
    // on the grid while the fourth went round on its own. A slot marked here is
    // driven by the game, at the skill below.
    bool     computer[GS_MAX_CARS];

    // How hard they push, from a driver who brakes far too early to one who is
    // quicker than you are. A point on `gs_ai_skill_margin`'s dial rather than
    // a name.
    uint8_t  skill;

    uint8_t  mode;                  // gs_mode
    uint16_t laps;

    // **Whether anybody is armed, and with what.** Off is the default and is
    // what every race was before there were weapons; on, everybody gets the
    // same of each, which is what `weapons` and the counts beside it are.
    // Kept as one switch and four counts rather than as four switches, because
    // "no weapons this time" is a thing somebody says out loud before a race
    // and should be one thing to press.
    bool     weapons;
    uint8_t  ammo[GS_HAZ_COUNT];    // indexed by gs_hazard_kind
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

    // --- who is signed in ---------------------------------------------------
    //
    // **An index into the roster, or -1 for nobody, and while it is -1 the only
    // screen that draws is the login one.** That is enforced in one place, at
    // the top of gs_menu_frame, rather than by every screen remembering to
    // check - a gate each caller has to opt into is a gate somebody forgets.
    int  signed_in;

    // The login screen's scratch. The typed password lives here for as long as
    // it takes to check it and is wiped immediately afterwards, because a
    // password sitting in a struct that gets memcpy'd around is a password in
    // more places than anybody intended.
    int  login_pick;                // which driver is highlighted, or -1
    char login_password[64];
    char login_confirm[64];
    char login_code[8];             // the six digits, when a second factor is set
    char login_error[112];
    char login_name[GS_PROFILE_NAME];
    bool login_making;              // the "new driver" fields are showing
    bool login_wants_code;          // the password was right and a code is due
    bool login_setting;             // an older driver is choosing a password
    bool focus_form;                // put the caret in the first box next frame

    // **Handed to the frontend exactly once, to prove the same name at a
    // server.** The server checks a password rather than a hash, so this is
    // the typed text and not what is stored - which is why it is taken and
    // wiped in one movement by gs_menu_take_server_login rather than read.
    // Only set when a server is configured; offline this stays empty.
    char     server_password[64];
    uint32_t server_code;
    bool     server_login_pending;

    // **Set by the frontend when this game was pointed at other people.**
    // Signing in then leads to the lobby rather than the title, because the
    // track, the roster and the moment the race starts are all the server's to
    // decide - offering a local setup screen would be offering a choice that
    // is not there.
    bool online;

    // **Set when somebody in the lobby asks to race.** A lobby that is already
    // full starts on its own, which is right the first time and wrong every
    // time after: leaving a race goes to the lobby, and a full lobby would put
    // the player straight back into the race they had just left. Once they have
    // left one under their own steam the lobby waits, and this is how it hears
    // that they are ready to go again.
    bool race_requested;

    // Whether the library screen was opened to choose a race or to tidy up.
    // The same screen either way - the difference is what the big button says
    // and where Back goes, which is the whole difference between "pick one" and
    // "look after these".
    bool tracks_for_race;

    // **Where Back goes from the records table.** It went to the title from
    // wherever it was opened, so looking up a time from the results screen
    // threw away the results. Records is a thing you glance at and come back
    // from, which means it has to know where "back" is.
    gs_screen records_from;

    // **Where Back goes from the race setup, and from the tracks list.**
    //
    // The same fault the records table had, in two more places, and the worse
    // one of them costs a race. Escape out of a race lands on the setup screen,
    // where Back went to the main menu - so the only way off a paused race was
    // to abandon it, and the race was still sitting in memory, unreachable.
    // Opening the tracks list from setup and pressing Back did the same to the
    // grid somebody had just filled in.
    //
    // A screen reachable from two places has to know which one it came from.
    // These are set by the frontend as it makes the move, so every path in is
    // covered by construction rather than by remembering to set it at each one.



    gs_screen setup_from;
    gs_screen tracks_from;

    // **The race is paused rather than over, and Back returns to it.**
    //
    // Every arrival at the race screen starts a *new* race, which is right for
    // GO and wrong for coming back to one already running. This says which of
    // the two is being asked for; the frontend clears it as it acts on it.
    bool resume;

    // **What the tracks screen is asking the frontend to do.** The menu owns
    // none of this: it cannot open the construction set, it cannot talk to a
    // server, and it should not learn how - so it raises a request and the
    // frontend acts on it, the same way the race setup is handed over rather
    // than acted on here.
    //
    // Getting to the editor used to be Tab from anywhere, which is not a thing
    // anybody finds and not a thing the tracks screen mentioned.
    bool edit_requested;      // open the picked track in the construction set
    bool new_requested;       // start a blank one there
    bool publish_requested;   // hand the picked track to everybody on the server
    bool withdraw_requested;  // stop listing it
    int  share_with;          // a lobby slot to hand it to, or -1
    bool share_on;            // hand it over, or take it back

    // **Exit, which only the frontend can actually do.** The menu cannot end
    // the loop - it does not own it - so it raises this and the frontend reads
    // it, the same way the race setup is handed over rather than acted on here.
    bool quit;

    // Waiting at a server. The menu owns none of the networking - it is handed
    // what the wire last heard and draws it, so a lobby screen cannot be a
    // reason the connection behaves differently.
    const gs_lobby *lobby;
    const char     *lobby_error;    // a refusal, in words meant for a person
    uint8_t         lobby_slot;
    bool            lobby_ready;
    float           track_progress;   // 1.0 when there is nothing to wait for

    // **How long this machine has been knocking with no answer.** A server that
    // refuses says so and that arrives as `lobby_error`; a server that cannot
    // *hear* us says nothing at all, because a wrong key means it cannot
    // decrypt what we sent and has nothing to reply to. Those two look
    // identical on screen - a quiet lobby - and the second one never resolves.
    float           knocking_for;
    char            server_text[80];

    // The library screen's scratch: what was picked, and what it is being
    // renamed to.
    int  picked;
    int  take;                      // handed to the frontend, then cleared
    char track_name[GS_LIBRARY_NAME];
    int  name_for;                  // which entry track_name is showing

    // The picked track as text somebody can paste. Held rather than made every
    // frame: a code is a few hundred characters of base64 over the whole track,
    // which is not work to repeat sixty times a second for a field nobody is
    // looking at.
    char track_code[GS_SHARE_MAX];
    int  code_for;                  // which entry track_code is showing

    // **Where the panel drawn last frame ended up, and how much of it was out
    // of sight.** Written by every screen on its way out. A panel taller than
    // the window puts its own first row above the top edge, where a window that
    // cannot be moved or resized leaves it unreachable - and that is not
    // something a screenshot test notices, because the screenshot looks fine.
    // It is something a number can say.
    gs_panel_report panel;

    // **Bookkeeping about the drawing, kept past `panel` on purpose.**
    //
    // Everything above is hashed to tell one state of the front end from
    // another while it is walked, and the hash stops here. These two belong
    // below that line: they are notes about the frame just drawn rather than
    // anything a person chose, and putting them above it multiplied the states
    // the walk had to explore until it stopped finishing.
    //
    // `focused` is which screen's panel has been given the focus - a panel that
    // has just appeared has to take it, or the first click on it is spent
    // giving it the focus instead of pressing what it landed on, which is what
    // a menu opened over a race looked like from a chair. `panel_focused` is
    // whether the panel drawn last frame was the one taking input, which is how
    // the test for that reads the answer.
    // **Set by the frontend when a menu opens over a race**, and cleared by the
    // menu as it takes the focus. Narrow on purpose: this began as "focus the
    // panel whenever the screen changes", which fires on every rebuild of the
    // menu - including the one the front-end walk does for each of its passes -
    // and took the focus off the dropdown it had just opened. The walk went
    // from 810 controls to 289. The fault reported was a dialog opened over a
    // race, so that is the only thing that asks for the focus.
    bool      take_focus;
    bool      panel_focused;

} gs_menu;

void gs_menu_init(gs_menu *m);

// Draw the current screen. Returns the screen to be on next frame, which is the
// same one unless something was clicked. `t` is the track a race would be on,
// for showing its records and its name.
// **What state this menu is in, as one number.**
//
// A walk that presses on from where it got to has to know when it has arrived
// somewhere it has already been, or it walks forever. A menu is a plain value,
// so this is a hash of it - but not of all of it, and what is left out is the
// point:
//
//  - **`lobby` is a borrowed view of the frontend's state, not the menu's.**
//    Two menus pointed at the same lobby are in the same state; a different
//    lobby is a different thing to seed a walk with, not something a press can
//    discover.
//  - **`lobby_error` is hashed by its message, not by its pointer.** The text
//    is what a player sees and is therefore state; where it happens to be
//    stored is not, and hashing the pointer would make the same message read as
//    two different states.
//  - **`track_progress` and `knocking_for` advance with the clock**, not with
//    anything anybody pressed. Hashing them would make every frame a state
//    nobody had been in before, which is the same as having no hash at all.
//  - **`panel` is a measurement of the last frame drawn**, an output rather
//    than an input, and it moves when the window is resized.
//
// Everything else is in, by byte and by construction rather than field by
// field, so a field added to this struct is hashed the day it is added.
uint64_t gs_menu_hash(const gs_menu *m);

gs_screen gs_menu_frame(gs_menu *m, const gs_track *t);

// **Where "back" goes from here, in one callable place.** Escape is the way out
// of everything, and what it means depends on where you are and whether this
// machine is racing other people: an online race backs out to the lobby, where
// there is a Play to press again, and never to a setup screen that decides a
// race the server owns. Returning GS_SCREEN_COUNT means there is nothing left
// to back out to, and the caller should quit.
//
// A rule in a key handler is a rule no test can reach, and every fault found by
// playing this so far has been in code exactly that shape.
// How long to knock before saying nobody is answering, in seconds. Long enough
// that a slow handshake is not called a failure, short enough that somebody is
// not left reading "Knocking..." wondering whether it is them.
#define GS_KNOCK_PATIENCE 6.0f

gs_screen gs_menu_back(const gs_menu *m, bool editing);

// **Whether the setup screen has a paused race behind it**, which decides what
// its buttons mean and where Escape goes. See gs_menu_setup_is_paused.
bool gs_menu_setup_is_paused(const gs_menu *m);

// **Can the lobby start a race right now?** A predicate rather than a condition
// buried in the drawing, because the first version of it was buried there and
// was wrong in a way nothing could see: it compared a count against a capacity,
// and before the server has answered both are zero - so it offered Race to
// somebody still knocking on the door, and did nothing when they pressed it.
bool gs_menu_lobby_can_race(const gs_menu *m);

// **Has the door gone unanswered long enough to say so?** A server that refuses
// sends a reason; a server that cannot decrypt what we sent has nothing to
// reply to, so the screen sat on "Knocking..." indefinitely and a wrong key
// looked exactly like a slow connection. A predicate, for the same reason
// gs_menu_lobby_can_race is one: it is a thing a test can call.
bool gs_menu_lobby_unanswered(const gs_menu *m);

// **The gate's whole rule, in one callable place.** Sign `index` in if the
// password and code are what that driver requires. Returns false and leaves
// `login_error` saying why otherwise.
//
// **Every driver has a password.** A driver carrying none cannot be signed in
// at all - not because the empty string fails the check, but because there is
// nothing to check against, and letting that through would make the door a
// picture of a door. Rosters written before passwords existed do contain such
// drivers, so the way through is to give them one: see gs_menu_set_password.
//
// Public so it can be tested without standing up an ImGui context: a rule that
// can only be exercised by clicking is a rule nothing checks.
bool gs_menu_sign_in(gs_menu *m, int index, const char *password,
                     const char *code);

// Sign in by name, the way somebody at the keyboard does it. **A name that is
// not on the roster and a name with the wrong password fail identically** - the
// screen never lists who is on this machine, and it must not answer the same
// question one guess at a time either.
bool gs_menu_sign_in_named(gs_menu *m, const char *name, const char *password,
                           const char *code);

// Put a password on a driver. `again` must match, and neither may be empty.
// This is how a driver from an older roster gets one, and how somebody changes
// theirs. False leaves `login_error` saying what was wrong.
bool gs_menu_set_password(gs_menu *m, int index, const char *password,
                          const char *again);

// Who is signed in, or "" when nobody is. The frontend needs the name to say
// it at a server, and everything else needs it to put on a result.
const char *gs_menu_driver(const gs_menu *m);

// Take the credentials for a server login, if one is waiting, and wipe them on
// the way out. Returns false when there is nothing to send. The password goes
// to the wire as itself because that is what the server verifies against its
// own stored hash - see docs/THREATS.md on what the tunnel is doing about that.
bool gs_menu_take_server_login(gs_menu *m, char *password, size_t cap,
                               uint32_t *code);

// Which track the library screen wants raced next, or -1. Cleared by reading
// it, so a choice is acted on once rather than every frame.
int gs_menu_take_choice(gs_menu *m);

// **Build the race the setup screen describes.** The dials, the machines and
// the grid - written here rather than in the client so that "what the setup
// screen means" is a rule with a test over it rather than a paragraph in a
// frontend nothing can reach. The paint is not in it: a colour cannot change
// where a car ends up and must not be able to, so it goes to the renderer.
void gs_setup_build(const gs_race_setup *s, const gs_track *t, gs_world *w);

// **What the cars nobody is driving would press, this tick.** Fills in only the
// slots marked as the game's; the rest are left exactly as they came in, which
// is whatever the pads and the keyboard said.
void gs_setup_drive(const gs_race_setup *s, const gs_world *w,
                    const gs_track *t, gs_input *in);

// Work out the finishing order and submit anything worth submitting. Called
// once, when a race ends.
// **The race is over: build the table and show it.** Places, records, the
// driver's tally - and the move to the results screen, which is part of
// finishing rather than something the caller remembers to do afterwards.
void gs_menu_finish(gs_menu *m, const gs_world *w, const gs_track *t);

// A time as people say them: 1:02.35, or "-" for a car that never finished.
void gs_time_text(char *out, size_t cap, uint32_t ticks);

// Load and save. The frontend owns the disk, so these take bytes rather than
// paths - and both of them are how "remembered between executions" happens.
// **How many bytes a store of this menu takes**, so that a caller can hold
// exactly that many and never the wrong number. The frontend used to guess -
// the roster, the records and four kilobytes of slack - and a library of
// twenty-two tracks is nearer ninety, so `gs_menu_save` refused every time and
// nothing anybody did was ever saved. A number the same code works out is not a
// guess that can go stale.
size_t gs_menu_size(const gs_menu *m);

size_t gs_menu_save(const gs_menu *m, uint8_t *buf, size_t cap);
bool   gs_menu_load(gs_menu *m, const uint8_t *buf, size_t len);

#endif // GS_MENU_H

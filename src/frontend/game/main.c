// main.c - the window, the frame loop and the split screen.
//
// **The simulation is deterministic; the presentation is not.** The world
// advances in fixed 120 Hz steps driven by an accumulator, and frames are drawn
// whenever the machine can manage, interpolating between the last two states.
// That split is why the same input log gives the same race at 30 fps and at
// 240, and it is not negotiable: physics that runs per frame is a desync with
// extra steps.
//
// Note the SDL3 error convention - most calls return bool, true on success,
// which is the opposite of SDL2's "0 means OK".
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "core/gs_clock.h"
#include "core/gs_sim.h"
#include "core/gs_track.h"
#include "gfx/gs_render.h"
#include "platform/gs_input.h"
#include <SDL3_image/SDL_image.h>

#include "platform/gs_paths.h"
#include "audio/gs_audio.h"
#include "audio/gs_music.h"
#include "platform/gs_winmem.h"
#include "platform/gs_wire.h"
#include "core/gs_net.h"
#include "ui/gs_editor.h"
#include "ui/gs_hud.h"
#include "ui/gs_menu.h"
#include "ui/gs_style.h"
#include "core/gs_ai.h"
#include "core/gs_analyse.h"
#include "core/gs_ghost.h"
#include "core/gs_records.h"

#include "dcimgui.h"
#include "backends/dcimgui_impl_sdl3.h"
#include "backends/dcimgui_impl_sdlrenderer3.h"

#define GS_WINDOW_W 1280
#define GS_WINDOW_H 720

// How long this machine waits for a silent one before calling the race over.
// Longer than the server's own fifteen-second patience, so a client that is
// merely having a bad moment gets dropped by the server first and this only
// fires for somebody who is genuinely gone.
#define GS_GIVE_UP_SECONDS 20

typedef struct gs_app {
    SDL_Window   *win;
    SDL_Renderer *ren;

    gs_track t;
    gs_world world;      // the state as of the last completed tick
    gs_world prev;       // the one before it, for interpolation
    gs_view  view[GS_MAX_CARS];
    uint8_t  views;

    gs_input_state input;
    gs_editor      editor;
    gs_split       split;

    // The run in progress, and the last one finished. Restarting hands the
    // recording to the ghost, so racing yourself costs one keypress and no
    // files at all - which is the version of the feature people actually use.
    gs_replay recording;
    gs_ghost  ghost;
    bool      show_ghost;

    // Whose ghost it is. Your own is replaced by every run you finish, which is
    // the point of it. Somebody else's is the thing you are trying to beat, and
    // replacing that with your own first attempt would be absurd.
    bool      ghost_borrowed;

    uint64_t last_ns;
    gs_clock clock;

    // --shot: draw this tick, write it out, and exit. This is how a change to
    // the renderer gets looked at rather than described, and how CI proves the
    // game can still find its own feet with no video driver at all.
    const char *shot_path;
    uint64_t    shot_at;
    bool        overlay;      // start with the painted-gravity overlay on
    bool        arc;          // start with the landing arc on
    bool        start_in_editor;

    // **Are we in the middle of building something?** Set when the tracks
    // screen opened the construction set, cleared on the way back to a menu.
    //
    // Tab used to open the editor from anywhere, which is how somebody was
    // expected to find it - a key nothing mentioned, on screens that never said
    // it existed. New and Edit on the tracks screen are how you get in now. But
    // Tab is also the build-drive-build loop, which is the single biggest thing
    // the original could not do, so it keeps working *inside* a session: drive
    // what you just built, Tab back, change it. It simply is not the front
    // door any more.
    bool        edit_session;
    float       zoom;         // 0 means the default
    uint8_t     players;      // 0 means the default of two
    bool        diverge;      // in shot mode, steer the cars apart and back
    bool        analyse_at_start;
    bool        showroom;    // every vehicle lined up, for looking at the art
    const char *audio_out;   // with --shot: write what the race sounded like
    uint8_t     showroom_from;
    const char *ghost_path;
    const char *ghost_out;   // with --shot: write the run that was captured

    // Online. The rollback session knows nothing about sockets and the wire
    // knows nothing about racing. Neither exists unless somebody asked for a
    // network race.
    gs_wire    *wire;
    gs_net      net;
    bool        online;
    const char *join_host;
    const char *server_host;      // meeting at a server instead

    // **Which server, exactly.** IK means the client already knows the
    // server's public key, and that is what stops somebody in the middle
    // answering in its place - so it is given rather than discovered, and a
    // client without one does not connect.
    uint8_t     server_key[GS_NOISE_KEY_BYTES];
    bool        has_server_key;
    const char *online_name;      // who to appear as
    bool        use_relay;        // go through the server, for awkward routers
    uint16_t    port;
    uint8_t     online_players;   // how many the host is waiting for
    bool        net_started;      // everybody is here and the race has begun

    // **Set when a player deliberately leaves an online race.** Escape out of a
    // race goes to the lobby, and the lobby starts a race the moment it is
    // ready - so on a lobby that is already full, leaving put the player
    // straight back into the race they had just left. Escape appeared to
    // restart the race instead of backing out of it, and there was no way to
    // the menu at all. The lobby waits for the player to ask, once they have
    // left one race under their own steam.
    bool        lobby_hold;
    uint64_t    music_hash;       // the track the current tune was written for

    // The front end. A session starts here, comes back here after every race,
    // and never needs a command line - which is the whole requirement.
    gs_menu  menu;
    bool     skip_menu;           // --shot and the like drop straight into a race
    bool     race_settled;        // the results have been worked out once
    int      shot_screen;         // with --screen: which one to show
    bool     want_screen;
    bool     session;             // run setup -> race -> results by itself
    bool     keep;                // and write what it did, which a session
                                  // otherwise refuses to - see gs_store_save
    const char *track_path;       // a track named on the command line
    bool     trace;               // say what is on screen, once a second
    bool     autodrive;           // the AI drives this machine's car

    // **What this machine's driver last asked for**, kept only so the trace can
    // say it. A report that the controls did not work is unanswerable without
    // it: a keyboard that cannot send two keys at once and a car that will not
    // turn at the speed it is doing feel the same from the driver's seat.
    gs_input last_input;
    // **When the last trace line went out, by the wall clock.**
    //
    // This was the world's tick, which does not advance on a menu - so the
    // front end printed one line when it arrived on a screen and then went
    // silent for as long as it stayed there. A report of "it got to this screen
    // and locked up" is then unanswerable from the log: a front end that has
    // stopped and a front end that is fine look identical, because neither says
    // anything. The wall clock advances wherever the game is.
    uint64_t traced_at;           // SDL_GetTicks() of the last trace line

    // **Frames drawn since the last trace line.** Reported as a rate, because
    // "nothing responds" and "responds once every two seconds" look identical
    // from a chair and are different faults - and the front end draws the live
    // world behind its menus, so the front end has a frame rate worth asking
    // about at all.
    uint32_t framed;
    bool     demo_library;        // a few tracks, for looking at the screen
    // **When this machine started waiting for somebody, and for how long it
    // will.** A stall is the rollback saying the other machine has gone quiet,
    // which is a thing that ends when a datagram arrives - and if it never
    // arrives, a race that waits silently forever is indistinguishable from a
    // game that has crashed. It is not a hypothetical: a player pressed Play
    // into a race whose other seat had been left by somebody who had gone, and
    // sat looking at a frozen screen with no idea why.
    uint32_t    waiting;        // stalled frames, for telling the player
    uint64_t    stalled_since;  // when it started, or 0 while the race is live

    // **A networked race is recorded from what everybody agreed, not from what
    // this machine is looking at.** The visible world is built partly on
    // guesses and most of them get rolled back; the confirmed world is built
    // only from inputs every peer actually sent. `net_recorded` is how far the
    // recording has been filled from it.
    uint32_t    net_recorded;

    // The visible race ends a dozen ticks before the agreed one does, because
    // the reveals run behind the commitments. Submitting at that moment would
    // hand in a recording missing its ending, so the client keeps talking until
    // the agreed race is over too.
    bool        net_settling;
    uint32_t    net_settle_frames;
    bool        quit;

    // **What the window was asked to open at, and what the window manager
    // heard.** Some managers park the frame where the client asked to be
    // born, so the position read back is a constant decoration's offset from
    // the one requested - and a memory saved as read then re-requested every
    // launch walks the window across the desk one launch at a time. The
    // offset is measured once, on the first frame, as asked-minus-read; the
    // save subtracts it. A drag during the session survives, because the
    // offset is the manager's constant and not the position.
    bool win_asked;                  // a remembered position went into creation
    bool win_measured;
    int  win_ask_x, win_ask_y;
    int  win_off_x, win_off_y;
    // **What the game ships, by hash, as of this start.** Filled by walking
    // assets/tracks/ and used twice: to withdraw the shipped tracks an older
    // version left in somebody's library, and to put the current ones in.
    uint64_t    stock[GS_LIBRARY_MAX];
    uint16_t    stock_count;
    bool        stock_listing;    // walking to list them, not to add them
} gs_app;

// The demo track, until the editor exists to make a real one. It is a
// prototype and it is named as one: a hard-coded track is not content.
// The stock tracks, loaded from assets/tracks/ rather than compiled in.
//
// **A track in C is a track nobody can edit, share or replace.** This one was a
// prototype from the day it was written, and it is gone: what ships is data,
// produced by tools/make_tracks.c, which the editor opens like anything else.

static SDL_EnumerationResult SDLCALL gs_take_track(void *userdata, const char *dir,
                                                   const char *name) {
    gs_app *a = (gs_app *)userdata;

    size_t n = SDL_strlen(name);
    if (n < 9 || SDL_strcmp(name + n - 8, ".gstrack") != 0) {
        return SDL_ENUM_CONTINUE;
    }

    char path[1024];
    SDL_snprintf(path, sizeof path, "%s%s", dir, name);

    size_t len = 0;
    void *bytes = SDL_LoadFile(path, &len);
    if (bytes == nullptr) return SDL_ENUM_CONTINUE;

    static gs_track loaded;
    if (gs_track_deserialize(&loaded, (const uint8_t *)bytes, len)) {
        // **Listing, not adding.** The walk happens twice: once to learn what
        // the game ships now, so anything it no longer ships can go, and once
        // to put what it does ship in. Doing it the other way round means the
        // withdrawn tracks are still occupying slots when the new ones arrive,
        // and a full library refuses them.
        if (a->stock_listing) {
            if (a->stock_count < GS_LIBRARY_MAX) {
                a->stock[a->stock_count++] = gs_track_hash(&loaded);
            } else {
                // More tracks ship than the library can hold. Said out loud,
                // because the ones past the end would be read as tracks the
                // game no longer ships and withdrawn on sight.
                SDL_Log("library: more than %u tracks ship - %s and any after "
                        "it cannot be offered", (unsigned)GS_LIBRARY_MAX, name);
            }
            SDL_free(bytes);
            return SDL_ENUM_CONTINUE;
        }

        // The file name is the track's name, tidied: "the-long-drop.gstrack"
        // reads as "the long drop".
        char label[GS_LIBRARY_NAME];
        SDL_strlcpy(label, name, sizeof label);
        char *dot = SDL_strrchr(label, '.');
        if (dot != nullptr) *dot = '\0';
        for (char *c = label; *c != '\0'; c++) {
            if (*c == '-' || *c == '_') *c = ' ';
        }
        // **Marked as the game's rather than the player's.** These are not
        // theirs to change or throw away: editing one takes a copy and edits
        // that, and deleting one is refused, so the library a player came with
        // is still there after an afternoon of building.
        if (gs_library_put_builtin(&a->menu.library, &loaded, label,
                                   "gearstick") < 0) {
            // A track that ships and is not offered is worth a line. This was
            // silent, and a library full of tracks an older version shipped is
            // exactly how it filled up.
            SDL_Log("library: no room for %s - %u of %u slots are taken", label,
                    (unsigned)a->menu.library.count, (unsigned)GS_LIBRARY_MAX);
        }
    }
    SDL_free(bytes);
    return SDL_ENUM_CONTINUE;
}

// Everything in assets/tracks/, into the library. Run on every start rather
// than only the first: a track that ships should come back if somebody deletes
// it, and putting the same track twice puts it once.
static void gs_load_stock_tracks(gs_app *a) {
    char dir[1024];
    const char *assets = gs_assets_dir();
    if (assets == nullptr) return;

    SDL_snprintf(dir, sizeof dir, "%s/tracks/", assets);
    SDL_EnumerateDirectory(dir, gs_take_track, a);
}

// **The library, reconciled with what the game ships now.**
//
// Run once on start and again after the store is read, because reading the
// store *replaces* the library - `gs_library_deserialize` clears it first, and
// has to, since a saved library is the whole of what somebody has. So the stock
// tracks loaded before it were being thrown away on every start by everybody
// who had ever played before: the comment above `gs_load_stock_tracks` promised
// a shipped track would come back if it was deleted, and for a returning player
// it never did. Nothing the game shipped after somebody's first run had any way
// of reaching them, which is how a library of two-gate routes outlived the
// generator that made them by six days.
//
// Withdrawing first and adding second, because the two have to happen in that
// order for a library near its limit - see gs_take_track.
static void gs_sync_stock_tracks(gs_app *a) {
    a->stock_count = 0;
    a->stock_listing = true;
    gs_load_stock_tracks(a);
    a->stock_listing = false;

    // Nothing found is a broken install, not a library the game has emptied.
    if (a->stock_count == 0) {
        SDL_Log("library: no tracks in %s/tracks/ - keeping what is here",
                gs_assets_dir());
        return;
    }

    uint16_t gone = gs_library_retire_builtins(&a->menu.library, a->stock,
                                               a->stock_count);
    uint16_t was = a->menu.library.count;
    gs_load_stock_tracks(a);

    if (gone > 0 || a->menu.library.count != was) {
        SDL_Log("library: %u shipped track(s), %u withdrawn, %u here now",
                (unsigned)a->stock_count, (unsigned)gone,
                (unsigned)a->menu.library.count);
        // Written down, so the next start does not do it again - and so the
        // library on disk is the one the player is looking at.
        a->menu.store_dirty = true;
    }
}

static void gs_layout(gs_app *a);static void gs_layout(gs_app *a);

// Drop into the world from wherever the editor is looking. **Nothing is
// loaded**: the track object the editor has been writing to is the track the
// car now drives on, so the loop between changing something and feeling it is
// a keypress rather than a save, a load and a restart. That loop is the single
// biggest thing thirty years of hardware buys this game.
static void gs_start_test_drive(gs_app *a) {
    gs_fix x = GS_INT(3), y = GS_INT(9);
    gs_angle heading = 0;
    gs_editor_drive_start(&a->editor, &a->t, &x, &y, &heading);

    // Under whatever the dials are set to. Tuning gravity and then driving
    // under the old one would make the panel a lie.
    gs_editor_apply_dials(&a->editor, &a->world);
    gs_world_add_car(&a->world, &a->t, (uint8_t)GS_VEH_STOCK_CAR, x, y, heading);

    // The second car alongside, offset across the direction of travel so both
    // start level rather than one behind the other.
    gs_fix ox = -gs_fix_mul(gs_sin(heading), GS_INT(2));
    gs_fix oy = gs_fix_mul(gs_cos(heading), GS_INT(2));
    gs_world_add_car(&a->world, &a->t, (uint8_t)GS_VEH_DUNE_BUGGY,
                     x + ox, y + oy, heading);

    a->prev = a->world;
    a->views = a->showroom ? 1 : a->world.car_count;
    for (uint8_t i = 0; i < a->views; i++) {
        a->view[i].car = i;
        a->view[i].cam.zoom = a->zoom > 0.0f ? a->zoom : GS_ISO_DEFAULT_ZOOM;
        gs_render_track_camera(&a->view[i], &a->t, &a->prev, &a->world, 1.0f);
    }
    gs_layout(a);
}

static void gs_start_race(gs_app *a) {
    static const uint8_t grid[GS_MAX_CARS] = {
        (uint8_t)GS_VEH_STOCK_CAR, (uint8_t)GS_VEH_DUNE_BUGGY,
        (uint8_t)GS_VEH_SPRINT_CAR, (uint8_t)GS_VEH_BAJA_BUG,
    };
    uint8_t players = a->players > 0 ? a->players : 2;

    gs_world_init(&a->world, GS_ONE);

    if (a->showroom) {
        // Every vehicle at once, parked, so a change to tools/make_meshes.py can
        // be looked at rather than described. Six of them and four player
        // colours, so two share - the paint is not what is being inspected.
        // Close together on purpose: the split screen merges when cars are
        // near each other, so parking them a tile apart gives one picture of
        // the whole line-up rather than four pictures of parts of it.
        for (uint8_t v = 0; v < GS_VEH_COUNT && v < GS_MAX_CARS; v++) {
            uint8_t which = (uint8_t)((v + a->showroom_from) % GS_VEH_COUNT);
            gs_world_add_car(&a->world, &a->t, which,
                             GS_INT(8) + GS_INT(2) * v, GS_INT(12), GS_DEG(30));
        }
    } else if (a->skip_menu || a->online) {
        // **An online race is the server's race, not this machine's.** The grid
        // is as many cars as the server said are playing, on the stock
        // machines, with the world's own dials - because every machine has to
        // build the identical world and the only thing they all agree about is
        // what the server told them. Reading the setup screen here is what a
        // client did for one afternoon after the front door was built, and it
        // put two cars on a one-player server's grid: the phantom sat on the
        // start line, the camera framed the pair of them, and in a real
        // two-player race the two machines would have built different worlds
        // from their own screens - which is the one thing rollback cannot
        // recover from.
        for (uint8_t i = 0; i < players; i++) {
            gs_fix sx, sy; gs_angle facing;
            // Centred for the cars the server named - every machine knows
            // the same count, so every machine builds the same grid.
            gs_track_grid_of(&a->t, i, players, &sx, &sy, &facing);
            gs_world_add_car(&a->world, &a->t, grid[i], sx, sy, facing);
        }

        // **A race with no lap count never ends.** This branch set the grid and
        // nothing else, so `laps_to_win` stayed at the zero `gs_world_init`
        // leaves - and zero means a race with no finish line at all. Every
        // online race was therefore unfinishable: a player crossed the line and
        // nothing happened, twice reported and correct both times.
        //
        // Not read off this machine's setup screen, for the reason the grid is
        // not: an online race is the server's race and two machines reading
        // their own screens build two different worlds. The protocol carries no
        // lap count, so it is derived from the track instead - which every
        // machine has, and has had checked on the way in.
        gs_world_set_mode(&a->world, GS_MODE_RACE);
        gs_world_set_laps(&a->world, (uint16_t)GS_DEFAULT_LAPS);
    } else {
        // From the setup screen: the dials, the machines and the paint. The
        // paint goes to the renderer rather than into the world, because a
        // colour cannot change where a car ends up and must not be able to.
        const gs_race_setup *set = &a->menu.setup;

        gs_setup_build(set, &a->t, &a->world);
        for (uint8_t i = 0; i < set->players && i < GS_MAX_CARS; i++) {
            gs_render_set_car_paint(i, set->colour[i]);
        }
        players = set->players;
    }
    // **Nobody drives until the lights go green.** Armed here rather than in
    // gs_world_init, so the analyser, the AI sweeps and the editor's background
    // ghost - none of which is a person who needs a moment to get ready - are
    // untouched and every replay ever recorded still lands where it did.
    //
    // Set identically on every machine because it is set before the first tick,
    // where every machine agrees the tick is zero. A countdown decided later,
    // or by one machine and sent, would be a different world on each screen.
    gs_world_set_countdown(&a->world, GS_COUNTDOWN_TICKS);

    // Nobody has missed anything yet.
    for (uint8_t i = 0; i < GS_MAX_CARS; i++) a->view[i].missed = false;

    a->race_settled = false;
    a->prev = a->world;

    // Whatever was just raced becomes the thing to beat. A recording of zero
    // ticks - the first race, or a restart from the grid - is not worth
    // carrying, so it is left alone and the previous ghost stays.
    if (a->recording.meta.tick_count > 0 && !a->ghost_borrowed) {
        gs_ghost_take(&a->ghost, &a->recording, &a->t);
        a->show_ghost = true;
    } else {
        gs_ghost_reset(&a->ghost, &a->t);
    }
    gs_replay_begin(&a->recording, &a->world, &a->t);

    // **Who is in each car, written into the recording as it starts.** A
    // recording that names nobody proves a time was driven and says nothing
    // about whose it is, so anyone who obtained one could hand it in as their
    // own and be correctly told it was genuine. A guest leaves their car blank,
    // which means "not recorded" - and a claim of identity against a blank is
    // refused rather than waved through.
    for (uint8_t i = 0; i < a->world.car_count && i < GS_MAX_CARS; i++) {
        int8_t who = a->menu.setup.profile[i];
        const char *name = (who >= 0 && who < (int8_t)a->menu.profiles.count)
                               ? a->menu.profiles.entry[who].name
                               : "";
        gs_replay_set_driver(&a->recording, i, name);
    }

    a->views = a->showroom ? 1 : a->world.car_count;
    for (uint8_t i = 0; i < a->views; i++) {
        a->view[i] = (gs_view){ 0 };
        a->view[i].car = i;
        a->view[i].cam.zoom = a->zoom > 0.0f ? a->zoom : GS_ISO_DEFAULT_ZOOM;
        a->view[i].show_gravity = a->overlay;
        a->view[i].show_arc = a->arc;
        gs_render_track_camera(&a->view[i], &a->t, &a->prev, &a->world, 1.0f);
    }

    if (a->showroom && a->world.car_count > 0) {
        // Centred on the line-up rather than following anybody, because nobody
        // is driving.
        const gs_car *first = &a->world.car[0];
        const gs_car *last = &a->world.car[a->world.car_count - 1];
        a->view[0].cam.cx = (gs_to_f(first->x) + gs_to_f(last->x)) * 0.5f;
        a->view[0].cam.cy = (gs_to_f(first->y) + gs_to_f(last->y)) * 0.5f;
        a->view[0].cam.cz = 0.0f;
    }
}

// --- the store --------------------------------------------------------------
//
// **Remembered between executions**, which is what makes a record a record and
// a profile a person. One file in the preferences directory, holding the
// drivers and everything they have done. The frontend owns the disk because
// src/core/ owns no I/O; the format is core's.

#define GS_STORE_FILENAME "gearstick.store"

static bool gs_store_path(char *out, size_t cap) {
    const char *dir = gs_pref_dir();
    if (dir == nullptr) return false;
    SDL_snprintf(out, cap, "%s%s", dir, GS_STORE_FILENAME);
    return true;
}

static void gs_store_load(gs_app *a) {
    char path[1024];
    if (!gs_store_path(path, sizeof path)) return;

    size_t n = 0;
    void *buf = SDL_LoadFile(path, &n);
    if (buf == nullptr) return;      // nothing saved yet, which is not an error

    if (!gs_menu_load(&a->menu, (const uint8_t *)buf, n)) {
        SDL_Log("store: %s is not readable - starting fresh", path);
    } else {
        SDL_Log("store: %u drivers, %u records", a->menu.profiles.count,
                a->menu.records.count);
    }
    SDL_free(buf);
}

// Hand a finished race to the server, with the inputs that produced it. `w` is
// the world being claimed about, which for a networked race is the confirmed one
// rather than the one on screen.
static void gs_submit_result(gs_app *a, const gs_world *w) {
    if (a->server_host == nullptr) return;

    static uint8_t proof[sizeof(gs_replay) + 4096];
    size_t n = gs_replay_serialize(&a->recording, proof, sizeof proof);
    const gs_car *me = &w->car[a->net.local];

    gs_wire_send_result(a->wire, gs_track_hash(&a->t), gs_conditions_hash(w),
                        w->laps_to_win, me->vehicle, me->best_lap,
                        me->finish_tick, proof, n);
}

// **Write down the race everybody agreed on, as it is agreed.** Not the visible
// one: that is built partly on guesses about the other cars and most of those
// are rolled back, so a recording taken from it would be a recording of things
// that did not happen.
static void gs_record_confirmed(gs_app *a) {
    uint32_t upto = gs_net_confirmed_tick(&a->net);

    while (a->net_recorded < upto) {
        const gs_input *in = gs_net_confirmed_input(&a->net, a->net_recorded);

        // Gone out of the window, which means this machine fell two seconds
        // behind its own network. The recording stops here rather than
        // continuing with a hole in it - a log with a gap re-races to somewhere
        // else and would be refused anyway, and being refused for the real
        // reason is worth more than being refused for a mysterious one.
        if (in == nullptr) {
            SDL_Log("net: the recording fell behind the race and cannot be "
                    "completed - nothing will be submitted");
            break;
        }
        if (!gs_replay_record(&a->recording, in)) break;
        a->net_recorded++;
    }
}

// About fifteen seconds at sixty frames a second. Long enough for a peer to
// finish sending what it owes over any link a race was playable on, short
// enough that a machine which walked away does not leave somebody staring at a
// results screen for ever.
#define GS_NET_SETTLE_FRAMES 900u

// Keep talking after the finish line until the agreed race has an ending, then
// submit it. Runs once a frame, not once a tick: nothing here is simulation.
static void gs_net_settle(gs_app *a) {
    if (!a->net_settling) return;

    gs_wire_poll(a->wire);

    uint8_t buf[GS_WIRE_MTU];
    size_t n;
    while ((n = gs_wire_recv(a->wire, buf, sizeof buf)) > 0) {
        gs_net_receive(&a->net, &a->t, buf, n);
    }
    n = gs_net_packet(&a->net, buf, sizeof buf);
    gs_wire_send(a->wire, buf, n);

    gs_record_confirmed(a);

    const gs_world *agreed = gs_net_confirmed(&a->net);
    if (agreed->over) {
        // **The ending everybody arrived at, written into the recording.** The
        // server re-races the log and has to land here; a log altered anywhere,
        // in any car's inputs, lands somewhere else.
        gs_replay_set_agreed(&a->recording, gs_net_agreed_hash(&a->net));
        gs_submit_result(a, agreed);
        a->net_settling = false;
        SDL_Log("net: the race is agreed at tick %u, and submitted",
                gs_net_confirmed_tick(&a->net));
        return;
    }

    if (++a->net_settle_frames > GS_NET_SETTLE_FRAMES) {
        a->net_settling = false;
        SDL_Log("net: nobody finished agreeing what happened - nothing "
                "submitted, which is better than submitting half a race");
    }
}

static void gs_store_save(gs_app *a) {
    if (!a->menu.store_dirty) return;

    // **A machine being told what to draw never writes the player's roster.**
    // --screen and --session invent drivers so there is somebody in the picture
    // and somebody in the results, and every one of those runs was saving them
    // into the store somebody actually plays with - which is how "ada" and
    // "bez" turn up on a machine that never asked for them. A screenshot is not
    // a session; it reads the store and leaves it alone.
    // **Unless it was asked to.** A check that races the real client from the
    // front door to the results has to be able to look at what the race left
    // behind, and the only place a time is left is the store. --keep is that
    // consent, given on a command line, by something that has already pointed
    // GEARSTICK_PREF_DIR at a throwaway.
    if (!a->keep &&
        (a->want_screen || a->session || a->showroom || a->shot_path != nullptr)) {
        a->menu.store_dirty = false;
        return;
    }

    char path[1024];
    if (!gs_store_path(path, sizeof path)) return;

    // **As many bytes as this store takes, asked rather than guessed.** What
    // was here was the roster, the records and four kilobytes of slack for
    // everything else - and "everything else" is the library, which is four
    // kilobytes for *one* track and ninety for the twenty-two that ship. So
    // gs_menu_save refused every time it was called, on every machine, and
    // nothing anybody did in the front end was ever written down. The message
    // below blamed the disk, which is why it went unnoticed for so long: it
    // printed SDL's last error, and SDL had not been asked to do anything yet.
    size_t cap = gs_menu_size(&a->menu);
    uint8_t *buf = (uint8_t *)SDL_malloc(cap);
    if (buf == nullptr) {
        SDL_Log("store: no room to build %zu bytes of store", cap);
        return;
    }

    size_t n = gs_menu_save(&a->menu, buf, cap);
    if (n == 0) {
        SDL_Log("store: could not build the store - %zu bytes was not enough "
                "for %u driver(s), %u record(s) and %u track(s)",
                cap, a->menu.profiles.count, a->menu.records.count,
                a->menu.library.count);
        SDL_free(buf);
        return;
    }
    if (!SDL_SaveFile(path, buf, n)) {
        SDL_Log("store: could not write %s: %s", path, SDL_GetError());
        SDL_free(buf);
        return;
    }
    SDL_free(buf);
    a->menu.store_dirty = false;
}

// --- the race, as something to listen to ------------------------------------
//
// The verification for sound in docs/COMPLETION_PLAN.md is "listened to, on all
// three platforms", which is the right test and is not one a machine can run.
// What a machine *can* do is hand somebody the thing to listen to, identical on
// every platform because it is the same synthesiser fed the same race.

#define GS_WAV_SECONDS 40
#define GS_WAV_FRAMES  (GS_AUDIO_RATE * GS_WAV_SECONDS)

static float gs_wav[GS_WAV_FRAMES * GS_AUDIO_CHANNELS];
static size_t gs_wav_used;

static void gs_shot_audio_tick(gs_app *a) {
    gs_audio_update(&a->world, &a->t, gs_to_f(a->world.car[0].x),
                    gs_to_f(a->world.car[0].y));

    int frames = GS_AUDIO_RATE / GS_TICK_HZ;
    size_t room = (size_t)GS_WAV_FRAMES - gs_wav_used;
    if ((size_t)frames > room) frames = (int)room;
    if (frames <= 0) return;

    gs_audio_render(&gs_wav[gs_wav_used * GS_AUDIO_CHANNELS], frames);
    gs_wav_used += (size_t)frames;
}

static void gs_put_le32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xffu);
}

static void gs_put_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static bool gs_write_wav(const char *path) {
    // Sixteen-bit PCM, because every player on every platform opens that and
    // this file exists to be double-clicked.
    uint32_t samples = (uint32_t)(gs_wav_used * GS_AUDIO_CHANNELS);
    uint32_t data_bytes = samples * 2u;

    uint8_t header[44];
    SDL_memcpy(header, "RIFF", 4);
    gs_put_le32(header + 4, 36u + data_bytes);
    SDL_memcpy(header + 8, "WAVEfmt ", 8);
    gs_put_le32(header + 16, 16u);                       // PCM chunk size
    gs_put_le16(header + 20, 1u);                        // PCM
    gs_put_le16(header + 22, GS_AUDIO_CHANNELS);
    gs_put_le32(header + 24, GS_AUDIO_RATE);
    gs_put_le32(header + 28, GS_AUDIO_RATE * GS_AUDIO_CHANNELS * 2u);
    gs_put_le16(header + 32, GS_AUDIO_CHANNELS * 2u);
    gs_put_le16(header + 34, 16u);
    SDL_memcpy(header + 36, "data", 4);
    gs_put_le32(header + 40, data_bytes);

    SDL_IOStream *io = SDL_IOFromFile(path, "wb");
    if (io == nullptr) return false;

    bool ok = SDL_WriteIO(io, header, sizeof header) == sizeof header;
    for (uint32_t i = 0; ok && i < samples; i++) {
        float v = SDL_clamp(gs_wav[i], -1.0f, 1.0f);
        int16_t q = (int16_t)(v * 32767.0f);
        uint8_t le[2];
        gs_put_le16(le, (uint16_t)q);
        ok = SDL_WriteIO(io, le, 2) == 2;
    }
    SDL_CloseIO(io);
    return ok;
}

// --- ghosts on disk --------------------------------------------------------
//
// A ghost you race yourself never needs a file - restarting hands the last run
// straight over. A file is for the other half of the feature: the run somebody
// else did, sent to you, raced against on your machine. Same bytes, same
// simulation, same car.

#define GS_GHOST_FILENAME "ghost.gsreplay"

static bool gs_ghost_path(char *out, size_t cap) {
    const char *dir = gs_pref_dir();
    if (dir == nullptr) return false;
    SDL_snprintf(out, cap, "%s%s", dir, GS_GHOST_FILENAME);
    return true;
}

static void gs_ghost_save(gs_app *a) {
    char path[1024];
    if (a->recording.meta.tick_count == 0 || !gs_ghost_path(path, sizeof path)) {
        SDL_Log("ghost: nothing recorded yet");
        return;
    }

    static uint8_t buf[sizeof(gs_replay) + 4096];
    size_t n = gs_replay_serialize(&a->recording, buf, sizeof buf);
    if (n == 0 || !SDL_SaveFile(path, buf, n)) {
        SDL_Log("ghost: save failed: %s", SDL_GetError());
        return;
    }
    SDL_Log("ghost: wrote %zu bytes to %s", n, path);
    a->ghost_borrowed = false;
}

static void gs_ghost_open(gs_app *a) {
    char path[1024];
    if (!gs_ghost_path(path, sizeof path)) return;

    size_t n = 0;
    void *buf = SDL_LoadFile(path, &n);
    if (buf == nullptr) {
        SDL_Log("ghost: nothing saved at %s", path);
        return;
    }

    // A ghost recorded on another track is refused rather than raced. The same
    // inputs somewhere else are a different race, and pretending otherwise
    // would put a car through the scenery and call it a lap time.
    bool ok = gs_ghost_load(&a->ghost, &a->t, (const uint8_t *)buf, n);
    SDL_free(buf);

    if (!ok) {
        SDL_Log("ghost: that recording is for a different track");
        return;
    }
    a->show_ghost = true;
    a->ghost_borrowed = true;
    SDL_Log("ghost: loaded %u ticks", gs_ghost_length(&a->ghost));
}

// Two views side by side. Four will merge into one when the cars are close -
// see docs/FEATURES.md - but that is a later phase and this is the shape it
// grows out of.
static void gs_layout(gs_app *a) {
    int w = 0, h = 0;
    SDL_GetRenderOutputSize(a->ren, &w, &h);

    SDL_Rect rects[GS_MAX_CARS];
    uint8_t n = gs_render_layout(a->views, w, h, rects);
    for (uint8_t i = 0; i < n; i++) a->view[i].rect = rects[i];
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv) {
    gs_app *a = SDL_calloc(1, sizeof *a);
    if (a == nullptr) return SDL_APP_FAILURE;
    *appstate = a;

    for (int i = 1; i < argc; i++) {
        if (SDL_strcmp(argv[i], "--shot") == 0 && i + 1 < argc) {
            a->shot_path = argv[++i];
        } else if (SDL_strcmp(argv[i], "--shot-at") == 0 && i + 1 < argc) {
            a->shot_at = (uint64_t)SDL_atoi(argv[++i]);
        } else if (SDL_strcmp(argv[i], "--overlay") == 0) {
            a->overlay = true;
        } else if (SDL_strcmp(argv[i], "--arc") == 0) {
            a->arc = true;
        } else if (SDL_strcmp(argv[i], "--players") == 0 && i + 1 < argc) {
            int n = SDL_atoi(argv[++i]);
            a->players = (uint8_t)(n < 1 ? 1 : (n > GS_MAX_CARS ? GS_MAX_CARS : n));
        } else if (SDL_strcmp(argv[i], "--zoom") == 0 && i + 1 < argc) {
            a->zoom = (float)SDL_atof(argv[++i]);
        } else if (SDL_strcmp(argv[i], "--diverge") == 0) {
            a->diverge = true;
        } else if (SDL_strcmp(argv[i], "--editor") == 0) {
            a->start_in_editor = true;
        } else if (SDL_strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            a->online = true;
            a->port = (uint16_t)SDL_atoi(argv[++i]);
            // How many people are coming, host included. Two by default,
            // because two is the common case and nobody should have to type a
            // number to race one friend.
            if (i + 1 < argc && argv[i + 1][0] >= '2' && argv[i + 1][0] <= '4' &&
                argv[i + 1][1] == '\0') {
                a->online_players = (uint8_t)SDL_atoi(argv[++i]);
            }
        } else if (SDL_strcmp(argv[i], "--server") == 0 && i + 2 < argc) {
            // Meet everybody at a server rather than at each other. Which
            // player you are is then the server's decision.
            a->online = true;
            a->server_host = argv[++i];
            a->port = (uint16_t)SDL_atoi(argv[++i]);
        } else if (SDL_strcmp(argv[i], "--server-key") == 0 && i + 1 < argc) {
            const char *hex = argv[++i];
            if (SDL_strlen(hex) != 64) {
                SDL_Log("--server-key wants 64 hex characters; the server "
                        "prints its key when it starts");
                return SDL_APP_FAILURE;
            }
            for (int k = 0; k < GS_NOISE_KEY_BYTES; k++) {
                unsigned byte = 0;
                for (int half = 0; half < 2; half++) {
                    char ch = hex[k * 2 + half];
                    unsigned digit =
                        (ch >= '0' && ch <= '9') ? (unsigned)(ch - '0')
                      : (ch >= 'a' && ch <= 'f') ? (unsigned)(ch - 'a' + 10)
                      : (ch >= 'A' && ch <= 'F') ? (unsigned)(ch - 'A' + 10)
                                                 : 16u;
                    if (digit > 15u) {
                        SDL_Log("--server-key is not hexadecimal");
                        return SDL_APP_FAILURE;
                    }
                    byte = byte * 16u + digit;
                }
                a->server_key[k] = (uint8_t)byte;
            }
            a->has_server_key = true;
        } else if (SDL_strcmp(argv[i], "--join") == 0 && i + 2 < argc) {
            a->online = true;
            a->join_host = argv[++i];
            a->port = (uint16_t)SDL_atoi(argv[++i]);
        } else if (SDL_strcmp(argv[i], "--ghost-out") == 0 && i + 1 < argc) {
            a->ghost_out = argv[++i];
        } else if (SDL_strcmp(argv[i], "--ghost") == 0 && i + 1 < argc) {
            a->ghost_path = argv[++i];
        } else if (SDL_strcmp(argv[i], "--audio-out") == 0 && i + 1 < argc) {
            a->audio_out = argv[++i];
        } else if (SDL_strcmp(argv[i], "--relay") == 0) {
            a->use_relay = true;
        } else if (SDL_strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            a->online_name = argv[++i];
        } else if (SDL_strcmp(argv[i], "--demo-library") == 0) {
            a->demo_library = true;
        } else if (SDL_strcmp(argv[i], "--track") == 0 && i + 1 < argc) {
            a->track_path = argv[++i];
        } else if (SDL_strcmp(argv[i], "--trace") == 0) {
            // **The client says what it is showing.** Every fault found by
            // playing this so far has been in the thirty seconds after the
            // green flag - the camera somewhere else, the controls dead, a
            // wreck with no way out - and none of it is reachable by a test
            // that cannot see the screen. A line a second, in key=value, is
            // what makes those things assertions instead of screenshots.
            a->trace = true;
        } else if (SDL_strcmp(argv[i], "--autodrive") == 0) {
            // The AI takes this machine's car through the ordinary loop -
            // input, network, camera and all - rather than the session's
            // straight-line simulation. That is what makes a scripted race a
            // test of the *client* and not of the simulation it already has
            // tests for.
            a->autodrive = true;
        } else if (SDL_strcmp(argv[i], "--keep") == 0) {
            a->keep = true;
        } else if (SDL_strcmp(argv[i], "--session") == 0) {
            // A whole session with nobody at the keyboard: the grid from the
            // setup screen, a race driven by the AI, and the results table it
            // produced. This is the plan's verification made runnable - "a full
            // session start to finish" is a thing that can be watched happen
            // rather than a thing somebody says they did.
            a->session = true;
        } else if (SDL_strcmp(argv[i], "--screen") == 0 && i + 1 < argc) {
            // For looking at the front end, and for capturing it. Named rather
            // than numbered, because a screenshot script that says "results" is
            // a screenshot script somebody can read.
            const char *name = argv[++i];
            a->shot_screen = SDL_strcmp(name, "login") == 0     ? GS_SCREEN_LOGIN
                           : SDL_strcmp(name, "title") == 0     ? GS_SCREEN_TITLE
                           : SDL_strcmp(name, "drivers") == 0   ? GS_SCREEN_PROFILES
                           : SDL_strcmp(name, "setup") == 0     ? GS_SCREEN_SETUP
                           : SDL_strcmp(name, "results") == 0   ? GS_SCREEN_RESULTS
                           : SDL_strcmp(name, "records") == 0   ? GS_SCREEN_RECORDS
                           : SDL_strcmp(name, "tracks") == 0    ? GS_SCREEN_TRACKS
                           : SDL_strcmp(name, "lobby") == 0     ? GS_SCREEN_LOBBY
                                                                : GS_SCREEN_RACE;
            a->want_screen = true;
        } else if (SDL_strcmp(argv[i], "--showroom") == 0) {
            a->showroom = true;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                a->showroom_from = (uint8_t)SDL_atoi(argv[++i]);
            }
        } else if (SDL_strcmp(argv[i], "--heatmap") == 0) {
            a->start_in_editor = true;
            a->analyse_at_start = true;
        } else if (SDL_strcmp(argv[i], "--help") == 0) {
            SDL_Log("gearstick - arrows drive car one, WASD car two.");
            SDL_Log("  --shot FILE     write a frame and exit");
            SDL_Log("  --shot-at TICK  which tick to write it at (default 0)");
            SDL_Log("  --overlay       start with the painted-gravity overlay on");
            SDL_Log("  --arc           start with the landing arc on");
            SDL_Log("  --editor        open in the construction set");
            SDL_Log("  --heatmap       open the editor with the analyser already run");
            SDL_Log("  --showroom      line up every vehicle, to look at the art");
            SDL_Log("  --screen NAME   login/title/drivers/setup/tracks/"
                    "results/records/lobby");
            SDL_Log("  --session       run a whole race by itself and stop on the results");
            SDL_Log("  --keep          let a session write what it did, which it otherwise will not");
        SDL_Log("  --track FILE    open this track rather than the library's first");
        SDL_Log("  --autodrive     let the AI drive this machine's car");
        SDL_Log("  --trace         print what is on screen once a second");
            SDL_Log("  --audio-out F   with --shot: write the race as a .wav to listen to");
            SDL_Log("  --zoom N        camera zoom, 1.0 being one tile to 64 px");
            SDL_Log("  --players N     one to four, split-screen to match");
            SDL_Log("  --diverge       with --shot: drive the cars apart, to see the split");
            SDL_Log("  M turns the music off and on.");
            SDL_Log("  H shows or hides the ghost of your last run,");
            SDL_Log("  F5 saves that run as a ghost file and F9 loads one.");
            SDL_Log("  --ghost FILE    race against a recorded run");
            SDL_Log("  --ghost-out F   with --shot: write the captured run as a ghost");
            SDL_Log("  --host PORT [N]   wait for N players in total (2-4, default 2)");
            SDL_Log("  --join HOST PORT  join somebody who is waiting");
            SDL_Log("  --server HOST PORT  meet everybody at a server");
            SDL_Log("  --server-key HEX    the public key of the server or "
                    "host being joined, which it prints when it starts");
            SDL_Log("  --name NAME     who to appear as online");
            SDL_Log("  --relay         send through the server, if peers cannot connect");
            SDL_Log("  J toggles the landing arc while airborne.");
            SDL_Log("  G toggles the painted-gravity overlay, R restarts, "
                    "Esc quits.");
            return SDL_APP_SUCCESS;
        }
    }

    // **Audio is in here or there is no sound.** SDL_OpenAudioDeviceStream needs
    // the subsystem up; without it every open fails with "Audio subsystem is not
    // initialized", gs_audio_open takes its no-device path, and the game races
    // in silence on every machine there is - which is exactly what it did until
    // somebody looked at why a wav capture could not be raced.
    //
    // Not asked for when capturing a wav: that path renders the mixer straight
    // to a file from this thread, so a subsystem would only add a callback
    // thread rendering the same mixer into a speaker at the same time.
    SDL_InitFlags subsystems = SDL_INIT_VIDEO | SDL_INIT_GAMEPAD;
    if (a->audio_out == nullptr) subsystems |= SDL_INIT_AUDIO;

    if (!SDL_Init(subsystems)) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    // **The window opens where it was left.** Size from the memory when
    // there is one; position only after checking the remembered spot is on a
    // display that still exists - monitors get unplugged, and a window
    // restored onto one that is gone is a game nobody can see or grab.
    gs_winmem wm;
    gs_winmem_default(&wm, GS_WINDOW_W, GS_WINDOW_H);
    char wm_path[1024];
    SDL_snprintf(wm_path, sizeof wm_path, "%s%s", gs_pref_dir(),
                 GS_WINMEM_FILE);
    gs_winmem_load(&wm, wm_path);

    // Whether the remembered spot is safe to go back to, asked of the
    // displays that exist right now - before the window exists, because the
    // position goes into its *creation*. A window manager honours where a
    // window asks to be born far more reliably than a move after it is on
    // screen: under WSLg a post-creation SDL_SetWindowPosition landed only
    // sometimes, and a position that lands only sometimes is a window that
    // wanders.
    bool place = false;
    if (wm.placed) {
        int n = 0;
        SDL_DisplayID *ids = SDL_GetDisplays(&n);
        if (ids != nullptr) {
            SDL_Rect bounds[16];
            int have = 0;
            for (int i = 0; i < n && have < (int)SDL_arraysize(bounds); i++) {
                if (SDL_GetDisplayBounds(ids[i], &bounds[have])) have++;
            }
            SDL_free(ids);
            place = gs_winmem_on_a_display(&wm, bounds, have);
        }
    }

    SDL_PropertiesID wprops = SDL_CreateProperties();
    SDL_SetStringProperty(wprops, SDL_PROP_WINDOW_CREATE_TITLE_STRING,
                          "gearstick");
    SDL_SetNumberProperty(wprops, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, wm.w);
    SDL_SetNumberProperty(wprops, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, wm.h);
    SDL_SetBooleanProperty(wprops, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN,
                           true);
    if (place) {
        SDL_SetNumberProperty(wprops, SDL_PROP_WINDOW_CREATE_X_NUMBER, wm.x);
        SDL_SetNumberProperty(wprops, SDL_PROP_WINDOW_CREATE_Y_NUMBER, wm.y);
        a->win_asked = true;
        a->win_ask_x = wm.x;
        a->win_ask_y = wm.y;
    }
    a->win = SDL_CreateWindowWithProperties(wprops);
    SDL_DestroyProperties(wprops);
    if (a->win == nullptr) {
        SDL_Log("SDL_CreateWindowWithProperties: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    a->ren = SDL_CreateRenderer(a->win, nullptr);
    if (a->ren == nullptr) {
        SDL_Log("SDL_CreateRenderer: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    // **The icon in the title bar and on the taskbar.** Not decoration: an
    // untitled window with the toolkit's default icon is what an unfinished
    // thing looks like, and it is the first thing anybody sees of the game.
    //
    // Generated rather than drawn - see tools/make_icon.py - so there is no
    // third-party art here either. A failure is logged and shrugged off: a
    // missing icon is a window with the wrong picture on it, which is not a
    // reason to refuse to start.
    {
        char icon_path[1024];
        gs_asset_path(icon_path, sizeof icon_path, "icon.png");
        SDL_Surface *icon = IMG_Load(icon_path);
        if (icon != nullptr) {
            SDL_SetWindowIcon(a->win, icon);
            SDL_DestroySurface(icon);
        } else {
            SDL_Log("icon: %s (%s)", SDL_GetError(), icon_path);
        }
    }

    gs_input_init(&a->input);

    // Dear ImGui, for the editor's panels. See CMakeLists.txt for why there is
    // C++ in a C project at all.
    ImGui_CreateContext(nullptr);
    gs_style_menu();
    ImGuiIO *io = ImGui_GetIO();
    // No imgui.ini. There is no window layout worth persisting yet, and the
    // default drops a file in whatever directory the game happened to start in.
    io->IniFilename = nullptr;
    // The panel is walkable with a pad. Half this game is two people on a sofa,
    // and an editor only one of them can drive is half a construction set.
    io->ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    if (!cImGui_ImplSDL3_InitForSDLRenderer(a->win, a->ren) ||
        !cImGui_ImplSDLRenderer3_Init(a->ren)) {
        SDL_Log("could not start Dear ImGui");
        return SDL_APP_FAILURE;
    }

    gs_menu_init(&a->menu);

    if (!gs_editor_init(&a->editor, 65536)) {
        SDL_Log("could not allocate the edit history");
        return SDL_APP_FAILURE;
    }

    // The stock tracks, and the first of them as the one loaded. A fresh
    // install has something to race on; a returning player has whatever their
    // library already held as well - which is put back over the top of this
    // when the store is read, and is why the shipped set is synced again there.
    gs_sync_stock_tracks(a);

    // **A track asked for by name wins over the library's first**, and says so
    // if it cannot be read rather than quietly racing something else. For
    // somebody opening a track they were sent, and for a check that needs a
    // race on ground that is not at height zero - which is where the camera
    // fault lived, invisible on the flat.
    bool have_track = false;
    if (a->track_path != nullptr) {
        size_t named_len = 0;
        void *named_bytes = SDL_LoadFile(a->track_path, &named_len);
        bool named_ok = false;
        if (named_bytes != nullptr) {
            static gs_track named;
            named_ok = gs_track_deserialize(&named, (const uint8_t *)named_bytes,
                                            named_len);
            SDL_free(named_bytes);
            if (named_ok) {
                a->t = named;
                a->menu.chosen = gs_library_find(&a->menu.library,
                                                 gs_track_hash(&a->t));
            }
        }
        if (!named_ok) SDL_Log("track: could not read %s", a->track_path);
        have_track = named_ok;
    }

    // **A named track that would not load must not race anyway.** It used to:
    // the failure was logged and then the "a track was named" branch skipped
    // the fallback, leaving `a->t` as it was found - all zeros, a track no tiles
    // wide. Sampling that indexes off the front of its own arrays, which the
    // sanitiser caught only because a test happened to name a track that had
    // been renamed. Falling back to the library beats a race on a track that
    // does not exist, and the log above still says what could not be read.
    const gs_track *first = gs_library_track(&a->menu.library, 0);
    if (have_track) {
        // already chosen above
    } else if (first != nullptr) {
        a->t = *first;
        a->menu.chosen = 0;
    } else {
        // Nothing on disk at all - a broken install, or somebody running from a
        // directory with no assets. Flat ground beats a black screen, and the
        // construction set can build on it.
        SDL_Log("no tracks in assets/tracks - starting on flat ground");
        // Flat by construction - gs_track_init zeroes every corner - so the
        // frontend states no geometry of its own even here.
        gs_track_init(&a->t, 40, 24, GS_SURF_PAVEMENT);
        gs_track_add_gate(&a->t, GS_INT(6), GS_INT(12), 0, GS_INT(6));
        gs_track_add_gate(&a->t, GS_INT(34), GS_INT(12), 0, GS_INT(6));
    }

    // **Decided before the race is built, because the race is built from it.**
    // A shot, a showroom or the construction set is a machine being told
    // exactly what to do and gets no front end; anything else is a person.
    // This used to be worked out a hundred lines further down, after the world
    // had already been built - so gs_start_race took its menu branch on every
    // start, and --players was quietly ignored on every one of them. Four
    // players raced as two, and a split screen was unreachable from the
    // command line while the help text offered it.
    //
    // **Online is a person, so online gets the front end.** It used to be in
    // this list, which is why a client pointed at a server went straight into a
    // race and never showed a menu: the one case where knowing who you are
    // matters most was the one case that never asked.
    a->skip_menu = (a->shot_path != nullptr && !a->want_screen && !a->session) ||
                   a->showroom || a->start_in_editor;

    // And a grid asked for on the command line is the grid the setup screen
    // offers, so the two cannot disagree about how many people are racing.
    if (a->players > 0) a->menu.setup.players = a->players;

    gs_start_race(a);

    if (a->online) {
        if (!gs_wire_init()) {
            SDL_Log("net: could not start networking: %s", SDL_GetError());
            return SDL_APP_FAILURE;
        }
        uint8_t want = a->online_players > 0 ? a->online_players : 2;
        const char *me = a->online_name != nullptr ? a->online_name : "driver";

        a->wire = a->server_host != nullptr
                      ? gs_wire_server(a->server_host, a->port, me,
                                       a->has_server_key ? a->server_key : nullptr)
                  : a->join_host != nullptr
                      ? gs_wire_join(a->join_host, a->port,
                                     a->has_server_key ? a->server_key : nullptr)
                      : gs_wire_host(a->port, want);

        if (a->wire != nullptr && a->server_host == nullptr &&
            a->join_host == nullptr && gs_wire_error(a->wire) == nullptr) {
            // Hosting. Everybody who joins has to already know this key, so it
            // is printed where a person can copy it.
            const uint8_t *key = gs_wire_public_key(a->wire);
            char hex[2 * GS_NOISE_KEY_BYTES + 1];
            for (int i = 0; i < GS_NOISE_KEY_BYTES; i++) {
                static const char *digits = "0123456789abcdef";
                hex[i * 2] = digits[key[i] >> 4];
                hex[i * 2 + 1] = digits[key[i] & 0xfu];
            }
            hex[2 * GS_NOISE_KEY_BYTES] = '\0';
            SDL_Log("net: hosting on port %u; the key to give people is %s",
                    a->port, hex);
        }

        const char *err = gs_wire_error(a->wire);
        if (a->wire == nullptr || err != nullptr) {
            SDL_Log("net: %s", err != nullptr ? err : "could not open a socket");
            return SDL_APP_FAILURE;
        }

        if (a->server_host != nullptr) {
            gs_wire_use_relay(a->wire, a->use_relay);
            SDL_snprintf(a->menu.server_text, sizeof a->menu.server_text,
                         "%s:%u, as %s%s", a->server_host, a->port, me,
                         a->use_relay ? "  (through the server)" : "");
            SDL_Log("net: meeting at %s", a->menu.server_text);
        } else {
            SDL_Log("net: %s on port %u",
                    a->join_host != nullptr ? "joining" : "hosting", a->port);
        }
    }

    // A ghost named on the command line is somebody else's run, so it survives
    // the restart that would otherwise replace it with your own.
    if (a->ghost_path != nullptr) {
        size_t n = 0;
        void *buf = SDL_LoadFile(a->ghost_path, &n);
        if (buf == nullptr) {
            SDL_Log("ghost: could not read %s", a->ghost_path);
        } else {
            if (gs_ghost_load(&a->ghost, &a->t, (const uint8_t *)buf, n)) {
                a->show_ghost = true;
                a->ghost_borrowed = true;
                SDL_Log("ghost: %s, %u ticks", a->ghost_path,
                        gs_ghost_length(&a->ghost));
            } else {
                SDL_Log("ghost: %s is for a different track", a->ghost_path);
            }
            SDL_free(buf);
        }
    }
    gs_layout(a);

    if (a->start_in_editor) {
        a->edit_session = true;
        gs_editor_toggle(&a->editor, &a->view[0]);
    }
    if (a->analyse_at_start) gs_editor_analyse(&a->editor, &a->t);

    SDL_Log("gearstick: assets at %s", gs_assets_dir());
    SDL_Log("gearstick: track hash 0x%016llx",
            (unsigned long long)gs_track_hash(&a->t));

    // Silence is not an error: a machine with no sound device races exactly the
    // same race, because nothing downstream of the simulation can reach back
    // into it.
    //
    // **Capturing a wav opens nothing.** The capture renders from this thread,
    // and a device thread rendering the same mixer alongside it would put half
    // of every buffer in the speaker and the other half in the file. What comes
    // out of --wav has to be what would come out of the speaker, or it is not
    // worth listening to.
    if (a->audio_out != nullptr) {
        gs_audio_open_silent();
    } else {
        gs_audio_open();
    }

    // **The track's own hash is the seed.** A track already carries an identity;
    // handing it to the composer means every track has its own tune, and
    // building a ramp changes the chorus. Nobody writes fifty pieces of music
    // and nobody hears the same one on all fifty tracks.
    gs_music_start(gs_track_hash(&a->t));

    // Where this start goes: straight to the grid for a machine, the front door
    // for a person. `skip_menu` was decided before the world was built - see
    // above, where the reason is written down.
    if (a->skip_menu) {
        a->menu.screen = GS_SCREEN_RACE;
    } else {
        gs_store_load(a);

        // **The store just replaced the library, so the shipped set goes back
        // in.** In this order and not the other: what somebody saved is the
        // authority on their own tracks, and what the game ships is the
        // authority on the game's.
        gs_sync_stock_tracks(a);

        a->menu.online = a->online;
        a->menu.screen = a->want_screen ? (gs_screen)a->shot_screen
                                        : GS_SCREEN_LOGIN;

        // **A screenshot is a machine being told exactly what to draw.** Asked
        // for a screen behind the door, it is signed in so the screen it wants
        // is the screen it gets; asked for the door itself, it is not. A person
        // is never in either branch - `--screen` is not something you can press.
        if (a->want_screen && a->shot_screen != GS_SCREEN_LOGIN) {
            if (a->menu.profiles.count == 0) {
                gs_profile_add(&a->menu.profiles, "player", GS_COLOUR_RED,
                               (uint8_t)GS_VEH_BAJA_BUG);
            }
            a->menu.signed_in = 0;
        }

        if (a->demo_library) {
            // The stock tracks are already in the library; this only picks one
            // so a screenshot has a selection in it.
            a->menu.picked = a->menu.library.count > 1 ? 1 : 0;
        }

        if (a->session) {
            // Two drivers with names, so the results table and the records have
            // somebody in them rather than a row of guests.
            gs_profile_add(&a->menu.profiles, "ada", GS_COLOUR_ORANGE,
                           (uint8_t)GS_VEH_SPRINT_CAR);
            gs_profile_add(&a->menu.profiles, "bez", GS_COLOUR_PURPLE,
                           (uint8_t)GS_VEH_DUNE_BUGGY);
            for (uint8_t i = 0; i < 2; i++) {
                a->menu.setup.profile[i] = (int8_t)i;
                a->menu.setup.vehicle[i] = a->menu.profiles.entry[i].vehicle;
                a->menu.setup.colour[i] = a->menu.profiles.entry[i].colour;
            }

            // **Two, unless somebody said otherwise.** This used to be two
            // whatever the command line asked for, so `--session --players 4`
            // ran a two-car race and said nothing about it - a flag that is
            // quietly ignored in one mode is worse than one that is not offered.
            a->menu.setup.players = a->players > 0 ? a->players : 2;
            a->menu.setup.laps = 2;
            // The session drives itself and never sees the door, but it does
            // reach the results screen afterwards - which is behind the gate.
            a->menu.signed_in = 0;
            a->menu.screen = GS_SCREEN_RACE;
            gs_start_race(a);
            gs_layout(a);
        }
    }

    gs_clock_init(&a->clock);
    a->last_ns = SDL_GetTicksNS();
    return SDL_APP_CONTINUE;
}

// --- what the client is showing -------------------------------------------
//
// **A line a second, in words a script can read.** Everything that has gone
// wrong in front of a player so far went wrong after the green flag: a camera
// pointed somewhere else, controls that did nothing, a wreck with no way out.
// None of it is reachable from a unit test, because it is a property of what
// ended up on the screen - so the client says what ended up on the screen, and
// tools/play_check.py makes assertions out of it. What the screens are called
// lives with the screens, in gs_menu.h.
static void gs_trace(gs_app *a, uint8_t views) {
    if (!a->trace) return;

    // Once a second wherever it is, and on every screen change, which is the
    // rate a person notices things at. **By the wall clock rather than the
    // world's tick**: a menu does not step the world, so a tick-based limit
    // said one thing on arrival and nothing ever again - and a screen that has
    // stopped responding is exactly the case where the log going quiet is the
    // thing you needed it to tell you.
    static gs_screen was = GS_SCREEN_COUNT;
    const uint64_t now_ms = SDL_GetTicks();
    bool moved = a->menu.screen != was;
    if (!moved && now_ms < a->traced_at + 1000u) return;
    was = a->menu.screen;
    const uint64_t since = now_ms - a->traced_at;
    // A screen change fires the line early, so the window can be a few
    // milliseconds - short windows say nothing and are reported as nothing
    // rather than as a wild number.
    const float fps = (since >= 250u && a->framed > 0)
                          ? (float)a->framed * 1000.0f / (float)since
                          : 0.0f;
    a->traced_at = now_ms;
    a->framed = 0;

    if (a->menu.screen != GS_SCREEN_RACE) {
        // **And what is on it**, not only which one it is. A check walking the
        // whole game from the door to the results has to be able to say that
        // the time it just set is *there*, and a screen name alone cannot.
        SDL_Log("trace screen=%s tick=%llu drivers=%u tracks=%u records=%u "
                "fps=%.1f",
                gs_screen_name(a->menu.screen),
                (unsigned long long)a->world.tick,
                (unsigned)a->menu.profiles.count,
                (unsigned)a->menu.library.count,
                (unsigned)a->menu.records.count, (double)fps);
        return;
    }

    uint8_t me = a->online && a->net_started ? a->net.local : 0;
    if (me >= a->world.car_count) me = 0;
    const gs_car *c = &a->world.car[me];

    // The pane this machine's driver is looking at: its own where the screen is
    // split, and the shared one where it is not.
    const gs_view *v = &a->view[0];
    for (uint8_t i = 0; i < views; i++) {
        if (a->view[i].car == me) { v = &a->view[i]; break; }
    }

    // **Is the car this machine drives actually on this machine's screen?**
    // That is the whole question a player asks first, and the one nothing was
    // asking: a camera left at the world origin draws a perfectly good race
    // that has no car in it.
    float sx = 0.0f, sy = 0.0f;
    gs_iso_project(&v->cam, gs_to_f(c->x), gs_to_f(c->y), gs_to_f(c->z), &sx, &sy);
    bool on = sx >= 0.0f && sx < v->cam.vw && sy >= 0.0f && sy < v->cam.vh;

    float speed = gs_to_f(gs_fix_len2(c->vx, c->vy));

    // **And whether the lights are still red**, which is the difference
    // between a car that will not drive and a car that is not allowed to yet.
    // Without it a watcher cannot tell those apart, and tools/play_check.py
    // spent its whole window on the countdown deciding the controls were
    // broken - a race held on the line looks exactly like a race whose input
    // path is disconnected, and the trace is the only place that can say which.
    // **And what the game was told to do**, which is the only way a report of
    // "the controls did not work" can be answered without guessing. A keyboard
    // that cannot report two keys at once and a car that will not turn at speed
    // feel identical from the driver's seat and are nothing alike; this says
    // which arrived.
    char held[8];
    int at = 0;
    if (a->last_input & GS_IN_ACCEL) held[at++] = 'A';
    if (a->last_input & GS_IN_BRAKE) held[at++] = 'B';
    if (a->last_input & GS_IN_LEFT)  held[at++] = 'L';
    if (a->last_input & GS_IN_RIGHT) held[at++] = 'R';
    if (a->last_input & GS_IN_FIRE)  held[at++] = 'F';
    if (at == 0) held[at++] = '-';
    held[at] = '\0';

    SDL_Log("trace screen=race tick=%llu cars=%u me=%u x=%.2f y=%.2f "
            "speed=%.2f wrecked=%u onscreen=%u sx=%.0f sy=%.0f "
            "cam=%.2f,%.2f zoom=%.2f lap=%u/%u over=%u stalls=%u held=%u "
            "input=%s fps=%.1f",
            (unsigned long long)a->world.tick, a->world.car_count, me,
            (double)gs_to_f(c->x), (double)gs_to_f(c->y), (double)speed,
            c->wrecked ? 1u : 0u, on ? 1u : 0u, (double)sx, (double)sy,
            (double)v->cam.cx, (double)v->cam.cy, (double)v->cam.zoom,
            c->laps, a->world.laps_to_win, a->world.over ? 1u : 0u,
            a->online ? a->net.stalls : 0u,
            gs_world_held(&a->world) ? 1u : 0u, held, (double)fps);
}

// **One step back, wherever back is from here.** Written once because two
// things ask for it: Escape, and a pad's cancel button - and a pad has no
// Escape key, so without this the only way off a screen with a pad is to walk
// to the button that says so. On the tracks screen that means stepping down
// through every track somebody owns.
//
// Returns true when there is nothing behind this screen and the game should
// stop - which only a key can ask for. **A pad's cancel button never quits.**
// The title screen says "Escape  quit" and means it; B is the button everybody
// presses to go back one step, reflexively, and having it close the game from
// the title is not a thing anybody asked for. Where there is nothing behind the
// screen, a pad's cancel does nothing at all.
// **Where a screen is being reached from, remembered as the move is made.**
//
// Two screens are reachable from more than one place and have to send Back
// where it came from: the race setup, which Escape out of a race lands on, and
// the tracks list, which the setup opens. Recorded here rather than at each
// button, because there are two paths that move the screen - a menu that
// returns the next one, and Escape - and a rule written at one of them is a
// rule the other does not follow. That is exactly how a paused race came to
// have no way back into it.
static void gs_note_origin(gs_app *a, gs_screen next) {
    if (next == GS_SCREEN_SETUP)  a->menu.setup_from = a->menu.screen;
    if (next == GS_SCREEN_TRACKS) a->menu.tracks_from = a->menu.screen;

    // **A menu opening over a race takes the focus.** It arrives behind the
    // race's own windows and ImGui leaves the focus where it was, so the first
    // click on it is spent taking the focus rather than pressing what it landed
    // on - reported from play as a dialog that ignores you once.
    if (a->menu.screen == GS_SCREEN_RACE && next != GS_SCREEN_RACE) {
        a->menu.take_focus = true;
    }
}

static bool gs_back_out(gs_app *a, bool may_quit) {
    // Back out one step rather than always quitting: quitting from a race
    // because you wanted the menu is the oldest bad habit in games, and the
    // title screen is where quitting belongs.
    //
    // **Where back goes is gs_menu_back's to say**, not this handler's, so that
    // it is a rule with a test rather than four lines nothing can reach. A
    // machine driving itself - a shot, a session, the showroom - has no front
    // end to back out to and quits.
    if (a->editor.active) {
        gs_editor_toggle(&a->editor, &a->view[0]);
        return false;
    }
    if (a->skip_menu) return may_quit;

    gs_screen back = gs_menu_back(&a->menu, false);
    if (back == GS_SCREEN_COUNT) return may_quit;

    // Leaving an online race is leaving the race: the lobby is where it can be
    // joined again, and it is only ready to be rejoined once this machine has
    // stopped racing.
    if (a->menu.screen == GS_SCREEN_RACE && a->online &&
        a->net_started && !a->net_settling) {
        gs_net_finish(&a->net);
        a->net_started = false;
        a->lobby_hold = true;
        // Not race_settled - see the note by the other place this used to be
        // cleared. The finished world outlives the screen, so clearing it here
        // re-runs the end of the race.
    }
    // **Escape back into a paused race resumes it rather than restarting.**
    // Nothing here builds a world - only the menu's own transition does - so
    // arriving this way is already the resume, and the flag is cleared so a
    // stale one cannot make the next GO do nothing.
    gs_note_origin(a, back);
    a->menu.resume = false;
    a->menu.screen = back;
    return false;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *e) {
    gs_app *a = (gs_app *)appstate;

    gs_input_event(&a->input, e);
    cImGui_ImplSDL3_ProcessEvent(e);

    switch (e->type) {
    case SDL_EVENT_QUIT:
        return SDL_APP_SUCCESS;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        gs_layout(a);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        // **A pad's cancel is back too**, and nothing else here listens for a
        // pad button: driving reads them by polling, and the construction set's
        // rebinding takes them where it draws. Only when a race is not on, and
        // only when the construction set is not waiting to be told which button
        // somebody meant.
        if (a->editor.active && a->editor.rebind_action >= 0) break;
        if (gs_input_is_back(e, a->menu.screen == GS_SCREEN_RACE) &&
            gs_back_out(a, gs_input_back_may_quit(e))) {
            return SDL_APP_SUCCESS;
        }
        break;
    case SDL_EVENT_KEY_DOWN:
        // **Typing is not a shortcut.** Dear ImGui says when a text field has
        // the keyboard, and while it does the game's hotkeys stay out of the
        // way. Without this, typing a driver called "gavin" toggles the ghost
        // on the g, restarts the race on the... there is no r in gavin, but
        // there is in "harry", and Tab opens the construction set over the top
        // of the form somebody is halfway through filling in.
        if (ImGui_GetIO()->WantCaptureKeyboard) break;

        if (gs_input_is_back(e, a->menu.screen == GS_SCREEN_RACE) &&
            gs_back_out(a, gs_input_back_may_quit(e))) {
            return SDL_APP_SUCCESS;
        }
        if (e->key.key == SDLK_G) {
            for (uint8_t i = 0; i < a->views; i++)
                a->view[i].show_gravity = !a->view[i].show_gravity;
        }
        // **Off unless asked for.** A permanent readout of where you are going
        // to land turns a judgement into a number to follow, and the arc not
        // being negotiable is what makes the take-off decision worth making.
        if (e->key.key == SDLK_J) {
            a->arc = !a->arc;
            for (uint8_t i = 0; i < a->views; i++) a->view[i].show_arc = a->arc;
        }
        // Restarting is not one machine's to decide when the race is shared:
        // everybody else would carry on racing the world this one threw away.
        if (e->key.key == SDLK_R && !a->online) gs_start_race(a);
        if (e->key.key == SDLK_H) a->show_ghost = !a->show_ghost;
        if (e->key.key == SDLK_M) {
            if (gs_music_playing()) gs_music_stop();
            else gs_music_start(gs_track_hash(&a->t));
        }
        if (e->key.key == SDLK_F5) gs_ghost_save(a);
        if (e->key.key == SDLK_F9) gs_ghost_open(a);
        // Tab is the whole loop: build, drive, build. No load step between
        // them, which is the single biggest thing the original could not do.
        //
        // **Only inside a session, though.** It is not how you reach the
        // construction set - that is New and Edit on the tracks screen, where
        // somebody would look for it - it is how you get between building the
        // thing and driving it once you are already building.
        if (e->key.key == SDLK_TAB && (a->editor.active || a->edit_session)) {
            gs_editor_toggle(&a->editor, &a->view[0]);
            // Leaving the editor drops a car where you were looking. Coming
            // back does nothing at all to the track, which is the other half of
            // the promise: the edits are still there and so is the history.
            if (!a->editor.active) gs_start_test_drive(a);
        }
        break;
    default:
        break;
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
    gs_app *a = (gs_app *)appstate;

    // The window manager's placement offset, measured once the window has
    // settled - see win_asked. Read here rather than at creation, because a
    // read taken before the manager has finished placing measures garbage.
    if (!a->win_measured) {
        a->win_measured = true;
        if (a->win_asked && a->win != nullptr) {
            int gx = 0, gy = 0;
            SDL_GetWindowPosition(a->win, &gx, &gy);
            a->win_off_x = gx - a->win_ask_x;
            a->win_off_y = gy - a->win_ask_y;
        }
    }

    uint64_t now = SDL_GetTicksNS();
    uint64_t delta = now - a->last_ns;
    a->last_ns = now;

    // How many fixed steps this frame owes the simulation. The catch-up cap and
    // the leftover fraction both live in gs_clock, which is in src/core/ so
    // that it can be tested without a window - see gs_clock.h for why that is
    // worth the trouble.
    uint32_t steps = gs_clock_advance(&a->clock, delta);

    // In shot mode the clock is ignored entirely and the ticks are counted out,
    // so the captured frame is the same frame on every machine.
    // A session drives itself to the flag, then stops on the results. Bounded,
    // because a track the AI cannot get round would otherwise spin here forever
    // - and the analyser exists precisely because that is a thing tracks do.
    if (a->session && a->menu.screen == GS_SCREEN_RACE) {
        steps = 0;
        gs_clock_init(&a->clock);

        // **As long as the track says a lap of it takes, times the laps.**
        //
        // Five minutes was written when a stock lap was under one, and the
        // stock set now takes four to five: `jupiter run` is 5m 15s, longer
        // than the whole budget, so a session on it stopped a lap short of the
        // flag and reported `winner 255, over no` in a line nobody reads. A
        // race that drives itself and cannot finish the tracks in the box is
        // not a bounded race, it is a broken one.
        //
        // gs_analyse_seconds is the project's own answer to "how long is this
        // track", used by the analyser and by the shipped-track test for the
        // same reason: it scales with the route rather than with a number typed
        // here, so the next track that gets longer does not need anybody to
        // remember this line. The bound is still a bound - a track the AI
        // cannot get round stops here rather than spinning forever, which is
        // the whole point of having one.
        const uint32_t laps = a->world.laps_to_win > 0 ? a->world.laps_to_win : 1;
        const uint64_t give_up = (uint64_t)gs_analyse_seconds(&a->t) *
                                 (uint64_t)laps * (uint64_t)GS_TICK_HZ;
        while (!a->world.over && a->world.tick < give_up) {
            gs_input in[GS_MAX_CARS] = { 0 };
            for (uint8_t i = 0; i < a->world.car_count; i++) {
                in[i] = gs_ai_drive(&a->world, &a->t, i);
            }
            a->prev = a->world;
            gs_world_step(&a->world, &a->t, in);
        }

        a->race_settled = true;
        gs_menu_finish(&a->menu, &a->world, &a->t);   // and it shows the results
        gs_store_save(a);
        SDL_Log("session: %u laps, winner %u, over %s",
                a->menu.setup.laps, a->world.winner, a->world.over ? "yes" : "no");
    }

    if (a->shot_path != nullptr) {
        steps = 0;
        gs_clock_init(&a->clock);

        // A capture of an online screen has to let the network run, or every
        // screenshot of a lobby is a screenshot of the word "Knocking".
        if (a->online && !a->net_started) {
            for (uint32_t i = 0; i < a->shot_at; i++) {
                gs_wire_poll(a->wire);
                SDL_Delay(2);
            }
        }

        while (a->world.tick < a->shot_at) {
            gs_input in[GS_MAX_CARS];
            for (uint8_t i = 0; i < GS_MAX_CARS; i++) in[i] = GS_IN_ACCEL;

            // Hold the second car back and then let it chase, so a capture can
            // show the screen deciding it needs two views and later that it
            // does not. Separating along the track beats steering apart: two
            // cars turning away from each other curve back together.
            if (a->diverge && a->world.car_count > 1) {
                in[1] = (a->world.tick < 420) ? (gs_input)0 : GS_IN_ACCEL;
            }

            a->prev = a->world;
            gs_replay_record(&a->recording, in);
            gs_world_step(&a->world, &a->t, in);
            if (a->show_ghost) gs_ghost_step(&a->ghost, &a->t);

            // One tick of audio for one tick of race, so the recording is what
            // the race sounded like rather than what the synthesiser does when
            // asked nicely. This is how "listened to" gets done at all: a race
            // nobody can hear cannot be verified by hearing it.
            if (a->audio_out != nullptr) gs_shot_audio_tick(a);

            // The split is a function of time as well as distance, so it has to
            // be advanced alongside the simulation rather than only at the end.
            int ww = 0, wh = 0;
            SDL_GetRenderOutputSize(a->ren, &ww, &wh);
            gs_split_update(&a->split, &a->t, &a->prev, &a->world, 1.0f, ww, wh,
                            1.0f / (float)GS_TICK_HZ);
        }
    }

    // The race is paused while the track is being built - a car ploughing on
    // through terrain that is changing under it helps nobody.
    if (a->editor.active) steps = 0;

    // And it is paused on every screen that is not the race, which is what
    // makes the front end a front end rather than an overlay on a race that
    // carries on behind it.
    if (a->menu.screen != GS_SCREEN_RACE) steps = 0;

    // A finished race is worked out once. Doing it every frame would resubmit
    // the same lap to the records table until somebody clicked something.
    if (a->world.over && !a->race_settled && !a->skip_menu) {
        a->race_settled = true;

        // The last dozen ticks were promised and not yet shown. Without this
        // they never are, and the other machine can never confirm the finish -
        // a race that is correct everywhere except the end.
        if (a->online && a->net_started) gs_net_finish(&a->net);
        gs_menu_finish(&a->menu, &a->world, &a->t);   // and it shows the results
        gs_store_save(a);

        // And to the server, with the inputs that produced it. The server
        // re-races them before it believes any of it, which is why the
        // recording goes along rather than the number alone.
        //
        // A networked race is not submitted here. The race the player is
        // looking at is over; the race everybody has *agreed* on is a dozen
        // ticks behind it, because the reveals trail the commitments. Handing
        // in a recording that stops short of its own ending would be handing in
        // one that cannot reproduce the ending it claims, so the client keeps
        // talking until the agreed race is over too - see gs_net_settle.
        if (a->online && a->net_started) {
            gs_net_finish(&a->net);
            a->net_settling = true;
            a->net_settle_frames = 0;
        } else if (a->online && a->server_host != nullptr) {
            gs_submit_result(a, &a->world);
        }

        steps = 0;
    }

    gs_net_settle(a);

    if (a->online && !a->net_started) {
        // Nobody races until everybody is here. The player count decides the
        // grid, and the grid has to be identical on every machine before a
        // single tick is simulated - rollback can recover from a wrong guess
        // about an input and from nothing at all about a wrong starting state.
        gs_wire_poll(a->wire);

        // Waiting is a thing with a picture. The menu is handed what the wire
        // last heard and draws it; it owns no networking of its own, so a
        // lobby screen cannot be a reason the connection behaves differently.
        if (a->server_host != nullptr) {
            a->menu.lobby = gs_wire_lobby(a->wire);
            a->menu.lobby_error = gs_wire_refusal(a->wire);
            a->menu.lobby_slot = gs_wire_local(a->wire);
            a->menu.lobby_ready = gs_wire_ready(a->wire);
            a->menu.track_progress =
                gs_wire_track_hash(a->wire) != 0 ? gs_wire_track_progress(a->wire)
                                                 : 1.0f;

            // Counted while there is nothing to show, and reset the moment
            // there is: a lobby that arrives late is not a lobby that failed.
            const gs_lobby *heard = a->menu.lobby;
            if (heard != nullptr && heard->capacity > 0) {
                a->menu.knocking_for = 0.0f;
            } else {
                a->menu.knocking_for += (float)delta / 1e9f;
            }
            if (a->menu.screen == GS_SCREEN_RACE) a->menu.screen = GS_SCREEN_LOBBY;
        }
        // **Nobody is dragged into a race from anywhere but the lobby.** The
        // server saying the grid is full means the race *can* start, not that
        // whoever is at this machine has asked to be in it - and they may well
        // be signed in and reading the records. Waiting in the lobby is how
        // somebody says yes, so waiting in the lobby is what this waits for.
        // Asking to race is what lifts the hold - see gs_menu.race_requested.
        if (a->menu.race_requested) {
            a->lobby_hold = false;
            a->menu.race_requested = false;
        }

        if (gs_wire_ready(a->wire) && a->menu.screen == GS_SCREEN_LOBBY &&
            !a->lobby_hold) {
            // **The server's track, not ours.** Whatever was loaded locally is
            // set aside: everybody has to be racing the same ground, and the
            // hash was checked on the way in, so this is the one moment the
            // game knows for certain that they are.
            static gs_track served;
            if (gs_wire_track(a->wire, &served)) {
                a->t = served;
                a->music_hash = gs_track_hash(&a->t);
                gs_music_start(a->music_hash);
                SDL_Log("net: racing the server's track %016llx",
                        (unsigned long long)a->music_hash);
            }

            a->players = gs_wire_players(a->wire);
            gs_start_race(a);

            // This machine's own secret for the race. Every salt it publishes
            // is derived from it and it never goes on the wire. The same
            // caveat as the server's session nonce applies and is written down
            // in docs/THREATS.md rather than left to be assumed: SDL's
            // generator is not a cryptographic one, so this is the shape the
            // defence takes and not yet a defence against somebody who can
            // predict it.
            uint8_t secret[GS_NET_SECRET_BYTES];
            for (size_t i = 0; i < sizeof secret; i++) {
                secret[i] = (uint8_t)(SDL_rand_bits() & 0xffu);
            }
            gs_net_begin(&a->net, &a->world, gs_wire_players(a->wire),
                         gs_wire_local(a->wire), secret);
            a->net_recorded = 0;
            a->net_settling = false;
            a->net_settle_frames = 0;
            a->net_started = true;
            a->menu.screen = GS_SCREEN_RACE;
            // **The grid is said out loud**, because "one player" and "two
            // cars" is what a race built from the wrong place looks like, and
            // it is the difference between a race and a desync waiting to
            // happen. tools/front_door_check.py reads this line and refuses a
            // race whose grid is not the size the server said.
            SDL_Log("net: %u players, driving car %u, %u car(s) on the grid",
                    a->net.players, a->net.local, a->world.car_count);
        }
        steps = 0;
    }

    if (a->online && a->net_started) {
        // Still saying we are here. The race goes peer to peer, so without this
        // the server hears nothing from a racing client and eventually drops
        // it - and being dropped mid-race is how a lobby loses somebody who
        // never left.
        gs_wire_poll(a->wire);

        for (uint32_t i = 0; i < steps; i++) {
            // Everything that has arrived, before anything is simulated: a
            // correction is worth more the earlier it lands, because it is
            // fewer ticks to replay.
            uint8_t buf[GS_WIRE_MTU];
            size_t n;
            while ((n = gs_wire_recv(a->wire, buf, sizeof buf)) > 0) {
                gs_net_receive(&a->net, &a->t, buf, n);
            }

            gs_input in[GS_MAX_CARS];
            gs_input_poll(&a->input, in, 1);
            if (a->autodrive) {
                in[0] = gs_ai_drive(&a->world, &a->t, a->net.local);
            }
            a->last_input = in[0];
            gs_net_local_input(&a->net, in[0]);

            n = gs_net_packet(&a->net, buf, sizeof buf);
            gs_wire_send(a->wire, buf, n);

            a->prev = *gs_net_world(&a->net);
            if (!gs_net_step(&a->net, &a->t)) {
                // **Two different reasons to stop, and only one of them is
                // worth waiting out.** A stall is the other machine being
                // quiet, and it ends when a datagram arrives. A broken promise
                // does not end: somebody's client has been modified, and there
                // is no honest reading of the rest of the race.
                if (gs_net_cheated(&a->net)) {
                    SDL_Log("net: car %u revealed an input it had not committed "
                            "to at tick %u - the race is abandoned",
                            a->net.cheat_by, a->net.cheat_tick);
                    a->net_started = false;
                    a->menu.screen = GS_SCREEN_RESULTS;
                    steps = 0;
                    break;
                }
                a->waiting++;
                if (a->stalled_since == 0) a->stalled_since = SDL_GetTicksNS();

                // **Waiting has an end.** The server drops a client that has
                // been silent for fifteen seconds, so a machine still waiting
                // after twenty is waiting for somebody nobody expects back.
                // The race stops here rather than never: what there is of it
                // goes to the results, which is a screen with a way off it.
                uint64_t waited = SDL_GetTicksNS() - a->stalled_since;
                if (waited > (uint64_t)GS_GIVE_UP_SECONDS * 1000000000ull) {
                    SDL_Log("net: nothing from the other machine for %d "
                            "seconds - the race is over at tick %u",
                            GS_GIVE_UP_SECONDS, a->net.local_tick);
                    gs_net_finish(&a->net);
                    a->net_started = false;
                    a->net_settling = false;
                    a->stalled_since = 0;
                    a->menu.screen = GS_SCREEN_RESULTS;
                }
                break;    // the other machine has gone quiet; wait for it
            }
            a->stalled_since = 0;
            gs_world seen = a->world;
            a->world = *gs_net_world(&a->net);
            gs_view_note_missed(a->view, GS_MAX_CARS, &a->t, &seen, &a->world);
        }
        steps = 0;
    }

    for (uint32_t i = 0; i < steps; i++) {
        gs_input in[GS_MAX_CARS];
        gs_input_poll(&a->input, in, a->world.car_count);

        // **The cars nobody is driving are driven.** Offline only: online, the
        // grid belongs to the server and every car on it belongs to a machine
        // that is sending its own inputs.
        if (!a->online) gs_setup_drive(&a->menu.setup, &a->world, &a->t, in);

        if (a->autodrive) {
            for (uint8_t c = 0; c < a->world.car_count; c++) {
                in[c] = gs_ai_drive(&a->world, &a->t, c);
            }
        }

        a->last_input = in[a->view[0].car < GS_MAX_CARS ? a->view[0].car : 0];

        a->prev = a->world;
        gs_replay_record(&a->recording, in);
        gs_world_step(&a->world, &a->t, in);
        gs_view_note_missed(a->view, GS_MAX_CARS, &a->t, &a->prev, &a->world);

        // Lockstep, one tick for one tick. The ghost is a race, not a
        // playback, so it has to be stepped by the same clock as everything
        // else or it drifts against the thing it is being compared to.
        if (a->show_ghost) gs_ghost_step(&a->ghost, &a->t);
    }

    // What the race sounds like, from where the camera is. Once a frame rather
    // than once a tick: the audio thread interpolates between updates by
    // chasing, and running this at 120 Hz would be work nobody could hear.
    if (a->editor.active) {
        gs_audio_silence();
    } else {
        gs_audio_update(&a->world, &a->t, a->view[0].cam.cx, a->view[0].cam.cy);
    }

    // Editing a track rewrites its theme, because editing a track changes its
    // hash and the hash is the tune. Checked once a frame rather than watched
    // for, which is the same way the editor's ghost notices.
    uint64_t now_hash = gs_track_hash(&a->t);
    if (now_hash != a->music_hash) {
        a->music_hash = now_hash;
        gs_music_start(now_hash);
    }

    float alpha = gs_to_f(gs_clock_alpha(&a->clock));

    cImGui_ImplSDLRenderer3_NewFrame();
    cImGui_ImplSDL3_NewFrame();
    ImGui_NewFrame();

    // While editing, one view of the whole window rather than a split screen:
    // you are looking at the part of the track you are building, not at a car.
    uint8_t views = a->editor.active ? (uint8_t)1 : a->views;
    if (a->editor.active) {
        int w = 0, h = 0;
        SDL_GetRenderOutputSize(a->ren, &w, &h);
        a->view[0].rect = (SDL_Rect){ 0, 0, w, h };
        a->view[0].cam.cx = a->editor.cam_x;
        a->view[0].cam.cy = a->editor.cam_y;
        a->view[0].cam.cz = 0.0f;
        a->view[0].cam.zoom = a->editor.zoom;
        // Only the editor paints the heatmap. Racing over it would be reading
        // the answers off the back of the book.
        a->view[0].heat = gs_editor_heat(&a->editor);

        // Panning. Held keys rather than events, so it accelerates smoothly and
        // does not depend on the OS's key-repeat rate.
        const bool *key = SDL_GetKeyboardState(nullptr);
        if (key != nullptr) {
            float pan = 12.0f * (float)delta / 1e9f;
            if (key[SDL_SCANCODE_LEFT])  { a->editor.cam_x -= pan; a->editor.cam_y += pan; }
            if (key[SDL_SCANCODE_RIGHT]) { a->editor.cam_x += pan; a->editor.cam_y -= pan; }
            if (key[SDL_SCANCODE_UP])    { a->editor.cam_x -= pan; a->editor.cam_y -= pan; }
            if (key[SDL_SCANCODE_DOWN])  { a->editor.cam_x += pan; a->editor.cam_y += pan; }
        }

        // The pad drives the editor too, unless ImGui is using it to walk the
        // panel - which is what NavActive means. Without that check the stick
        // would move the cursor and the selection at once.
        ImGuiIO *eio = ImGui_GetIO();
        if (!eio->NavActive) {
            gs_pad_edit pad;
            gs_input_editor_pad(&a->input, &pad);
            float dt = (float)delta / 1e9f;
            if (gs_editor_pad_input(&a->editor, &a->t, &pad, dt)) {
                gs_editor_toggle(&a->editor, &a->view[0]);
                gs_start_test_drive(a);
            }
        }

        // A tool's density for the tool, and room to breathe for the front
        // end. Same colours, so they are recognisably one program.
        gs_style_editor();
        gs_editor_frame(&a->editor, &a->t, &a->view[0], &a->input);
        gs_style_menu();

        // Two ticks a frame at sixty frames a second is the ghost running at
        // real time. It is a headless simulation of the track being edited, so
        // it costs almost nothing and it notices its own track changing.
        gs_editor_ghost_step(&a->editor, &a->t, 2);
    }

    SDL_SetRenderDrawColor(a->ren, 18, 20, 26, 255);
    SDL_RenderClear(a->ren);

    // Racing: the screen decides for itself how many views it wants. Cars that
    // are close share one, because a collision is legible when both cars are in
    // the same picture and that is the whole argument for this camera.
    // One view, filling the window, behind a menu. A title screen over a split
    // screen reads as two things going wrong at once, and nobody is driving
    // either of them.
    if (a->menu.screen != GS_SCREEN_RACE && !a->editor.active) {
        int ww = 0, wh = 0;
        SDL_GetRenderOutputSize(a->ren, &ww, &wh);
        views = 1;
        a->view[0].rect = (SDL_Rect){ 0, 0, ww, wh };
        a->view[0].cam.vw = (float)ww;
        a->view[0].cam.vh = (float)wh;
    }

    if (!a->editor.active && !a->showroom && a->menu.screen == GS_SCREEN_RACE) {
        int ww = 0, wh = 0;
        SDL_GetRenderOutputSize(a->ren, &ww, &wh);
        gs_split_update(&a->split, &a->t, &a->prev, &a->world, alpha, ww, wh,
                        (float)delta / 1e9f);

        // **The views the frontend already has**, so that everything on them
        // which the splitter does not own survives being re-placed: the
        // overlay, the arc, and whether this driver has driven past a
        // checkpoint. That last one was set every tick and thrown away every
        // frame, because this used to hand the splitter a blank array and copy
        // three fields back by name.
        views = gs_split_views(&a->split, &a->t, &a->prev, &a->world, alpha,
                               ww, wh, a->view);
        for (uint8_t i = 0; i < views; i++) {
            a->view[i].show_arc = a->arc;
            a->view[i].heat = nullptr;
            if (a->zoom > 0.0f) a->view[i].cam.zoom = a->zoom;
        }
    }

    a->framed++;
    gs_trace(a, views);

    for (uint8_t i = 0; i < views; i++) {
        gs_render_view(a->ren, &a->t, &a->prev, &a->world, alpha, &a->view[i]);
    }

    // The ghost of your last run, in every view, under the cars. It is drawn
    // after the terrain and before nothing: a ghost that hid a real car would
    // be worse than no ghost at all.
    if (!a->editor.active && a->show_ghost) {
        const gs_car *gp = gs_ghost_prev_car(&a->ghost);
        const gs_car *gn = gs_ghost_car(&a->ghost);
        if (gp != nullptr && gn != nullptr) {
            for (uint8_t i = 0; i < views; i++) {
                gs_render_ghost_lerp(a->ren, &a->t, gp, gn, alpha, &a->view[i]);
            }
        }
    }

    // The HUD, one per view, over the race and under nothing. Only while a race
    // is what is on the screen: the editor has its own panels and the front end
    // is not a race, and a lap counter over a title screen is furniture.
    if (!a->editor.active && a->menu.screen == GS_SCREEN_RACE) {
        for (uint8_t i = 0; i < views; i++) {
            float waited = a->stalled_since == 0 ? 0.0f
                         : (float)(SDL_GetTicksNS() - a->stalled_since) / 1e9f;
            gs_hud_draw(&a->world, &a->t, &a->view[i], (uint32_t)a->world.tick,
                        waited, a->online);
        }
    }

    // The front end, over whatever is on the screen. A title over a parked grid
    // beats a title over nothing: the game is visibly a game before anybody has
    // pressed anything.
    if (!a->editor.active && a->menu.screen != GS_SCREEN_RACE) {
        gs_screen next = gs_menu_frame(&a->menu, &a->t);

        // **Exit means exit.** Only the frontend owns the loop, so the menu
        // raises a flag and this is where it is acted on.
        if (a->menu.quit) return SDL_APP_SUCCESS;

        // Signing in locally says who is at this keyboard. Saying the same
        // thing to a server is a separate claim it checks for itself, so the
        // credentials go across the moment there is somewhere to send them.
        // Taken rather than read, so they are sent once and then gone.
        if (a->wire != nullptr) {
            char password[64];
            uint32_t code = 0;
            if (gs_menu_take_server_login(&a->menu, password, sizeof password,
                                          &code)) {
                gs_wire_login(a->wire, gs_menu_driver(&a->menu), password,
                              code);
                SDL_memset(password, 0, sizeof password);
            }
        }

        // A track chosen from the library becomes the track. Taken rather than
        // read, so a choice is acted on once instead of every frame.
        int take = gs_menu_take_choice(&a->menu);
        const gs_track *picked = gs_library_track(&a->menu.library, take);
        if (picked != nullptr) {
            a->t = *picked;
            a->menu.chosen = take;
            a->music_hash = gs_track_hash(&a->t);
            gs_music_start(a->music_hash);
            // The undo history belongs to the track that was being edited.
            // Keeping it across a load would let undo apply one track's edits
            // to another.
            gs_edit_reset(a->editor.log);
        }

        // **The construction set, reached from the screen about tracks.** It
        // used to be Tab from anywhere - a key nothing mentioned, on a screen
        // that never said the editor existed.
        if (a->menu.new_requested) {
            a->menu.new_requested = false;

            // A blank field to build on, rather than whatever happened to be
            // loaded. Big enough for a loop and flat, because levelling
            // somebody else's hills is not the start of an idea.
            gs_track_init(&a->t, 48, 40, GS_SURF_PAVEMENT);
            a->menu.chosen = -1;
            a->music_hash = gs_track_hash(&a->t);
            gs_edit_reset(a->editor.log);

            a->menu.screen = GS_SCREEN_RACE;
            next = GS_SCREEN_RACE;
            a->edit_session = true;
            if (!a->editor.active) gs_editor_toggle(&a->editor, &a->view[0]);
        }

        if (a->menu.edit_requested) {
            a->menu.edit_requested = false;

            // **A track that came with the game is edited as a copy.** The
            // original stays in the library exactly as it shipped, and what
            // opens is the player's own from the first keystroke - which is
            // better than refusing, and better than letting them change it and
            // finding out at save time.
            const gs_library_entry *e =
                gs_library_at(&a->menu.library, a->menu.picked);
            if (e != nullptr) {
                a->t = e->track;
                a->music_hash = gs_track_hash(&a->t);
                gs_edit_reset(a->editor.log);

                if (e->builtin) {
                    char label[GS_LIBRARY_NAME];
                    SDL_snprintf(label, sizeof label, "%s (copy)", e->name);
                    int at = gs_library_put(&a->menu.library, &a->t, label, "");
                    a->menu.chosen = at;
                    a->menu.picked = at;
                    a->menu.store_dirty = true;
                } else {
                    a->menu.chosen = a->menu.picked;
                }

                a->menu.screen = GS_SCREEN_RACE;
                next = GS_SCREEN_RACE;
                a->edit_session = true;
                if (!a->editor.active) gs_editor_toggle(&a->editor, &a->view[0]);
            }
        }

        // Sharing, which only happens where there is a server to share into.
        if (a->menu.publish_requested) {
            a->menu.publish_requested = false;
            const gs_library_entry *e =
                gs_library_at(&a->menu.library, a->menu.picked);
            if (e != nullptr && a->online && a->wire != nullptr) {
                gs_wire_publish(a->wire, &e->track, e->name);
                SDL_snprintf(a->menu.status, sizeof a->menu.status,
                             "sent %s to the server", e->name);
            }
        }

        if (a->menu.withdraw_requested) {
            a->menu.withdraw_requested = false;
            const gs_library_entry *e =
                gs_library_at(&a->menu.library, a->menu.picked);
            if (e != nullptr && a->online && a->wire != nullptr) {
                gs_wire_withdraw(a->wire, e->hash);
                SDL_snprintf(a->menu.status, sizeof a->menu.status,
                             "took %s down", e->name);
            }
        }

        if (a->menu.share_with >= 0) {
            int slot = a->menu.share_with;
            a->menu.share_with = -1;

            const gs_library_entry *e =
                gs_library_at(&a->menu.library, a->menu.picked);
            // **Named by the key the server watched them prove**, not by a
            // string somebody typed - which is the whole reason sharing is with
            // people you are in a room with.
            const uint8_t *with = a->wire != nullptr
                                      ? gs_wire_peer_key(a->wire, (uint8_t)slot)
                                      : nullptr;
            if (e != nullptr && with != nullptr) {
                gs_wire_share(a->wire, e->hash, with, a->menu.share_on);
                SDL_snprintf(a->menu.status, sizeof a->menu.status,
                             a->menu.share_on ? "handed %s over"
                                              : "took %s back",
                             e->name);
            }
        }

        if (next != a->menu.screen) {
            // **Where a screen was reached from, recorded as the move is made.**
            // One place rather than one per button, so a path added later is
            // covered without anybody remembering to set it - which is how the
            // setup screen came to send a paused race to the main menu.
            gs_note_origin(a, next);

            if (next == GS_SCREEN_RACE) {
                // **Coming back to a paused race is not starting one.** Every
                // arrival here used to build a new world, which is right for GO
                // and threw away the race somebody had merely stepped out of.
                // The world is untouched while a menu is up - the step count is
                // zeroed off the race screen - so there is nothing to restore.
                if (a->menu.resume) {
                    a->menu.resume = false;
                } else {
                    gs_start_race(a);
                    gs_layout(a);
                }
            }

            // **Asking for another online race means there can be another
            // one.** `net_started` was only ever cleared when somebody cheated,
            // so after one race the whole online block above stopped running:
            // the lobby froze on whatever it last heard and no second race
            // could ever begin. Nothing noticed while a client went straight
            // into one race and stayed there; a menu you can come back to is
            // what made it reachable.
            //
            // Not while settling: the finished race is still being agreed with
            // everybody else, and throwing that away loses the result.
            // **Arriving at the lobby on purpose means waiting there.** The
            // lobby starts a race the moment it is ready, which is right the
            // first time somebody walks into it and wrong every time after:
            // pressing "Back to the lobby" on the results put them straight
            // into another race without being asked. Any menu-driven arrival
            // holds; the Race button in the lobby is how somebody says go.
            if (next == GS_SCREEN_LOBBY) a->lobby_hold = true;

            if (next == GS_SCREEN_LOBBY && a->net_started && !a->net_settling) {
                a->net_started = false;

                // **`race_settled` is not cleared here, and that is the point.**
                // It used to be, and the world it belongs to is the finished
                // one - which is still loaded until a new race replaces it. So
                // the next frame found `world.over` true and `race_settled`
                // false, ran the whole end-of-race path a second time,
                // submitted the same result again and put the screen back on
                // the results. Somebody who left the results for the tracks
                // screen, chose a track and pressed race was thrown straight
                // back to the results they had just left.
                //
                // gs_start_race clears it, which is the only moment it is
                // honestly clear: when there is a new race for it to describe.
            }
            // Back to a menu ends the building session: Tab is the loop while
            // you are working on something, and nothing at all once you are not.
            if (next != GS_SCREEN_RACE) a->edit_session = false;

            a->menu.screen = next;
            gs_store_save(a);
        }
    }

    if (a->editor.active) {
        const gs_car *ghost = gs_editor_ghost_car(&a->editor);
        if (ghost != nullptr) gs_render_ghost(a->ren, &a->t, ghost, &a->view[0]);
        gs_editor_draw_cursor(&a->editor, a->ren, &a->t, &a->view[0]);
    }

    // Dividers between the panes, so a split screen reads as several views
    // rather than as one confusing one. Faded by how merged the screen is: the
    // panes have already converged on the same picture by the time it appears,
    // so a line arriving at full strength would be the only sudden thing left.
    if (views > 1) {
        int w = 0, h = 0;
        SDL_GetRenderOutputSize(a->ren, &w, &h);
        uint8_t alpha8 = (uint8_t)(255.0f * (1.0f - a->split.merge));

        SDL_SetRenderDrawBlendMode(a->ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(a->ren, 8, 9, 12, alpha8);
        SDL_RenderFillRect(a->ren,
                           &(SDL_FRect){ (float)(w / 2 - 2), 0.0f, 4.0f, (float)h });
        if (views > 2) {
            SDL_RenderFillRect(a->ren,
                               &(SDL_FRect){ 0.0f, (float)(h / 2 - 2), (float)w, 4.0f });
        }
    }

    ImGui_Render();
    cImGui_ImplSDLRenderer3_RenderDrawData(ImGui_GetDrawData(), a->ren);

    // **An online capture waits for the race, rather than stepping it.** The
    // block above counts ticks out by hand, which is right for a race this
    // machine owns and wrong for one it does not: an online world is replaced
    // every tick by what the rollback agreed, so a locally stepped one is a
    // picture of a race nobody else is in. Online, the frame loop does the
    // racing and the capture waits for the tick it was asked for.
    bool waiting_for_tick = a->online && a->net_started &&
                            a->world.tick < a->shot_at;

    if (a->shot_path != nullptr && !waiting_for_tick) {
        SDL_Surface *s = SDL_RenderReadPixels(a->ren, nullptr);
        if (s == nullptr || !SDL_SaveBMP(s, a->shot_path)) {
            SDL_Log("could not write %s: %s", a->shot_path, SDL_GetError());
            if (s != nullptr) SDL_DestroySurface(s);
            return SDL_APP_FAILURE;
        }
        SDL_Log("wrote %s at tick %llu", a->shot_path,
                (unsigned long long)a->world.tick);
        SDL_DestroySurface(s);

        if (a->audio_out != nullptr) {
            if (!gs_write_wav(a->audio_out)) {
                SDL_Log("could not write %s: %s", a->audio_out, SDL_GetError());
                return SDL_APP_FAILURE;
            }
            SDL_Log("wrote %s, %.1f seconds", a->audio_out,
                    (double)gs_wav_used / (double)GS_AUDIO_RATE);
        }

        if (a->ghost_out != nullptr) {
            static uint8_t buf[sizeof(gs_replay) + 4096];
            size_t n = gs_replay_serialize(&a->recording, buf, sizeof buf);
            if (n == 0 || !SDL_SaveFile(a->ghost_out, buf, n)) {
                SDL_Log("could not write %s: %s", a->ghost_out, SDL_GetError());
                return SDL_APP_FAILURE;
            }
            SDL_Log("wrote %s, %u ticks", a->ghost_out,
                    a->recording.meta.tick_count);
        }
        return SDL_APP_SUCCESS;
    }

    SDL_RenderPresent(a->ren);
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    (void)result;
    gs_app *a = (gs_app *)appstate;
    if (a == nullptr) return;

    // Where the window is now is where it opens next time - unless it is
    // minimised, because a minimised window's position is wherever the
    // window system parked it, which is nowhere anybody chose.
    if (a->win != nullptr &&
        (SDL_GetWindowFlags(a->win) & SDL_WINDOW_MINIMIZED) == 0) {
        gs_winmem wm;
        gs_winmem_default(&wm, GS_WINDOW_W, GS_WINDOW_H);
        SDL_GetWindowPosition(a->win, &wm.x, &wm.y);
        SDL_GetWindowSize(a->win, &wm.w, &wm.h);
        wm.x -= a->win_off_x;
        wm.y -= a->win_off_y;
        wm.placed = true;
        char wm_path[1024];
        SDL_snprintf(wm_path, sizeof wm_path, "%s%s", gs_pref_dir(),
                     GS_WINMEM_FILE);
        gs_winmem_save(&wm, wm_path);
    }

    gs_store_save(a);
    gs_audio_close();
    if (a->wire != nullptr) {
        gs_wire_close(a->wire);
        gs_wire_quit();
    }
    gs_editor_quit(&a->editor);
    if (a->ren != nullptr) {
        cImGui_ImplSDLRenderer3_Shutdown();
        cImGui_ImplSDL3_Shutdown();
        ImGui_DestroyContext(nullptr);
    }
    gs_input_quit(&a->input);
    if (a->ren != nullptr) SDL_DestroyRenderer(a->ren);
    if (a->win != nullptr) SDL_DestroyWindow(a->win);
    SDL_free(a);
}

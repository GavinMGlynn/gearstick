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
#include "platform/gs_paths.h"
#include "audio/gs_audio.h"
#include "audio/gs_music.h"
#include "platform/gs_wire.h"
#include "core/gs_net.h"
#include "ui/gs_editor.h"
#include "core/gs_ghost.h"

#include "dcimgui.h"
#include "backends/dcimgui_impl_sdl3.h"
#include "backends/dcimgui_impl_sdlrenderer3.h"

#define GS_WINDOW_W 1280
#define GS_WINDOW_H 720

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
    bool        start_in_editor;
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
    uint16_t    port;
    uint8_t     online_players;   // how many the host is waiting for
    bool        net_started;      // everybody is here and the race has begun
    uint64_t    music_hash;       // the track the current tune was written for
    uint32_t    waiting;     // ticks spent stalled, for telling the player
    bool        quit;
} gs_app;

// The demo track, until the editor exists to make a real one. It is a
// prototype and it is named as one: a hard-coded track is not content.
static void gs_demo_track(gs_track *t) {
    gs_track_init(t, 40, 24, GS_SURF_PAVEMENT);

    for (uint8_t y = 0; y <= t->h; y++) {
        for (uint8_t x = 0; x <= t->w; x++) {
            gs_fix h = 0;
            if (x > 10 && x < 15) h = (gs_fix)((int64_t)GS_INT(1) * (x - 10) / 4);
            else if (x >= 15 && x < 22) h = GS_INT(1);
            else if (x >= 22 && x < 26) h = (gs_fix)((int64_t)GS_INT(1) * (26 - x) / 4);

            // A shallow bowl across the middle, so there is something to be
            // thrown out of sideways.
            int32_t dy = (int32_t)y - (int32_t)t->h / 2;
            h -= (gs_fix)((int64_t)GS_INT(1) * (12 - dy * dy) / 90);
            gs_track_set_corner(t, x, y, h);
        }
    }

    for (uint8_t x = 0; x < t->w; x++) {
        for (uint8_t y = 0; y < t->h; y++) {
            if (x >= 26 && x < 34) gs_track_set_surface(t, x, y, GS_SURF_ICE);
            else if (y < 6 || y >= t->h - 6) gs_track_set_surface(t, x, y, GS_SURF_DIRT);

            // A painted low-gravity pocket over the landing, so the brush is
            // visible from the first run.
            if (x >= 15 && x < 21) gs_track_set_gravity(t, x, y, GS_RATIO(35, 100));
        }
    }

    // A route, so there is something to look at and something to drive. Like
    // the terrain around it this is a prototype: it goes when the editor can
    // author a track worth shipping.
    gs_track_add_gate(t, GS_INT(6), GS_INT(12), 0, GS_INT(5));
    gs_track_add_gate(t, GS_INT(20), GS_INT(12), 0, GS_INT(5));
    gs_track_add_gate(t, GS_INT(34), GS_INT(12), 0, GS_INT(5));
}

static void gs_layout(gs_app *a);

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
        gs_render_track_camera(&a->view[i], &a->prev, &a->world, 1.0f);
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
    } else {
        for (uint8_t i = 0; i < players; i++) {
            gs_world_add_car(&a->world, &a->t, grid[i],
                             GS_INT(3), GS_INT(7) + GS_INT(4) * i, 0);
        }
    }
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

    a->views = a->showroom ? 1 : a->world.car_count;
    for (uint8_t i = 0; i < a->views; i++) {
        a->view[i] = (gs_view){ 0 };
        a->view[i].car = i;
        a->view[i].cam.zoom = a->zoom > 0.0f ? a->zoom : GS_ISO_DEFAULT_ZOOM;
        a->view[i].show_gravity = a->overlay;
        gs_render_track_camera(&a->view[i], &a->prev, &a->world, 1.0f);
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
            SDL_Log("  --editor        open in the construction set");
            SDL_Log("  --heatmap       open the editor with the analyser already run");
            SDL_Log("  --showroom      line up every vehicle, to look at the art");
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
            SDL_Log("  G toggles the painted-gravity overlay, R restarts, "
                    "Esc quits.");
            return SDL_APP_SUCCESS;
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    if (!SDL_CreateWindowAndRenderer("gearstick", GS_WINDOW_W, GS_WINDOW_H,
                                     SDL_WINDOW_RESIZABLE, &a->win, &a->ren)) {
        SDL_Log("SDL_CreateWindowAndRenderer: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    gs_input_init(&a->input);

    // Dear ImGui, for the editor's panels. See CMakeLists.txt for why there is
    // C++ in a C project at all.
    ImGui_CreateContext(nullptr);
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

    if (!gs_editor_init(&a->editor, 65536)) {
        SDL_Log("could not allocate the edit history");
        return SDL_APP_FAILURE;
    }

    gs_demo_track(&a->t);
    gs_start_race(a);

    if (a->online) {
        if (!gs_wire_init()) {
            SDL_Log("net: could not start networking: %s", SDL_GetError());
            return SDL_APP_FAILURE;
        }
        uint8_t want = a->online_players > 0 ? a->online_players : 2;
        a->wire = a->join_host != nullptr
                      ? gs_wire_join(a->join_host, a->port)
                      : gs_wire_host(a->port, want);

        const char *err = gs_wire_error(a->wire);
        if (a->wire == nullptr || err != nullptr) {
            SDL_Log("net: %s", err != nullptr ? err : "could not open a socket");
            return SDL_APP_FAILURE;
        }

        SDL_Log("net: %s on port %u", a->join_host != nullptr ? "joining" : "hosting",
                a->port);
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

    if (a->start_in_editor) gs_editor_toggle(&a->editor, &a->view[0]);
    if (a->analyse_at_start) gs_editor_analyse(&a->editor, &a->t);

    SDL_Log("gearstick: assets at %s", gs_assets_dir());
    SDL_Log("gearstick: track hash 0x%016llx",
            (unsigned long long)gs_track_hash(&a->t));

    // Silence is not an error: a machine with no sound device races exactly the
    // same race, because nothing downstream of the simulation can reach back
    // into it.
    gs_audio_open();

    // **The track's own hash is the seed.** A track already carries an identity;
    // handing it to the composer means every track has its own tune, and
    // building a ramp changes the chorus. Nobody writes fifty pieces of music
    // and nobody hears the same one on all fifty tracks.
    gs_music_start(gs_track_hash(&a->t));

    gs_clock_init(&a->clock);
    a->last_ns = SDL_GetTicksNS();
    return SDL_APP_CONTINUE;
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
    case SDL_EVENT_KEY_DOWN:
        if (e->key.key == SDLK_ESCAPE) return SDL_APP_SUCCESS;
        if (e->key.key == SDLK_G) {
            for (uint8_t i = 0; i < a->views; i++)
                a->view[i].show_gravity = !a->view[i].show_gravity;
        }
        if (e->key.key == SDLK_R) gs_start_race(a);
        if (e->key.key == SDLK_H) a->show_ghost = !a->show_ghost;
        if (e->key.key == SDLK_M) {
            if (gs_music_playing()) gs_music_stop();
            else gs_music_start(gs_track_hash(&a->t));
        }
        if (e->key.key == SDLK_F5) gs_ghost_save(a);
        if (e->key.key == SDLK_F9) gs_ghost_open(a);
        // Tab is the whole loop: build, drive, build. No load step between
        // them, which is the single biggest thing the original could not do.
        if (e->key.key == SDLK_TAB) {
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
    if (a->shot_path != nullptr) {
        steps = 0;
        gs_clock_init(&a->clock);
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
            gs_split_update(&a->split, &a->world, ww, wh, 1.0f / (float)GS_TICK_HZ);
        }
    }

    // The race is paused while the track is being built - a car ploughing on
    // through terrain that is changing under it helps nobody.
    if (a->editor.active) steps = 0;

    if (a->online && !a->net_started) {
        // Nobody races until everybody is here. The player count decides the
        // grid, and the grid has to be identical on every machine before a
        // single tick is simulated - rollback can recover from a wrong guess
        // about an input and from nothing at all about a wrong starting state.
        gs_wire_poll(a->wire);
        if (gs_wire_ready(a->wire)) {
            a->players = gs_wire_players(a->wire);
            gs_start_race(a);
            gs_net_begin(&a->net, &a->world, gs_wire_players(a->wire),
                         gs_wire_local(a->wire));
            a->net_started = true;
            SDL_Log("net: %u players, driving car %u", a->net.players, a->net.local);
        }
        steps = 0;
    }

    if (a->online && a->net_started) {
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
            gs_net_local_input(&a->net, in[0]);

            n = gs_net_packet(&a->net, buf, sizeof buf);
            gs_wire_send(a->wire, buf, n);

            a->prev = *gs_net_world(&a->net);
            if (!gs_net_step(&a->net, &a->t)) {
                a->waiting++;
                break;    // the other machine has gone quiet; wait for it
            }
            a->world = *gs_net_world(&a->net);
        }
        steps = 0;
    }

    for (uint32_t i = 0; i < steps; i++) {
        gs_input in[GS_MAX_CARS];
        gs_input_poll(&a->input, in, a->world.car_count);

        a->prev = a->world;
        gs_replay_record(&a->recording, in);
        gs_world_step(&a->world, &a->t, in);

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

        gs_editor_frame(&a->editor, &a->t, &a->view[0], &a->input);

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
    if (!a->editor.active && !a->showroom) {
        int ww = 0, wh = 0;
        SDL_GetRenderOutputSize(a->ren, &ww, &wh);
        gs_split_update(&a->split, &a->world, ww, wh, (float)delta / 1e9f);

        gs_view merged[GS_MAX_CARS];
        views = gs_split_views(&a->split, &a->world, ww, wh, merged);
        for (uint8_t i = 0; i < views; i++) {
            merged[i].show_gravity = a->view[i].show_gravity;
            merged[i].heat = nullptr;
            if (a->zoom > 0.0f) merged[i].cam.zoom = a->zoom;
            a->view[i] = merged[i];
        }
    }

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

    if (a->shot_path != nullptr) {
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

// window.c - a window on the server. See window.h.
//
// Dear ImGui through the same C bindings the game uses, drawn with SDL's
// renderer into an ordinary resizable window. Nothing here reads the server:
// it is handed facts and a log and hands back what was pressed.
#include "window.h"

#include "ui/gs_style.h"
#include "ui/gs_ui_probe.h"

#include "dcimgui.h"
#include "dcimgui_impl_sdl3.h"
#include "dcimgui_impl_sdlrenderer3.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define GS_WINDOW_W 760
#define GS_WINDOW_H 540

static struct {
    SDL_Window   *win;
    SDL_Renderer *ren;
    bool          open;
    bool          closed;       // the user closed it: stop
    bool          video;        // the video subsystem is ours to quit
} gs_w;

// --- pressing, for a test ----------------------------------------------------
//
// **A button pressed by name, through the button.** The check that pins the
// operator's controls cannot click a window nobody can see, and a flag that
// dropped a client directly would test the flag. So the window keeps every
// control it draws - the same item hooks the game's own walk uses - and a
// press asked for by label is delivered to the control's id on the next
// frame, which runs the button's own path: the press, the ask, the drop.
#define GS_PRESS_MAX 4
#define GS_ITEMS_MAX 256
static const char *gs_press_want[GS_PRESS_MAX];
static int         gs_press_count;
static gs_ui_item  gs_items[GS_ITEMS_MAX];
static bool        gs_probing;

void gs_window_press(const char *label) {
    if (gs_press_count < GS_PRESS_MAX) gs_press_want[gs_press_count++] = label;
}

// After a frame: any wanted label that was drawn is pressed next frame, once.
static void gs_press_wanted(void) {
    if (!gs_probing || gs_press_count == 0) return;
    int n = gs_ui_probe_count();
    if (n > GS_ITEMS_MAX) n = GS_ITEMS_MAX;
    for (int w = 0; w < gs_press_count; w++) {
        for (int i = 0; i < n; i++) {
            if (gs_items[i].label[0] == '\0' || gs_items[i].disabled) continue;
            if (strcmp(gs_items[i].label, gs_press_want[w]) != 0) continue;
            gs_ui_probe_press(gs_items[i].id);
            // Forget it: one press, however many frames the button stays.
            gs_press_want[w] = gs_press_want[gs_press_count - 1];
            gs_press_count--;
            w--;
            break;
        }
    }
}

// --- the log ------------------------------------------------------------------

void gs_srv_log_add(gs_srv_log *log, const char *text) {
    SDL_strlcpy(log->line[log->next], text, GS_SRV_LOG_WIDTH);
    log->next = (log->next + 1) % GS_SRV_LOG_LINES;
    if (log->count < GS_SRV_LOG_LINES) log->count++;
}

const char *gs_srv_log_line(const gs_srv_log *log, int i) {
    const int first = (log->next + GS_SRV_LOG_LINES - log->count) % GS_SRV_LOG_LINES;
    return log->line[(first + i) % GS_SRV_LOG_LINES];
}

// --- the dump -----------------------------------------------------------------
//
// **A test reads a window the way the window was told what to show.** Every
// line of text the window draws goes through gs_say, which keeps a copy of
// the frame's lines when asked; the check that pins the window against the
// terminal reads those back. Not a screenshot, which would need a reader
// that can read; and not the facts struct, which would be the window's
// input rather than its output.
#define GS_DUMP_BYTES 8192
static char gs_dump[GS_DUMP_BYTES];
static size_t gs_dump_len;
static bool gs_dumping;

static void gs_keep(const char *line) {
    if (!gs_dumping) return;
    const size_t n = strlen(line);
    if (gs_dump_len + n + 2 > GS_DUMP_BYTES) return;
    memcpy(gs_dump + gs_dump_len, line, n);
    gs_dump_len += n;
    gs_dump[gs_dump_len++] = '\n';
    gs_dump[gs_dump_len] = '\0';
}

// A line of text on the window, and in the dump.
static void gs_say(const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(1);
static void gs_say(const char *fmt, ...) {
    char line[192];
    va_list ap;
    va_start(ap, fmt);
    (void)SDL_vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    ImGui_TextUnformatted(line);
    gs_keep(line);
}

// A cell of the table: shown in its column, and its row is kept whole by
// the caller so the dump holds the terminal's own row.
static void gs_cell(const char *text) {
    ImGui_TableNextColumn();
    ImGui_TextUnformatted(text);
}

void gs_window_dump_print(void) {
    const char *p = gs_dump;
    while (*p != '\0') {
        const char *e = strchr(p, '\n');
        if (e == nullptr) e = p + strlen(p);
        printf("window: %.*s\n", (int)(e - p), p);
        p = *e == '\n' ? e + 1 : e;
    }
    fflush(stdout);
}

bool gs_window_shot(const char *path) {
    if (!gs_w.open) return false;
    SDL_Surface *frame = SDL_RenderReadPixels(gs_w.ren, nullptr);
    if (frame == nullptr) return false;
    const bool ok = SDL_SaveBMP(frame, path);
    SDL_DestroySurface(frame);
    return ok;
}

// --- opening and closing ------------------------------------------------------

bool gs_window_open(const char *icon_path) {
    if (gs_w.open) return true;

    // **No display is not an error.** A server on a machine with no screen
    // is the usual server; SDL says so here, and the caller logs it.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        SDL_Log("no window: %s", SDL_GetError());
        return false;
    }
    gs_w.video = true;

    gs_w.win = SDL_CreateWindow("gearstick server", GS_WINDOW_W, GS_WINDOW_H,
                                SDL_WINDOW_RESIZABLE);
    if (gs_w.win == nullptr) {
        SDL_Log("no window: %s", SDL_GetError());
        gs_window_close();
        return false;
    }
    gs_w.ren = SDL_CreateRenderer(gs_w.win, nullptr);
    if (gs_w.ren == nullptr) {
        SDL_Log("no window: %s", SDL_GetError());
        gs_window_close();
        return false;
    }

    // The game's icon, for the same reason the game has one: a window with
    // the toolkit's default icon is what an unfinished thing looks like.
    if (icon_path != nullptr) {
        SDL_Surface *icon = IMG_Load(icon_path);
        if (icon != nullptr) {
            SDL_SetWindowIcon(gs_w.win, icon);
            SDL_DestroySurface(icon);
        }
    }

    ImGui_CreateContext(nullptr);
    gs_style_menu();
    ImGuiIO *io = ImGui_GetIO();
    io->IniFilename = nullptr;
    if (!cImGui_ImplSDL3_InitForSDLRenderer(gs_w.win, gs_w.ren) ||
        !cImGui_ImplSDLRenderer3_Init(gs_w.ren)) {
        SDL_Log("no window: could not start Dear ImGui");
        ImGui_DestroyContext(nullptr);
        gs_window_close();
        return false;
    }

    if (gs_press_count > 0) {
        gs_ui_probe_start(gs_items, GS_ITEMS_MAX);
        gs_probing = true;
    }

    gs_w.open = true;
    gs_w.closed = false;
    return true;
}

void gs_window_close(void) {
    if (gs_probing) {
        gs_ui_probe_stop();
        gs_probing = false;
    }
    if (gs_w.open) {
        cImGui_ImplSDLRenderer3_Shutdown();
        cImGui_ImplSDL3_Shutdown();
        ImGui_DestroyContext(nullptr);
    }
    if (gs_w.ren != nullptr) SDL_DestroyRenderer(gs_w.ren);
    if (gs_w.win != nullptr) SDL_DestroyWindow(gs_w.win);
    if (gs_w.video) SDL_QuitSubSystem(SDL_INIT_VIDEO);
    gs_w.ren = nullptr;
    gs_w.win = nullptr;
    gs_w.video = false;
    gs_w.open = false;
}

bool gs_window_is_open(void) {
    return gs_w.open;
}

bool gs_window_pump(void) {
    if (!gs_w.open) return false;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        cImGui_ImplSDL3_ProcessEvent(&e);
        if (e.type == SDL_EVENT_QUIT ||
            e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            gs_w.closed = true;
        }
    }
    return !gs_w.closed;
}

// --- drawing ------------------------------------------------------------------

void gs_window_draw(const gs_srv_facts *f, const gs_srv_log *log,
                    gs_srv_ask *ask, bool dump) {
    ask->drop_slot = -1;
    ask->take_down = false;
    if (!gs_w.open) return;

    gs_dumping = dump;
    gs_dump_len = 0;
    gs_dump[0] = '\0';

    if (gs_probing) gs_ui_probe_frame();
    cImGui_ImplSDLRenderer3_NewFrame();
    cImGui_ImplSDL3_NewFrame();
    ImGui_NewFrame();

    // One panel the size of the window, and nothing else: the window is the
    // dashboard, not a desktop with a dashboard on it.
    const ImGuiViewport *vp = ImGui_GetMainViewport();
    ImGui_SetNextWindowPos(vp->WorkPos, ImGuiCond_Always);
    ImGui_SetNextWindowSize(vp->WorkSize, ImGuiCond_Always);
    ImGui_Begin("server", nullptr,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoSavedSettings);

    gs_say("gearstick server            port %u        up %llu:%02llu:%02llu",
           f->port, (unsigned long long)(f->up_s / 3600u),
           (unsigned long long)((f->up_s / 60u) % 60u),
           (unsigned long long)(f->up_s % 60u));
    ImGui_Separator();

    // The table the terminal draws, with a column the terminal has no room
    // for: what to do about a row.
    if (ImGui_BeginTable("clients", 8,
                         ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui_TableSetupColumn("", 0);
        ImGui_TableSetupColumn("driver", 0);
        ImGui_TableSetupColumn("from", 0);
        ImGui_TableSetupColumn("ping", 0);
        ImGui_TableSetupColumn("in", 0);
        ImGui_TableSetupColumn("out", 0);
        ImGui_TableSetupColumn("", 0);
        ImGui_TableSetupColumn("", 0);
        ImGui_TableHeadersRow();
        gs_keep("driver from ping in out");

        for (int i = 0; i < GS_PROTO_MAX_PLAYERS; i++) {
            const gs_srv_row *r = &f->row[i];
            if (i >= f->capacity) continue;
            char slot[8], in[16], out[16], line[192];
            SDL_snprintf(slot, sizeof slot, "%d", i);
            ImGui_TableNextRow();
            ImGui_PushIDInt(i);
            if (!r->used) {
                gs_cell(slot);
                gs_cell("-");
                gs_cell("");
                gs_cell("");
                gs_cell("");
                gs_cell("");
                gs_cell("");
                gs_cell("");
                SDL_snprintf(line, sizeof line, "%-3d %-16s %-22s %6s %8s %8s",
                             i, "-", "", "", "", "");
            } else {
                SDL_snprintf(in, sizeof in, "%u", r->in);
                SDL_snprintf(out, sizeof out, "%u", r->out);
                gs_cell(slot);
                gs_cell(r->name);
                gs_cell(r->from);
                gs_cell(r->ping);
                gs_cell(in);
                gs_cell(out);
                gs_cell(r->quiet ? "quiet" : "");
                ImGui_TableNextColumn();
                if (ImGui_SmallButton("drop")) ask->drop_slot = i;
                SDL_snprintf(line, sizeof line, "%-3d %-16s %-22s %6s %8u %8u%s",
                             i, r->name, r->from, r->ping, r->in, r->out,
                             r->quiet ? "  quiet" : "");
            }
            ImGui_PopID();
            gs_keep(line);
        }
        ImGui_EndTable();
    }
    ImGui_Separator();

    gs_say("%u of %u here, peak %u        refused %u", f->here, f->capacity,
           f->peak, f->refused);
    gs_say("datagrams  in %u (%s)   out %u (%s)   relayed %u", f->total_in,
           f->in_bytes, f->total_out, f->out_bytes, f->relayed);
    if (f->store) {
        gs_say("remembered %d driver(s), %d record(s), %d track(s)"
               "   results %u, kept %u",
               f->drivers, f->records, f->tracks, f->results, f->kept);
        if (f->rejected > 0) {
            gs_say("rejected   %u time(s) that the replay did not produce",
                   f->rejected);
        }
    }
    if (f->track) {
        gs_say("track      %016llx, %zu bytes, %u chunks sent",
               (unsigned long long)f->track_hash, f->track_len, f->chunks_sent);
        ImGui_SameLine();
        if (ImGui_SmallButton("take the track down")) ask->take_down = true;
    }
    if (f->up_s > 0) {
        gs_say("rate       %.1f in/s   %.1f out/s", f->in_rate, f->out_rate);
    }

    // What the terminal scrolls away. Newest at the bottom, and the view
    // follows the bottom.
    ImGui_SeparatorText("arrivals and departures");
    if (ImGui_BeginChild("log", (ImVec2){ 0.0f, -28.0f }, ImGuiChildFlags_Borders, 0)) {
        for (int i = 0; i < log->count; i++) {
            gs_say("%s", gs_srv_log_line(log, i));
        }
        if (ImGui_GetScrollY() >= ImGui_GetScrollMaxY()) ImGui_SetScrollHereY(1.0f);
    }
    ImGui_EndChild();
    ImGui_TextDisabled("close this window to stop the server");

    ImGui_End();

    ImGui_Render();
    gs_press_wanted();
    SDL_SetRenderDrawColor(gs_w.ren, 24, 24, 28, 255);
    SDL_RenderClear(gs_w.ren);
    cImGui_ImplSDLRenderer3_RenderDrawData(ImGui_GetDrawData(), gs_w.ren);
    SDL_RenderPresent(gs_w.ren);
}

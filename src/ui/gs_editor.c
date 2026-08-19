// gs_editor.c - see gs_editor.h.

#include <stdio.h>

#include "dcimgui.h"

#include "platform/gs_paths.h"
#include "ui/gs_editor.h"

#define GS_EDITOR_HISTORY 65536
#define GS_TRACK_FILENAME "current.gstrack"

bool gs_editor_init(gs_editor *e, uint32_t history) {
    *e = (gs_editor){ 0 };

    e->brush = GS_BRUSH_RAISE;
    e->surface = GS_SURF_PAVEMENT;
    e->gravity = 1.0f;
    e->radius = 1;
    e->step = 0.25f;
    e->gate_heading = 0.0f;
    e->gate_width = 2.5f;
    e->zoom = 1.0f;

    // Sixty-five thousand edits of history is about eight hundred kilobytes,
    // which is nothing, and is far more strokes than anyone makes between
    // saves. The cap exists because src/core/ owns no allocator, not because
    // the history is meant to run out.
    e->log = (gs_edit_log *)SDL_malloc(gs_edit_log_bytes(history));
    if (e->log == nullptr) return false;
    gs_edit_log_init(e->log, history);

    SDL_snprintf(e->status, sizeof e->status, "%s", "ready");
    return true;
}

void gs_editor_quit(gs_editor *e) {
    if (e->log != nullptr) SDL_free(e->log);
    e->log = nullptr;
}

void gs_editor_toggle(gs_editor *e, const gs_view *view) {
    if (!e->active) {
        // Start looking where the race was looking. Anything else moves the
        // world out from under the player at the moment they switch.
        e->cam_x = view->cam.cx;
        e->cam_y = view->cam.cy;
        e->zoom = view->cam.zoom > 0.0f ? view->cam.zoom : 1.0f;
    }
    e->active = !e->active;
}

// One application of the brush at a tile. Corners and tiles are indexed
// differently - a 4x4 track has 5x5 corners - so raising ground and painting a
// surface do not walk the same rectangle.
static void gs_brush_at(gs_editor *e, gs_track *t, int tx, int ty) {
    switch ((gs_brush)e->brush) {
    case GS_BRUSH_RAISE:
    case GS_BRUSH_LOWER: {
        if (tx < 0 || ty < 0 || tx > (int)t->w || ty > (int)t->h) return;
        gs_fix now = gs_track_height(t, GS_INT(tx), GS_INT(ty));
        gs_fix delta = (gs_fix)(e->step * (float)GS_ONE);
        gs_fix want = e->brush == GS_BRUSH_RAISE ? now + delta : now - delta;
        gs_edit_corner(e->log, t, (uint8_t)tx, (uint8_t)ty, want);
        break;
    }
    case GS_BRUSH_SURFACE:
        if (tx < 0 || ty < 0 || tx >= (int)t->w || ty >= (int)t->h) return;
        gs_edit_surface(e->log, t, (uint8_t)tx, (uint8_t)ty, (gs_surface)e->surface);
        break;
    case GS_BRUSH_GRAVITY:
        if (tx < 0 || ty < 0 || tx >= (int)t->w || ty >= (int)t->h) return;
        gs_edit_gravity(e->log, t, (uint8_t)tx, (uint8_t)ty,
                        (gs_fix)(e->gravity * (float)GS_ONE));
        break;
    case GS_BRUSH_GATE:
    case GS_BRUSH_COUNT:
        break;
    }
}

void gs_editor_paint(gs_editor *e, gs_track *t, float wx, float wy) {
    // A gate is placed, not painted: it goes where the pointer is rather than
    // over a disc of tiles, and one click makes one of them.
    if (e->brush == GS_BRUSH_GATE) {
        gs_angle heading = (gs_angle)(int32_t)(e->gate_heading / 360.0f * 65536.0f);
        int at = gs_track_add_gate(t, (gs_fix)(wx * (float)GS_ONE),
                                   (gs_fix)(wy * (float)GS_ONE), heading,
                                   (gs_fix)(e->gate_width * (float)GS_ONE));
        if (at < 0) {
            SDL_snprintf(e->status, sizeof e->status,
                         "the route is full at %d gates", GS_TRACK_MAX_GATES);
        } else {
            SDL_snprintf(e->status, sizeof e->status, "%s %d",
                         at == 0 ? "placed the start line, gate" : "placed gate", at);
        }
        return;
    }

    int cx = (int)SDL_floorf(wx);
    int cy = (int)SDL_floorf(wy);
    int r = e->radius;

    for (int y = cy - r; y <= cy + r; y++) {
        for (int x = cx - r; x <= cx + r; x++) {
            // Round, not square: a square brush leaves corners in the terrain
            // that nobody drew and everybody then has to sand off.
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy > r * r + r) continue;
            gs_brush_at(e, t, x, y);
        }
    }
}

static void gs_brush_stroke(gs_editor *e, gs_track *t) {
    gs_editor_paint(e, t, e->hover_x, e->hover_y);
}

static const char *const gs_surface_names[] = { "pavement", "dirt", "ice" };

static void gs_editor_palette(gs_editor *e, gs_track *t) {
    // Placed and sized explicitly rather than left to auto-fit. Two reasons,
    // and the second one cost an hour: a tool panel should appear in the same
    // corner every time rather than wherever the layout algorithm put it, and
    // **an auto-fitting window is hidden for its first frame** while ImGui
    // measures it - so a one-frame capture of an auto-sized panel renders
    // nothing at all, which looks exactly like the panel being broken.
    ImGui_SetNextWindowPos((ImVec2){ 16.0f, 16.0f }, ImGuiCond_FirstUseEver);
    ImGui_SetNextWindowSize((ImVec2){ 340.0f, 460.0f }, ImGuiCond_FirstUseEver);

    if (!ImGui_Begin("Construction set", nullptr, 0)) {
        ImGui_End();
        return;
    }

    ImGui_SeparatorText("Brush");
    ImGui_RadioButtonIntPtr("raise", &e->brush, GS_BRUSH_RAISE);
    ImGui_SameLine();
    ImGui_RadioButtonIntPtr("lower", &e->brush, GS_BRUSH_LOWER);
    ImGui_SameLine();
    ImGui_RadioButtonIntPtr("surface", &e->brush, GS_BRUSH_SURFACE);
    ImGui_SameLine();
    ImGui_RadioButtonIntPtr("gravity", &e->brush, GS_BRUSH_GRAVITY);
    // Second row: five of these do not fit across the panel, and a control
    // clipped at the edge is a control nobody finds.
    ImGui_RadioButtonIntPtr("gate", &e->brush, GS_BRUSH_GATE);

    if (e->brush != GS_BRUSH_GATE) ImGui_SliderInt("radius", &e->radius, 0, 8);

    if (e->brush == GS_BRUSH_RAISE || e->brush == GS_BRUSH_LOWER) {
        ImGui_SliderFloat("step (tiles)", &e->step, 0.05f, 2.0f);
    } else if (e->brush == GS_BRUSH_SURFACE) {
        ImGui_ComboChar("surface", &e->surface, gs_surface_names, GS_SURF_COUNT);
    } else if (e->brush == GS_BRUSH_GATE) {
        // The route. Gate zero is the start and the finish; the rest say which
        // way round, in the order they were placed.
        ImGui_SliderFloat("heading (deg)", &e->gate_heading, 0.0f, 359.0f);
        ImGui_SliderFloat("half width", &e->gate_width, 0.5f, 8.0f);
        ImGui_Text("click to place gate %u", t->gate_count);

        for (uint8_t i = 0; i < t->gate_count; i++) {
            char label[32];
            SDL_snprintf(label, sizeof label, "remove##%u", i);
            ImGui_Text("%u: %.1f, %.1f", i, (double)gs_to_f(t->gate[i].x),
                       (double)gs_to_f(t->gate[i].y));
            ImGui_SameLine();
            if (ImGui_Button(label)) {
                gs_track_remove_gate(t, i);
                break;
            }
        }
    } else {
        // The gravity brush. The presets are named because "Jupiter" tells a
        // player something a number does not, and the slider is continuous
        // because the original's fourteen steps were a 6502 limitation.
        ImGui_SliderFloat("gravity (x)", &e->gravity, 0.0f, 3.9f);
        ImGui_Text("%s", e->gravity < 0.1f  ? "almost nothing"
                         : e->gravity < 0.3f ? "Moon-ish"
                         : e->gravity < 0.6f ? "Mars-ish"
                         : e->gravity < 1.4f ? "Earth-ish"
                                             : "Jupiter-ish");
    }

    ImGui_SeparatorText("History");
    uint32_t undo = gs_edit_undo_depth(e->log);
    uint32_t redo = gs_edit_redo_depth(e->log);

    if (undo == 0) ImGui_BeginDisabled(true);
    if (ImGui_Button("undo")) gs_edit_undo(e->log, t);
    if (undo == 0) ImGui_EndDisabled();
    ImGui_SameLine();
    if (redo == 0) ImGui_BeginDisabled(true);
    if (ImGui_Button("redo")) gs_edit_redo(e->log, t);
    if (redo == 0) ImGui_EndDisabled();
    ImGui_SameLine();
    ImGui_Text("%u back, %u forward", undo, redo);

    ImGui_SeparatorText("Track");
    ImGui_Text("%u x %u tiles, %zu bytes", t->w, t->h, gs_track_size(t));
    // Identity by content: this number *is* the track, which is what lets two
    // people who built the same thing share times without a server agreeing.
    ImGui_Text("id %016llx", (unsigned long long)gs_track_hash(t));

    if (ImGui_Button("save")) gs_editor_save(e, t);
    ImGui_SameLine();
    if (ImGui_Button("load")) gs_editor_load(e, t);

    ImGui_SeparatorText("");
    ImGui_Text("%s", e->status);
    ImGui_Text("Tab races it. Arrows pan. Drag to paint.");

    ImGui_End();
}

void gs_editor_frame(gs_editor *e, gs_track *t, const gs_view *view) {
    gs_editor_palette(e, t);

    ImGuiIO *io = ImGui_GetIO();

    // Where the pointer is over the world. Asked every frame rather than only
    // while painting, because the cursor has to show what the brush would do
    // before it is committed to doing it.
    ImVec2 mouse = ImGui_GetMousePos();
    gs_camera cam = view->cam;
    cam.vw = (float)view->rect.w;
    cam.vh = (float)view->rect.h;
    e->hover_on = gs_iso_pick(&cam, t, mouse.x - (float)view->rect.x,
                              mouse.y - (float)view->rect.y,
                              &e->hover_x, &e->hover_y);

    // The palette is a window over the world, so a click on it is not a click
    // on the track. WantCaptureMouse is how ImGui says which.
    if (io->WantCaptureMouse) {
        if (e->stroke) {
            gs_edit_end(e->log);
            e->stroke = false;
        }
        return;
    }

    // A drag is one undo step, however many tiles it touched.
    if (ImGui_IsMouseClicked(ImGuiMouseButton_Left) && e->hover_on) {
        gs_edit_begin(e->log);
        e->stroke = true;
    }
    if (e->stroke && ImGui_IsMouseDown(ImGuiMouseButton_Left)) {
        gs_brush_stroke(e, t);
    }
    if (e->stroke && !ImGui_IsMouseDown(ImGuiMouseButton_Left)) {
        gs_edit_end(e->log);
        e->stroke = false;
    }
}

void gs_editor_draw_cursor(const gs_editor *e, SDL_Renderer *ren,
                           const gs_track *t, const gs_view *view) {
    SDL_SetRenderViewport(ren, &view->rect);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 255, 245, 180, 220);

    gs_camera cam = view->cam;
    cam.vw = (float)view->rect.w;
    cam.vh = (float)view->rect.h;

    // Outline every tile the brush would touch, on the ground rather than flat
    // on the screen - so the cursor climbs a ramp with the terrain and reads as
    // being *on* the track rather than drawn over a picture of it.
    //
    // "No cursor" is expressed by not drawing one. Saying it with a radius of
    // -1 looks tidier and is a trap: `cy + r` then underflows past INT_MIN and
    // the loop runs four billion times.
    if (e->hover_on && e->brush != GS_BRUSH_GATE) {
        int cx = (int)SDL_floorf(e->hover_x);
        int cy = (int)SDL_floorf(e->hover_y);
        int r = e->radius;

        for (int y = cy - r; y <= cy + r; y++) {
            for (int x = cx - r; x <= cx + r; x++) {
                int dx = x - cx, dy = y - cy;
                if (dx * dx + dy * dy > r * r + r) continue;
                if (x < 0 || y < 0 || x >= (int)t->w || y >= (int)t->h) continue;

                static const int ox[5] = { 0, 1, 1, 0, 0 };
                static const int oy[5] = { 0, 0, 1, 1, 0 };
                SDL_FPoint p[5];
                for (int i = 0; i < 5; i++) {
                    gs_fix wx = GS_INT(x + ox[i]);
                    gs_fix wy = GS_INT(y + oy[i]);
                    gs_fix wz = gs_track_height(t, wx, wy);
                    gs_iso_project(&cam, gs_to_f(wx), gs_to_f(wy), gs_to_f(wz) + 0.02f,
                                   &p[i].x, &p[i].y);
                }
                SDL_RenderLines(ren, p, 5);
            }
        }
    }
    // The route, drawn as the lines cars have to cross. Gate zero is the start
    // and is drawn brighter, because "which one is the finish" is the first
    // thing anyone asks of a track they did not build.
    for (uint8_t i = 0; i < t->gate_count; i++) {
        const gs_gate *g = &t->gate[i];

        float fx = gs_to_f(gs_cos(g->heading));
        float fy = gs_to_f(gs_sin(g->heading));
        float gx = gs_to_f(g->x), gy = gs_to_f(g->y);
        float hw = gs_to_f(g->half_width);

        // Across the direction of travel: that is what a car drives through.
        float ax = gx + fy * hw, ay = gy - fx * hw;
        float bx = gx - fy * hw, by = gy + fx * hw;

        gs_fix az = gs_track_height(t, (gs_fix)(ax * (float)GS_ONE), (gs_fix)(ay * (float)GS_ONE));
        gs_fix bz = gs_track_height(t, (gs_fix)(bx * (float)GS_ONE), (gs_fix)(by * (float)GS_ONE));

        SDL_FPoint line[2];
        gs_iso_project(&cam, ax, ay, gs_to_f(az) + 0.05f, &line[0].x, &line[0].y);
        gs_iso_project(&cam, bx, by, gs_to_f(bz) + 0.05f, &line[1].x, &line[1].y);

        if (i == 0) SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
        else SDL_SetRenderDrawColor(ren, 120, 220, 255, 210);
        SDL_RenderLines(ren, line, 2);

        // A stub pointing the way through, so a gate placed backwards is
        // obvious while building rather than while racing.
        gs_fix cz = gs_track_height(t, g->x, g->y);
        SDL_FPoint arrow[2];
        gs_iso_project(&cam, gx, gy, gs_to_f(cz) + 0.05f, &arrow[0].x, &arrow[0].y);
        gs_iso_project(&cam, gx + fx * 1.2f, gy + fy * 1.2f, gs_to_f(cz) + 0.05f,
                       &arrow[1].x, &arrow[1].y);
        SDL_RenderLines(ren, arrow, 2);
    }

    SDL_SetRenderViewport(ren, nullptr);
}

static bool gs_track_path(char *out, size_t cap) {
    const char *dir = gs_pref_dir();
    if (dir == nullptr) return false;
    SDL_snprintf(out, cap, "%s%s", dir, GS_TRACK_FILENAME);
    return true;
}

bool gs_editor_save(gs_editor *e, const gs_track *t) {
    char path[1024];
    if (!gs_track_path(path, sizeof path)) {
        SDL_snprintf(e->status, sizeof e->status, "%s", "no writable directory");
        return false;
    }

    static uint8_t buf[GS_TRACK_TILES * 4 + 4096];
    size_t n = gs_track_serialize(t, buf, sizeof buf);
    if (n == 0 || !SDL_SaveFile(path, buf, n)) {
        SDL_snprintf(e->status, sizeof e->status, "save failed: %s", SDL_GetError());
        return false;
    }

    SDL_snprintf(e->status, sizeof e->status, "saved %zu bytes to %s", n, path);
    return true;
}

bool gs_editor_load(gs_editor *e, gs_track *t) {
    char path[1024];
    if (!gs_track_path(path, sizeof path)) return false;

    size_t n = 0;
    void *data = SDL_LoadFile(path, &n);
    if (data == nullptr) {
        SDL_snprintf(e->status, sizeof e->status, "nothing saved yet");
        return false;
    }

    bool ok = gs_track_deserialize(t, (const uint8_t *)data, n);
    SDL_free(data);

    if (!ok) {
        // The track is untouched - see gs_track_deserialize. A refused file
        // leaves you editing what you were editing.
        SDL_snprintf(e->status, sizeof e->status, "%s", "that file is not a track");
        return false;
    }

    // The history describes edits to a track that is no longer here.
    gs_edit_log_init(e->log, e->log->cap);
    SDL_snprintf(e->status, sizeof e->status, "loaded %zu bytes, id %016llx", n,
                 (unsigned long long)gs_track_hash(t));
    return true;
}

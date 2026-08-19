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
    e->zoom = GS_ISO_DEFAULT_ZOOM;
    e->ghost_on = true;
    e->rebind_player = -1;
    e->rebind_action = -1;
    e->dial_gravity = 1.0f;
    e->dial_drag = 1.0f;
    e->dial_friction = 1.0f;
    e->dial_damage = 1.0f;

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
    if (!e->active && !e->placed) {
        // The first time in, start looking where the race was looking - anything
        // else moves the world out from under the player as they switch.
        //
        // Only the first time, though. Coming back from a test drive has to put
        // you where you were *building*, not where the car happened to stop.
        // That is what makes it a snap back rather than a journey home.
        e->cam_x = view->cam.cx;
        e->cam_y = view->cam.cy;
        e->zoom = view->cam.zoom > 0.0f ? view->cam.zoom : GS_ISO_DEFAULT_ZOOM;
        e->placed = true;
    }
    e->active = !e->active;
}

bool gs_editor_drive_start(const gs_editor *e, const gs_track *t,
                           gs_fix *x, gs_fix *y, gs_angle *heading) {
    if (e->hover_on) {
        *x = (gs_fix)(e->hover_x * (float)GS_ONE);
        *y = (gs_fix)(e->hover_y * (float)GS_ONE);
        // Pointing the way the start line does, so a drive from the middle of
        // the track still faces the way the track goes.
        *heading = t->gate_count > 0 ? t->gate[0].heading : (gs_angle)0;
        return true;
    }

    if (t->gate_count > 0) {
        *x = t->gate[0].x;
        *y = t->gate[0].y;
        *heading = t->gate[0].heading;
        return true;
    }

    return false;
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

// How long a ghost run lasts before it starts over. Twelve seconds is enough to
// clear any jump on a track this size and short enough that a change shows up
// again quickly.
#define GS_GHOST_TICKS (GS_TICK_HZ * 12)

void gs_editor_apply_dials(const gs_editor *e, gs_world *w) {
    gs_world_init(w, (gs_fix)(e->dial_gravity * (float)GS_ONE));
    w->drag_scale = (gs_fix)(e->dial_drag * (float)GS_ONE);
    w->friction_scale = (gs_fix)(e->dial_friction * (float)GS_ONE);
    w->damage_scale = (gs_fix)(e->dial_damage * (float)GS_ONE);
}

static void gs_ghost_restart(gs_editor *e, const gs_track *t) {
    e->ghost_track = gs_track_hash(t);
    e->ghost_ticks = 0;
    gs_editor_apply_dials(e, &e->ghost);

    if (t->gate_count == 0) return;
    gs_world_add_car(&e->ghost, t, (uint8_t)GS_VEH_STOCK_CAR,
                     t->gate[0].x, t->gate[0].y, t->gate[0].heading);
}

void gs_editor_ghost_step(gs_editor *e, const gs_track *t, uint32_t ticks) {
    if (!e->ghost_on) return;

    // The track changing is what restarts it, and the track's own hash is how
    // that is noticed - no notification to forget to send, and no way for an
    // edit to slip past.
    // A dial moving has to restart it too. The track's hash does not change
    // when gravity does, and a ghost still flying under the old one is worse
    // than no ghost: it is a wrong answer that looks like a right one.
    uint64_t under = gs_track_hash(t)
                     ^ (uint64_t)(uint32_t)e->ghost.gravity
                     ^ ((uint64_t)(uint32_t)e->ghost.drag_scale << 8)
                     ^ ((uint64_t)(uint32_t)e->ghost.friction_scale << 16)
                     ^ ((uint64_t)(uint32_t)e->ghost.damage_scale << 24);
    gs_world want;
    gs_editor_apply_dials(e, &want);
    uint64_t wanted = gs_track_hash(t)
                      ^ (uint64_t)(uint32_t)want.gravity
                      ^ ((uint64_t)(uint32_t)want.drag_scale << 8)
                      ^ ((uint64_t)(uint32_t)want.friction_scale << 16)
                      ^ ((uint64_t)(uint32_t)want.damage_scale << 24);

    if (under != wanted || gs_track_hash(t) != e->ghost_track ||
        e->ghost.car_count == 0) {
        gs_ghost_restart(e, t);
        if (e->ghost.car_count == 0) return;
    }

    for (uint32_t i = 0; i < ticks; i++) {
        if (e->ghost_ticks >= GS_GHOST_TICKS) {
            gs_ghost_restart(e, t);
            if (e->ghost.car_count == 0) return;
        }
        // Throttle and nothing else. There is no AI yet, and a ghost that drives
        // straight and fast is exactly what answers "what does this ramp do" -
        // which is the question being asked while a ramp is being built.
        gs_input in[GS_MAX_CARS] = { GS_IN_ACCEL, 0, 0, 0 };
        gs_world_step(&e->ghost, t, in);
        e->ghost_ticks++;
    }
}

const gs_car *gs_editor_ghost_car(const gs_editor *e) {
    if (!e->ghost_on || e->ghost.car_count == 0) return nullptr;
    return &e->ghost.car[0];
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

    ImGui_SeparatorText("Dials");
    // Continuous, not stepped. The original's fourteen gravity settings were a
    // 6502 limitation; the planet names were doing real work and are kept as
    // shortcuts rather than as the only choices.
    ImGui_SliderFloat("gravity (x Earth)", &e->dial_gravity, 0.0f, 3.0f);
    for (int i = 0; i < GS_GRAVITY_PRESETS; i++) {
        if (i % 4 != 0) ImGui_SameLine();
        if (ImGui_Button(gs_gravity_presets[i].name)) {
            e->dial_gravity = gs_to_f(gs_gravity_presets[i].scale);
        }
    }
    ImGui_SliderFloat("air drag", &e->dial_drag, 0.0f, 4.0f);
    ImGui_SliderFloat("friction", &e->dial_friction, 0.0f, 2.0f);
    ImGui_SliderFloat("damage", &e->dial_damage, 0.0f, 4.0f);

    ImGui_Checkbox("live ghost", &e->ghost_on);
    ImGui_SameLine();
    ImGui_Text("%s", t->gate_count > 0 ? "" : "(needs a start line)");

    ImGui_SeparatorText("Route");
    // Continuously, rather than on demand. A check you have to ask for is a
    // check you ask for once the track is finished, which is the worst moment
    // to find out the start line hangs off the edge.
    gs_track_issue issue = gs_track_validate(t);
    if (issue.problem == GS_TRACK_OK) {
        ImGui_Text("%u gates: %s", t->gate_count, gs_track_problem_text(issue.problem));
    } else if (issue.other >= 0) {
        ImGui_Text("PROBLEM: %s (gates %d and %d)",
                   gs_track_problem_text(issue.problem), issue.gate, issue.other);
    } else if (issue.gate >= 0) {
        ImGui_Text("PROBLEM: %s (gate %d)",
                   gs_track_problem_text(issue.problem), issue.gate);
    } else {
        ImGui_Text("PROBLEM: %s", gs_track_problem_text(issue.problem));
    }

    ImGui_SeparatorText("Track");
    ImGui_Text("%u x %u tiles, %zu bytes", t->w, t->h, gs_track_size(t));
    // Identity by content: this number *is* the track, which is what lets two
    // people who built the same thing share times without a server agreeing.
    ImGui_Text("id %016llx", (unsigned long long)gs_track_hash(t));

    if (ImGui_Button("save")) gs_editor_save(e, t);
    ImGui_SameLine();
    if (ImGui_Button("load")) gs_editor_load(e, t);

    ImGui_Checkbox("controls...", &e->show_controls);

    ImGui_SeparatorText("");
    ImGui_Text("%s", e->status);
    ImGui_Text("Tab races it. Arrows pan. Drag to paint.");

    ImGui_End();
}

// How fast the pad drives the cursor across the track, in tiles a second. Slow
// enough to land on the tile you meant, fast enough to cross a track without
// putting the pad down.
#define GS_PAD_CURSOR_SPEED 9.0f

bool gs_editor_pad_input(gs_editor *e, gs_track *t, const gs_pad_edit *pad, float dt) {
    if (!pad->present) return false;

    if (pad->x != 0.0f || pad->y != 0.0f) {
        // Screen-relative, not world-relative. Pushing the stick right has to
        // move the cursor right *on the screen*, and in an isometric view that
        // is a diagonal in the world - anything else is unusable within
        // seconds.
        float step = GS_PAD_CURSOR_SPEED * dt;
        e->hover_x += (pad->x + pad->y) * step * 0.5f;
        e->hover_y += (pad->y - pad->x) * step * 0.5f;

        e->hover_x = GS_CLAMP(e->hover_x, 0.0f, (float)t->w - 0.01f);
        e->hover_y = GS_CLAMP(e->hover_y, 0.0f, (float)t->h - 0.01f);
        e->hover_on = true;

        // The camera follows rather than the cursor running off the edge of it.
        e->cam_x = e->hover_x;
        e->cam_y = e->hover_y;
    }

    if (pad->zoom != 0.0f) {
        e->zoom = GS_CLAMP(e->zoom - pad->zoom * dt, 0.4f, 3.0f);
    }

    if (pad->next_brush) {
        e->brush = (e->brush + 1) % GS_BRUSH_COUNT;
    }
    if (pad->undo) gs_edit_undo(e->log, t);
    if (pad->redo) gs_edit_redo(e->log, t);

    // A held button is one stroke, exactly as a held mouse button is.
    if (pad->paint && !e->stroke) {
        gs_edit_begin(e->log);
        e->stroke = true;
    }
    if (pad->paint) gs_editor_paint(e, t, e->hover_x, e->hover_y);
    if (!pad->paint && e->stroke) {
        gs_edit_end(e->log);
        e->stroke = false;
    }

    return pad->drive;
}

// The name of whatever a control is currently pointed at, for the panel.
static void gs_bind_label(const gs_bindings *b, uint8_t player, gs_action a,
                          char *out, size_t cap) {
    const char *key = b->key[player][a] == GS_KEY_NONE
                          ? nullptr
                          : SDL_GetScancodeName(b->key[player][a]);
    if (b->button[player][a] == GS_BUTTON_NONE) {
        SDL_snprintf(out, cap, "%s", key != nullptr && key[0] != '\0' ? key : "unset");
        return;
    }
    const char *btn = SDL_GetGamepadStringForButton(
        (SDL_GamepadButton)b->button[player][a]);
    if (key != nullptr && key[0] != '\0') {
        SDL_snprintf(out, cap, "%s / %s", key, btn != nullptr ? btn : "?");
    } else {
        SDL_snprintf(out, cap, "%s", btn != nullptr ? btn : "?");
    }
}

// While a control is waiting to be told what it is, the next key or pad button
// pressed becomes it. Escape leaves it alone, because changing your mind has to
// be possible without binding something by accident.
static void gs_capture_rebind(gs_editor *e, gs_input_state *input) {
    if (e->rebind_player < 0) return;

    int count = 0;
    const bool *keys = SDL_GetKeyboardState(&count);
    if (keys != nullptr) {
        if (keys[SDL_SCANCODE_ESCAPE]) {
            e->rebind_player = -1;
            e->rebind_action = -1;
            SDL_snprintf(e->status, sizeof e->status, "%s", "left alone");
            return;
        }
        for (int k = 0; k < count; k++) {
            if (!keys[k]) continue;
            gs_bind_set_key(&input->bind, (uint8_t)e->rebind_player,
                            (gs_action)e->rebind_action, (SDL_Scancode)k);
            SDL_snprintf(e->status, sizeof e->status, "bound to %s",
                         SDL_GetScancodeName((SDL_Scancode)k));
            e->rebind_player = -1;
            e->rebind_action = -1;
            return;
        }
    }

    for (int i = 0; i < input->pads; i++) {
        if (input->pad[i] == nullptr) continue;
        for (int btn = 0; btn < SDL_GAMEPAD_BUTTON_COUNT && btn < 32; btn++) {
            if (!SDL_GetGamepadButton(input->pad[i], (SDL_GamepadButton)btn)) continue;
            gs_bind_set_button(&input->bind, (uint8_t)e->rebind_player,
                               (gs_action)e->rebind_action, (int16_t)btn);
            SDL_snprintf(e->status, sizeof e->status, "bound to %s",
                         SDL_GetGamepadStringForButton((SDL_GamepadButton)btn));
            e->rebind_player = -1;
            e->rebind_action = -1;
            return;
        }
    }
}

static void gs_controls_panel(gs_editor *e, gs_input_state *input) {
    if (!e->show_controls) return;

    ImGui_SetNextWindowPos((ImVec2){ 380.0f, 16.0f }, ImGuiCond_FirstUseEver);
    ImGui_SetNextWindowSize((ImVec2){ 420.0f, 420.0f }, ImGuiCond_FirstUseEver);

    if (!ImGui_Begin("Controls", &e->show_controls, 0)) {
        ImGui_End();
        return;
    }

    ImGui_Text("Click a control, then press what you want it to be.");
    ImGui_Text("Escape leaves it alone.");
    ImGui_Separator();

    for (uint8_t p = 0; p < GS_MAX_CARS; p++) {
        char header[32];
        SDL_snprintf(header, sizeof header, "player %u", p + 1);
        ImGui_SeparatorText(header);

        for (int a = 0; a < GS_ACT_COUNT; a++) {
            char label[96], id[128];
            gs_bind_label(&input->bind, p, (gs_action)a, label, sizeof label);

            bool waiting = e->rebind_player == (int)p && e->rebind_action == a;
            SDL_snprintf(id, sizeof id, "%-11s %s##%u_%d",
                         gs_action_name((gs_action)a),
                         waiting ? "press something..." : label, p, a);

            if (ImGui_Button(id)) {
                e->rebind_player = (int)p;
                e->rebind_action = a;
            }
        }
    }

    ImGui_Separator();
    if (ImGui_Button("save")) {
        SDL_snprintf(e->status, sizeof e->status, "%s",
                     gs_input_save_bindings(input) ? "controls saved"
                                                   : "could not save controls");
    }
    ImGui_SameLine();
    if (ImGui_Button("defaults")) {
        gs_bind_defaults(&input->bind);
        SDL_snprintf(e->status, sizeof e->status, "%s", "controls back to defaults");
    }

    ImGui_End();
}

void gs_editor_frame(gs_editor *e, gs_track *t, const gs_view *view,
                     gs_input_state *input) {
    gs_editor_palette(e, t);
    if (input != nullptr) {
        gs_controls_panel(e, input);
        gs_capture_rebind(e, input);
    }

    ImGuiIO *io = ImGui_GetIO();

    // Where the pointer is over the world. Asked every frame rather than only
    // while painting, because the cursor has to show what the brush would do
    // before it is committed to doing it.
    ImVec2 mouse = ImGui_GetMousePos();
    gs_camera cam = view->cam;
    cam.vw = (float)view->rect.w;
    cam.vh = (float)view->rect.h;

    // Only when the pointer actually moved. Otherwise a stationary mouse drags
    // the cursor back every frame and the pad cannot move it anywhere - the two
    // fight, and the pad loses sixty times a second.
    if (mouse.x != e->last_mouse_x || mouse.y != e->last_mouse_y) {
        e->last_mouse_x = mouse.x;
        e->last_mouse_y = mouse.y;
        e->hover_on = gs_iso_pick(&cam, t, mouse.x - (float)view->rect.x,
                                  mouse.y - (float)view->rect.y,
                                  &e->hover_x, &e->hover_y);
    }

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

// gs_hud.c - see gs_hud.h.

#include "ui/gs_hud.h"

#include "dcimgui.h"
#include "ui/gs_menu.h"
#include "ui/gs_style.h"

#include <SDL3/SDL.h>

// Inset from the corner of the view it belongs to. Far enough in that a car at
// the edge of the screen is not under it, close enough that it reads as part of
// the frame rather than a dialog somebody left open.
#define GS_HUD_PAD 10.0f

// Wide enough for "position" under a two-digit-over-two-digit number, which is
// the widest thing in here.
#define GS_HUD_W 132.0f

// How much bigger than the body text the two kinds of number are drawn.
#define GS_HUD_BIG   2.2f
#define GS_HUD_SMALL 1.5f

// A number and what it is, the number large and the label small under it. The
// eye reads the number at a glance and only reads the label the first time,
// which is what the whole thing has to survive: it is looked at while driving.
static void gs_hud_stat(const char *label, const char *value, float scale) {
    float x = ImGui_GetCursorPosX();

    ImGui_SetWindowFontScale(scale);
    ImGui_TextUnformatted(value);
    ImGui_SetWindowFontScale(1.0f);

    ImGui_SetCursorPosX(x);
    float r, g, b;
    gs_style_accent(&r, &g, &b);
    ImGui_PushStyleColorImVec4(ImGuiCol_Text, (ImVec4){ r, g, b, 0.85f });
    ImGui_TextUnformatted(label);
    ImGui_PopStyleColor();
}

// What did not fit in the panel last time it was drawn - see gs_hud_overflow.
static float gs_hud_hidden = 0.0f;

// The damage bar. A number from 0 to 255 means nothing to a driver; a bar that
// is running out means something immediately, and the colour says how worried to
// be without anybody having to read it.
static void gs_hud_damage(const gs_car *c, float width) {
    float left = 1.0f - (float)c->damage / 255.0f;
    if (left < 0.0f) left = 0.0f;

    // Green through amber to red, so a glance is enough.
    ImVec4 tint = { 0.35f + (1.0f - left) * 0.55f, 0.30f + left * 0.55f,
                    0.22f, 1.0f };
    if (c->wrecked) tint = (ImVec4){ 0.60f, 0.16f, 0.16f, 1.0f };

    ImGui_PushStyleColorImVec4(ImGuiCol_PlotHistogram, tint);
    ImGui_ProgressBar(left, (ImVec2){ width, 8.0f }, "");
    ImGui_PopStyleColor();

    float r, g, b;
    gs_style_accent(&r, &g, &b);
    if (c->wrecked) {
        ImGui_PushStyleColorImVec4(ImGuiCol_Text,
                                   (ImVec4){ 0.95f, 0.35f, 0.30f, 1.0f });
        ImGui_TextUnformatted("YOU DIED");
        ImGui_PopStyleColor();
    } else {
        ImGui_PushStyleColorImVec4(ImGuiCol_Text, (ImVec4){ r, g, b, 0.85f });
        ImGui_TextUnformatted("condition");
        ImGui_PopStyleColor();
    }
}

// **A wreck is not the end of the session, and the screen has to say so.** A
// car that cannot move is in a race that will never finish, so nothing takes
// the player anywhere and the HUD is the only thing still talking to them.
// Escape is the way out of a race wherever it is being raced - the setup screen
// on this machine, the lobby when the race belongs to a server - so that is
// what this says, without having to be told which it is.
static void gs_hud_way_out(bool online) {
    ImGui_PushStyleColorImVec4(ImGuiCol_Text,
                               ImGui_GetStyle()->Colors[ImGuiCol_TextDisabled]);
    // **Both ways out, named by the key that does them.** "You are dead" with
    // nothing after it is half a message: the question it leaves is what to do
    // now, and the answer is two keys. Restarting is not offered in a race
    // other people are in, because it is not one machine's to restart.
    // Short enough to fit the panel, which is narrow on purpose: it is read
    // while driving. "R" and "Esc" are the whole message.
    if (!online) ImGui_TextUnformatted("R  restart");
    ImGui_TextUnformatted(online ? "Esc  lobby" : "Esc  menu");
    ImGui_PopStyleColor();
}

void gs_hud_draw(const gs_world *w, const gs_track *t, const gs_view *v,
                 uint32_t tick, float waited, bool online) {
    if (w == nullptr || t == nullptr || v == nullptr) return;
    if (v->car >= w->car_count) return;

    const gs_car *c = &w->car[v->car];
    if (!c->active) return;

    // Clipped to its own view, so four players get four HUDs and none of them
    // is reading somebody else's.
    ImGui_SetNextWindowPos((ImVec2){ (float)v->rect.x + GS_HUD_PAD,
                                     (float)v->rect.y + GS_HUD_PAD },
                           ImGuiCond_Always);
    ImGui_SetNextWindowBgAlpha(0.45f);

    // **Sized here rather than auto-fitted.** An auto-resizing ImGui window is
    // invisible for its first frame, and a screenshot is one frame - so the HUD
    // would be on screen for a player and absent from every capture, which is
    // both a verification that cannot work and a bug nobody would notice. The
    // same trap cost an afternoon on the editor's palette; see gs_menu.c.
    float line = ImGui_GetTextLineHeight();
    const ImGuiStyle *st = ImGui_GetStyle();

    // **The rows this is about to draw, listed in the order it draws them.**
    // The size and the contents are one list rather than two that have to be
    // kept in step - which they were not: every state was a little short, the
    // plain one by eleven pixels, and the wreck message by a whole line. What
    // is drawn below adds a row here, or the test that renders every state and
    // asks what did not fit will say so.
    float row_big = line * GS_HUD_BIG + line + st->ItemSpacing.y * 2.0f;
    float row_small = line * GS_HUD_SMALL + line + st->ItemSpacing.y * 2.0f;

    float height = st->WindowPadding.y * 2.0f
                 + row_big * 2.0f          // position, lap
                 + row_small * 2.0f        // this lap, best
                 + 8.0f + st->ItemSpacing.y * 2.0f + line   // the bar, labelled
                 // And the four deliberate gaps between those groups. An
                 // ImGui_Spacing() is an item of no height that still costs a
                 // gap, and forgetting them is the whole of the twenty-nine
                 // pixels this panel was short in every state it has.
                 + st->ItemSpacing.y * 4.0f;
    // A finished car's time, and the gap above it.
    if (c->finish_tick != 0) height += row_small + st->ItemSpacing.y;
    // The way out, when there is a wreck to need one. Counted here rather than
    // hoped for: a panel sized before its contents is how a button ends up half
    // outside the box it is in.
    // The one or two keys a wrecked driver is offered.
    if (c->wrecked) {
        height += (line + st->ItemSpacing.y) * (online ? 1.0f : 2.0f);
    }
    // And the race waiting for somebody: what is happening, how long it has
    // been happening, and - when there is no wreck message already saying it -
    // the way out.
    if (waited > 0.5f) {
        height += st->ItemSpacing.y + (line + st->ItemSpacing.y) * 2.0f;
        if (!c->wrecked) {
            height += (line + st->ItemSpacing.y) * (online ? 1.0f : 2.0f);
        }
    }

    ImGui_SetNextWindowSize((ImVec2){ GS_HUD_W, height }, ImGuiCond_Always);

    // A window per view: ImGui keys its state by name, so two views sharing one
    // name would be one window drawn twice in the same place.
    char id[32];
    SDL_snprintf(id, sizeof id, "##hud%u", v->car);

    if (ImGui_Begin(id, nullptr,
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs |
                    ImGuiWindowFlags_NoSavedSettings)) {
        char text[32];

        // --- Position. First, and biggest, because it is the question.
        uint8_t place = gs_world_place(w, t, v->car);
        uint8_t racing = 0;
        for (uint8_t i = 0; i < w->car_count; i++) {
            if (w->car[i].active) racing++;
        }
        SDL_snprintf(text, sizeof text, "%u/%u", place, racing);
        gs_hud_stat("position", text, GS_HUD_BIG);

        // --- Lap. The target counts from the world rather than the setup, so a
        // race whose length was changed shows the length being raced.
        ImGui_Spacing();
        if (w->laps_to_win > 0) {
            uint16_t on = (uint16_t)(c->laps + 1);
            if (on > w->laps_to_win) on = w->laps_to_win;
            SDL_snprintf(text, sizeof text, "%u/%u", on, w->laps_to_win);
        } else {
            SDL_snprintf(text, sizeof text, "%u", (uint16_t)(c->laps + 1));
        }
        gs_hud_stat("lap", text, GS_HUD_BIG);

        // --- The lap being driven, counting up. Ticks since the last crossing,
        // which is the same clock the simulation will judge the lap by.
        //
        // **A wrecked car is not driving a lap.** The clock used to carry on
        // past a wreck, so a car that had been dead for a minute and a half
        // read as somebody on a very slow lap - which says the opposite of what
        // happened. There is no lap being driven, so there is no time to show.
        //
        // The tick it stopped at is not shown, because the simulation does not
        // record when a car was wrecked and adding a field to the car to carry
        // it would change the world hash - which is every existing replay,
        // ghost and shared time, for a line of text.
        //
        // **Before the flag it is a countdown instead**, in the same row rather
        // than an extra one: the panel is sized from the rows it has, and a row
        // that exists for three seconds would leave a hole in it for the rest
        // of the race. The light tree beside the grid is the thing being read
        // at that moment; this is for anybody whose eyes are on their own car.
        ImGui_Spacing();
        uint32_t counting = gs_world_countdown(w);
        if (counting > 0) {
            SDL_snprintf(text, sizeof text, "%u",
                         (unsigned)((counting + (uint32_t)GS_TICK_HZ - 1u) /
                                    (uint32_t)GS_TICK_HZ));
            gs_hud_stat("get ready", text, GS_HUD_SMALL);
        } else if (c->wrecked) {
            gs_hud_stat("this lap", "-", GS_HUD_SMALL);
        } else {
            uint32_t running = tick > c->lap_start ? tick - c->lap_start : 0;
            gs_time_text(text, sizeof text, running);
            gs_hud_stat("this lap", text, GS_HUD_SMALL);
        }

        // --- And the one to beat.
        ImGui_Spacing();
        gs_time_text(text, sizeof text, c->best_lap);
        gs_hud_stat("best", text, GS_HUD_SMALL);

        // --- What is left of the car.
        ImGui_Spacing();
        // **As wide as the room there is**, not as wide as the window less a
        // padding somebody wrote down once. The HUD is drawn in the menu's
        // style, whose window padding is twenty-two a side rather than the
        // eight assumed here, so the bar was twenty-eight pixels wider than the
        // panel it sits in and ran off the edge of it.
        gs_hud_damage(c, ImGui_GetContentRegionAvail().x);

        if (c->wrecked) gs_hud_way_out(online);

        // **Waiting, and for how long.** Half a second of it is a bad moment on
        // the network and not worth mentioning; longer than that and the person
        // watching a still screen deserves to know it is the other machine and
        // not this one.
        if (waited > 0.5f) {
            ImGui_Spacing();
            ImGui_PushStyleColorImVec4(ImGuiCol_Text,
                                       (ImVec4){ 0.95f, 0.6f, 0.3f, 1.0f });
            ImGui_TextUnformatted("WAITING");
            ImGui_Text("%.0fs quiet", (double)waited);
            ImGui_PopStyleColor();
            if (!c->wrecked) gs_hud_way_out(online);
        }

        // --- And whether this one is over. A finished car still gets a HUD,
        // because the others are still racing and the screen is still theirs.
        if (c->finish_tick != 0) {
            ImGui_Spacing();
            gs_time_text(text, sizeof text, c->finish_tick);
            gs_hud_stat("finished", text, GS_HUD_SMALL);
        }

        gs_hud_hidden = ImGui_GetScrollMaxY();
    }
    ImGui_End();
}

float gs_hud_overflow(void) { return gs_hud_hidden; }

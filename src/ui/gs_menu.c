#include "ui/gs_menu.h"

#include "dcimgui.h"
#include "gfx/gs_render.h"
#include "ui/gs_style.h"

#include <SDL3/SDL.h>

#include <stdio.h>

// The gravity buttons, the same set the construction set offers, because a
// player who learned "Mars" in the editor should find "Mars" here.
static const struct { const char *name; gs_fix g; } gs_gravities[] = {
    { "Ceres",   GS_RATIO(3, 100) },
    { "Moon",    GS_RATIO(17, 100) },
    { "Mars",    GS_RATIO(38, 100) },
    { "Venus",   GS_RATIO(90, 100) },
    { "Earth",   GS_ONE },
    { "Saturn",  GS_RATIO(107, 100) },
    { "Neptune", GS_RATIO(114, 100) },
    { "Jupiter", GS_RATIO(253, 100) },
};
#define GS_GRAVITY_COUNT ((int)(sizeof gs_gravities / sizeof gs_gravities[0]))

void gs_time_text(char *out, size_t cap, uint32_t ticks) {
    if (ticks == 0) {
        SDL_snprintf(out, cap, "-");
        return;
    }
    // Hundredths, because that is the resolution a lap time is argued about at,
    // and 120 Hz gives plenty more than that.
    uint32_t total = ticks * 100u / (uint32_t)GS_TICK_HZ;
    uint32_t mins = total / 6000u;
    uint32_t secs = (total / 100u) % 60u;
    uint32_t cents = total % 100u;
    if (mins > 0) SDL_snprintf(out, cap, "%u:%02u.%02u", mins, secs, cents);
    else SDL_snprintf(out, cap, "%u.%02u", secs, cents);
}

void gs_menu_init(gs_menu *m) {
    SDL_zerop(m);
    gs_profiles_clear(&m->profiles);
    gs_records_clear(&m->records);

    m->screen = GS_SCREEN_TITLE;
    m->editing = -1;
    m->picking_for = -1;
    m->new_colour = GS_COLOUR_RED;

    m->setup.players = 2;
    m->setup.mode = (uint8_t)GS_MODE_RACE;
    m->setup.laps = 3;
    m->setup.gravity = GS_ONE;
    m->setup.gravity_preset = 4;                 // Earth

    for (uint8_t i = 0; i < GS_MAX_CARS; i++) {
        m->setup.profile[i] = -1;
        m->setup.vehicle[i] = (uint8_t)(i % GS_VEH_COUNT);
        m->setup.colour[i] = (uint8_t)(i % GS_COLOUR_COUNT);
    }
}

// --- the store --------------------------------------------------------------
//
// Profiles and records in one file, because they are one thing: a record with
// a name on it is only a record if the name still means somebody.

#define GS_STORE_MAGIC   0x54535347u   // "GSST"
#define GS_STORE_VERSION 1u

size_t gs_menu_save(const gs_menu *m, uint8_t *buf, size_t cap) {
    size_t head = 12;
    if (cap < head) return 0;

    size_t pn = gs_profiles_serialize(&m->profiles, buf + head, cap - head);
    if (pn == 0 && m->profiles.count > 0) return 0;

    size_t rn = gs_records_serialize(&m->records, buf + head + pn, cap - head - pn);
    if (rn == 0 && m->records.count > 0) return 0;

    buf[0] = (uint8_t)(GS_STORE_MAGIC & 0xffu);
    buf[1] = (uint8_t)((GS_STORE_MAGIC >> 8) & 0xffu);
    buf[2] = (uint8_t)((GS_STORE_MAGIC >> 16) & 0xffu);
    buf[3] = (uint8_t)((GS_STORE_MAGIC >> 24) & 0xffu);
    buf[4] = (uint8_t)GS_STORE_VERSION;
    buf[5] = 0; buf[6] = 0; buf[7] = 0;
    buf[8]  = (uint8_t)(pn & 0xffu);
    buf[9]  = (uint8_t)((pn >> 8) & 0xffu);
    buf[10] = (uint8_t)((pn >> 16) & 0xffu);
    buf[11] = (uint8_t)((pn >> 24) & 0xffu);
    return head + pn + rn;
}

bool gs_menu_load(gs_menu *m, const uint8_t *buf, size_t len) {
    if (len < 12) return false;
    uint32_t magic = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                     ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    if (magic != GS_STORE_MAGIC || buf[4] != GS_STORE_VERSION) return false;

    size_t pn = (size_t)buf[8] | ((size_t)buf[9] << 8) |
                ((size_t)buf[10] << 16) | ((size_t)buf[11] << 24);
    if (12 + pn > len) return false;

    if (pn > 0 && !gs_profiles_deserialize(&m->profiles, buf + 12, pn)) return false;
    if (12 + pn < len) {
        if (!gs_records_deserialize(&m->records, buf + 12 + pn, len - 12 - pn)) {
            return false;
        }
    }
    return true;
}

// --- shared bits ------------------------------------------------------------

// A heading inside a panel: the accent colour, and space above it. Cheaper to
// read than a separator with text in it, and it does not draw a line across
// something that is already inside a box.
static void gs_heading(const char *text) {
    ImGui_Dummy((ImVec2){ 0.0f, 4.0f });
    float r, g, b;
    gs_style_accent(&r, &g, &b);
    ImGui_PushStyleColorImVec4(ImGuiCol_Text, (ImVec4){ r, g, b, 1.0f });
    ImGui_TextUnformatted(text);
    ImGui_PopStyleColor();
    ImGui_Spacing();
}

// A label in a fixed column with its control beside it, so a form reads down
// the left edge instead of zig-zagging - which is most of what made the first
// version of this look thrown together.
#define GS_LABEL_W 96.0f

static void gs_field(const char *label) {
    ImGui_TextUnformatted(label);
    ImGui_SameLine();
    ImGui_SetCursorPosX(ImGui_GetCursorPosX() +
                        (GS_LABEL_W - ImGui_CalcTextSize(label).x) -
                        ImGui_GetStyle()->ItemSpacing.x);
}

// One button, centred, the width of the panel.
static bool gs_wide_button(const char *label, float height) {
    return ImGui_ButtonEx(label, (ImVec2){ -1.0f, height });
}

// The loud one. Exactly one per screen, and it is always the thing that starts
// something.
static bool gs_go_button(const char *label, float w, float h) {
    float r, g, b;
    gs_style_accent(&r, &g, &b);
    ImGui_PushStyleColorImVec4(ImGuiCol_Button, (ImVec4){ r, g, b, 0.90f });
    ImGui_PushStyleColorImVec4(ImGuiCol_ButtonHovered,
                               (ImVec4){ r * 1.2f, g * 1.2f, b * 1.1f, 1.0f });
    ImGui_PushStyleColorImVec4(ImGuiCol_ButtonActive,
                               (ImVec4){ r * 1.3f, g * 1.3f, b * 1.15f, 1.0f });
    bool hit = ImGui_ButtonEx(label, (ImVec2){ w, h });
    ImGui_PopStyleColorEx(3);
    return hit;
}

// Centred on the viewport, at a size the caller works out.
//
// **Not auto-sized**, though it obviously wants to be: an auto-fitting ImGui
// window is invisible for its first frame, and a screenshot is one frame - so
// the whole front end would photograph as an empty screen. That cost an
// afternoon once already, on the editor's palette. The screens with tables in
// them compute their height from the number of rows instead, which gets the
// same result and can be captured.
static void gs_centre_window(const char *title, float w, float h) {
    ImGuiViewport *vp = ImGui_GetMainViewport();
    ImGui_SetNextWindowPosEx((ImVec2){ vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                       vp->WorkPos.y + vp->WorkSize.y * 0.5f },
                             ImGuiCond_Always, (ImVec2){ 0.5f, 0.5f });
    ImGui_SetNextWindowSize((ImVec2){ w, h }, ImGuiCond_Always);
    (void)title;
}

// One table row, near enough, for working a panel's height out from its
// contents rather than guessing at one that fits the worst case.
static float gs_row_height(void) {
    return ImGui_GetTextLineHeight() + ImGui_GetStyle()->CellPadding.y * 2.0f +
           ImGui_GetStyle()->ItemSpacing.y;
}

static void gs_swatch(uint8_t colour) {
    SDL_FColor c = gs_render_paint_colour(colour);
    ImGui_ColorButtonEx("##swatch", (ImVec4){ c.r, c.g, c.b, 1.0f },
                        ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker |
                        ImGuiColorEditFlags_NoAlpha,
                        (ImVec2){ 17.0f, 17.0f });
}

static const char *gs_profile_name_of(const gs_menu *m, int index) {
    if (index < 0 || index >= (int)m->profiles.count) return "guest";
    return m->profiles.entry[index].name;
}

// --- the screens ------------------------------------------------------------

static gs_screen gs_title(gs_menu *m) {
    gs_screen next = GS_SCREEN_TITLE;
    gs_centre_window("title", 460.0f, 400.0f);

    if (ImGui_Begin("##title", nullptr,
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar)) {
        // The name, big and centred. A title screen whose title is the same
        // size as its buttons is a settings dialog.
        ImGui_SetWindowFontScale(2.6f);
        float w = ImGui_CalcTextSize("GEARSTICK").x;
        ImGui_SetCursorPosX((ImGui_GetWindowWidth() - w) * 0.5f);
        ImGui_TextUnformatted("GEARSTICK");
        ImGui_SetWindowFontScale(1.0f);

        ImGui_PushStyleColorImVec4(ImGuiCol_Text,
                                   ImGui_GetStyle()->Colors[ImGuiCol_TextDisabled]);
        w = ImGui_CalcTextSize("a construction racer").x;
        ImGui_SetCursorPosX((ImGui_GetWindowWidth() - w) * 0.5f);
        ImGui_TextUnformatted("a construction racer");
        ImGui_PopStyleColor();

        ImGui_Dummy((ImVec2){ 0.0f, 18.0f });

        if (gs_go_button("RACE", -1.0f, 44.0f)) next = GS_SCREEN_SETUP;
        ImGui_Spacing();
        if (gs_wide_button("Drivers", 34.0f)) next = GS_SCREEN_PROFILES;
        if (gs_wide_button("Records", 34.0f)) next = GS_SCREEN_RECORDS;

        ImGui_Dummy((ImVec2){ 0.0f, 14.0f });
        ImGui_Separator();
        ImGui_Spacing();

        ImGui_PushStyleColorImVec4(ImGuiCol_Text,
                                   ImGui_GetStyle()->Colors[ImGuiCol_TextDisabled]);
        ImGui_TextUnformatted("Tab      the construction set");
        ImGui_TextUnformatted("Escape   quit");
        if (m->profiles.count == 0) {
            ImGui_TextUnformatted("No drivers yet - start with Drivers.");
        } else {
            ImGui_Text("%u driver%s known", m->profiles.count,
                       m->profiles.count == 1 ? "" : "s");
        }
        ImGui_PopStyleColor();

        if (m->status[0] != '\0') ImGui_TextUnformatted(m->status);
    }
    ImGui_End();
    return next;
}

static gs_screen gs_profiles_screen(gs_menu *m) {
    gs_screen next = GS_SCREEN_PROFILES;
    gs_centre_window("drivers", 560.0f, 440.0f);

    if (ImGui_Begin("Drivers", nullptr,
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoCollapse)) {
        ImGui_TextUnformatted("Who is playing. A record with a name on it is "
                              "somebody's record.");
        ImGui_Separator();

        for (uint8_t i = 0; i < m->profiles.count; i++) {
            gs_profile *p = &m->profiles.entry[i];
            ImGui_PushIDInt((int)i);

            gs_swatch(p->colour);
            ImGui_SameLine();
            ImGui_Text("%-14s %s", p->name, gs_vehicle(p->vehicle)->name);
            ImGui_SameLine();
            ImGui_Text("  %u race%s, %u won", p->races, p->races == 1 ? "" : "s",
                       p->wins);

            ImGui_SameLine();
            if (ImGui_SmallButton("edit")) {
                m->editing = (int)i;
                SDL_strlcpy(m->new_name, p->name, sizeof m->new_name);
                m->new_colour = p->colour;
                m->new_vehicle = p->vehicle;
            }
            ImGui_SameLine();
            if (ImGui_SmallButton("remove")) {
                // Their records stay. A record belongs to the track, and
                // deleting somebody should not rewrite a track's history.
                gs_profile_remove(&m->profiles, i);
                m->store_dirty = true;
                ImGui_PopID();
                break;
            }
            ImGui_PopID();
        }

        ImGui_Separator();
        ImGui_TextUnformatted(m->editing >= 0 ? "Editing" : "New driver");

        ImGui_InputText("name", m->new_name, sizeof m->new_name, 0);

        int colour = (int)m->new_colour;
        for (int c = 0; c < GS_COLOUR_COUNT; c++) {
            if (c > 0) ImGui_SameLine();
            ImGui_PushIDInt(1000 + c);
            SDL_FColor sc = gs_render_paint_colour((uint8_t)c);
            if (ImGui_ColorButtonEx(gs_colour_name((uint8_t)c),
                                    (ImVec4){ sc.r, sc.g, sc.b, 1.0f },
                                    ImGuiColorEditFlags_NoPicker,
                                    (ImVec2){ 22.0f, 22.0f })) {
                colour = c;
            }
            ImGui_PopID();
        }
        m->new_colour = (uint8_t)colour;
        ImGui_SameLine();
        ImGui_Text("%s", gs_colour_name(m->new_colour));

        for (uint8_t v = 0; v < GS_VEH_COUNT; v++) {
            if (v > 0 && v % 3 != 0) ImGui_SameLine();
            ImGui_PushIDInt(2000 + v);
            if (ImGui_RadioButton(gs_vehicle(v)->name, m->new_vehicle == v)) {
                m->new_vehicle = v;
            }
            ImGui_PopID();
        }

        if (m->editing >= 0) {
            if (ImGui_Button("save")) {
                gs_profile *p = &m->profiles.entry[m->editing];
                SDL_strlcpy(p->name, m->new_name, sizeof p->name);
                p->colour = m->new_colour;
                p->vehicle = m->new_vehicle;
                m->editing = -1;
                m->new_name[0] = '\0';
                m->store_dirty = true;
            }
            ImGui_SameLine();
            if (ImGui_Button("cancel")) {
                m->editing = -1;
                m->new_name[0] = '\0';
            }
        } else {
            if (ImGui_Button("add")) {
                int added = gs_profile_add(&m->profiles, m->new_name,
                                           m->new_colour, m->new_vehicle);
                if (added < 0) {
                    SDL_snprintf(m->status, sizeof m->status,
                                 "that name is taken, empty, or the roster is full");
                } else {
                    m->new_name[0] = '\0';
                    m->store_dirty = true;
                    m->status[0] = '\0';
                }
            }
        }

        ImGui_Separator();
        if (ImGui_Button("back")) next = GS_SCREEN_TITLE;
        if (m->status[0] != '\0') {
            ImGui_SameLine();
            ImGui_TextUnformatted(m->status);
        }
    }
    ImGui_End();
    return next;
}

static gs_screen gs_setup_screen(gs_menu *m, const gs_track *t) {
    gs_screen next = GS_SCREEN_SETUP;
    gs_centre_window("setup", 760.0f, 600.0f);

    if (ImGui_Begin("Race setup", nullptr,
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoCollapse)) {
        gs_track_issue issue = gs_track_validate(t);
        bool ok = issue.problem == GS_TRACK_OK;

        // --- The track, in its own box, because it is context rather than a
        // thing being chosen here. Choosing tracks is the library's job and it
        // does not exist yet - see docs/FEATURES.md, the platform section.
        ImGui_BeginChild("track", (ImVec2){ 0.0f, 56.0f }, ImGuiChildFlags_Borders, 0);
        ImGui_Text("Track %016llx", (unsigned long long)gs_track_hash(t));
        if (ok) {
            ImGui_PushStyleColorImVec4(ImGuiCol_Text,
                                       ImGui_GetStyle()->Colors[ImGuiCol_TextDisabled]);
            ImGui_Text("%u x %u tiles, %u gates", t->w, t->h, t->gate_count);
            ImGui_PopStyleColor();
        } else {
            ImGui_PushStyleColorImVec4(ImGuiCol_Text, (ImVec4){ 0.95f, 0.5f, 0.3f, 1.0f });
            ImGui_Text("%s", gs_track_problem_text(issue.problem));
            ImGui_PopStyleColor();
        }
        ImGui_EndChild();

        // --- Two columns: the rules on the left, gravity on the right. Both
        // are short, and side by side they fit on one screen without scrolling.
        gs_heading("THE RACE");

        ImGui_BeginGroup();
        int mode = (int)m->setup.mode;
        gs_field("mode");
        ImGui_SetNextItemWidth(200.0f);
        if (ImGui_BeginCombo("##mode",
                             mode == (int)GS_MODE_RACE ? "first past the flag"
                                                       : "last one driving", 0)) {
            if (ImGui_SelectableEx("first past the flag", mode == (int)GS_MODE_RACE,
                                   0, (ImVec2){ 0.0f, 0.0f })) {
                mode = (int)GS_MODE_RACE;
            }
            if (ImGui_SelectableEx("last one driving",
                                   mode == (int)GS_MODE_DESTRUCTION, 0,
                                   (ImVec2){ 0.0f, 0.0f })) {
                mode = (int)GS_MODE_DESTRUCTION;
            }
            ImGui_EndCombo();
        }
        m->setup.mode = (uint8_t)mode;

        int laps = (int)m->setup.laps;
        gs_field("laps");
        ImGui_SetNextItemWidth(200.0f);
        ImGui_BeginDisabled(m->setup.mode != (uint8_t)GS_MODE_RACE);
        ImGui_SliderInt("##laps", &laps, 1, 20);
        ImGui_EndDisabled();
        m->setup.laps = (uint16_t)laps;

        int players = (int)m->setup.players;
        gs_field("drivers");
        ImGui_SetNextItemWidth(200.0f);
        ImGui_SliderInt("##players", &players, 1, GS_MAX_CARS);
        m->setup.players = (uint8_t)players;
        ImGui_EndGroup();

        ImGui_SameLine();
        ImGui_Dummy((ImVec2){ 24.0f, 0.0f });
        ImGui_SameLine();

        ImGui_BeginGroup();
        ImGui_TextUnformatted("gravity");
        ImGui_Spacing();
        for (int g = 0; g < GS_GRAVITY_COUNT; g++) {
            if (g % 4 != 0) ImGui_SameLine();
            ImGui_PushIDInt(3000 + g);
            bool on = m->setup.gravity_preset == g;
            if (on) {
                float r, gg, b;
                gs_style_accent(&r, &gg, &b);
                ImGui_PushStyleColorImVec4(ImGuiCol_Button, (ImVec4){ r, gg, b, 0.9f });
            }
            if (ImGui_ButtonEx(gs_gravities[g].name, (ImVec2){ 76.0f, 0.0f })) {
                m->setup.gravity = gs_gravities[g].g;
                m->setup.gravity_preset = g;
            }
            if (on) ImGui_PopStyleColor();
            ImGui_PopID();
        }
        ImGui_PushStyleColorImVec4(ImGuiCol_Text,
                                   ImGui_GetStyle()->Colors[ImGuiCol_TextDisabled]);
        ImGui_Text("%.2f x Earth", (double)m->setup.gravity / (double)GS_ONE);
        ImGui_PopStyleColor();
        ImGui_EndGroup();

        // --- The grid, as a table. One row per driver, columns that line up,
        // which is the whole difference between a form and a pile of widgets.
        gs_heading("THE GRID");

        if (ImGui_BeginTable("grid", 4,
                             ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            ImGui_TableSetupColumnEx("", ImGuiTableColumnFlags_WidthFixed, 40.0f, 0);
            ImGui_TableSetupColumnEx("driver", ImGuiTableColumnFlags_WidthFixed, 160.0f, 0);
            ImGui_TableSetupColumnEx("machine", ImGuiTableColumnFlags_WidthFixed, 165.0f, 0);
            ImGui_TableSetupColumnEx("paint", ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
            ImGui_TableHeadersRow();

            for (uint8_t i = 0; i < m->setup.players; i++) {
                ImGui_TableNextRow();
                ImGui_PushIDInt(4000 + i);

                ImGui_TableSetColumnIndex(0);
                ImGui_Text("%u", i + 1);

                ImGui_TableSetColumnIndex(1);
                ImGui_SetNextItemWidth(-1.0f);
                if (ImGui_BeginCombo("##who",
                                     gs_profile_name_of(m, m->setup.profile[i]), 0)) {
                    if (ImGui_SelectableEx("guest", m->setup.profile[i] < 0, 0,
                                           (ImVec2){ 0.0f, 0.0f })) {
                        m->setup.profile[i] = -1;
                    }
                    for (uint8_t k = 0; k < m->profiles.count; k++) {
                        ImGui_PushIDInt(k);
                        if (ImGui_SelectableEx(m->profiles.entry[k].name,
                                               m->setup.profile[i] == (int8_t)k, 0,
                                               (ImVec2){ 0.0f, 0.0f })) {
                            m->setup.profile[i] = (int8_t)k;
                            m->setup.colour[i] = m->profiles.entry[k].colour;
                            m->setup.vehicle[i] = m->profiles.entry[k].vehicle;
                        }
                        ImGui_PopID();
                    }
                    ImGui_EndCombo();
                }

                ImGui_TableSetColumnIndex(2);
                ImGui_SetNextItemWidth(-1.0f);
                int v = (int)m->setup.vehicle[i];
                if (ImGui_BeginCombo("##veh", gs_vehicle((uint8_t)v)->name, 0)) {
                    for (uint8_t k = 0; k < GS_VEH_COUNT; k++) {
                        if (ImGui_SelectableEx(gs_vehicle(k)->name, v == (int)k, 0,
                                               (ImVec2){ 0.0f, 0.0f })) {
                            v = (int)k;
                        }
                    }
                    ImGui_EndCombo();
                }
                m->setup.vehicle[i] = (uint8_t)v;

                ImGui_TableSetColumnIndex(3);
                for (int c = 0; c < GS_COLOUR_COUNT; c++) {
                    if (c > 0) ImGui_SameLine();
                    ImGui_PushIDInt(c);

                    // Dimmed rather than made transparent: a translucent swatch
                    // shows ImGui's checkerboard through it, which reads as a
                    // broken image rather than as an unselected colour. The one
                    // that is chosen is the bright one.
                    SDL_FColor sc = gs_render_paint_colour((uint8_t)c);
                    bool on = m->setup.colour[i] == (uint8_t)c;
                    float k = on ? 1.0f : 0.34f;
                    if (ImGui_ColorButtonEx(gs_colour_name((uint8_t)c),
                                            (ImVec4){ sc.r * k, sc.g * k, sc.b * k, 1.0f },
                                            ImGuiColorEditFlags_NoPicker |
                                            ImGuiColorEditFlags_NoTooltip |
                                            ImGuiColorEditFlags_NoAlpha,
                                            (ImVec2){ 17.0f, 17.0f })) {
                        m->setup.colour[i] = (uint8_t)c;
                    }
                    ImGui_PopID();
                }
                ImGui_PopID();
            }
            ImGui_EndTable();
        }

        // --- What there is to beat, before the race rather than after it.
        gs_heading("TO BEAT");

        gs_world probe;
        gs_world_init(&probe, GS_ONE);
        probe.gravity = m->setup.gravity;
        const gs_record *best = gs_records_best_lap(&m->records, gs_track_hash(t),
                                                    gs_conditions_hash(&probe));
        if (best != nullptr) {
            char text[32];
            gs_time_text(text, sizeof text, best->lap);
            ImGui_Text("%s   %s, %s", text, best->who, gs_vehicle(best->vehicle)->name);
        } else {
            ImGui_PushStyleColorImVec4(ImGuiCol_Text,
                                       ImGui_GetStyle()->Colors[ImGuiCol_TextDisabled]);
            ImGui_TextUnformatted("Nobody has been round this one at this gravity.");
            ImGui_PopStyleColor();
        }

        // --- The one loud button, on its own line at the bottom.
        ImGui_Dummy((ImVec2){ 0.0f, 8.0f });
        ImGui_Separator();
        ImGui_Spacing();

        ImGui_BeginDisabled(!ok);
        if (gs_go_button("GO", 160.0f, 42.0f)) next = GS_SCREEN_RACE;
        ImGui_EndDisabled();

        ImGui_SameLine();
        if (ImGui_ButtonEx("Back", (ImVec2){ 100.0f, 42.0f })) next = GS_SCREEN_TITLE;

        if (!ok) {
            ImGui_SameLine();
            ImGui_PushStyleColorImVec4(ImGuiCol_Text, (ImVec4){ 0.95f, 0.5f, 0.3f, 1.0f });
            ImGui_TextUnformatted("  the route has to be sound - fix it in the\n"
                                  "  construction set (Tab)");
            ImGui_PopStyleColor();
        }
    }
    ImGui_End();
    return next;
}

static gs_screen gs_results_screen(gs_menu *m) {
    gs_screen next = GS_SCREEN_RESULTS;
    gs_centre_window("results", 720.0f,
                     168.0f + gs_row_height() * (float)(m->result_count + 1));

    if (ImGui_Begin("Results", nullptr,
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoCollapse)) {
        if (ImGui_BeginTable("results", 6,
                             ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            ImGui_TableSetupColumnEx("", ImGuiTableColumnFlags_WidthFixed, 34.0f, 0);
            ImGui_TableSetupColumnEx("driver", ImGuiTableColumnFlags_WidthFixed, 150.0f, 0);
            ImGui_TableSetupColumnEx("machine", ImGuiTableColumnFlags_WidthFixed, 130.0f, 0);
            ImGui_TableSetupColumnEx("time", ImGuiTableColumnFlags_WidthFixed, 100.0f, 0);
            ImGui_TableSetupColumnEx("best lap", ImGuiTableColumnFlags_WidthFixed, 100.0f, 0);
            ImGui_TableSetupColumnEx("", ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
            ImGui_TableHeadersRow();

            for (uint8_t i = 0; i < m->result_count; i++) {
                const gs_result_row *r = &m->result[i];
                char total[32], lap[32];
                gs_time_text(total, sizeof total, r->finish_tick);
                gs_time_text(lap, sizeof lap, r->best_lap);

                ImGui_TableNextRow();
                ImGui_PushIDInt(i);

                ImGui_TableSetColumnIndex(0);
                // The winner's number in the accent, so the eye lands on it
                // before it has read anything.
                if (r->place == 1) {
                    float ar, ag, ab;
                    gs_style_accent(&ar, &ag, &ab);
                    ImGui_PushStyleColorImVec4(ImGuiCol_Text, (ImVec4){ ar, ag, ab, 1.0f });
                    ImGui_Text("%u", r->place);
                    ImGui_PopStyleColor();
                } else {
                    ImGui_Text("%u", r->place);
                }

                ImGui_TableSetColumnIndex(1);
                gs_swatch(m->setup.colour[r->car]);
                ImGui_SameLine();
                ImGui_TextUnformatted(gs_profile_name_of(m, m->setup.profile[r->car]));

                ImGui_TableSetColumnIndex(2);
                ImGui_TextUnformatted(gs_vehicle(m->setup.vehicle[r->car])->name);

                ImGui_TableSetColumnIndex(3);
                ImGui_TextUnformatted(total);

                ImGui_TableSetColumnIndex(4);
                ImGui_TextUnformatted(lap);

                ImGui_TableSetColumnIndex(5);
                if (r->wrecked) {
                    ImGui_PushStyleColorImVec4(ImGuiCol_Text,
                                               (ImVec4){ 0.95f, 0.45f, 0.3f, 1.0f });
                    ImGui_TextUnformatted("wrecked");
                    ImGui_PopStyleColor();
                    if (r->beat_lap || r->beat_race) ImGui_SameLine();
                }
                if (r->beat_lap || r->beat_race) {
                    float ar, ag, ab;
                    gs_style_accent(&ar, &ag, &ab);
                    ImGui_PushStyleColorImVec4(ImGuiCol_Text, (ImVec4){ ar, ag, ab, 1.0f });
                    ImGui_TextUnformatted(r->beat_lap && r->beat_race ? "lap + race record"
                                          : r->beat_lap ? "lap record" : "race record");
                    ImGui_PopStyleColor();
                }
                ImGui_PopID();
            }
            ImGui_EndTable();
        }

        ImGui_Dummy((ImVec2){ 0.0f, 10.0f });
        ImGui_Separator();
        ImGui_Spacing();

        if (gs_go_button("Race again", 160.0f, 40.0f)) next = GS_SCREEN_RACE;
        ImGui_SameLine();
        if (ImGui_ButtonEx("Setup", (ImVec2){ 110.0f, 40.0f })) next = GS_SCREEN_SETUP;
        ImGui_SameLine();
        if (ImGui_ButtonEx("Records", (ImVec2){ 110.0f, 40.0f })) next = GS_SCREEN_RECORDS;
        ImGui_SameLine();
        if (ImGui_ButtonEx("Title", (ImVec2){ 110.0f, 40.0f })) next = GS_SCREEN_TITLE;
    }
    ImGui_End();
    return next;
}

static gs_screen gs_records_screen(gs_menu *m, const gs_track *t) {
    gs_screen next = GS_SCREEN_RECORDS;
    // Counted before the window opens, so the panel is the height of its table
    // rather than the height of the biggest table it could ever hold.
    gs_world probe;
    gs_world_init(&probe, GS_ONE);
    probe.gravity = m->setup.gravity;
    uint64_t conditions = gs_conditions_hash(&probe);

    const gs_record *rows[16];
    uint16_t n = gs_records_for(&m->records, gs_track_hash(t), conditions, rows, 16);

    gs_centre_window("records", 720.0f,
                     214.0f + gs_row_height() * (float)(n > 0 ? n + 1 : 1));

    if (ImGui_Begin("Records", nullptr,
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoCollapse)) {
        ImGui_Text("Track %016llx", (unsigned long long)gs_track_hash(t));
        ImGui_PushStyleColorImVec4(ImGuiCol_Text,
                                   ImGui_GetStyle()->Colors[ImGuiCol_TextDisabled]);
        ImGui_Text("at %.2f x Earth gravity - change it and this is a different table",
                   (double)m->setup.gravity / (double)GS_ONE);
        ImGui_PopStyleColor();

        gs_heading("BEST LAPS");

        if (n == 0) {
            ImGui_PushStyleColorImVec4(ImGuiCol_Text,
                                       ImGui_GetStyle()->Colors[ImGuiCol_TextDisabled]);
            ImGui_TextUnformatted("Nobody has been round this one yet.");
            ImGui_PopStyleColor();
        } else if (ImGui_BeginTable("records", 5,
                                    ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_SizingFixedFit)) {
            ImGui_TableSetupColumnEx("", ImGuiTableColumnFlags_WidthFixed, 34.0f, 0);
            ImGui_TableSetupColumnEx("driver", ImGuiTableColumnFlags_WidthFixed, 150.0f, 0);
            ImGui_TableSetupColumnEx("machine", ImGuiTableColumnFlags_WidthFixed, 130.0f, 0);
            ImGui_TableSetupColumnEx("best lap", ImGuiTableColumnFlags_WidthFixed, 110.0f, 0);
            ImGui_TableSetupColumnEx("race", ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
            ImGui_TableHeadersRow();

            for (uint16_t i = 0; i < n; i++) {
                char lap[32], race[32];
                gs_time_text(lap, sizeof lap, rows[i]->lap);
                gs_time_text(race, sizeof race, rows[i]->race);

                ImGui_TableNextRow();
                ImGui_TableSetColumnIndex(0);
                if (i == 0) {
                    float ar, ag, ab;
                    gs_style_accent(&ar, &ag, &ab);
                    ImGui_PushStyleColorImVec4(ImGuiCol_Text, (ImVec4){ ar, ag, ab, 1.0f });
                    ImGui_Text("%u", i + 1);
                    ImGui_PopStyleColor();
                } else {
                    ImGui_Text("%u", i + 1);
                }
                ImGui_TableSetColumnIndex(1);
                ImGui_TextUnformatted(rows[i]->who);
                ImGui_TableSetColumnIndex(2);
                ImGui_TextUnformatted(gs_vehicle(rows[i]->vehicle)->name);
                ImGui_TableSetColumnIndex(3);
                ImGui_TextUnformatted(lap);
                ImGui_TableSetColumnIndex(4);
                ImGui_Text("%s over %u", race, rows[i]->laps);
            }
            ImGui_EndTable();
        }

        ImGui_Dummy((ImVec2){ 0.0f, 10.0f });
        ImGui_Separator();
        ImGui_Spacing();
        if (ImGui_ButtonEx("Back", (ImVec2){ 120.0f, 38.0f })) next = GS_SCREEN_TITLE;
    }
    ImGui_End();
    return next;
}

static gs_screen gs_lobby_screen(gs_menu *m) {
    gs_screen next = GS_SCREEN_LOBBY;

    int rows = (m->lobby != nullptr && m->lobby->capacity > 0)
                   ? m->lobby->capacity : 4;
    gs_centre_window("lobby", 640.0f, 220.0f + gs_row_height() * (float)(rows + 1));

    if (ImGui_Begin("Lobby", nullptr,
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoCollapse)) {
        ImGui_TextUnformatted(m->server_text);

        if (m->lobby_error != nullptr) {
            // A refusal is the whole message. Nothing else on this screen
            // matters if the server would not have us.
            ImGui_PushStyleColorImVec4(ImGuiCol_Text, (ImVec4){ 0.95f, 0.5f, 0.3f, 1.0f });
            ImGui_TextUnformatted(m->lobby_error);
            ImGui_PopStyleColor();
            ImGui_Spacing();
            ImGui_Separator();
            ImGui_Spacing();
            if (ImGui_ButtonEx("Back", (ImVec2){ 120.0f, 38.0f })) {
                next = GS_SCREEN_TITLE;
            }
            ImGui_End();
            return next;
        }

        gs_heading("WHO IS HERE");

        // **Nothing heard yet is not an empty lobby.** A server that has not
        // answered has no capacity either, and drawing that as a table of
        // nobody under "waiting for 0 more players" is a screen describing a
        // roster it has never seen.
        bool heard = m->lobby != nullptr && m->lobby->capacity > 0;

        if (!heard) {
            ImGui_PushStyleColorImVec4(ImGuiCol_Text,
                                       ImGui_GetStyle()->Colors[ImGuiCol_TextDisabled]);
            ImGui_TextUnformatted("Knocking...");
            ImGui_PopStyleColor();
        } else if (ImGui_BeginTable("lobby", 3,
                                    ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_SizingFixedFit)) {
            ImGui_TableSetupColumnEx("", ImGuiTableColumnFlags_WidthFixed, 40.0f, 0);
            ImGui_TableSetupColumnEx("driver", ImGuiTableColumnFlags_WidthFixed, 200.0f, 0);
            ImGui_TableSetupColumnEx("", ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
            ImGui_TableHeadersRow();

            for (uint8_t i = 0; i < m->lobby->capacity; i++) {
                const gs_lobby_player *p = &m->lobby->player[i];
                ImGui_TableNextRow();

                ImGui_TableSetColumnIndex(0);
                ImGui_Text("%u", i + 1);

                ImGui_TableSetColumnIndex(1);
                if (!p->present) {
                    ImGui_PushStyleColorImVec4(ImGuiCol_Text,
                                               ImGui_GetStyle()->Colors[ImGuiCol_TextDisabled]);
                    ImGui_TextUnformatted("waiting...");
                    ImGui_PopStyleColor();
                } else {
                    ImGui_TextUnformatted(p->name);
                }

                ImGui_TableSetColumnIndex(2);
                if (p->present && i == m->lobby_slot) {
                    float r, g, b;
                    gs_style_accent(&r, &g, &b);
                    ImGui_PushStyleColorImVec4(ImGuiCol_Text, (ImVec4){ r, g, b, 1.0f });
                    ImGui_TextUnformatted("you");
                    ImGui_PopStyleColor();
                }
            }
            ImGui_EndTable();
        }

        ImGui_Spacing();
        if (m->track_progress < 1.0f) {
            // Waiting for the ground rather than for people, which is a
            // different wait and should not look like the same one.
            ImGui_Text("Receiving the track... %.0f%%",
                       (double)(m->track_progress * 100.0f));
        } else if (m->lobby_ready) {
            ImGui_TextUnformatted("Everybody is here.");
        } else if (heard) {
            uint8_t want = (uint8_t)(m->lobby->capacity - m->lobby->count);
            ImGui_Text("Waiting for %u more player%s.", want, want == 1 ? "" : "s");
        }

        ImGui_Spacing();
        ImGui_Separator();
        ImGui_Spacing();
        if (ImGui_ButtonEx("Leave", (ImVec2){ 120.0f, 38.0f })) next = GS_SCREEN_TITLE;
    }
    ImGui_End();
    return next;
}

gs_screen gs_menu_frame(gs_menu *m, const gs_track *t) {
    switch (m->screen) {
    case GS_SCREEN_TITLE:    return gs_title(m);
    case GS_SCREEN_PROFILES: return gs_profiles_screen(m);
    case GS_SCREEN_SETUP:    return gs_setup_screen(m, t);
    case GS_SCREEN_RESULTS:  return gs_results_screen(m);
    case GS_SCREEN_RECORDS:  return gs_records_screen(m, t);
    case GS_SCREEN_LOBBY:    return gs_lobby_screen(m);
    default:                 return m->screen;
    }
}

// --- what a race did --------------------------------------------------------

void gs_menu_finish(gs_menu *m, const gs_world *w, const gs_track *t) {
    m->result_count = 0;

    uint64_t track = gs_track_hash(t);
    uint64_t conditions = gs_conditions_hash(w);

    for (uint8_t i = 0; i < w->car_count && i < GS_MAX_CARS; i++) {
        const gs_car *c = &w->car[i];
        gs_result_row *r = &m->result[m->result_count++];
        SDL_zerop(r);
        r->car = i;
        r->finish_tick = c->finish_tick;
        r->best_lap = c->best_lap;
        r->laps = c->laps;
        r->damage = c->damage;
        r->wrecked = c->wrecked;
    }

    // Finishers by time, then everybody else by how far they got. A car that
    // never finished still has a position, because "fourth" is a result and
    // "did not finish" on its own throws away who was ahead of whom.
    for (uint8_t i = 1; i < m->result_count; i++) {
        gs_result_row key = m->result[i];
        int j = (int)i - 1;
        while (j >= 0) {
            const gs_result_row *a = &m->result[j];
            bool worse;
            if (a->finish_tick != 0 && key.finish_tick != 0) {
                worse = a->finish_tick > key.finish_tick;
            } else if (a->finish_tick == 0 && key.finish_tick == 0) {
                worse = a->laps < key.laps;
            } else {
                worse = a->finish_tick == 0;      // finishing beats not finishing
            }
            if (!worse) break;
            m->result[j + 1] = m->result[j];
            j--;
        }
        m->result[j + 1] = key;
    }

    for (uint8_t i = 0; i < m->result_count; i++) m->result[i].place = (uint8_t)(i + 1);

    // Submit, and update the people who were driving. Only profiles: a guest is
    // somebody who has not said who they are, and inventing a row for them
    // would fill the table with "guest".
    for (uint8_t i = 0; i < m->result_count; i++) {
        gs_result_row *r = &m->result[i];
        int8_t who = m->setup.profile[r->car];
        if (who < 0 || who >= (int8_t)m->profiles.count) continue;

        gs_record_beat beat = gs_records_submit(
            &m->records, track, conditions, m->setup.vehicle[r->car],
            w->mode, w->laps_to_win, r->best_lap, r->finish_tick,
            m->profiles.entry[who].name);

        r->beat_lap = beat.lap;
        r->beat_race = beat.race;

        gs_profile_raced(&m->profiles, (uint8_t)who, r->place == 1,
                         r->place <= 3, r->wrecked, r->laps);
        m->store_dirty = true;
    }
}

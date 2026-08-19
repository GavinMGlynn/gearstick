#include "ui/gs_menu.h"

#include "dcimgui.h"
#include "gfx/gs_render.h"

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

static void gs_centre_window(const char *title, float w, float h) {
    ImGuiViewport *vp = ImGui_GetMainViewport();
    ImGui_SetNextWindowPos((ImVec2){ vp->WorkPos.x + (vp->WorkSize.x - w) * 0.5f,
                                     vp->WorkPos.y + (vp->WorkSize.y - h) * 0.5f },
                           ImGuiCond_Always);
    ImGui_SetNextWindowSize((ImVec2){ w, h }, ImGuiCond_Always);
    (void)title;
}

static void gs_swatch(uint8_t colour) {
    SDL_FColor c = gs_render_paint_colour(colour);
    ImGui_ColorButtonEx("##swatch", (ImVec4){ c.r, c.g, c.b, 1.0f },
                        ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
                        (ImVec2){ 18.0f, 18.0f });
}

static const char *gs_profile_name_of(const gs_menu *m, int index) {
    if (index < 0 || index >= (int)m->profiles.count) return "guest";
    return m->profiles.entry[index].name;
}

// --- the screens ------------------------------------------------------------

static gs_screen gs_title(gs_menu *m) {
    gs_screen next = GS_SCREEN_TITLE;
    gs_centre_window("title", 420.0f, 330.0f);

    if (ImGui_Begin("gearstick", nullptr,
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoCollapse)) {
        ImGui_TextUnformatted("GEARSTICK");
        ImGui_TextUnformatted("a construction racer");
        ImGui_Separator();
        ImGui_Spacing();

        if (ImGui_ButtonEx("Race", (ImVec2){ -1.0f, 34.0f })) next = GS_SCREEN_SETUP;
        if (ImGui_ButtonEx("Drivers", (ImVec2){ -1.0f, 30.0f })) next = GS_SCREEN_PROFILES;
        if (ImGui_ButtonEx("Records", (ImVec2){ -1.0f, 30.0f })) next = GS_SCREEN_RECORDS;

        ImGui_Spacing();
        ImGui_TextUnformatted("Tab opens the construction set.");
        ImGui_TextUnformatted("Escape quits.");

        ImGui_Spacing();
        ImGui_Separator();
        if (m->profiles.count == 0) {
            ImGui_TextUnformatted("Nobody has said who they are yet.");
        } else {
            ImGui_Text("%u driver%s known", m->profiles.count,
                       m->profiles.count == 1 ? "" : "s");
        }
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
    gs_centre_window("setup", 620.0f, 500.0f);

    if (ImGui_Begin("Race setup", nullptr,
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoCollapse)) {
        gs_track_issue issue = gs_track_validate(t);
        ImGui_Text("Track %016llx, %u x %u",
                   (unsigned long long)gs_track_hash(t), t->w, t->h);
        ImGui_Text("Route: %s", gs_track_problem_text(issue.problem));

        ImGui_Separator();
        ImGui_TextUnformatted("The race");

        int mode = (int)m->setup.mode;
        ImGui_RadioButtonIntPtr("first past the flag", &mode, (int)GS_MODE_RACE);
        ImGui_SameLine();
        ImGui_RadioButtonIntPtr("last one driving", &mode, (int)GS_MODE_DESTRUCTION);
        m->setup.mode = (uint8_t)mode;

        int laps = (int)m->setup.laps;
        ImGui_SliderInt("laps", &laps, 1, 20);
        m->setup.laps = (uint16_t)laps;

        int players = (int)m->setup.players;
        ImGui_SliderInt("players", &players, 1, GS_MAX_CARS);
        m->setup.players = (uint8_t)players;

        ImGui_Separator();
        ImGui_TextUnformatted("Gravity");
        for (int g = 0; g < GS_GRAVITY_COUNT; g++) {
            if (g > 0 && g % 4 != 0) ImGui_SameLine();
            ImGui_PushIDInt(3000 + g);
            bool on = m->setup.gravity_preset == g;
            if (on) ImGui_PushStyleColorImVec4(ImGuiCol_Button,
                                               (ImVec4){ 0.35f, 0.5f, 0.7f, 1.0f });
            if (ImGui_Button(gs_gravities[g].name)) {
                m->setup.gravity = gs_gravities[g].g;
                m->setup.gravity_preset = g;
            }
            if (on) ImGui_PopStyleColor();
            ImGui_PopID();
        }
        ImGui_Text("%.2fx Earth", (double)m->setup.gravity / (double)GS_ONE);

        ImGui_Separator();
        ImGui_TextUnformatted("The grid");

        for (uint8_t i = 0; i < m->setup.players; i++) {
            ImGui_PushIDInt(4000 + i);
            ImGui_Text("Player %u", i + 1);
            ImGui_SameLine();

            gs_swatch(m->setup.colour[i]);
            ImGui_SameLine();

            if (ImGui_SmallButton(gs_profile_name_of(m, m->setup.profile[i]))) {
                m->picking_for = (m->picking_for == (int)i) ? -1 : (int)i;
            }
            ImGui_SameLine();

            int v = (int)m->setup.vehicle[i];
            ImGui_SetNextItemWidth(150.0f);
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

            // The roster, opened under whichever player asked for it.
            if (m->picking_for == (int)i) {
                ImGui_Indent();
                if (ImGui_SmallButton("guest")) {
                    m->setup.profile[i] = -1;
                    m->picking_for = -1;
                }
                for (uint8_t p = 0; p < m->profiles.count; p++) {
                    ImGui_PushIDInt(5000 + p);
                    ImGui_SameLine();
                    if (ImGui_SmallButton(m->profiles.entry[p].name)) {
                        m->setup.profile[i] = (int8_t)p;
                        m->setup.colour[i] = m->profiles.entry[p].colour;
                        m->setup.vehicle[i] = m->profiles.entry[p].vehicle;
                        m->picking_for = -1;
                    }
                    ImGui_PopID();
                }
                ImGui_Unindent();
            }
            ImGui_PopID();
        }

        ImGui_Separator();

        // The record standing on this track under these conditions, so a player
        // knows what they are driving at before they start rather than after.
        gs_world probe;
        gs_world_init(&probe, GS_ONE);
        probe.gravity = m->setup.gravity;
        const gs_record *best = gs_records_best_lap(&m->records, gs_track_hash(t),
                                                    gs_conditions_hash(&probe));
        if (best != nullptr) {
            char text[32];
            gs_time_text(text, sizeof text, best->lap);
            ImGui_Text("Lap record here: %s by %s (%s)", text, best->who,
                       gs_vehicle(best->vehicle)->name);
        } else {
            ImGui_TextUnformatted("No lap record here yet.");
        }

        ImGui_Separator();
        bool ok = issue.problem == GS_TRACK_OK;
        ImGui_BeginDisabled(!ok);
        if (ImGui_ButtonEx("GO", (ImVec2){ 120.0f, 34.0f })) next = GS_SCREEN_RACE;
        ImGui_EndDisabled();
        if (!ok) {
            ImGui_SameLine();
            ImGui_TextUnformatted("the route has to be sound first - use the "
                                  "construction set");
        }
        ImGui_SameLine();
        if (ImGui_Button("back")) next = GS_SCREEN_TITLE;
    }
    ImGui_End();
    return next;
}

static gs_screen gs_results_screen(gs_menu *m) {
    gs_screen next = GS_SCREEN_RESULTS;
    gs_centre_window("results", 620.0f, 400.0f);

    if (ImGui_Begin("Results", nullptr,
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoCollapse)) {
        ImGui_Text("%-4s %-14s %-12s %-10s %-10s %s", "", "driver", "machine",
                   "time", "best lap", "");
        ImGui_Separator();

        for (uint8_t i = 0; i < m->result_count; i++) {
            const gs_result_row *r = &m->result[i];
            char total[32], lap[32];
            gs_time_text(total, sizeof total, r->finish_tick);
            gs_time_text(lap, sizeof lap, r->best_lap);

            gs_swatch(m->setup.colour[r->car]);
            ImGui_SameLine();
            ImGui_Text("%u. %-14s %-12s %-10s %-10s %s%s%s",
                       r->place,
                       gs_profile_name_of(m, m->setup.profile[r->car]),
                       gs_vehicle(m->setup.vehicle[r->car])->name,
                       total, lap,
                       r->wrecked ? "WRECKED " : "",
                       r->beat_lap ? "LAP RECORD " : "",
                       r->beat_race ? "RACE RECORD" : "");
        }

        ImGui_Separator();
        if (ImGui_ButtonEx("Race again", (ImVec2){ 120.0f, 30.0f })) {
            next = GS_SCREEN_RACE;
        }
        ImGui_SameLine();
        if (ImGui_Button("Change the setup")) next = GS_SCREEN_SETUP;
        ImGui_SameLine();
        if (ImGui_Button("Title")) next = GS_SCREEN_TITLE;
    }
    ImGui_End();
    return next;
}

static gs_screen gs_records_screen(gs_menu *m, const gs_track *t) {
    gs_screen next = GS_SCREEN_RECORDS;
    gs_centre_window("records", 620.0f, 420.0f);

    if (ImGui_Begin("Records", nullptr,
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoCollapse)) {
        gs_world probe;
        gs_world_init(&probe, GS_ONE);
        probe.gravity = m->setup.gravity;
        uint64_t conditions = gs_conditions_hash(&probe);

        ImGui_Text("Track %016llx at %.2fx Earth",
                   (unsigned long long)gs_track_hash(t),
                   (double)m->setup.gravity / (double)GS_ONE);
        ImGui_TextUnformatted("A record is a time on a track under conditions. "
                              "Change the gravity and it is a different table.");
        ImGui_Separator();

        const gs_record *rows[16];
        uint16_t n = gs_records_for(&m->records, gs_track_hash(t), conditions,
                                    rows, 16);
        if (n == 0) {
            ImGui_TextUnformatted("Nobody has been round this one yet.");
        }
        for (uint16_t i = 0; i < n; i++) {
            char lap[32], race[32];
            gs_time_text(lap, sizeof lap, rows[i]->lap);
            gs_time_text(race, sizeof race, rows[i]->race);
            ImGui_Text("%2u. %-14s %-12s lap %-9s %u laps %-9s",
                       i + 1, rows[i]->who, gs_vehicle(rows[i]->vehicle)->name,
                       lap, rows[i]->laps, race);
        }

        ImGui_Separator();
        if (ImGui_Button("back")) next = GS_SCREEN_TITLE;
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

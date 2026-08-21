#include "ui/gs_menu.h"

#include "dcimgui.h"
#include "gfx/gs_render.h"
#include "net/gs_auth.h"
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

    m->screen = GS_SCREEN_LOGIN;
    m->signed_in = -1;
    m->login_pick = -1;
    m->chosen = -1;
    m->picked = -1;
    m->take = -1;
    m->name_for = -1;
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
// Profiles, records and the library in one file, because they are one thing: a
// record with a name on it is only a record if the name still means somebody,
// and it is only a record of anything if the track is still here.
//
// Version 2 added the library. Version 1 files are refused rather than
// half-read - see the tail in COMPLETION_PLAN.md about a migration path, which
// this is now the first real reason to build.

#define GS_STORE_MAGIC   0x54535347u   // "GSST"
#define GS_STORE_VERSION 2u

size_t gs_menu_save(const gs_menu *m, uint8_t *buf, size_t cap) {
    size_t head = 16;
    if (cap < head) return 0;

    size_t pn = gs_profiles_serialize(&m->profiles, buf + head, cap - head);
    if (pn == 0 && m->profiles.count > 0) return 0;

    size_t rn = gs_records_serialize(&m->records, buf + head + pn, cap - head - pn);
    if (rn == 0 && m->records.count > 0) return 0;

    size_t ln = gs_library_serialize(&m->library, buf + head + pn + rn,
                                     cap - head - pn - rn);
    if (ln == 0 && m->library.count > 0) return 0;

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
    buf[12] = (uint8_t)(rn & 0xffu);
    buf[13] = (uint8_t)((rn >> 8) & 0xffu);
    buf[14] = (uint8_t)((rn >> 16) & 0xffu);
    buf[15] = (uint8_t)((rn >> 24) & 0xffu);
    return head + pn + rn + ln;
}

bool gs_menu_load(gs_menu *m, const uint8_t *buf, size_t len) {
    if (len < 16) return false;
    uint32_t magic = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                     ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    if (magic != GS_STORE_MAGIC || buf[4] != GS_STORE_VERSION) return false;

    size_t pn = (size_t)buf[8] | ((size_t)buf[9] << 8) |
                ((size_t)buf[10] << 16) | ((size_t)buf[11] << 24);
    size_t rn = (size_t)buf[12] | ((size_t)buf[13] << 8) |
                ((size_t)buf[14] << 16) | ((size_t)buf[15] << 24);
    if (16 + pn + rn > len) return false;

    if (pn > 0 && !gs_profiles_deserialize(&m->profiles, buf + 16, pn)) return false;
    if (rn > 0 && !gs_records_deserialize(&m->records, buf + 16 + pn, rn)) {
        return false;
    }
    if (16 + pn + rn < len) {
        if (!gs_library_deserialize(&m->library, buf + 16 + pn + rn,
                                    len - 16 - pn - rn)) {
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

// The rename box follows what is picked, so it never shows a name belonging to
// something else.
// **Kept in step by watching, not by remembering to call it.** A field that has
// to be refreshed at every place a selection can change is a field that is
// eventually stale somewhere - and an empty rename box over a track that has a
// name looks like the track lost its name.
static void gs_set_name(gs_menu *m, int index) {
    const gs_library_entry *e = gs_library_at(&m->library, index);
    SDL_strlcpy(m->track_name, e != nullptr ? e->name : "", sizeof m->track_name);
    m->name_for = index;
}

static void gs_follow_selection(gs_menu *m) {
    if (m->name_for != m->picked) gs_set_name(m, m->picked);
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
// How much of the window a panel leaves alone on each side, when it is big
// enough to want all of it.
#define GS_PANEL_MARGIN 8.0f

static void gs_centre_window(const char *title, float w, float h) {
    ImGuiViewport *vp = ImGui_GetMainViewport();

    // **Never bigger than the window it is in.** These panels cannot be moved,
    // resized or collapsed, so a panel taller than the window is centred with
    // its own first rows above the top edge and no way to reach them - which is
    // what a library of thirty-two tracks did to the screen you choose a track
    // on. Clamped, what does not fit scrolls instead, which is reachable.
    // The margin is so a panel that only just fits does not sit edge to edge
    // with the window, which reads as a drawing mistake.
    float most_w = vp->WorkSize.x - GS_PANEL_MARGIN * 2.0f;
    float most_h = vp->WorkSize.y - GS_PANEL_MARGIN * 2.0f;
    if (w > most_w) w = most_w;
    if (h > most_h) h = most_h;

    ImGui_SetNextWindowPosEx((ImVec2){ vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                       vp->WorkPos.y + vp->WorkSize.y * 0.5f },
                             ImGuiCond_Always, (ImVec2){ 0.5f, 0.5f });
    ImGui_SetNextWindowSize((ImVec2){ w, h }, ImGuiCond_Always);
    (void)title;
}

// **What the panel came out as**, noted on the way out of every screen.
//
// A panel is centred at a size its screen worked out, and a size worked out by
// hand goes stale the moment somebody adds a button to the screen: the panel
// stays the height it was and the last thing in it is drawn half outside. Worse
// is a panel taller than the window, which puts its own first row *above* the
// top edge - and these windows cannot be moved or resized, so what is up there
// is unreachable rather than merely ugly. Neither shows up in a screenshot
// test, because both screenshots look like a panel. They show up here.
static void gs_panel_measure(gs_menu *m) {
    ImGuiViewport *vp = ImGui_GetMainViewport();
    ImVec2 pos = ImGui_GetWindowPos();
    ImVec2 size = ImGui_GetWindowSize();
    m->panel = (gs_panel_report){ pos.x, pos.y, size.x, size.y,
                                  vp->WorkSize.x, vp->WorkSize.y,
                                  ImGui_GetScrollMaxY() };
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

// --- the door ---------------------------------------------------------------

static uint64_t gs_now(void);

// Forget what was typed. Called the moment a password has been checked, right
// or wrong: gs_menu is a big struct that gets saved, copied and passed around,
// and a password left in it is a password in more places than anybody meant.
static void gs_forget_typing(gs_menu *m) {
    SDL_memset(m->login_password, 0, sizeof m->login_password);
    SDL_memset(m->login_confirm, 0, sizeof m->login_confirm);
    SDL_memset(m->login_code, 0, sizeof m->login_code);
}

// **The whole rule, in one function.** Whether somebody gets in is decided
// here and nowhere else, so there is one thing to read when asking what the
// gate actually checks.
bool gs_menu_sign_in(gs_menu *m, int index, const char *password,
                     const char *code) {
    m->login_error[0] = '\0';
    if (password == nullptr) password = "";
    if (code == nullptr) code = "";

    if (index < 0 || index >= (int)m->profiles.count) {
        SDL_strlcpy(m->login_error, "pick a driver first", sizeof m->login_error);
        return false;
    }

    gs_profile *p = &m->profiles.entry[index];

    // **No password, no entry.** A driver from a roster written before
    // passwords existed has none, and the answer is to give them one rather
    // than to wave them through - a door that opens for anybody carrying no key
    // is a picture of a door.
    if (p->password[0] == '\0') {
        SDL_strlcpy(m->login_error,
                    "this driver has no password yet - set one to sign in",
                    sizeof m->login_error);
        return false;
    }

    {
        if (!gs_auth_check_password(p->password, password)) {
            // **The same words whichever half was wrong.** Saying "no such
            // driver" and "wrong password" differently tells somebody guessing
            // which half to keep.
            SDL_strlcpy(m->login_error, "that is not the right password",
                        sizeof m->login_error);
            gs_forget_typing(m);
            return false;
        }
    }

    if (p->totp_len > 0) {
        // Six digits, and nothing else. strtoul on "12 34" would happily read
        // 12 and leave the rest, which is a code that half-works.
        uint32_t typed = 0;
        int digits = 0;
        for (const char *c = code; *c != '\0'; c++) {
            if (*c < '0' || *c > '9') { digits = -1; break; }
            typed = typed * 10u + (uint32_t)(*c - '0');
            digits++;
        }
        int64_t step = 0;
        if (digits != GS_AUTH_DIGITS ||
            !gs_auth_check_code(p->totp, p->totp_len, typed,
                                (int64_t)gs_now(), 1, &step)) {
            SDL_strlcpy(m->login_error, "that code is not right, or has expired",
                        sizeof m->login_error);
            gs_forget_typing(m);
            return false;
        }
    }

    m->signed_in = index;

    // Keep what was typed just long enough for the frontend to prove the same
    // name at a server, if there is one. gs_menu_take_server_login wipes it.
    SDL_strlcpy(m->server_password, password, sizeof m->server_password);
    m->server_code = 0;
    for (const char *c = code; *c >= '0' && *c <= '9'; c++)
        m->server_code = m->server_code * 10u + (uint32_t)(*c - '0');
    m->server_login_pending = true;

    gs_forget_typing(m);

    // The driver who signed in is the one who races, so the first player slot
    // is theirs. Anything else means signing in and then being asked again.
    m->setup.profile[0] = (int8_t)m->signed_in;
    m->setup.vehicle[0] = p->vehicle;
    m->setup.colour[0] = p->colour;
    return true;
}

// What to call a track on screen. A track is *known* by its hash - that is what
// makes two copies of the same ground the same track, and it is what a record is
// filed under - but "4ac9ccc660fae00f" is not a thing anybody can talk about. If
// the library has this one, its name is used; if it does not, the hash is all
// there is and is shown short rather than in full.
static const char *gs_track_label(const gs_menu *m, const gs_track *t) {
    static char label[GS_LIBRARY_NAME + 24];
    uint64_t hash = gs_track_hash(t);

    int at = gs_library_find(&m->library, hash);
    const gs_library_entry *e = at >= 0 ? gs_library_at(&m->library, at) : nullptr;
    if (e != nullptr && e->name[0] != '\0') {
        SDL_strlcpy(label, e->name, sizeof label);
        return label;
    }

    // Not one of ours: an unnamed track somebody just built, or one that came
    // from a server. Sixteen hex digits is a fingerprint, not a name, so only
    // enough of it to tell two apart is shown.
    SDL_snprintf(label, sizeof label, "unnamed track (%04llx)",
                 (unsigned long long)(hash & 0xffffu));
    return label;
}

static gs_screen gs_login_screen(gs_menu *m) {
    gs_screen next = GS_SCREEN_LOGIN;
    gs_centre_window("login", 470.0f, 430.0f);

    if (ImGui_Begin("##login", nullptr,
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar)) {
        ImGui_SetWindowFontScale(2.6f);
        float w = ImGui_CalcTextSize("GEARSTICK").x;
        ImGui_SetCursorPosX((ImGui_GetWindowWidth() - w) * 0.5f);
        ImGui_TextUnformatted("GEARSTICK");
        ImGui_SetWindowFontScale(1.0f);

        ImGui_PushStyleColorImVec4(ImGuiCol_Text,
                                   ImGui_GetStyle()->Colors[ImGuiCol_TextDisabled]);
        const char *sub = m->login_making  ? "a new driver"
                        : m->login_setting ? "choose a password"
                                           : "who is driving?";
        w = ImGui_CalcTextSize(sub).x;
        ImGui_SetCursorPosX((ImGui_GetWindowWidth() - w) * 0.5f);
        ImGui_TextUnformatted(sub);
        ImGui_PopStyleColor();

        ImGui_Dummy((ImVec2){ 0.0f, 14.0f });

        if (m->login_making) {
            // --- making somebody new ------------------------------------
            if (m->focus_form) { ImGui_SetKeyboardFocusHere(); m->focus_form = false; }
            ImGui_InputText("name", m->new_name, sizeof m->new_name, 0);
            ImGui_InputTextEx("password", m->login_password,
                              sizeof m->login_password,
                              ImGuiInputTextFlags_Password, nullptr, nullptr);
            ImGui_InputTextEx("again", m->login_confirm, sizeof m->login_confirm,
                              ImGuiInputTextFlags_Password, nullptr, nullptr);

            ImGui_PushStyleColorImVec4(ImGuiCol_Text,
                                       ImGui_GetStyle()->Colors[ImGuiCol_TextDisabled]);
            ImGui_TextWrapped("Tab moves between the boxes. Every driver has a "
                              "password.");
            ImGui_PopStyleColor();

            ImGui_Dummy((ImVec2){ 0.0f, 8.0f });
            if (gs_go_button("CREATE", -1.0f, 40.0f)) {
                // Nothing is created until everything is right: a form
                // abandoned half way through must leave nobody behind.
                if (m->new_name[0] == '\0') {
                    SDL_strlcpy(m->login_error, "a driver needs a name",
                                sizeof m->login_error);
                } else if (m->login_password[0] == '\0') {
                    SDL_strlcpy(m->login_error, "a driver needs a password",
                                sizeof m->login_error);
                } else if (SDL_strcmp(m->login_password, m->login_confirm) != 0) {
                    SDL_strlcpy(m->login_error, "those two passwords are different",
                                sizeof m->login_error);
                } else {
                    int added = gs_profile_add(&m->profiles, m->new_name,
                                               m->new_colour, m->new_vehicle);
                    if (added < 0) {
                        SDL_strlcpy(m->login_error,
                                    "that name is taken, empty, or the roster is full",
                                    sizeof m->login_error);
                    } else if (!gs_menu_set_password(m, added, m->login_password,
                                                     m->login_confirm)) {
                        gs_profile_remove(&m->profiles, (uint8_t)added);
                    } else {
                        // Their name carries over to the sign-in box, and the
                        // password does not: they just chose it, so they can
                        // type it.
                        SDL_strlcpy(m->login_name, m->new_name, sizeof m->login_name);
                        m->login_making = false;
                        m->new_name[0] = '\0';
                        m->store_dirty = true;
                        m->login_error[0] = '\0';
                        m->focus_form = true;
                        gs_forget_typing(m);
                    }
                }
            }
            if (gs_wide_button("Back", 32.0f)) {
                m->login_making = false;
                m->new_name[0] = '\0';
                m->login_error[0] = '\0';
                m->focus_form = true;
                gs_forget_typing(m);
            }
        } else if (m->login_setting) {
            // --- an older driver choosing a password ---------------------
            ImGui_Text("%s", m->login_name);
            if (m->focus_form) { ImGui_SetKeyboardFocusHere(); m->focus_form = false; }
            ImGui_InputTextEx("password", m->login_password,
                              sizeof m->login_password,
                              ImGuiInputTextFlags_Password, nullptr, nullptr);
            ImGui_InputTextEx("again", m->login_confirm, sizeof m->login_confirm,
                              ImGuiInputTextFlags_Password, nullptr, nullptr);

            ImGui_Dummy((ImVec2){ 0.0f, 10.0f });
            if (gs_go_button("SET IT AND SIGN IN", -1.0f, 44.0f)) {
                int at = gs_profile_find(&m->profiles, m->login_name);
                if (at >= 0 &&
                    gs_menu_set_password(m, at, m->login_password,
                                         m->login_confirm) &&
                    gs_menu_sign_in(m, at, m->login_password, "")) {
                    m->login_setting = false;
                    next = GS_SCREEN_TITLE;
                }
            }
            if (gs_wide_button("Back", 32.0f)) {
                m->login_setting = false;
                m->login_error[0] = '\0';
                m->focus_form = true;
                gs_forget_typing(m);
            }
        } else {
            // --- signing in ----------------------------------------------
            //
            // **Typed, not chosen from a list.** A list of everybody on the
            // machine answers "who is here" to whoever sits down, which is
            // half of what a password is protecting.
            if (m->focus_form) { ImGui_SetKeyboardFocusHere(); m->focus_form = false; }
            ImGui_InputText("driver", m->login_name, sizeof m->login_name, 0);
            ImGui_InputTextEx("password", m->login_password,
                              sizeof m->login_password,
                              ImGuiInputTextFlags_Password, nullptr, nullptr);

            // Only once the password was right, so this gives away nothing the
            // password did not already.
            if (m->login_wants_code) {
                ImGui_InputTextEx("code", m->login_code, sizeof m->login_code,
                                  ImGuiInputTextFlags_CharsDecimal, nullptr,
                                  nullptr);
            }

            ImGui_PushStyleColorImVec4(ImGuiCol_Text,
                                       ImGui_GetStyle()->Colors[ImGuiCol_TextDisabled]);
            ImGui_TextWrapped("Tab moves between the boxes.");
            ImGui_PopStyleColor();

            ImGui_Dummy((ImVec2){ 0.0f, 10.0f });
            if (gs_go_button("SIGN IN", -1.0f, 44.0f)) {
                if (gs_menu_sign_in_named(m, m->login_name, m->login_password,
                                          m->login_code)) {
                    next = GS_SCREEN_TITLE;
                } else {
                    // Whatever was typed goes, right or wrong. The name stays,
                    // because retyping a name you already got right is a
                    // punishment for mistyping a password.
                    gs_forget_typing(m);
                    m->focus_form = true;
                }
            }
            ImGui_Spacing();
            if (gs_wide_button("New driver", 32.0f)) {
                m->login_making = true;
                m->login_error[0] = '\0';
                m->login_wants_code = false;
                m->focus_form = true;
                gs_forget_typing(m);
            }
        }

        if (gs_wide_button("Exit", 32.0f)) m->quit = true;

        if (m->login_error[0] != '\0') {
            ImGui_Dummy((ImVec2){ 0.0f, 6.0f });
            ImGui_PushStyleColorImVec4(ImGuiCol_Text,
                                       (ImVec4){ 0.95f, 0.45f, 0.40f, 1.0f });
            ImGui_TextWrapped("%s", m->login_error);
            ImGui_PopStyleColor();
        }
        gs_panel_measure(m);
    }
    ImGui_End();
    return next;
}

static gs_screen gs_title(gs_menu *m) {
    gs_screen next = GS_SCREEN_TITLE;
    gs_centre_window("title", 460.0f, 530.0f);

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

        // Who is at the keyboard, said plainly. A machine several people share
        // should never leave somebody guessing whose records they are about to
        // add to.
        char who[64];
        SDL_snprintf(who, sizeof who, "driving as %s", gs_menu_driver(m));
        w = ImGui_CalcTextSize(who).x;
        ImGui_SetCursorPosX((ImGui_GetWindowWidth() - w) * 0.5f);
        ImGui_TextUnformatted(who);
        ImGui_PopStyleColor();

        ImGui_Dummy((ImVec2){ 0.0f, 18.0f });

        // **Play starts at the track, the way it did in 1985.** Choosing what
        // to race on is the first decision, not something buried behind the
        // settings - and the settings screen is where it lands afterwards.
        if (gs_go_button("PLAY", -1.0f, 44.0f)) {
            if (m->online) {
                // **Joining, not choosing.** The track, the grid and the moment
                // it starts all belong to the server, so offering a local track
                // chooser here would be offering a choice that is not there.
                next = GS_SCREEN_LOBBY;
            } else {
                m->tracks_for_race = true;
                next = GS_SCREEN_TRACKS;
            }
        }
        ImGui_Spacing();
        if (gs_wide_button("Tracks", 34.0f)) {
            m->tracks_for_race = false;
            next = GS_SCREEN_TRACKS;
        }
        if (gs_wide_button("Profile", 34.0f)) next = GS_SCREEN_PROFILES;
        if (gs_wide_button("Records", 34.0f)) next = GS_SCREEN_RECORDS;
        if (gs_wide_button("Exit", 34.0f)) m->quit = true;

        ImGui_Dummy((ImVec2){ 0.0f, 14.0f });
        ImGui_Separator();
        ImGui_Spacing();

        ImGui_PushStyleColorImVec4(ImGuiCol_Text,
                                   ImGui_GetStyle()->Colors[ImGuiCol_TextDisabled]);
        ImGui_TextUnformatted("Tab      the construction set");
        ImGui_TextUnformatted("Escape   quit");
        ImGui_Text("%u driver%s, %u track%s", m->profiles.count,
                   m->profiles.count == 1 ? "" : "s", m->library.count,
                   m->library.count == 1 ? "" : "s");
        ImGui_PopStyleColor();

        // Signing out returns to the door. The gate at the top of
        // gs_menu_frame does the rest, which is why this only has to forget.
        if (ImGui_SmallButton("sign out")) {
            m->signed_in = -1;
            m->login_pick = -1;
            m->login_error[0] = '\0';
        }

        if (m->status[0] != '\0') ImGui_TextUnformatted(m->status);
        gs_panel_measure(m);
    }
    ImGui_End();
    return next;
}

static gs_screen gs_profiles_screen(gs_menu *m) {
    gs_screen next = GS_SCREEN_PROFILES;
    gs_centre_window("drivers", 560.0f, 510.0f);

    if (ImGui_Begin("Drivers", nullptr,
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoCollapse)) {
        // **Your own driver, and nobody else's.** This screen used to list the
        // whole roster with an edit and a remove beside every row, which meant
        // signing in as anybody let you rename, repaint and delete everybody -
        // and a door that lets whoever walks through it edit the other people
        // on the machine is not much of a door.
        if (m->signed_in < 0 || m->signed_in >= (int)m->profiles.count) {
            ImGui_TextUnformatted("Nobody is signed in.");
            ImGui_Separator();
            if (ImGui_Button("back")) next = GS_SCREEN_TITLE;
            gs_panel_measure(m);
            ImGui_End();
            return next;
        }

        gs_profile *me = &m->profiles.entry[m->signed_in];

        ImGui_TextUnformatted("Your driver. A record with a name on it is "
                              "somebody's record.");
        ImGui_Separator();

        gs_swatch(me->colour);
        ImGui_SameLine();
        ImGui_Text("%-14s %s", me->name, gs_vehicle(me->vehicle)->name);
        ImGui_SameLine();
        ImGui_Text("  %u race%s, %u won", me->races, me->races == 1 ? "" : "s",
                   me->wins);

        ImGui_Separator();

        // The fields start on whoever is signed in, so this screen edits them
        // without anybody having to press an "edit" button first.
        if (m->editing != m->signed_in) {
            m->editing = m->signed_in;
            SDL_strlcpy(m->new_name, me->name, sizeof m->new_name);
            m->new_colour = me->colour;
            m->new_vehicle = me->vehicle;
        }

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

        if (ImGui_Button("save")) {
            if (m->new_name[0] == '\0') {
                SDL_snprintf(m->status, sizeof m->status, "a driver needs a name");
            } else {
                int clash = gs_profile_find(&m->profiles, m->new_name);
                if (clash >= 0 && clash != m->signed_in) {
                    SDL_snprintf(m->status, sizeof m->status,
                                 "somebody else on this machine is called that");
                } else {
                    SDL_strlcpy(me->name, m->new_name, sizeof me->name);
                    me->colour = m->new_colour;
                    me->vehicle = m->new_vehicle;
                    m->store_dirty = true;
                    SDL_snprintf(m->status, sizeof m->status, "saved");
                }
            }
        }
        ImGui_SameLine();
        if (ImGui_Button("undo")) {
            SDL_strlcpy(m->new_name, me->name, sizeof m->new_name);
            m->new_colour = me->colour;
            m->new_vehicle = me->vehicle;
            m->status[0] = '\0';
        }

        // --- the lock on the driver who is signed in ------------------------
        //
        // **Only your own.** Changing somebody else's password from a screen
        // they are not standing at would make the gate decorative.
        {
            ImGui_Separator();
            ImGui_Text("%s's password", me->name);

            ImGui_InputTextEx("new", m->login_password, sizeof m->login_password,
                              ImGuiInputTextFlags_Password, nullptr, nullptr);
            ImGui_InputTextEx("confirm", m->login_confirm,
                              sizeof m->login_confirm,
                              ImGuiInputTextFlags_Password, nullptr, nullptr);

            // **Changed, never removed.** Every driver has a password, so
            // there is no button here that takes one off - the way to stop
            // using a driver is to remove the driver.
            if (ImGui_Button("change")) {
                if (gs_menu_set_password(m, m->signed_in, m->login_password,
                                         m->login_confirm)) {
                    SDL_snprintf(m->status, sizeof m->status, "password changed");
                } else {
                    SDL_strlcpy(m->status, m->login_error, sizeof m->status);
                }
                gs_forget_typing(m);
            }
        }

        ImGui_Separator();
        if (ImGui_Button("back")) next = GS_SCREEN_TITLE;
        if (m->status[0] != '\0') {
            ImGui_SameLine();
            ImGui_TextUnformatted(m->status);
        }
        gs_panel_measure(m);
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
        ImGui_Text("%s", gs_track_label(m, t));
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
        if (ImGui_ButtonEx("Tracks", (ImVec2){ 110.0f, 42.0f })) {
            next = GS_SCREEN_TRACKS;
        }
        ImGui_SameLine();
        if (ImGui_ButtonEx("Back", (ImVec2){ 100.0f, 42.0f })) next = GS_SCREEN_TITLE;

        if (!ok) {
            ImGui_SameLine();
            ImGui_PushStyleColorImVec4(ImGuiCol_Text, (ImVec4){ 0.95f, 0.5f, 0.3f, 1.0f });
            ImGui_TextUnformatted("  the route has to be sound - fix it in the\n"
                                  "  construction set (Tab)");
            ImGui_PopStyleColor();
        }
        gs_panel_measure(m);
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
        gs_panel_measure(m);
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
                     222.0f + gs_row_height() * (float)(n > 0 ? n + 1 : 1));

    if (ImGui_Begin("Records", nullptr,
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoCollapse)) {
        ImGui_Text("%s", gs_track_label(m, t));
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
        gs_panel_measure(m);
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
            gs_panel_measure(m);
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
        gs_panel_measure(m);
    }
    ImGui_End();
    return next;
}

bool gs_menu_sign_in_named(gs_menu *m, const char *name, const char *password,
                           const char *code) {
    m->login_error[0] = '\0';
    m->login_wants_code = false;
    m->login_setting = false;

    // **One answer for both kinds of wrong.** "No such driver" and "wrong
    // password" told apart is a way to ask who is on this machine one guess at
    // a time, which is the same question the removed list used to answer for
    // free.
    static const char *refuse = "that name and password do not match";

    int at = gs_profile_find(&m->profiles, name != nullptr ? name : "");
    if (at < 0) {
        SDL_strlcpy(m->login_error, refuse, sizeof m->login_error);
        return false;
    }

    // A driver from a roster written before passwords existed. Saying so needs
    // the name first, which somebody had to know to get here.
    if (m->profiles.entry[at].password[0] == '\0') {
        m->login_setting = true;
        SDL_strlcpy(m->login_error,
                    "this driver has no password yet - choose one",
                    sizeof m->login_error);
        return false;
    }

    if (!gs_menu_sign_in(m, at, password, code)) {
        // The code is only ever asked for once the password was right, so
        // saying a code is due gives nothing away that the password did not.
        if (m->profiles.entry[at].totp_len > 0 &&
            gs_auth_check_password(m->profiles.entry[at].password,
                                   password != nullptr ? password : "")) {
            m->login_wants_code = true;
            SDL_strlcpy(m->login_error,
                        code == nullptr || code[0] == '\0'
                            ? "this driver needs the code from your phone"
                            : "that code is not right, or has expired",
                        sizeof m->login_error);
        } else {
            SDL_strlcpy(m->login_error, refuse, sizeof m->login_error);
        }
        return false;
    }
    return true;
}

bool gs_menu_set_password(gs_menu *m, int index, const char *password,
                          const char *again) {
    m->login_error[0] = '\0';
    if (password == nullptr) password = "";
    if (again == nullptr) again = "";

    if (index < 0 || index >= (int)m->profiles.count) {
        SDL_strlcpy(m->login_error, "pick a driver first", sizeof m->login_error);
        return false;
    }
    if (password[0] == '\0') {
        SDL_strlcpy(m->login_error, "a driver needs a password",
                    sizeof m->login_error);
        return false;
    }
    if (SDL_strcmp(password, again) != 0) {
        SDL_strlcpy(m->login_error, "those two passwords are different",
                    sizeof m->login_error);
        return false;
    }

    gs_profile *p = &m->profiles.entry[index];
    if (!gs_auth_hash_password(password, p->password, sizeof p->password)) {
        SDL_strlcpy(m->login_error, "could not set that password",
                    sizeof m->login_error);
        return false;
    }
    m->store_dirty = true;
    return true;
}

const char *gs_menu_driver(const gs_menu *m) {
    if (m->signed_in < 0 || m->signed_in >= (int)m->profiles.count) return "";
    return m->profiles.entry[m->signed_in].name;
}

bool gs_menu_take_server_login(gs_menu *m, char *password, size_t cap,
                               uint32_t *code) {
    if (!m->server_login_pending) return false;
    if (password != nullptr && cap > 0)
        SDL_strlcpy(password, m->server_password, cap);
    if (code != nullptr) *code = m->server_code;

    SDL_memset(m->server_password, 0, sizeof m->server_password);
    m->server_code = 0;
    m->server_login_pending = false;
    return true;
}

int gs_menu_take_choice(gs_menu *m) {
    int take = m->take;
    m->take = -1;
    return take;
}

// Everything on the library screen that is not the list of tracks: the two
// lines at the top, the two headings, what is known about the one picked, and
// the buttons along the bottom. Measured once by growing it until nothing was
// hidden, which is what the panel test now checks on every build.
#define GS_TRACKS_CHROME 348.0f

static gs_screen gs_tracks_screen(gs_menu *m, const gs_track *t) {
    gs_screen next = GS_SCREEN_TRACKS;

    // **The list scrolls; the panel does not grow.** A library is the one thing
    // on any of these screens with no upper bound worth designing around - it
    // holds thirty-two tracks today and the whole point of the editor is that
    // it fills up - so the space left over after everything else decides how
    // many rows are on screen at once, and the rest are a scroll away. Sizing
    // the panel to the library instead is what put its first entry above the
    // top of the window, on a panel that cannot be moved.
    ImGuiViewport *vp = ImGui_GetMainViewport();
    float row = gs_row_height();
    float spare = vp->WorkSize.y - GS_PANEL_MARGIN * 2.0f - GS_TRACKS_CHROME;

    int rows = m->library.count > 0 ? m->library.count : 1;
    int fits = (int)(spare / row) - 1;          // less the table's header row
    if (fits < 3) fits = 3;                     // something to aim at, always
    if (rows > fits) rows = fits;

    float list_h = row * (float)(rows + 1);
    gs_centre_window("tracks", 720.0f, GS_TRACKS_CHROME + list_h);

    if (ImGui_Begin("Tracks", nullptr,
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoCollapse)) {
        ImGui_TextUnformatted("Everything you have built. A track is known by "
                              "what it is, so the same");
        ImGui_TextUnformatted("track from two people is one entry.");

        gs_heading("THE LIBRARY");

        if (m->library.count == 0) {
            ImGui_PushStyleColorImVec4(ImGuiCol_Text,
                                       ImGui_GetStyle()->Colors[ImGuiCol_TextDisabled]);
            ImGui_TextUnformatted("Nothing here yet. Build something in the "
                                  "construction set and keep it.");
            ImGui_PopStyleColor();
        } else if (ImGui_BeginTableEx("library", 4,
                                      ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_SizingFixedFit,
                                      (ImVec2){ 0.0f, list_h }, 0.0f)) {
            ImGui_TableSetupColumnEx("", ImGuiTableColumnFlags_WidthFixed, 30.0f, 0);
            ImGui_TableSetupColumnEx("track", ImGuiTableColumnFlags_WidthFixed, 240.0f, 0);
            ImGui_TableSetupColumnEx("by", ImGuiTableColumnFlags_WidthFixed, 130.0f, 0);
            ImGui_TableSetupColumnEx("", ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
            // The heading stays put while the tracks go past it, or a scrolled
            // list is four unlabelled columns.
            ImGui_TableSetupScrollFreeze(0, 1);
            ImGui_TableHeadersRow();

            uint64_t here = gs_track_hash(t);

            for (uint16_t i = 0; i < m->library.count; i++) {
                const gs_library_entry *e = gs_library_at(&m->library, (int)i);
                ImGui_TableNextRow();
                ImGui_PushIDInt(6000 + i);

                ImGui_TableSetColumnIndex(0);
                ImGui_Text("%u", i + 1);

                ImGui_TableSetColumnIndex(1);
                if (ImGui_SelectableEx(e->name, m->picked == (int)i,
                                       ImGuiSelectableFlags_SpanAllColumns,
                                       (ImVec2){ 0.0f, 0.0f })) {
                    m->picked = (int)i;
                }

                ImGui_TableSetColumnIndex(2);
                ImGui_TextUnformatted(e->author[0] != '\0' ? e->author : "-");

                ImGui_TableSetColumnIndex(3);
                if (e->hash == here) {
                    float r, g, b;
                    gs_style_accent(&r, &g, &b);
                    ImGui_PushStyleColorImVec4(ImGuiCol_Text, (ImVec4){ r, g, b, 1.0f });
                    ImGui_TextUnformatted("loaded");
                    ImGui_PopStyleColor();
                }
                ImGui_PopID();
            }
            ImGui_EndTable();
        }

        gs_heading("THIS ONE");

        gs_follow_selection(m);
        const gs_library_entry *picked = gs_library_at(&m->library, m->picked);
        if (picked == nullptr) {
            ImGui_PushStyleColorImVec4(ImGuiCol_Text,
                                       ImGui_GetStyle()->Colors[ImGuiCol_TextDisabled]);
            ImGui_TextUnformatted("Nothing chosen.");
            ImGui_PopStyleColor();
        } else {
            ImGui_Text("%u x %u, %u gates, %016llx", picked->track.w,
                       picked->track.h, picked->track.gate_count,
                       (unsigned long long)picked->hash);

            gs_field("name");
            ImGui_SetNextItemWidth(260.0f);
            if (ImGui_InputText("##name", m->track_name, sizeof m->track_name, 0)) {
                // Renaming is not editing: the track is the same track, so the
                // hash does not move and nothing else in the library cares.
                gs_library_put(&m->library, &picked->track, m->track_name,
                               picked->author);
                m->store_dirty = true;
            }
        }

        ImGui_Dummy((ImVec2){ 0.0f, 8.0f });
        ImGui_Separator();
        ImGui_Spacing();

        ImGui_BeginDisabled(picked == nullptr);
        if (gs_go_button(m->tracks_for_race ? "Race this one" : "Load",
                         m->tracks_for_race ? 160.0f : 130.0f, 38.0f)) {
            m->take = m->picked;
            next = GS_SCREEN_SETUP;
        }
        ImGui_SameLine();
        if (ImGui_ButtonEx("Forget", (ImVec2){ 110.0f, 38.0f })) {
            if (picked != nullptr) {
                gs_library_remove(&m->library, picked->hash);
                m->picked = -1;
                m->store_dirty = true;
            }
        }
        ImGui_EndDisabled();

        ImGui_SameLine();
        if (ImGui_ButtonEx("Keep this one", (ImVec2){ 150.0f, 38.0f })) {
            // Whatever is loaded, into the library. The commonest thing
            // somebody wants after building something.
            int at = gs_library_put(&m->library, t, "untitled", "");
            if (at >= 0) {
                m->picked = at;
                m->store_dirty = true;
            } else {
                SDL_snprintf(m->status, sizeof m->status, "the library is full");
            }
        }
        ImGui_SameLine();
        if (ImGui_ButtonEx("Back", (ImVec2){ 100.0f, 38.0f })) next = GS_SCREEN_TITLE;

        if (m->status[0] != '\0') ImGui_TextUnformatted(m->status);
        gs_panel_measure(m);
    }
    ImGui_End();
    return next;
}

gs_screen gs_menu_frame(gs_menu *m, const gs_track *t) {
    // **The gate, enforced once.** Every other screen is unreachable until
    // somebody has signed in, and this is the only place that is decided - a
    // check each screen had to remember to make is a check one of them
    // eventually will not. It also catches a screen arrived at from outside,
    // which is how --shot asks for one by name.
    if (m->signed_in < 0 || m->signed_in >= (int)m->profiles.count) {
        m->signed_in = -1;
        m->screen = GS_SCREEN_LOGIN;
    }

    switch (m->screen) {
    case GS_SCREEN_LOGIN:    return gs_login_screen(m);
    case GS_SCREEN_TITLE:    return gs_title(m);
    case GS_SCREEN_PROFILES: return gs_profiles_screen(m);
    case GS_SCREEN_SETUP:    return gs_setup_screen(m, t);
    case GS_SCREEN_RESULTS:  return gs_results_screen(m);
    case GS_SCREEN_RECORDS:  return gs_records_screen(m, t);
    case GS_SCREEN_LOBBY:    return gs_lobby_screen(m);
    case GS_SCREEN_TRACKS:   return gs_tracks_screen(m, t);
    default:                 return m->screen;
    }
}

// --- what a race did --------------------------------------------------------

// The wall clock, in Unix seconds, for stamping a record with the day it was
// set. Nothing in src/core/ may read a clock - it links nothing, and a result
// that depended on the time of day would not be reproducible - so it is asked
// for out here and passed in.
static uint64_t gs_now(void) {
    SDL_Time now = 0;
    if (!SDL_GetCurrentTime(&now)) return 0;
    return (uint64_t)(now / 1000000000);
}

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

        // The clock comes from here rather than from the table: src/core/ links
        // nothing, and a simulation that could read the time is a simulation
        // whose answer depends on when it ran.
        gs_record_beat beat = gs_records_submit(
            &m->records, track, conditions, m->setup.vehicle[r->car],
            w->mode, w->laps_to_win, r->best_lap, r->finish_tick,
            m->profiles.entry[who].name, gs_now());

        r->beat_lap = beat.lap;
        r->beat_race = beat.race;

        gs_profile_raced(&m->profiles, (uint8_t)who, r->place == 1,
                         r->place <= 3, r->wrecked, r->laps, gs_now());
        m->store_dirty = true;
    }
}

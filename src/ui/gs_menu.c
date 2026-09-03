#include "ui/gs_menu.h"

#include "dcimgui.h"
#include "gfx/gs_render.h"
#include "net/gs_auth.h"
#include "ui/gs_style.h"

#include <SDL3/SDL.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

// The gravity buttons are **the simulation's own list**, which is also the list
// the construction set's palette offers - so a player who learned "Mars" in the
// editor finds the same Mars here, and finds it because it is the same eight
// entries rather than because somebody kept two copies in step. This file had a
// second copy of them for a while; it agreed to the last digit, which is how
// that sort of thing survives long enough to stop agreeing.

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
    m->share_with = -1;
    m->code_for = -1;

    // **Not zero.** GS_SCREEN_LOGIN *is* zero, so a records screen that had
    // never been told where it came from sent Back to the sign-in door - which
    // is the front end throwing somebody out for glancing at a lap time. The
    // screen-graph test found this within an hour of the field being added.
    m->records_from = GS_SCREEN_TITLE;
    m->take_focus = false;
    m->confirm_delete = false;
    m->setup_from = GS_SCREEN_TITLE;
    m->tracks_from = GS_SCREEN_TITLE;
    m->resume = false;

    m->setup.players = 2;
    m->setup.mode = (uint8_t)GS_MODE_RACE;
    m->setup.laps = 3;

    // **Off, and stocked for when it is turned on.** A first race should be a
    // race; the numbers are here so that switching weapons on is one press
    // rather than one press and then four dials from zero.
    m->setup.weapons = false;
    m->setup.ammo[GS_HAZ_OIL]   = 3;
    m->setup.ammo[GS_HAZ_MINE]  = 2;
    m->setup.ammo[GS_HAZ_SMOKE] = 3;
    m->setup.ammo[GS_HAZ_FLAME] = 2;
    m->setup.gravity = GS_ONE;
    m->setup.gravity_preset = 4;                 // Earth
    m->setup.skill = GS_AI_SKILL_DEFAULT;

    // **The second car is somebody by default.** Starting a race and finding
    // one car on the grid is the thing this exists to stop; the first slot is
    // whoever is at the keyboard and the rest are the game until somebody says
    // otherwise.
    for (uint8_t i = 0; i < GS_MAX_CARS; i++) m->setup.computer[i] = i > 0;

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

// The header, and then each of the three parts at whatever they serialise to.
size_t gs_menu_size(const gs_menu *m) {
    return 16 + gs_profiles_size(&m->profiles) + gs_records_size(&m->records) +
           gs_library_size(&m->library);
}

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

    // The code follows the selection too, and only when it changes: a code is
    // the whole track packed and base64'd, which is not work to repeat sixty
    // times a second for a field nobody may be looking at.
    if (m->code_for != m->picked) {
        m->code_for = m->picked;
        m->track_code[0] = '\0';
        const gs_library_entry *e = gs_library_at(&m->library, m->picked);
        if (e != nullptr) {
            gs_track_to_code(&e->track, m->track_code, sizeof m->track_code);
        }
    }
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

// **What every screen's window is opened with.** Fixed where it is put, and
// scrolling in both directions when what is on it does not fit - which is the
// promise gs_centre_window makes when it clamps a panel to the window it is in.
#define GS_PANEL_FLAGS (ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | \
                        ImGuiWindowFlags_NoCollapse)

// **What this panel is opened with**, which depends on whether it fitted.
//
// The horizontal scrollbar is asked for only when the width was actually taken
// away, and that is not fussiness. A window carrying the flag shows the
// scrollbar whenever its contents are as wide as it is - and a Separator, or
// anything else drawn at the full width, *is* exactly as wide as it is. So
// every panel with a rule across it grew a permanent scrollbar with the grip
// filling the whole track: fourteen pixels of furniture that scrolls nothing,
// on screens that fit perfectly well. Found by photographing them.
//
// Whether the panel was clamped is known right here, before anything is drawn,
// which is what makes this exact rather than a guess about content.
static ImGuiWindowFlags gs_centre_window(const char *title, float w, float h) {
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
    const bool squeezed = w > most_w;
    if (w > most_w) w = most_w;
    if (h > most_h) h = most_h;

    ImGui_SetNextWindowPosEx((ImVec2){ vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                       vp->WorkPos.y + vp->WorkSize.y * 0.5f },
                             ImGuiCond_Always, (ImVec2){ 0.5f, 0.5f });
    ImGui_SetNextWindowSize((ImVec2){ w, h }, ImGuiCond_Always);
    (void)title;
    return squeezed ? (GS_PANEL_FLAGS | ImGuiWindowFlags_HorizontalScrollbar)
                    : GS_PANEL_FLAGS;
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

    // **And whether it is the window listening.** A panel opened over a race
    // arrives behind the race's own windows, and a panel that is drawn but not
    // focused eats the first click on it to take the focus - which from a chair
    // is a dialog that ignores you once and then works, and was reported as
    // exactly that. Recorded here because here is inside every panel's begin
    // and end, which is the only place the question can be asked.
    m->panel_focused = ImGui_IsWindowFocused(
        ImGuiFocusedFlags_RootAndChildWindows);

    m->panel = (gs_panel_report){ pos.x, pos.y, size.x, size.y,
                                  vp->WorkSize.x, vp->WorkSize.y,
                                  ImGui_GetScrollMaxY(), ImGui_GetScrollMaxX() };
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

    // **Shorter when there is nothing to fill in.** A machine nobody has driven
    // yet shows one sentence and one button, and a panel sized for a form that
    // is not there is a rectangle of empty screen under it. The roster being
    // empty is a settled fact rather than something that changes while somebody
    // is looking at it, which is why this may depend on it and the tracks
    // screen's list may not.
    const bool nobody = m->profiles.count == 0 && !m->login_making &&
                        !m->login_setting;
    const ImGuiWindowFlags panel =
        gs_centre_window("login", 470.0f, nobody ? 260.0f : 430.0f);

    if (ImGui_Begin("##login", nullptr,
                    panel | ImGuiWindowFlags_NoTitleBar)) {
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
        } else if (m->profiles.count == 0) {
            // --- nobody has driven here yet -------------------------------
            //
            // **The loud button has to be the one that can work.** On a machine
            // where nobody has a driver yet, SIGN IN cannot succeed under any
            // name or any password - and it was the big blue one, three times
            // the size of the button beside it. The first thing a new player
            // was invited to press was the only thing on the screen guaranteed
            // to fail, and what it says when it fails is that the driver does
            // not exist, which reads like the game refusing them.
            //
            // There is nothing to sign in to, so the boxes are not drawn
            // either. One sentence and one button.
            ImGui_PushStyleColorImVec4(ImGuiCol_Text,
                                       ImGui_GetStyle()->Colors[ImGuiCol_TextDisabled]);
            ImGui_TextWrapped("Nobody has driven here yet.");
            ImGui_PopStyleColor();

            ImGui_Dummy((ImVec2){ 0.0f, 10.0f });
            if (gs_go_button("NEW DRIVER", -1.0f, 44.0f)) {
                m->login_making = true;
                m->login_error[0] = '\0';
                m->login_wants_code = false;
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
    const ImGuiWindowFlags panel = gs_centre_window("title", 460.0f, 530.0f);

    if (ImGui_Begin("##title", nullptr,
                    panel | ImGuiWindowFlags_NoTitleBar)) {
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
        if (gs_wide_button("Records", 34.0f)) {
            m->records_from = GS_SCREEN_TITLE;
            next = GS_SCREEN_RECORDS;
        }
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
    const ImGuiWindowFlags panel = gs_centre_window("drivers", 560.0f, 510.0f);

    if (ImGui_Begin("Drivers", nullptr, panel)) {
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

// **Is there a race waiting behind this screen?**
//
// Only offline: online, Escape out of a race goes to the lobby and the race
// belongs to the server, so there is nothing here to step back into.
bool gs_menu_setup_is_paused(const gs_menu *m) {
    return !m->online && m->setup_from == GS_SCREEN_RACE;
}

// Where Back goes from the tracks list - the setup it was opened from, or the
// main menu. Checked rather than trusted, the same way the records table does
// it: a stored screen that is neither is a bug, and the main menu is the answer
// that is never wrong.
static gs_screen gs_tracks_back(const gs_menu *m) {
    return m->tracks_from == GS_SCREEN_SETUP ? GS_SCREEN_SETUP : GS_SCREEN_TITLE;
}

static gs_screen gs_setup_screen(gs_menu *m, const gs_track *t) {
    gs_screen next = GS_SCREEN_SETUP;
    // **One row per driver, so the panel is that much taller.** The grid draws
    // a row for every car in the race - two combo boxes and sixteen paint
    // swatches - and this window was a fixed six hundred pixels whatever the
    // count. At three drivers thirty-five pixels of it were below the bottom
    // edge and at four, seventy-two: a panel that cannot be moved or resized,
    // with the Race button on the far side of the fold. Found by measuring
    // every screen from every state the walk is seeded in rather than from the
    // one state that happened to be two.
    // Everything above and below the grid. The weapons row went in under the
    // gravity buttons and is worth sixty of it: a heading, a switch and a row
    // of counts.
    #define GS_SETUP_CHROME 564.0f
    #define GS_SETUP_MAX_AMMO 9
    const float grid_row = ImGui_GetFrameHeight() + ImGui_GetStyle()->ItemSpacing.y;
    const int   grid_rows = m->setup.players > 0 ? (int)m->setup.players : 1;
    // **Wide enough for the eight planets to fit beside the rules.** Seven
    // hundred and sixty was, until a skill dial went in beside the driver
    // count: the left-hand column grew, the gravity buttons went with it, and
    // the last two of them were drawn thirty-three pixels past the panel's
    // right edge - Venus and Jupiter ending in the middle of their own names,
    // with nothing to press on the other side. Nothing in the item hooks can
    // see that, because what they are handed has already been clipped. What
    // sees it is `panel.wider`, and what found it in the first place was a
    // photograph.
    #define GS_SETUP_WIDE 800.0f
    const ImGuiWindowFlags panel =
        gs_centre_window("setup", GS_SETUP_WIDE,
                         GS_SETUP_CHROME + grid_row * (float)grid_rows);

    if (ImGui_Begin("Race setup", nullptr, panel)) {
        gs_track_issue issue = gs_track_validate(t);
        bool ok = issue.problem == GS_TRACK_OK;

        // --- The track, in its own box, because it is context rather than a
        // thing being chosen here. Choosing tracks is the library's job and it
        // does not exist yet - see docs/FEATURES.md, the platform section.
        // **Fifty-six, and it needs all of it.** It was cut to fifty to pay for
        // the weapons row, on the reasoning that two lines of text do not need
        // fifty-six pixels - and the second line came out with its descenders
        // sliced off. Nothing could see that: a child region clipping its own
        // contents is what a child region is for, so no panel measurement is
        // looking. A photograph was. The six pixels came from the gap above the
        // buttons instead, which had them to give.
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

        // **Laps are a loop's question and nobody else's.** A path has a start
        // at one end and a finish at the other, so "three laps" of one would
        // mean driving back down it twice with nothing marking the way -
        // gs_world_laps_needed races it once whatever this says, and a slider
        // that is quietly ignored is worse than one that is not offered.
        bool lapped = t != nullptr && gs_track_is_circuit(t) &&
                      m->setup.mode == (uint8_t)GS_MODE_RACE;

        int laps = (int)m->setup.laps;
        gs_field("laps");
        ImGui_SetNextItemWidth(200.0f);
        ImGui_BeginDisabled(!lapped);
        ImGui_SliderInt("##laps", &laps, 1, 20);
        ImGui_EndDisabled();
        m->setup.laps = (uint16_t)laps;

        if (m->setup.mode == (uint8_t)GS_MODE_RACE && !lapped) {
            gs_field("");
            ImGui_TextUnformatted("a path is raced once, end to end.");
        }

        int players = (int)m->setup.players;
        gs_field("drivers");
        ImGui_SetNextItemWidth(110.0f);
        ImGui_SliderInt("##players", &players, 1, GS_MAX_CARS);
        m->setup.players = (uint8_t)players;

        // **How hard the ones nobody is driving push.** On the same line as how
        // many there are, because the panel at four drivers is already as tall
        // as the window will take: a row of its own put two pixels of it below
        // the bottom edge, and raising the panel's height does not help - it is
        // clamped to the window it cannot be moved out of. Drawn dead when
        // nobody is being driven by the game, because a dial that changes
        // nothing is worse than a dial that is not there.
        bool any_computer = false;
        for (uint8_t i = 0; i < m->setup.players; i++) {
            if (m->setup.computer[i]) any_computer = true;
        }

        int skill = (int)m->setup.skill;
        ImGui_SameLine();
        ImGui_TextUnformatted("skill");
        ImGui_SameLine();
        ImGui_SetNextItemWidth(110.0f);
        ImGui_BeginDisabled(!any_computer);
        ImGui_SliderInt("##skill", &skill, 0, GS_AI_SKILL_STEPS);
        ImGui_EndDisabled();
        m->setup.skill = (uint8_t)skill;

        ImGui_EndGroup();

        ImGui_SameLine();
        ImGui_Dummy((ImVec2){ 24.0f, 0.0f });
        ImGui_SameLine();

        ImGui_BeginGroup();
        ImGui_TextUnformatted("gravity");
        ImGui_Spacing();
        for (int g = 0; g < GS_GRAVITY_PRESETS; g++) {
            if (g % 4 != 0) ImGui_SameLine();
            ImGui_PushIDInt(3000 + g);
            bool on = m->setup.gravity_preset == g;
            if (on) {
                float r, gg, b;
                gs_style_accent(&r, &gg, &b);
                ImGui_PushStyleColorImVec4(ImGuiCol_Button, (ImVec4){ r, gg, b, 0.9f });
            }
            if (ImGui_ButtonEx(gs_gravity_presets[g].name, (ImVec2){ 76.0f, 0.0f })) {
                m->setup.gravity = gs_gravity_presets[g].scale;
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

        // --- What everybody is carrying. **One line, and no heading of its
        // own**, because there is not the height for one: this panel already
        // fills a 720-tall screen at four drivers, and the rule that nothing on
        // it sits below the fold at the size the game opens at is worth more
        // than a section title. It is a race setting like the mode and the
        // gravity above it, so it sits with them.
        //
        // One switch and four counts: "no weapons this time" is a thing
        // somebody says out loud before a race and should be one thing to
        // press, and three slicks and one mine is a different race from one
        // slick and three mines - choosing that is the point.
        ImGui_Checkbox("weapons", &m->setup.weapons);

        ImGui_BeginDisabled(!m->setup.weapons);
        for (int k = GS_HAZ_NONE + 1; k < GS_HAZ_COUNT; k++) {
            ImGui_SameLine();
            ImGui_PushIDInt(3100 + k);
            ImGui_SetNextItemWidth(70.0f);
            int n = (int)m->setup.ammo[k];
            if (ImGui_SliderInt(gs_hazard_name((gs_hazard_kind)k), &n, 0,
                                GS_SETUP_MAX_AMMO)) {
                m->setup.ammo[k] = (uint8_t)GS_CLAMP(n, 0, GS_SETUP_MAX_AMMO);
            }
            ImGui_PopID();
        }
        ImGui_EndDisabled();

        // Where the control is said out loud. The HUD says it again while the
        // race is on, because this screen is gone by then.
        ImGui_SameLine();
        ImGui_PushStyleColorImVec4(ImGuiCol_Text,
                                   ImGui_GetStyle()->Colors[ImGuiCol_TextDisabled]);
        ImGui_TextUnformatted("tap to drop, hold to swap");
        ImGui_PopStyleColor();

        // --- The grid, as a table. One row per driver, columns that line up,
        // which is the whole difference between a form and a pile of widgets.
        gs_heading("THE GRID");

        // **Laid out at the width the screen was designed for, however narrow
        // the window is.** A stretched column squeezed into a smaller window
        // does not scroll - it shrinks, and clips what no longer fits inside
        // its own cell, where the window's scrollbar cannot reach it. At six
        // hundred and forty across that took the last two paint colours off
        // every driver's row: eight to choose from, six that could be seen or
        // pressed, and no scrollbar hinting there were more.
        //
        // So the table is given an explicit width rather than "whatever is
        // left". Past the design width it still stretches to fill the window;
        // below it, the difference becomes something the panel can be scrolled
        // sideways to, which is the promise every one of these windows makes.
        // The chrome is measured rather than assumed, because whether there is
        // a vertical scrollbar taking fourteen pixels depends on the driver
        // count.
        const float room  = ImGui_GetContentRegionAvail().x;
        const float built = GS_SETUP_WIDE - (ImGui_GetWindowWidth() - room);
        const float wide  = room > built ? room : built;

        if (ImGui_BeginTableEx("grid", 4,
                               ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit,
                               (ImVec2){ wide, 0.0f }, 0.0f)) {
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
                const char *who = m->setup.computer[i]
                                      ? "computer"
                                      : gs_profile_name_of(m, m->setup.profile[i]);
                if (ImGui_BeginCombo("##who", who, 0)) {
                    // **Nobody, and the game drives it.** First in the list
                    // because it is what an empty slot should be: a race with a
                    // seat spare is a race with somebody in it.
                    if (ImGui_SelectableEx("computer", m->setup.computer[i], 0,
                                           (ImVec2){ 0.0f, 0.0f })) {
                        m->setup.computer[i] = true;
                    }
                    if (ImGui_SelectableEx("guest",
                                           !m->setup.computer[i] &&
                                               m->setup.profile[i] < 0,
                                           0, (ImVec2){ 0.0f, 0.0f })) {
                        m->setup.profile[i] = -1;
                        m->setup.computer[i] = false;
                    }
                    for (uint8_t k = 0; k < m->profiles.count; k++) {
                        ImGui_PushIDInt(k);
                        if (ImGui_SelectableEx(m->profiles.entry[k].name,
                                               !m->setup.computer[i] &&
                                                   m->setup.profile[i] == (int8_t)k,
                                               0, (ImVec2){ 0.0f, 0.0f })) {
                            m->setup.profile[i] = (int8_t)k;
                            m->setup.computer[i] = false;
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

        // The same conditions a race under these dials would have. Built the
        // way a race builds them rather than by hand: setting `gravity` from
        // the dial directly stores a multiple of Earth where an acceleration
        // belongs, and a table looked up under conditions no race ever had is
        // a table that is always empty.
        gs_world probe;
        gs_world_init(&probe, m->setup.gravity);
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
        // Two rather than eight. Six pixels, given back to the box at the top
        // of this screen that was cut into to pay for the weapons row - see
        // there. A gap above a separator is the cheapest height on the panel.
        ImGui_Dummy((ImVec2){ 0.0f, 2.0f });
        ImGui_Separator();
        ImGui_Spacing();

        // **What these mean depends on whether a race is waiting behind them.**
        //
        // Escape out of a race lands here, and it used to land on a screen whose
        // only ways off were "start a new race" and "main menu" - so a race you
        // stepped out of for a moment could not be stepped back into, though it
        // was still sitting there paused. "Back" meaning "abandon the race you
        // are in" is the wrong word for the wrong thing.
        //
        // Paused, the row reads: GO starts a *new* race on these settings, Back
        // returns to the one already running, and the main menu is its own
        // button rather than something Back does by surprise.
        const bool paused = gs_menu_setup_is_paused(m);

        ImGui_BeginDisabled(!ok);
        if (gs_go_button(paused ? "NEW RACE" : "GO", 160.0f, 42.0f)) {
            next = GS_SCREEN_RACE;
            m->resume = false;
        }
        ImGui_EndDisabled();

        ImGui_SameLine();
        if (paused) {
            // **The same three widths as the row below**, because the panel has
            // to hold it at 640x480 and a fourth button abreast does not fit -
            // nor does a second row, which costs a row of height the screen has
            // not got. Choosing a different track is what the race you are
            // going back to is *on*, so it is the one of the four to leave for
            // when you are not standing in a paused race.
            if (ImGui_ButtonEx("Resume", (ImVec2){ 110.0f, 42.0f })) {
                next = GS_SCREEN_RACE;
                m->resume = true;
            }
            ImGui_SameLine();
            if (ImGui_ButtonEx("Main menu", (ImVec2){ 100.0f, 42.0f })) {
                next = GS_SCREEN_TITLE;
            }
        } else {
            if (ImGui_ButtonEx("Tracks", (ImVec2){ 110.0f, 42.0f })) {
                next = GS_SCREEN_TRACKS;
            }
            ImGui_SameLine();
            if (ImGui_ButtonEx("Back", (ImVec2){ 100.0f, 42.0f })) {
                next = GS_SCREEN_TITLE;
            }
        }

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

// **The column is as wide as the widest thing that goes in it.** A stretch
// column gets what is left over, and what was left over on the results screen
// was eighty pixels - so the one line that tells somebody they have just set a
// record read "lap + race r". Asking the font how wide the sentence is cannot
// go stale when the font changes, and it is the whole sentence or nothing.
#define GS_RECORD_NOTE "lap + race record"

// The five columns before the note, which are what they hold at their widest:
// a place, a name, a machine, a time and a time.
#define GS_RESULT_COLUMNS (34.0f + 150.0f + 130.0f + 100.0f + 100.0f)

static gs_screen gs_results_screen(gs_menu *m) {
    gs_screen next = GS_SCREEN_RESULTS;

    // **Wide enough for its own table, worked out rather than guessed.** A
    // table wider than the window it is in does not overflow; the last column
    // gives up what is missing, and the last column here is the one line that
    // says somebody has just set a record. So the panel is the columns, plus
    // the padding a cell puts on either side of each of them, plus the window's
    // own margin - and nothing has to be re-tuned when the font changes.
    ImGuiStyle *style = ImGui_GetStyle();
    float note = ImGui_CalcTextSize(GS_RECORD_NOTE).x + style->CellPadding.x * 2.0f;
    float w = GS_RESULT_COLUMNS + note + style->CellPadding.x * 2.0f * 6.0f +
              style->WindowPadding.x * 2.0f;

    const ImGuiWindowFlags panel =
        gs_centre_window("results", w,
                         168.0f + gs_row_height() * (float)(m->result_count + 1));

    if (ImGui_Begin("Results", nullptr, panel)) {
        if (ImGui_BeginTable("results", 6,
                             ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            ImGui_TableSetupColumnEx("", ImGuiTableColumnFlags_WidthFixed, 34.0f, 0);
            ImGui_TableSetupColumnEx("driver", ImGuiTableColumnFlags_WidthFixed, 150.0f, 0);
            ImGui_TableSetupColumnEx("machine", ImGuiTableColumnFlags_WidthFixed, 130.0f, 0);
            ImGui_TableSetupColumnEx("time", ImGuiTableColumnFlags_WidthFixed, 100.0f, 0);
            ImGui_TableSetupColumnEx("best lap", ImGuiTableColumnFlags_WidthFixed, 100.0f, 0);
            ImGui_TableSetupColumnEx("", ImGuiTableColumnFlags_WidthFixed, note, 0);
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
                    // The same string the column was measured from, so the
                    // two cannot drift apart.
                    ImGui_TextUnformatted(r->beat_lap && r->beat_race ? GS_RECORD_NOTE
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

        // **Online, the way on is the lobby.** A race on a server is the
        // server's race - this machine does not choose the track, the grid or
        // when it starts - so "race again" is really "back to the room where
        // that is decided", and saying so is the difference between a button
        // that works and a button somebody presses twice.
        if (gs_go_button(m->online ? "Back to the lobby" : "Race again",
                         m->online ? 200.0f : 160.0f, 40.0f)) {
            next = m->online ? GS_SCREEN_LOBBY : GS_SCREEN_RACE;
        }
        ImGui_SameLine();
        // The setup screen decides a race this machine owns, which an online
        // race is not: reading it would build a different world from everybody
        // else's. Offered offline and not online.
        ImGui_BeginDisabled(m->online);
        if (ImGui_ButtonEx("Setup", (ImVec2){ 110.0f, 40.0f })) next = GS_SCREEN_SETUP;
        ImGui_EndDisabled();
        ImGui_SameLine();
        if (ImGui_ButtonEx("Records", (ImVec2){ 110.0f, 40.0f })) {
            m->records_from = GS_SCREEN_RESULTS;
            next = GS_SCREEN_RECORDS;
        }
        ImGui_SameLine();
        // "Title" is what the screen is called in the code and means nothing to
        // anybody looking at it. It is the main menu, so it says so.
        if (ImGui_ButtonEx("Main menu", (ImVec2){ 140.0f, 40.0f })) {
            next = GS_SCREEN_TITLE;
        }
        gs_panel_measure(m);
    }
    ImGui_End();
    return next;
}

// **Where the records screen goes back to.** It is opened from more than one
// place - the title, the setup screen, the results - so "back" means the screen
// that opened it and not a fixed destination. Anything else is the title, which
// is the safe answer for a value nobody could have arrived from.
//
// Written once, here, because it was written twice: the button on the screen
// knew this and Escape did not, so a player who opened the records from their
// results and pressed Escape was put on the main menu instead of back where
// they were. Two ways out of one screen going to two different places.
static gs_screen gs_records_back(const gs_menu *m) {
    const bool sane = m->records_from == GS_SCREEN_TITLE ||
                      m->records_from == GS_SCREEN_RESULTS ||
                      m->records_from == GS_SCREEN_SETUP;
    return sane ? m->records_from : GS_SCREEN_TITLE;
}

static gs_screen gs_records_screen(gs_menu *m, const gs_track *t) {
    gs_screen next = GS_SCREEN_RECORDS;
    // Counted before the window opens, so the panel is the height of its table
    // rather than the height of the biggest table it could ever hold.
    // Built the way a race builds it - see the note on the other one of these.
    gs_world probe;
    gs_world_init(&probe, m->setup.gravity);
    uint64_t conditions = gs_conditions_hash(&probe);

    const gs_record *rows[16];
    uint16_t n = gs_records_for(&m->records, gs_track_hash(t), conditions, rows, 16);

    const ImGuiWindowFlags panel =
        gs_centre_window("records", 720.0f,
                         222.0f + gs_row_height() * (float)(n > 0 ? n + 1 : 1));

    if (ImGui_Begin("Records", nullptr, panel)) {
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
        if (ImGui_ButtonEx("Back", (ImVec2){ 120.0f, 38.0f })) {
            next = gs_records_back(m);
        }
        gs_panel_measure(m);
    }
    ImGui_End();
    return next;
}

static gs_screen gs_lobby_screen(gs_menu *m) {
    gs_screen next = GS_SCREEN_LOBBY;

    int rows = (m->lobby != nullptr && m->lobby->capacity > 0)
                   ? m->lobby->capacity : 4;
    const ImGuiWindowFlags panel =
        gs_centre_window("lobby", 640.0f,
                         220.0f + gs_row_height() * (float)(rows + 1));

    if (ImGui_Begin("Lobby", nullptr, panel)) {
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

            // **A door nobody answers, said out loud.** A server that refuses
            // sends a reason and it arrives as lobby_error. A server that
            // cannot decrypt what we sent has nothing to reply to at all, so
            // the screen stayed on "Knocking..." for as long as somebody was
            // willing to look at it - which is what a player did, twice, with
            // no way to tell a slow connection from a wrong key.
            if (gs_menu_lobby_unanswered(m)) {
                ImGui_Spacing();
                ImGui_PushStyleColorImVec4(ImGuiCol_Text,
                                           (ImVec4){ 0.95f, 0.6f, 0.3f, 1.0f });
                ImGui_Text("No answer after %.0f seconds.",
                           (double)m->knocking_for);
                ImGui_PopStyleColor();
                ImGui_TextUnformatted("The server is not running, the address is");
                ImGui_TextUnformatted("wrong, or its key is not the one it prints");
                ImGui_TextUnformatted("when it starts.");
            }
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
        // **Race**, for a lobby that is waiting on the player rather than on
        // another player. Offered only when it can actually work, because a
        // button that does nothing is worse than no button.
        //
        // `m->lobby_ready` and not a count comparison of its own. The first go
        // at this asked whether count had reached capacity - and before the
        // server has answered, both are zero, so it offered Race to somebody
        // still knocking on the door and did nothing when they pressed it. It
        // also read through `m->lobby` without checking it was there. Every
        // other line on this screen already goes through `heard` or
        // `lobby_ready` for exactly this reason.
        if (gs_menu_lobby_can_race(m)) {
            if (ImGui_ButtonEx("Race", (ImVec2){ 120.0f, 38.0f })) {
                m->race_requested = true;
            }
            ImGui_SameLine();
        }
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
// **What a track's details are given, and no more.** The block under THIS ONE
// grew a name, a note, a code, publishing and a row per person you could hand
// it to - and the last of those has no fixed size, because it depends on how
// many people are in the room. A panel whose height depends on that is a panel
// that is the right size until somebody joins.
//
// So it scrolls inside a box of its own instead, and the panel's height stops
// depending on the lobby entirely.
#define GS_TRACKS_DETAIL 176.0f

// Everything but the detail box and the list: the blurb, the headings, two rows
// of buttons and the panel's own padding.
#define GS_TRACKS_CHROME (394.0f)

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
    // The detail box is only there when there is a track to detail, so with
    // nothing chosen the list gets the room instead of a blank box holding it.
    // **While the question is up, the detail panel stands down.** It is the tall
    // optional half of this screen, and it is about the track being asked about
    // - which the question already names. Trading it for the question keeps the
    // panel the size it was rather than adding to the bottom of a screen that
    // was already the tightest fit in the game.
    bool detailing = !m->confirm_delete &&
                     gs_library_at(&m->library, m->picked) != nullptr;
    float detail_h = detailing ? GS_TRACKS_DETAIL : 0.0f;

    // **What the question costs, taken out of the list rather than added under
    // it.** The question is a line of text and a row of buttons, drawn after
    // everything else - and the panel is sized before any of it exists, so it
    // has to be counted here. Counted as *less room for the list*, not as more
    // panel: added on top it is seventeen rows of a thirty-two track library
    // under the fold at 640x480, which is a list you cannot see the bottom of
    // while being asked a question about the middle of it.
    const float asking_h =
        m->confirm_delete ? ImGui_GetTextLineHeight() + 34.0f +
                            ImGui_GetStyle()->ItemSpacing.y * 4.0f
                          : 0.0f;

    float spare = vp->WorkSize.y - GS_PANEL_MARGIN * 2.0f -
                  GS_TRACKS_CHROME - detail_h - asking_h;

    int rows = m->library.count > 0 ? m->library.count : 1;
    int fits = (int)(spare / row) - 1;          // less the table's header row
    if (fits < 3) fits = 3;                     // something to aim at, always
    if (rows > fits) rows = fits;

    // **And the question takes its rows off the list even when the list fits.**
    // Subtracting from `spare` above only helps when the window is what limits
    // the list; on a taller one the limit is the library's own length, `rows`
    // is left alone and the question goes on the end - seventeen rows of a
    // thirty-two track library under the fold.
    float list_h = row * (float)(rows + 1);

    const ImGuiWindowFlags panel =
        gs_centre_window("tracks", 720.0f,
                         GS_TRACKS_CHROME + detail_h + list_h + asking_h);

    if (ImGui_Begin("Tracks", nullptr, panel)) {
        // **While the question is up, nothing underneath it answers.**
        //
        // That is what a modal means, and it is also what makes the question
        // affordable: the walk explores a screen by pressing what is on it, and
        // a list of thirty-two tracks left live under an unanswered question is
        // thirty-two more states with the question up. Inert, the question is
        // one small state with two buttons in it.
        // **Inert underneath, not gone.** That is what a modal means, and it
        // is what keeps the question one small state rather than one per track
        // in the library: nothing under it can be pressed, so nothing under it
        // is a branch for the walk to follow.
        ImGui_BeginDisabled(m->confirm_delete);

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

        // **No box at all when there is nothing to put in it**, rather than a
        // box of no height.
        //
        // The panel is sized without the detail box when nothing is chosen -
        // that is what `detail_h` being zero means - and a child asked for a
        // height of zero does not take none of the panel, it takes **all of
        // what is left**. So the box swallowed the space under it and the two
        // rows of buttons beneath, New and Back among them, were laid out past
        // the bottom edge of a window that cannot be moved, resized or
        // scrolled. Nothing drew wrong; everything was simply somewhere nobody
        // could press it. Found by a machine building a track and then trying
        // to keep it.
        // `detailing` rather than `picked`, so what is drawn and what the panel
        // was sized for cannot disagree - they are the same answer asked once.
        if (!detailing) {
            ImGui_PushStyleColorImVec4(ImGuiCol_Text,
                                       ImGui_GetStyle()->Colors[ImGuiCol_TextDisabled]);
            ImGui_TextUnformatted("Nothing chosen.");
            ImGui_PopStyleColor();
        } else {
            ImGui_BeginChild("detail", (ImVec2){ 0.0f, detail_h },
                             ImGuiChildFlags_None, ImGuiWindowFlags_None);
            ImGui_Text("%u x %u, %u gates, %016llx", picked->track.w,
                       picked->track.h, picked->track.gate_count,
                       (unsigned long long)picked->hash);

            gs_field("name");
            ImGui_SetNextItemWidth(260.0f);
            ImGui_BeginDisabled(picked->builtin);
            if (ImGui_InputText("##name", m->track_name, sizeof m->track_name, 0)) {
                // Renaming is not editing: the track is the same track, so the
                // hash does not move and nothing else in the library cares.
                gs_library_put(&m->library, &picked->track, m->track_name,
                               picked->author);
                m->store_dirty = true;
            }
            ImGui_EndDisabled();

            if (picked->builtin) {
                ImGui_PushStyleColorImVec4(
                    ImGuiCol_Text, ImGui_GetStyle()->Colors[ImGuiCol_TextDisabled]);
                ImGui_TextUnformatted("came with the game. Edit makes a copy "
                                      "that is yours.");
                ImGui_PopStyleColor();
            }

            // --- handing it to somebody -------------------------------------
            //
            // **Sharing needs a server, because sharing is with people.** The
            // code below is always there - it is a track as text and needs
            // nobody's permission - and the rest appears when there is a room
            // to share into.
            gs_heading("SHARING");

            gs_field("code");
            ImGui_SetNextItemWidth(360.0f);
            ImGui_InputText("##code", m->track_code, sizeof m->track_code,
                            ImGuiInputTextFlags_ReadOnly);
            ImGui_SameLine();
            if (ImGui_ButtonEx("Copy", (ImVec2){ 80.0f, 0.0f })) {
                ImGui_SetClipboardText(m->track_code);
                SDL_snprintf(m->status, sizeof m->status, "copied");
            }

            if (!m->online) {
                ImGui_PushStyleColorImVec4(
                    ImGuiCol_Text, ImGui_GetStyle()->Colors[ImGuiCol_TextDisabled]);
                ImGui_TextUnformatted("Join a server to hand it to somebody, or "
                                      "to publish it.");
                ImGui_PopStyleColor();
            } else {
                if (ImGui_ButtonEx("Publish to everybody", (ImVec2){ 200.0f, 32.0f })) {
                    m->publish_requested = true;
                }
                ImGui_SameLine();
                if (ImGui_ButtonEx("Take it down", (ImVec2){ 140.0f, 32.0f })) {
                    m->withdraw_requested = true;
                }

                // **Named by their key, not by a string somebody typed.** The
                // lobby carries the public key the server watched each player
                // prove, so sharing is with somebody you are in a room with.
                if (m->lobby != nullptr && m->lobby->count > 0) {
                    ImGui_Spacing();
                    ImGui_TextUnformatted("Or hand it to one of these:");
                    for (uint8_t i = 0; i < m->lobby->count; i++) {
                        const char *who = m->lobby->player[i].name;
                        if (who[0] == '\0') continue;
                        if (i == m->lobby_slot) continue;   // not to yourself

                        ImGui_PushIDInt(7200 + i);
                        if (ImGui_ButtonEx(who, (ImVec2){ 150.0f, 28.0f })) {
                            m->share_with = (int)i;
                            m->share_on = true;
                        }
                        ImGui_SameLine();
                        if (ImGui_ButtonEx("take back", (ImVec2){ 100.0f, 28.0f })) {
                            m->share_with = (int)i;
                            m->share_on = false;
                        }
                        ImGui_PopID();
                    }
                }
            }
            ImGui_EndChild();
        }

        ImGui_Dummy((ImVec2){ 0.0f, 8.0f });
        ImGui_Separator();
        ImGui_Spacing();

        bool builtin = picked != nullptr && picked->builtin;

        // **Racing is not this machine's to start while it is on a server.**
        // The track is the server's, the grid is the server's and so is the
        // moment it begins - so choosing a track here and pressing race led to
        // a setup screen that could not start anything, and left somebody back
        // on the results wondering what they had done wrong. Loading one to
        // look at, edit or share is still fine, which is what the button says
        // instead.
        ImGui_BeginDisabled(picked == nullptr);
        if (gs_go_button(m->online ? "Load"
                                   : (m->tracks_for_race ? "Race this one" : "Load"),
                         m->tracks_for_race && !m->online ? 160.0f : 130.0f, 38.0f)) {
            m->take = m->picked;
            next = m->online ? GS_SCREEN_TRACKS : GS_SCREEN_SETUP;
        }

        // **Edit, where somebody would look for it.** This was Tab from
        // anywhere - a key nothing mentioned, on a screen that is about tracks
        // and never said the construction set existed.
        ImGui_SameLine();
        if (ImGui_ButtonEx(builtin ? "Edit a copy" : "Edit",
                           (ImVec2){ 130.0f, 38.0f })) {
            m->take = m->picked;
            m->edit_requested = true;
        }

        // **A track that came with the game is not yours to throw away.** The
        // copy an edit makes is, which is why the button above still works on
        // one and this one does not.
        ImGui_SameLine();
        ImGui_BeginDisabled(builtin);
        if (ImGui_ButtonEx("Delete", (ImVec2){ 110.0f, 38.0f })) {
            if (picked != nullptr) m->confirm_delete = true;
        }
        ImGui_EndDisabled();
        ImGui_EndDisabled();

        // **Two rows, because six buttons do not fit across a panel this
        // wide.** The last of them ran off the right-hand edge, and a control
        // clipped at the edge is a control nobody finds - the same fault the
        // brush palette had. What you do *to this track* is on the first row;
        // what you do to the library is on the second.
        ImGui_SameLine();
        if (ImGui_ButtonEx("New", (ImVec2){ 90.0f, 38.0f })) {
            m->new_requested = true;
        }

        ImGui_Spacing();
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
        // Back to whoever opened it: the main menu, or the race setup somebody
        // was halfway through filling in - which this used to throw away.
        if (ImGui_ButtonEx("Back", (ImVec2){ 100.0f, 38.0f })) {
            next = gs_tracks_back(m);
        }

        ImGui_EndDisabled();     // the screen-wide one, opened at the top

        // **And the question, when one is outstanding.**
        //
        // A row on the panel rather than a window over it: this screen is
        // already centred and clamped to fit, and a second window on top is a
        // second thing to size, to keep on screen and to walk. It names the
        // track, because "are you sure?" without a name is a question nobody
        // can answer safely.
        if (m->confirm_delete) {
            // Looked up here rather than shared with the screen above, which is
            // not drawn while this is: the question replaces it.
            const gs_library_entry *going = gs_library_at(&m->library, m->picked);

            if (going == nullptr) {
                m->confirm_delete = false;   // the pick went; so does the question
            } else {
                ImGui_Spacing();
                ImGui_PushStyleColorImVec4(ImGuiCol_Text,
                                           (ImVec4){ 0.95f, 0.5f, 0.3f, 1.0f });
                ImGui_Text("Delete \"%s\"? This cannot be undone.", going->name);
                ImGui_PopStyleColor();

                if (ImGui_ButtonEx("Delete it", (ImVec2){ 120.0f, 34.0f })) {
                    gs_library_remove(&m->library, going->hash);
                    m->picked = -1;
                    m->store_dirty = true;
                    m->confirm_delete = false;
                }
                ImGui_SameLine();
                if (ImGui_ButtonEx("Keep it", (ImVec2){ 120.0f, 34.0f })) {
                    m->confirm_delete = false;
                }
            }
        }

        if (m->status[0] != '\0') ImGui_TextUnformatted(m->status);
        gs_panel_measure(m);
    }
    ImGui_End();
    return next;
}

bool gs_menu_lobby_unanswered(const gs_menu *m) {
    // Only while there is nothing to show. A lobby that arrived, however
    // slowly, is a lobby that answered - and a server that refused said why,
    // which is a better message than this one.
    if (m->lobby != nullptr && m->lobby->capacity > 0) return false;
    if (m->lobby_error != nullptr && m->lobby_error[0] != '\0') return false;
    return m->knocking_for > GS_KNOCK_PATIENCE;
}

bool gs_menu_lobby_can_race(const gs_menu *m) {
    // Heard from at all, everybody present, and the ground arrived. Any of
    // those missing and starting a race is something the client cannot do.
    if (m->lobby == nullptr || m->lobby->capacity == 0) return false;
    if (!m->lobby_ready) return false;
    return m->track_progress >= 1.0f;
}

gs_screen gs_menu_back(const gs_menu *m, bool editing) {
    // The construction set is a layer over whatever is underneath it, so
    // closing it is the first thing Escape does and it changes no screen.
    if (editing) return m->screen;

    // **Every screen named, and no `default`.** A default here is a screen
    // added next year quietly getting a way out that nobody chose - which is
    // exactly how six surfaces came to sound like pavement. `-Wswitch` makes
    // that a build failure instead.
    switch (m->screen) {
    case GS_SCREEN_RACE:
        // **Out of a race, and online that means the lobby.** A wrecked car in
        // a race that cannot end is otherwise a dead screen with no way off it
        // - which is what an online player got, because the setup screen this
        // used to go to decides a race that belongs to the server.
        return m->online ? GS_SCREEN_LOBBY : GS_SCREEN_SETUP;

    case GS_SCREEN_RESULTS:
        // **And out of the results, online, is the lobby too** - the same place
        // the button on the screen says, which it did not used to be. The
        // results of a server's race led back to the main menu on Escape and to
        // the lobby on the button beside it: one screen, two ways out, two
        // different places, and the one a player reaches for by reflex was the
        // one that left the room.
        return m->online ? GS_SCREEN_LOBBY : GS_SCREEN_TITLE;

    case GS_SCREEN_RECORDS:
        // Whichever screen opened it - see gs_records_back.
        return gs_records_back(m);

    case GS_SCREEN_TITLE:
        return GS_SCREEN_COUNT;      // nothing behind the title but the door

    case GS_SCREEN_LOGIN:
        return GS_SCREEN_COUNT;      // and nothing behind the door but leaving

    case GS_SCREEN_SETUP:
        // **Escape out of a paused race goes back into it**, which is what the
        // key meant when it was pressed: stepping out of a race for a moment
        // and stepping back in. Otherwise it is the main menu, as before.
        return gs_menu_setup_is_paused(m) ? GS_SCREEN_RACE : GS_SCREEN_TITLE;

    case GS_SCREEN_TRACKS:
        // Whichever screen opened it - see gs_tracks_back.
        return gs_tracks_back(m);

    case GS_SCREEN_PROFILES:
    case GS_SCREEN_LOBBY:
        return GS_SCREEN_TITLE;

    case GS_SCREEN_COUNT:
        break;                       // not a screen; nothing to leave
    }
    return GS_SCREEN_TITLE;
}

// FNV-1a, because the only property wanted here is that different states give
// different numbers. Nothing is stored under this hash and nothing travels, so
// none of the reasons to reach for something stronger apply.
// **Eight bytes at a time, because this runs over six hundred kilobytes of menu
// on every step of a walk.** A byte at a time measured at 1.18 ms a call and was
// 87% of what walking the front end cost; a word at a time is the same hash for
// the purpose it has and is not the bottleneck any more. It reads whatever
// order the machine stores its words in, which would matter if this number ever
// travelled or was written down - it does neither.
static uint64_t gs_fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    size_t i = 0;

    for (; i + sizeof(uint64_t) <= n; i += sizeof(uint64_t)) {
        uint64_t word;
        memcpy(&word, b + i, sizeof word);
        h ^= word;
        h *= 0x100000001b3ULL;
    }
    for (; i < n; i++) {
        h ^= (uint64_t)b[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

uint64_t gs_menu_hash(const gs_menu *m) {
    const uint8_t *b = (const uint8_t *)m;
    uint64_t h = 0xcbf29ce484222325ULL;

    // Everything up to the borrowed lobby pointer.
    h = gs_fnv(h, b, offsetof(gs_menu, lobby));

    // Then what the lobby screen owns itself, up to the two clocks.
    h = gs_fnv(h, b + offsetof(gs_menu, lobby_slot),
               offsetof(gs_menu, track_progress) - offsetof(gs_menu, lobby_slot));

    // Then the rest, up to the panel measurement.
    h = gs_fnv(h, b + offsetof(gs_menu, server_text),
               offsetof(gs_menu, panel) - offsetof(gs_menu, server_text));

    // And the message itself rather than where it is kept. The terminator goes
    // in so that a message and that message with something appended cannot
    // collide, and the no-message case is a byte of its own rather than
    // nothing at all.
    const char *e = m->lobby_error;
    h = gs_fnv(h, e != nullptr ? e : "", e != nullptr ? strlen(e) + 1 : 1);

    return h;
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

    // **A panel that has just appeared takes the focus.**
    //
    // Reported from play: Escape out of a race puts the setup screen up and
    // "no control works, eventually the controls do become accessible" - the
    // first click was being spent focusing the dialog rather than pressing what
    // it landed on. A menu opened over a race arrives behind the race's own
    // windows, and ImGui leaves the focus where it was; the person driving has
    // no way to know that the screen in front of them is not the one listening.
    //
    // Only on the frame the screen changes. Doing it every frame would drag the
    // focus back to the panel from whatever is inside it, which is a text field
    // losing what somebody is halfway through typing.
    if (m->take_focus) {
        ImGui_SetNextWindowFocus();
        m->take_focus = false;
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

void gs_setup_build(const gs_race_setup *s, const gs_track *t, gs_world *w) {
    // **The dial is a multiple of Earth; `gravity` is an acceleration.**
    // Assigning one to the other set a race's gravity to 1.0 tiles per second
    // squared instead of Earth's 2.45 - so every race started from the setup
    // screen ran at forty percent of the gravity it said it was at, and the
    // records table it wrote could never be found again, because the screen
    // looking it up made the same mistake differently. gs_world_init is the one
    // place that knows the conversion.
    gs_world_init(w, s->gravity);
    gs_world_set_mode(w, (gs_mode)s->mode);
    gs_world_set_laps(w, s->mode == (uint8_t)GS_MODE_RACE ? s->laps : 0);

    // **Armed before anybody is on the grid**, so every car added below gets
    // the loadout without this having to remember to do it twice. Turned off
    // means zero of everything, which is the same world every race had before
    // weapons existed - not a special case anywhere in the simulation.
    for (int k = GS_HAZ_NONE + 1; k < GS_HAZ_COUNT; k++) {
        gs_world_arm(w, (gs_hazard_kind)k, s->weapons ? s->ammo[k] : 0);
    }

    for (uint8_t i = 0; i < s->players && i < GS_MAX_CARS; i++) {
        gs_fix sx, sy;
        gs_angle facing;
        gs_track_grid(t, i, &sx, &sy, &facing);
        gs_world_add_car(w, t, s->vehicle[i], sx, sy, facing);
    }
}

void gs_setup_drive(const gs_race_setup *s, const gs_world *w,
                    const gs_track *t, gs_input *in) {
    if (s == nullptr || w == nullptr || t == nullptr || in == nullptr) return;

    const gs_fix margin = gs_ai_skill_margin((int)s->skill);
    for (uint8_t i = 0; i < w->car_count && i < GS_MAX_CARS; i++) {
        if (!s->computer[i]) continue;
        in[i] = gs_ai_drive_at(w, t, i, margin);
    }
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
    //
    // **And never the game's own cars.** An opponent is not a person and its
    // time is not a record: a records table with the computer at the top of it
    // is a table nobody can get on, and the setting it drove at is not written
    // anywhere the table could say.
    for (uint8_t i = 0; i < m->result_count; i++) {
        gs_result_row *r = &m->result[i];
        if (m->setup.computer[r->car]) continue;
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

    // **The chequered flag is a screen change, and it belongs here.** Every
    // other move between screens is a decision the front end makes and can be
    // asked about; this one was two assignments in the client's frame loop, so
    // the results screen was the one screen nothing walking the menu could
    // reach and the graph had a hole in it that no test could see. Setting it
    // where the results are built costs nothing and makes "every screen is
    // reachable from the title" a property rather than an exception.
    //
    // Abandoning a race is not finishing one - a client caught lying, or a
    // machine gone quiet - and those still go to the results from where they
    // are noticed, because there is nothing to build there.
    m->screen = GS_SCREEN_RESULTS;
}

const char *gs_screen_name(gs_screen s) {
    switch (s) {
    case GS_SCREEN_LOGIN:    return "login";
    case GS_SCREEN_TITLE:    return "title";
    case GS_SCREEN_PROFILES: return "drivers";
    case GS_SCREEN_SETUP:    return "setup";
    case GS_SCREEN_RACE:     return "race";
    case GS_SCREEN_RESULTS:  return "results";
    case GS_SCREEN_RECORDS:  return "records";
    case GS_SCREEN_LOBBY:    return "lobby";
    case GS_SCREEN_TRACKS:   return "tracks";
    case GS_SCREEN_COUNT:    break;      // not a screen; not nameable
    }
    return "?";
}
